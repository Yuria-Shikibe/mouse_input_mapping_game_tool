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
    bool trigger_key(key_code code) const noexcept {
        return settings_.enabled && settings_.trigger.kind == input_kind::keyboard && settings_.trigger.code == code;
    }
    bool trigger_mouse() const noexcept {
        return settings_.enabled && settings_.trigger.kind == input_kind::mouse;
    }
    bool output_key(key_code code) const {
        return settings_.enabled && ((key_bound(settings_.first) && code == settings_.first)
            || (key_bound(settings_.second) && code == settings_.second));
    }
    void seed(key_code code) { first_.seed_physical(code); second_.seed_physical(code); }
    template<class Sink> void output(key_event event, Sink&& send) {
        if (!settings_.enabled || !(first_.physical(event, send) || second_.physical(event, send))) send(event);
    }
    template<class Sink> void keyboard(std::uintptr_t device, key_code code, bool down,
        bool enabled, int output_keyboard, Sink&& send) {
        if (trigger_key(code)) trigger(device, down, enabled, output_keyboard, send);
    }
    template<class Sink> void mouse(std::uintptr_t device, unsigned short flags, bool enabled,
        int output_keyboard, Sink&& send) {
        if (!trigger_mouse()) return;
        const auto down_bit = static_cast<unsigned short>(1u << (settings_.trigger.code * 2));
        const auto up_bit = static_cast<unsigned short>(down_bit << 1);
        if (flags & down_bit) trigger(device, true, enabled, output_keyboard, send);
        if (flags & up_bit) trigger(device, false, enabled, output_keyboard, send);
    }
    bool owns_device(std::uintptr_t device) const noexcept {
        for (const auto& entry : devices_) if (entry.used && entry.device == device && entry.down) return true;
        return false;
    }
    bool held() const noexcept {
        for (const auto& entry : devices_) if (entry.used && entry.down) return true;
        return false;
    }
    template<class Sink> void remove_device(std::uintptr_t device, Sink&& send) {
        for (auto& entry : devices_) if (entry.used && entry.device == device) entry = {};
        set(any_held(), 0, send);
    }
    template<class Sink> void reset(Sink&& send) {
        for (auto& entry : devices_) entry = {};
        set(false, 0, send);
    }
    template<class Sink> void cancel(Sink&& send) {
        for (auto& entry : devices_) entry.held = false;
        set(false, 0, send);
    }
private:
    struct device_state {
        std::uintptr_t device{};
        bool used = false, down = false, held = false;
    };
    device_state* device_entry(std::uintptr_t device) noexcept {
        for (auto& entry : devices_) if (entry.used && entry.device == device) return &entry;
        for (auto& entry : devices_) if (!entry.used) {
            entry = {device, true, false, false};
            return &entry;
        }
        return nullptr;
    }
    bool any_held() const noexcept {
        for (const auto& entry : devices_) if (entry.used && entry.held) return true;
        return false;
    }
    template<class Sink> void trigger(std::uintptr_t device, bool down, bool enabled,
        int output_keyboard, Sink&& send) {
        auto* entry = device_entry(device);
        if (!entry) return;
        if (down) {
            if (!entry->down) {
                entry->down = true;
                entry->held = enabled;
            }
        } else {
            *entry = {};
        }
        set(any_held() && enabled, output_keyboard, send);
    }
    template<class Sink> void set(bool down, int device, Sink&& send) {
        std::exception_ptr failure;
        const auto next = down ? direction::left : direction::idle;
        try { first_.set_direction(next, device, send); } catch (...) { failure = std::current_exception(); }
        try { second_.set_direction(next, device, send); } catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
    }
    chord_settings settings_;
    key_router first_, second_;
    // Keep trigger ownership per input device. Mouse device identities are not
    // limited to keyboard IDs (and are pointer-sized in the Raw Input backend).
    std::array<device_state, 64> devices_{};
};
}
