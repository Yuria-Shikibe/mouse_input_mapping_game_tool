#pragma once
#include "config.hpp"
#include <windows.h>

namespace mouse_mapping {
inline int binding_vk(input_binding binding) {
    constexpr std::array buttons{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
    if (binding.kind == input_kind::mouse) return buttons.at(binding.code);
    if (binding.kind == input_kind::keyboard) return static_cast<int>(MapVirtualKeyW(binding.code, MAPVK_VSC_TO_VK_EX));
    return 0;
}
// Separate device tables prevent keyboard IDs from colliding with mouse handles.
class bypass_state {
public:
    explicit bypass_state(const configuration& config) : bindings_(config.bypass_keys) {}
    void seed() {
        for (std::size_t i = 0; i < 2; ++i)
            initial_[i] = (GetAsyncKeyState(binding_vk(bindings_[i])) & 0x8000) != 0;
    }
    bool held() const {
        if (initial_[0] || initial_[1]) return true;
        for (const auto& item : keyboards_) if (item.second[0] || item.second[1]) return true;
        for (const auto& item : mice_) if (item.second[0] || item.second[1]) return true;
        return false;
    }
    bool keyboard_binding(key_code code) const {
        for (const auto binding : bindings_)
            if (binding == input_binding{input_kind::keyboard, code}) return true;
        return false;
    }
    void keyboard(std::uintptr_t device, key_code code, bool down) {
        for (std::size_t i = 0; i < 2; ++i)
            if (bindings_[i] == input_binding{input_kind::keyboard, code}) {
                initial_[i] = false;
                keyboards_[device][i] = down;
            }
    }
    // Return whether any DOWN in this packet requests bypass, including DOWN+UP.
    bool mouse(std::uintptr_t device, unsigned short flags) {
        bool pressed = false;
        for (std::size_t i = 0; i < 2; ++i) {
            if (bindings_[i].kind != input_kind::mouse) continue;
            const auto shift = bindings_[i].code * 2;
            if (flags & (1u << shift)) { initial_[i] = false; mice_[device][i] = true; pressed = true; }
            if (flags & (2u << shift)) { initial_[i] = false; mice_[device][i] = false; }
        }
        return pressed;
    }
    void remove_mouse(std::uintptr_t device) { mice_.erase(device); }
    void remove_keyboard(std::uintptr_t device) { keyboards_.erase(device); }
    bool owns_device(std::uintptr_t device, bool mouse) const {
        const auto& devices = mouse ? mice_ : keyboards_;
        for (const auto& item : devices)
            if (item.first == device) return item.second[0] || item.second[1];
        return false;
    }
    // Startup state has no device identity. Clear it once Windows reports release.
    void refresh_initial() {
        for (std::size_t i = 0; i < 2; ++i)
            if (initial_[i] && !(GetAsyncKeyState(binding_vk(bindings_[i])) & 0x8000)) initial_[i] = false;
    }
private:
    std::array<input_binding, 2> bindings_;
    std::array<bool, 2> initial_{};
    device_table<std::array<bool, 2>> keyboards_, mice_;
};
}
