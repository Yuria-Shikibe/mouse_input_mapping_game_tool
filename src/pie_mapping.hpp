#pragma once
#include "config.hpp"
#include <exception>
#include <numbers>

namespace mouse_mapping {
enum class pie_direction : unsigned { none, up, down, left, right };

// Pure input state. No windows, allocation, sample history or frame clock.
class pie_mapping {
public:
    explicit pie_mapping(const pie_settings& settings)
        : deadzone2_(double(settings.deadzone_counts) * settings.deadzone_counts),
          radius2_(double(settings.radius_counts) * settings.radius_counts),
          radius_(settings.radius_counts), hold_(settings.key_hold_ms),
          boundary_(std::tan((45.0 + settings.hysteresis_degrees) * std::numbers::pi / 180.0)) {}

    void begin() noexcept { x_ = y_ = 0; selected_ = pie_direction::none; }
    pie_direction selected() const noexcept { return selected_; }
    void motion(std::int32_t dx, std::int32_t dy) noexcept {
        if (dx == 0 && dy == 0) return;
        x_ += dx; y_ += dy;
        auto distance2 = x_ * x_ + y_ * y_;
        if (distance2 > radius2_) {
            const double scale = radius_ / std::sqrt(distance2);
            x_ *= scale; y_ *= scale;
            distance2 = radius2_;
        }
        if (distance2 <= deadzone2_ * 0.25) { selected_ = pie_direction::none; return; }
        if (selected_ == pie_direction::none && distance2 < deadzone2_) return;
        const double ax = std::abs(x_), ay = std::abs(y_);
        // Retain the current sector through its enlarged angular boundary.
        switch (selected_) {
        case pie_direction::up: if (y_ < 0 && ax <= ay * boundary_) return; break;
        case pie_direction::down: if (y_ > 0 && ax <= ay * boundary_) return; break;
        case pie_direction::left: if (x_ < 0 && ay <= ax * boundary_) return; break;
        case pie_direction::right: if (x_ > 0 && ay <= ax * boundary_) return; break;
        default: break;
        }
        selected_ = ax >= ay ? (x_ < 0 ? pie_direction::left : pie_direction::right)
            : (y_ < 0 ? pie_direction::up : pie_direction::down);
    }

    void seed_physical(key_code code) { horizontal_.seed_physical(code); vertical_.seed_physical(code); }
    template<class Sink> bool physical(key_event event, Sink&& send) {
        return horizontal_.physical(event, send) || vertical_.physical(event, send);
    }
    template<class Sink> void confirm(time_point now, int keyboard, Sink&& send) {
        if (selected_ == pie_direction::none) return;
        pending_ = selected_; // One pending confirmation; newest intent wins.
        keyboard_ = keyboard;
        tick(now, send);
    }
    template<class Sink> void tick(time_point now, Sink&& send) {
        if (active_ && now >= due_) {
            release_keys(send);
            active_ = false;
            due_ = now + milliseconds(1);
        }
        if (!active_ && pending_ != pie_direction::none && now >= due_) {
            const auto next = pending_;
            // Mark ownership before sending so failure cleanup can retry.
            active_ = true;
            due_ = now + hold_;
            horizontal_.set_direction(next == pie_direction::left ? direction::left :
                next == pie_direction::right ? direction::right : direction::idle, keyboard_, send);
            vertical_.set_direction(next == pie_direction::up ? direction::left :
                next == pie_direction::down ? direction::right : direction::idle, keyboard_, send);
            pending_ = pie_direction::none;
        }
    }
    template<class Sink> void cancel(Sink&& send) {
        begin();
        cancel_output(send);
    }
    template<class Sink> void cancel_output(Sink&& send) {
        pending_ = pie_direction::none;
        release_keys(send);
        active_ = false;
        due_ = {};
    }
    std::optional<time_point> deadline() const noexcept {
        return active_ || pending_ != pie_direction::none ? std::optional(due_) : std::nullopt;
    }
private:
    template<class Sink> void release_keys(Sink&& send) {
        std::exception_ptr failure;
        try { horizontal_.set_direction(direction::idle, keyboard_, send); }
        catch (...) { failure = std::current_exception(); }
        try { vertical_.set_direction(direction::idle, keyboard_, send); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
    }
    double x_ = 0, y_ = 0, deadzone2_, radius2_, radius_;
    milliseconds hold_;
    double boundary_;
    pie_direction selected_ = pie_direction::none, pending_ = pie_direction::none;
    key_router horizontal_{0xe04b, 0xe04d}, vertical_{0xe048, 0xe050};
    int keyboard_ = 1;
    bool active_ = false;
    time_point due_{};
};
}
