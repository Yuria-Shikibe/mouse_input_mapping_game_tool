#pragma once

#include "sensitivity_curve.hpp"
#include "input_storage.hpp"
#include <bit>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <algorithm>
#include <optional>
#include <stdexcept>

namespace mouse_mapping {

using key_code = std::uint16_t; // Scan code, with 0xe000 for an E0 prefix.
using clock_type = std::chrono::steady_clock;
using time_point = clock_type::time_point;
using milliseconds = std::chrono::milliseconds;

enum class direction { idle, left, right };

// Observes mouse intent before physical-key ownership is reconciled. Observers
// must never interfere with input delivery or cleanup.
struct direction_observer {
    void* context = nullptr;
    void (*changed)(void*, direction) noexcept = nullptr;
    void operator()(direction value) const noexcept { if (changed) changed(context, value); }
};

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
        samples_.reserve(static_cast<std::size_t>(settings_.window_ms) * 16 + 1);
    }

    direction update(double delta_x, time_point now) {
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
        if (samples_.empty()) total_ = 0; // Discard floating-point subtraction residue.
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

    double take_confirmed_counts() {
        const auto result = confirmed_counts_;
        confirmed_counts_ = 0;
        return result;
    }

    std::optional<time_point> deadline() const {
        return direction_ == direction::idle ? std::nullopt
            : std::optional(last_confirmed_ + milliseconds(settings_.release_ms));
    }

private:
    struct sample { time_point time; double delta_x; };
    void confirm(direction next, time_point now) {
        confirmed_counts_ = total_ < 0 ? -total_ : total_;
        direction_ = next;
        last_confirmed_ = now;
        samples_.clear();
        total_ = 0;
    }
    filter_settings settings_;
    bool preserve_pending_;
    sample_ring<sample> samples_;
    double total_ = 0;
    double confirmed_counts_ = 0;
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
    key_router(key_code left, key_code right, bool keyboard_override = false)
        : codes_{left, right}, keyboard_override_(keyboard_override) {}

    // Adopt a key held before input interception began, without injecting a duplicate.
    void seed_physical(key_code code) {
        const int index = code == codes_[0] ? 0 : code == codes_[1] ? 1 : -1;
        if (index < 0) return;
        initial_physical_[index] = true;
        output_down_[index] = true;
    }

    template<class sink_type>
    bool physical(key_event event, sink_type&& send) {
        if (event.device < 1 || event.device > 10) return false;
        const int index = event.code == codes_[0] ? 0 : event.code == codes_[1] ? 1 : -1;
        if (index < 0) return false;
        if (initial_physical_[index]) {
            initial_physical_[index] = false;
            output_device_[index] = event.device;
        }
        const auto bit = static_cast<std::uint16_t>(1u << (event.device - 1));
        const bool repeated = (physical_[index] & bit) && event.down;
        if (event.down) physical_[index] |= bit;
        else physical_[index] &= static_cast<std::uint16_t>(~bit);
        const bool previous = output_down_[index];
        reconcile(send);
        // A key may already have been held before the program started. Its
        // first observed release must still reach Windows.
        if (!event.down && !previous && !output_down_[index]) send(event);
        if (repeated && previous && output_down_[index])
            send(key_event{output_device_[index], event.code, true});
        return true;
    }

    template<class sink_type>
    void set_direction(direction next, int device, sink_type&& send) {
        const bool changed = desired_ != next;
        desired_ = next;
        if (changed) observer_(next);
        if (device >= 1 && device <= 10) desired_device_ = device;
        reconcile(send);
    }

    void observe(direction_observer observer) noexcept {
        const bool changed = observer_.context != observer.context || observer_.changed != observer.changed;
        observer_ = observer;
        if (changed) observer_(desired_);
    }

private:
    bool physically_down(int index) const {
        return initial_physical_[index] || physical_[index] != 0;
    }
    int physical_device(int index) const {
        return physical_[index] ? std::countr_zero(physical_[index]) + 1 : 0;
    }
    bool wanted(int index) const {
        const bool blocked = keyboard_override_ && (physically_down(0) || physically_down(1));
        return physically_down(index) || (!blocked
            && ((index == 0 && desired_ == direction::left)
                || (index == 1 && desired_ == direction::right)));
    }
    template<class sink_type>
    void sync(int index, bool next, sink_type&& send) {
        if (next == output_down_[index]) return;
        const int device = next && physically_down(index) ? physical_device(index) : desired_device_;
        if (next && (device < 1 || device > 10)) throw std::runtime_error("No output keyboard selected");
        send(key_event{next ? device : output_device_[index], codes_[index], next});
        output_down_[index] = next;
        if (next) output_device_[index] = device;
    }
    template<class sink_type>
    void reconcile(sink_type&& send) {
        const std::array next{wanted(0), wanted(1)};
        // Always release stale ownership before pressing a replacement.
        for (int index = 0; index < 2; ++index)
            if (!next[index]) sync(index, false, send);
        for (int index = 0; index < 2; ++index)
            if (next[index]) sync(index, true, send);
    }
    std::array<key_code, 2> codes_;
    std::array<std::uint16_t, 2> physical_{};
    std::array<bool, 2> output_down_{};
    std::array<bool, 2> initial_physical_{};
    std::array<int, 2> output_device_{};
    direction desired_ = direction::idle;
    int desired_device_ = 0;
    bool keyboard_override_ = false;
    direction_observer observer_;
};

// One direction pair, with optional displacement-driven key pulses.
class axis_mapping {
public:
    void seed_physical(key_code code) { router_.seed_physical(code); }
    void observe(direction_observer observer) noexcept { router_.observe(observer); }
    axis_mapping(filter_settings filter, key_code negative, key_code positive,
                 bool pulse, double hold_ratio, int period_ms, bool keyboard_override = false,
                 sensitivity_settings sensitivity = {}, double smoothing_factor = 1.0)
        : filter_(filter, pulse), router_(negative, positive, keyboard_override), pulse_(pulse), ratio_(hold_ratio),
          period_(period_ms), pulse_counts_(filter.start_counts),
          sensitivity_(std::move(sensitivity)), curve_(sensitivity_.points), active_ratio_(hold_ratio),
          smoothing_factor_(smoothing_factor), smoothing_timeout_(filter.release_ms) {}

    template<class Sink>
    void update(std::int32_t delta, time_point now, int device, Sink&& send) {
        expire_smoothing(now);
        double input = 0;
        // Only actual motion advances lerp; zero packets must not replay its tail.
        if (delta != 0) {
            last_motion_ = now;
            smoothed_ = smoothing_factor_ == 1.0 ? static_cast<double>(delta)
                : std::lerp(smoothed_, static_cast<double>(delta), smoothing_factor_);
            input = smoothed_;
        }
        const auto previous = direction_;
        direction_ = filter_.update(input, now);
        const auto counts = filter_.take_confirmed_counts();
        if (pulse_ && sensitivity_.enabled) {
            if (previous != direction::idle && direction_ != previous) clear_speed();
            if (input) { speed_samples_.push_back({now, input}); speed_total_ += input; }
            expire_speed(now);
        }
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
        if (counts && (sensitivity_.enabled || ratio_ > 0)) credit_ = std::min<double>(pulse_counts_, credit_ + counts);
        service(now, device, send);
    }

    template<class Sink>
    void tick(time_point now, int device, Sink&& send) {
        expire_smoothing(now);
        const auto next = filter_.tick(now);
        if (!pulse_) { router_.set_direction(next, device, send); return; }
        if (next == direction::idle) { credit_ = 0; clear_speed(); }
        direction_ = next;
        service(now, device, send);
    }

    template<class Sink>
    void release(int device, Sink&& send) {
        smoothed_ = 0;
        last_motion_.reset();
        filter_.reset();
        clear_speed();
        direction_ = direction::idle;
        credit_ = 0;
        pressed_ = false;
        due_.reset();
        router_.set_direction(direction::idle, device, send);
    }

    template<class Sink>
    bool physical(key_event event, Sink&& send) { return router_.physical(event, send); }

    std::optional<time_point> deadline() const {
        auto result = filter_.deadline();
        if (pulse_ && (pressed_ || credit_ >= pulse_counts_) && due_ && (!result || *due_ < *result)) result = due_;
        return result;
    }

private:
    clock_type::duration down_time() const {
        const auto nanos = std::max<std::int64_t>(1000000,
            static_cast<std::int64_t>(period_ * active_ratio_ * 1000000.0));
        return std::chrono::duration_cast<clock_type::duration>(std::chrono::nanoseconds(nanos));
    }
    clock_type::duration up_time() const {
        const auto nanos = std::max<std::int64_t>(1000000,
            static_cast<std::int64_t>(period_ * (1.0 - active_ratio_) * 1000000.0));
        return std::chrono::duration_cast<clock_type::duration>(std::chrono::nanoseconds(nanos));
    }
    template<class Sink>
    void service(time_point now, int device, Sink&& send) {
        if (pressed_ && due_ && now >= *due_) {
            router_.set_direction(direction::idle, device, send);
            pressed_ = false;
            due_ = now + up_time();
        }
        if (!pressed_ && direction_ != direction::idle && (sensitivity_.enabled || ratio_ > 0)
            && credit_ >= pulse_counts_ && (!due_ || now >= *due_)) {
            expire_speed(now);
            const double next_ratio = sensitivity_.enabled
                ? curve_.evaluate(std::abs(static_cast<double>(speed_total_)) / 0.03 / sensitivity_.full_speed)
                : ratio_;
            if (next_ratio <= 0) { credit_ = 0; return; }
            router_.set_direction(direction_, device, send);
            active_ratio_ = next_ratio;
            pressed_ = true;
            credit_ -= pulse_counts_;
            due_ = now + down_time();
        }
    }
    void clear_speed() { speed_samples_.clear(); speed_total_ = 0; }
    void expire_speed(time_point now) {
        while (!speed_samples_.empty() && now - speed_samples_.front().time >= milliseconds(30)) {
            speed_total_ -= speed_samples_.front().delta;
            speed_samples_.pop_front();
        }
        if (speed_samples_.empty()) speed_total_ = 0;
    }
    void expire_smoothing(time_point now) {
        if (last_motion_ && now - *last_motion_ >= smoothing_timeout_) {
            smoothed_ = 0;
            last_motion_.reset();
        }
    }
    struct speed_sample { time_point time; double delta; };
    motion_filter filter_;
    key_router router_;
    bool pulse_;
    double ratio_;
    int period_;
    int pulse_counts_;
    sensitivity_settings sensitivity_;
    sensitivity_curve curve_;
    double active_ratio_;
    double smoothing_factor_;
    milliseconds smoothing_timeout_;
    double smoothed_ = 0;
    std::optional<time_point> last_motion_;
    sample_ring<speed_sample> speed_samples_;
    double speed_total_ = 0;
    direction direction_ = direction::idle;
    double credit_ = 0;
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
