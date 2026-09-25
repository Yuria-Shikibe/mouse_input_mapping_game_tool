#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <deque>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace mouse_mapping {

using key_code = std::uint16_t; // Scan code, with 0xe000 for an E0 prefix.
using clock_type = std::chrono::steady_clock;
using time_point = clock_type::time_point;
using milliseconds = std::chrono::milliseconds;

enum class direction { idle, left, right };

struct filter_settings {
    int window_ms = 30;
    int start_counts = 3;
    int reverse_counts = 6;
    int release_ms = 60;
};

class motion_filter {
public:
    explicit motion_filter(filter_settings settings = {}, bool pulse = false)
        : settings_(settings), preserve_pending_(pulse) {
        // Slow motion needs time to cross the same noise threshold as fast motion.
        if (pulse) settings_.window_ms = std::max({settings_.window_ms, 2 * settings_.release_ms, 120});
    }

    direction update(std::int32_t delta_x, time_point now) {
        confirmed_counts_ = 0;
        tick(now);
        if (delta_x == 0) return direction_;
        samples_.push_back({now, delta_x});
        total_ += delta_x;
        const auto sign = direction_ == direction::left ? -1 : 1;
        if (direction_ == direction::idle) {
            if (total_ >= settings_.start_counts) confirm(direction::right, now);
            else if (total_ <= -settings_.start_counts) confirm(direction::left, now);
        } else if (total_ * sign <= -settings_.reverse_counts) {
            confirm(direction_ == direction::left ? direction::right : direction::left, now);
        } else if (total_ * sign >= settings_.start_counts) {
            confirm(direction_, now);
        }
        return direction_;
    }

    direction tick(time_point now) {
        while (!samples_.empty() && now - samples_.front().time >= milliseconds(settings_.window_ms)) {
            total_ -= samples_.front().delta_x;
            samples_.pop_front();
        }
        if (direction_ != direction::idle && now - last_confirmed_ >= milliseconds(settings_.release_ms)) {
            if (preserve_pending_) {
                // Direction lifetime and unconfirmed displacement have separate
                // clocks. Only a new nonzero sample can confirm the next direction.
                direction_ = direction::idle;
                confirmed_counts_ = 0;
            } else {
                reset();
            }
        }
        return direction_;
    }

    void reset() {
        direction_ = direction::idle;
        samples_.clear();
        total_ = 0;
        confirmed_counts_ = 0;
    }

    std::int64_t take_confirmed_counts() {
        const auto result = confirmed_counts_;
        confirmed_counts_ = 0;
        return result;
    }

private:
    struct sample { time_point time; std::int32_t delta_x; };
    void confirm(direction next, time_point now) {
        confirmed_counts_ = total_ < 0 ? -total_ : total_;
        direction_ = next;
        last_confirmed_ = now;
        samples_.clear();
        total_ = 0;
    }
    filter_settings settings_;
    bool preserve_pending_;
    std::deque<sample> samples_;
    std::int64_t total_ = 0;
    std::int64_t confirmed_counts_ = 0;
    direction direction_ = direction::idle;
    time_point last_confirmed_{};
};

struct key_event {
    int device;
    key_code code;
    bool down;
};

// Reconcile physical ownership with mapping ownership. A failed send must not
// change the recorded OS state, so cleanup can retry the outstanding transition.
class key_router {
public:
    key_router(key_code left, key_code right) : codes_{left, right} {}

    template<class sink_type>
    bool physical(key_event event, sink_type&& send) {
        if (event.device < 1 || event.device > 10) return false;
        const int index = event.code == codes_[0] ? 0 : event.code == codes_[1] ? 1 : -1;
        if (index < 0) return false;
        const bool repeated = physical_[event.device - 1][index] && event.down;
        physical_[event.device - 1][index] = event.down;
        const bool previous = output_down_[index];
        sync(index, event.device, send);
        // A key may already have been held before the program started. Its
        // first observed release must still reach Windows.
        if (!event.down && !previous && !output_down_[index]) send(event);
        if (repeated && previous && output_down_[index])
            send(key_event{output_device_[index], event.code, true});
        return true;
    }

    template<class sink_type>
    void set_direction(direction next, int device, sink_type&& send) {
        desired_ = next;
        // Release the opposite key before pressing a new one.
        const int first = next == direction::left ? 1 : 0;
        sync(first, device, send);
        sync(1 - first, device, send);
    }

private:
    bool physically_down(int index) const {
        for (const auto& keyboard : physical_) if (keyboard[index]) return true;
        return false;
    }
    template<class sink_type>
    void sync(int index, int device, sink_type&& send) {
        const bool wanted = physically_down(index)
            || (index == 0 && desired_ == direction::left)
            || (index == 1 && desired_ == direction::right);
        if (wanted == output_down_[index]) return;
        if (wanted && (device < 1 || device > 10)) throw std::runtime_error("No output keyboard selected");
        send(key_event{wanted ? device : output_device_[index], codes_[index], wanted});
        output_down_[index] = wanted;
        if (wanted) output_device_[index] = device;
    }
    std::array<key_code, 2> codes_;
    std::array<std::array<bool, 2>, 10> physical_{};
    std::array<bool, 2> output_down_{};
    std::array<int, 2> output_device_{};
    direction desired_ = direction::idle;
};

// One direction pair, with optional displacement-driven key pulses.
class axis_mapping {
public:
    axis_mapping(filter_settings filter, key_code negative, key_code positive,
                 bool pulse, double hold_ratio, int period_ms)
        : filter_(filter, pulse), router_(negative, positive), pulse_(pulse), ratio_(hold_ratio),
          period_(period_ms), pulse_counts_(filter.start_counts) {}

    template<class Sink>
    void update(std::int32_t delta, time_point now, int device, Sink&& send) {
        const auto previous = direction_;
        direction_ = filter_.update(delta, now);
        const auto counts = filter_.take_confirmed_counts();
        if (!pulse_) { router_.set_direction(direction_, device, send); return; }
        if (direction_ != previous) {
            credit_ = 0;
            if (pressed_) {
                router_.set_direction(direction::idle, device, send);
                pressed_ = false;
                due_ = now + up_time();
            }
        }
        // One confirmed movement is enough to respond; retain at most one pulse.
        if (counts && ratio_ > 0) credit_ = std::min<std::int64_t>(pulse_counts_, credit_ + counts);
        service(now, device, send);
    }

    template<class Sink>
    void tick(time_point now, int device, Sink&& send) {
        const auto next = filter_.tick(now);
        if (!pulse_) { router_.set_direction(next, device, send); return; }
        if (next == direction::idle) credit_ = 0;
        direction_ = next;
        service(now, device, send);
    }

    template<class Sink>
    void release(int device, Sink&& send) {
        filter_.reset();
        direction_ = direction::idle;
        credit_ = 0;
        pressed_ = false;
        due_.reset();
        router_.set_direction(direction::idle, device, send);
    }

    template<class Sink>
    bool physical(key_event event, Sink&& send) { return router_.physical(event, send); }

    std::optional<time_point> deadline() const {
        return pulse_ && (pressed_ || credit_ >= pulse_counts_) ? due_ : std::nullopt;
    }

private:
    clock_type::duration down_time() const {
        const auto nanos = std::max<std::int64_t>(1000000,
            static_cast<std::int64_t>(period_ * ratio_ * 1000000.0));
        return std::chrono::duration_cast<clock_type::duration>(std::chrono::nanoseconds(nanos));
    }
    clock_type::duration up_time() const {
        const auto nanos = std::max<std::int64_t>(1000000,
            static_cast<std::int64_t>(period_ * (1.0 - ratio_) * 1000000.0));
        return std::chrono::duration_cast<clock_type::duration>(std::chrono::nanoseconds(nanos));
    }
    template<class Sink>
    void service(time_point now, int device, Sink&& send) {
        if (pressed_ && due_ && now >= *due_) {
            router_.set_direction(direction::idle, device, send);
            pressed_ = false;
            due_ = now + up_time();
        }
        if (!pressed_ && direction_ != direction::idle && ratio_ > 0
            && credit_ >= pulse_counts_ && (!due_ || now >= *due_)) {
            router_.set_direction(direction_, device, send);
            pressed_ = true;
            credit_ -= pulse_counts_;
            due_ = now + down_time();
        }
    }
    motion_filter filter_;
    key_router router_;
    bool pulse_;
    double ratio_;
    int period_;
    int pulse_counts_;
    direction direction_ = direction::idle;
    std::int64_t credit_ = 0;
    bool pressed_ = false;
    std::optional<time_point> due_;
};

class toggle_latch {
public:
    bool update(int device, bool down) {
        if (device < 1 || device > 10) return false;
        const bool fire = down && !down_[device - 1];
        down_[device - 1] = down;
        return fire;
    }
private:
    std::array<bool, 10> down_{};
};

} // namespace mouse_mapping
