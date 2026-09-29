#pragma once
#include "config.hpp"
#include "button_mapping.hpp"
#include "wheel_mapping.hpp"
#include <exception>

namespace mouse_mapping {
// Two independent filters allow diagonals and independent axis timeouts.
class xy_mapping {
public:
    explicit xy_mapping(const configuration& source) : xy_mapping(effective_config(source), 0) {}
private:
    xy_mapping(const configuration& config, int)
        : horizontal_(config.axis_filter(false), config.left_key, config.right_key, config.x_pulse_enabled,
              config.x_hold_ratio, config.pulse_period_ms, config.x_keyboard_override_enabled, config.x_curve, config.x_smoothing_factor),
          vertical_(config.axis_filter(true), config.up_key, config.down_key, config.y_pulse_enabled,
              config.y_hold_ratio, config.pulse_period_ms, false, config.y_curve, config.y_smoothing_factor), buttons_(config.mouse_keys),
          wheel_(config.wheel_keys[0], config.wheel_keys[1]), y_enabled_(y_enabled(config)), user_(config.map_y) {}
public:

    void observe(direction_observer x, direction_observer y) noexcept {
        horizontal_.observe(x);
        vertical_.observe(y);
    }

    template<class Sink>
    void buttons(std::uintptr_t device, unsigned short flags, Sink&& send) {
        if (user_) buttons_.update(device, flags, [&](std::size_t index, key_event event) {
            outputs_.send(button_first_source + index, event, send);
        });
    }
    template<class Sink>
    void wheel(std::uintptr_t device, std::int16_t delta, time_point now, Sink&& send) {
        if (user_) wheel_.update(device, delta, now, [&](key_event event) {
            outputs_.send(wheel_source, event, send);
        });
    }
    template<class Sink>
    void remove_mouse(std::uintptr_t device, Sink&& send) {
        // Axis motion is merged across mice; dropping one device invalidates
        // both histories. Attempt every release even if a send fails.
        std::exception_ptr failure;
        try { horizontal_.release(1, [&](key_event event) { outputs_.send(horizontal_source, event, send); }); }
        catch (...) { failure = std::current_exception(); }
        try { vertical_.release(1, [&](key_event event) { outputs_.send(vertical_source, event, send); }); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        try { buttons_.remove(device, [&](std::size_t index, key_event event) {
            outputs_.send(button_first_source + index, event, send);
        }); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        wheel_.remove(device);
        if (failure) std::rethrow_exception(failure);
    }

    template<class Sink>
    void update(std::int32_t x, std::int32_t y, time_point now, Sink&& send) {
        horizontal_.update(x, now, 1, [&](key_event event) { outputs_.send(horizontal_source, event, send); });
        if (y_enabled_) vertical_.update(y, now, 1, [&](key_event event) { outputs_.send(vertical_source, event, send); });
    }
    template<class Sink>
    void tick(time_point now, Sink&& send, bool pause_xy = false) {
        if (!pause_xy) horizontal_.tick(now, 1, [&](key_event event) { outputs_.send(horizontal_source, event, send); });
        if (!pause_xy && y_enabled_) vertical_.tick(now, 1, [&](key_event event) { outputs_.send(vertical_source, event, send); });
        if (user_) wheel_.tick(now, [&](key_event event) { outputs_.send(wheel_source, event, send); });
    }
    template<class Sink>
    void release_motion(Sink&& send) {
        std::exception_ptr failure;
        try { horizontal_.release(1, [&](key_event event) { outputs_.send(horizontal_source, event, send); }); }
        catch (...) { failure = std::current_exception(); }
        try { vertical_.release(1, [&](key_event event) { outputs_.send(vertical_source, event, send); }); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
    }
    template<class Sink>
    bool physical(key_event event, Sink&& send) {
        return horizontal_.physical(event, [&](key_event output) { outputs_.send(horizontal_source, output, send); })
            || (y_enabled_ && vertical_.physical(event, [&](key_event output) { outputs_.send(vertical_source, output, send); }))
            || (user_ && (buttons_.physical(event, [&](std::size_t index, key_event output) {
                    outputs_.send(button_first_source + index, output, send);
                }) || wheel_.physical(event, [&](key_event output) { outputs_.send(wheel_source, output, send); })));
    }
    template<class Sink>
    void release(Sink&& send) {
        // Attempt both axes even if one output fails.
        std::exception_ptr failure;
        try { horizontal_.release(1, [&](key_event event) { outputs_.send(horizontal_source, event, send); }); }
        catch (...) { failure = std::current_exception(); }
        try { vertical_.release(1, [&](key_event event) { outputs_.send(vertical_source, event, send); }); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        try { buttons_.release([&](std::size_t index, key_event event) {
            outputs_.send(button_first_source + index, event, send);
        }); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        try { wheel_.release([&](key_event event) { outputs_.send(wheel_source, event, send); }); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
    }
    std::optional<time_point> deadline(bool pause_xy = false) const {
        auto earliest = pause_xy ? std::nullopt : horizontal_.deadline();
        for (const auto candidate : {pause_xy ? std::nullopt : vertical_.deadline(), wheel_.deadline()})
            if (candidate && (!earliest || *candidate < *earliest)) earliest = candidate;
        return earliest;
    }
private:
    static constexpr std::size_t horizontal_source = 0;
    static constexpr std::size_t vertical_source = 1;
    static constexpr std::size_t button_first_source = 2;
    static constexpr std::size_t wheel_source = 7;
    axis_mapping horizontal_, vertical_;
    button_mapping buttons_;
    wheel_mapping wheel_;
    key_ownership_merger outputs_;
    bool y_enabled_, user_;
};
} // namespace mouse_mapping
