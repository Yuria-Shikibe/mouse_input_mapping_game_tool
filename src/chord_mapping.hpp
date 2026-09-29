#pragma once
#include "config.hpp"
#include <exception>

namespace mouse_mapping {
// Merge the chord with the existing output stream so releasing either owner
// cannot release a key still held by the other owner.
class chord_mapping {
public:
    explicit chord_mapping(chord_settings settings) : settings_(settings),
        first_(settings.first, 0), second_(settings.second, 0) {}
    bool trigger_key(key_code code) const { return settings_.enabled && code == settings_.trigger; }
    bool output_key(key_code code) const { return settings_.enabled && (code == settings_.first || code == settings_.second); }
    void seed(key_code code) { first_.seed_physical(code); second_.seed_physical(code); }
    template<class Sink> void output(key_event event, Sink&& send) {
        if (!settings_.enabled || !(first_.physical(event, send) || second_.physical(event, send))) send(event);
    }
    template<class Sink> void trigger(key_event event, bool enabled, Sink&& send) {
        if (!trigger_key(event.code) || event.device < 1 || event.device > 10) return;
        const auto bit = static_cast<unsigned>(1u << (event.device - 1));
        if (!event.down) { physical_ &= ~bit; held_ &= ~bit; }
        else {
            if (enabled && !(physical_ & bit)) held_ |= bit;
            physical_ |= bit;
        }
        set(held_ != 0 && enabled, event.device, send);
    }
    bool owns_device(int device) const { return device >= 1 && device <= 10 && (physical_ & (1u << (device - 1))); }
    bool held() const { return physical_ != 0; }
    template<class Sink> void remove_device(int device, Sink&& send) {
        trigger({device, settings_.trigger, false}, true, send);
    }
    template<class Sink> void cancel(Sink&& send) { held_ = 0; set(false, 1, send); }
private:
    template<class Sink> void set(bool down, int device, Sink&& send) {
        std::exception_ptr failure;
        const auto next = down ? direction::left : direction::idle;
        try { first_.set_direction(next, device, send); } catch (...) { failure = std::current_exception(); }
        try { second_.set_direction(next, device, send); } catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
    }
    chord_settings settings_;
    key_router first_, second_;
    unsigned held_ = 0, physical_ = 0;
};
}
