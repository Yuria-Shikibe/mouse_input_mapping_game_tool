#pragma once
#include "mapping.hpp"
#include <deque>
#include <map>

namespace mouse_mapping {
// Vertical wheel notches are momentary presses. Keep fractional wheel travel
// per mouse so high-resolution devices do not lose small increments.
class wheel_mapping {
public:
    explicit wheel_mapping(key_code up, key_code down)
        : router_(up, down), same_key_(up == down) {}

    template<class Sink>
    void update(std::uintptr_t device, std::int16_t delta, time_point now, Sink&& send) {
        auto& remainder = remainders_[device];
        remainder += delta;
        while (remainder >= 120) {
            if (pending_.size() < 8) pending_.push_back(direction::left);
            remainder -= 120;
        }
        while (remainder <= -120) {
            if (pending_.size() < 8) pending_.push_back(same_key_ ? direction::left : direction::right);
            remainder += 120;
        }
        tick(now, send);
    }

    template<class Sink>
    void tick(time_point now, Sink&& send) {
        if (active_ && now >= due_) {
            router_.set_direction(direction::idle, 1, send);
            active_ = false;
            due_ = now + milliseconds(10);
        }
        if (!active_ && !pending_.empty() && now >= due_) {
            router_.set_direction(pending_.front(), 1, send);
            pending_.pop_front();
            active_ = true;
            due_ = now + milliseconds(10);
        }
    }

    std::optional<time_point> deadline() const {
        return active_ || !pending_.empty() ? std::optional<time_point>(due_) : std::nullopt;
    }

    template<class Sink>
    bool physical(key_event event, Sink&& send) { return router_.physical(event, send); }

    void remove(std::uintptr_t device) { remainders_.erase(device); }

    template<class Sink>
    void release(Sink&& send) {
        remainders_.clear();
        pending_.clear();
        active_ = false;
        router_.set_direction(direction::idle, 1, send);
    }

private:
    key_router router_;
    bool same_key_;
    std::map<std::uintptr_t, int> remainders_;
    std::deque<direction> pending_;
    bool active_ = false;
    time_point due_{};
};
} // namespace mouse_mapping
