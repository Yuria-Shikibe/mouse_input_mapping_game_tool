#pragma once
#include "config.hpp"
#include "button_mapping.hpp"
#include "wheel_mapping.hpp"
#include <exception>

namespace mouse_mapping {
// Two independent filters allow diagonals and independent axis timeouts.
class xy_mapping {
public:
    explicit xy_mapping(const configuration& config)
        : horizontal_(config.filter, config.left_key, config.right_key, config.x_pulse_enabled,
              config.x_hold_ratio, config.pulse_period_ms),
          vertical_(config.filter, config.up_key, config.down_key, config.y_pulse_enabled,
              config.y_hold_ratio, config.pulse_period_ms), buttons_(config.mouse_keys),
          wheel_(config.wheel_keys[0], config.wheel_keys[1]) {}

    template<class Sink>
    void buttons(std::uintptr_t device, unsigned short flags, Sink&& send) {
        buttons_.update(device, flags, send);
    }
    template<class Sink>
    void wheel(std::uintptr_t device, std::int16_t delta, time_point now, Sink&& send) {
        wheel_.update(device, delta, now, send);
    }
    template<class Sink>
    void remove_mouse(std::uintptr_t device, Sink&& send) {
        buttons_.remove(device, send);
        wheel_.remove(device);
    }

    template<class Sink>
    void update(std::int32_t x, std::int32_t y, time_point now, Sink&& send) {
        horizontal_.update(x, now, 1, send);
        vertical_.update(y, now, 1, send);
    }
    template<class Sink>
    void tick(time_point now, Sink&& send) {
        horizontal_.tick(now, 1, send);
        vertical_.tick(now, 1, send);
        wheel_.tick(now, send);
    }
    template<class Sink>
    bool physical(key_event event, Sink&& send) {
        return horizontal_.physical(event, send) || vertical_.physical(event, send)
            || buttons_.physical(event, send) || wheel_.physical(event, send);
    }
    template<class Sink>
    void release(Sink&& send) {
        // Attempt both axes even if one output fails.
        std::exception_ptr failure;
        try { horizontal_.release(1, send); }
        catch (...) { failure = std::current_exception(); }
        try { vertical_.release(1, send); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        try { buttons_.release(send); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        try { wheel_.release(send); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
    }
    std::optional<time_point> deadline() const {
        auto earliest = horizontal_.deadline();
        for (const auto candidate : {vertical_.deadline(), wheel_.deadline()})
            if (candidate && (!earliest || *candidate < *earliest)) earliest = candidate;
        return earliest;
    }
private:
    axis_mapping horizontal_, vertical_;
    button_mapping buttons_;
    wheel_mapping wheel_;
};
} // namespace mouse_mapping
