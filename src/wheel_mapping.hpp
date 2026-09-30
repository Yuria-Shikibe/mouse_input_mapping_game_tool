#pragma once
#include "mapping.hpp"
#include <array>

namespace mouse_mapping {
// Vertical wheel notches are momentary presses. Keep fractional wheel travel
// per mouse so high-resolution devices do not lose small increments.
class wheel_mapping {
public:
    explicit wheel_mapping(key_code up, key_code down)
        : router_(up, down), up_bound_(key_bound(up)), down_bound_(key_bound(down)), same_key_(up == down) {}

    template<class Sink>
    void update(std::uintptr_t device, std::int16_t delta, time_point now, Sink&& send) {
        auto& remainder = remainders_[device];
        remainder += delta;
        while (remainder >= wheel_delta) {
            if (up_bound_) enqueue(direction::left);
            remainder -= wheel_delta;
        }
        while (remainder <= -wheel_delta) {
            if (down_bound_) enqueue(same_key_ ? direction::left : direction::right);
            remainder += wheel_delta;
        }
        tick(now, send);
    }

    template<class Sink>
    void tick(time_point now, Sink&& send) {
        if (active_ && now >= due_) {
            router_.set_direction(direction::idle, 1, send);
            active_ = false;
            due_ = now + key_interval;
        }
        if (!active_ && pending_count_ != 0 && now >= due_) {
            router_.set_direction(dequeue(), 1, send);
            active_ = true;
            due_ = now + key_interval;
        }
    }

    std::optional<time_point> deadline() const {
        return active_ || pending_count_ != 0 ? std::optional(due_) : std::nullopt;
    }

    template<class Sink>
    bool physical(key_event event, Sink&& send) { return router_.physical(event, send); }

    void remove(std::uintptr_t device) { remainders_.erase(device); }

    template<class Sink>
    void release(Sink&& send) {
        remainders_.clear();
        pending_head_ = 0;
        pending_count_ = 0;
        active_ = false;
        router_.set_direction(direction::idle, 1, send);
    }

private:
    static constexpr auto wheel_delta = 120;
    static constexpr auto maximum_pending = std::size_t{8};
    static constexpr auto key_interval = milliseconds{10};

    void enqueue(direction value) noexcept {
        if (pending_count_ == maximum_pending) return;
        pending_[(pending_head_ + pending_count_) % maximum_pending] = value;
        ++pending_count_;
    }
    direction dequeue() noexcept {
        const auto value = pending_[pending_head_];
        pending_head_ = (pending_head_ + 1) % maximum_pending;
        --pending_count_;
        return value;
    }

    key_router router_;
    const bool up_bound_, down_bound_;
    const bool same_key_;
    device_table<int> remainders_;
    std::array<direction, maximum_pending> pending_{};
    std::size_t pending_head_ = 0;
    std::size_t pending_count_ = 0;
    bool active_ = false;
    time_point due_{};
};
} // namespace mouse_mapping
