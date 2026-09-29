#pragma once
#include "pie_overlay.hpp"
#include <exception>
#include <iostream>

namespace mouse_mapping {
struct pie_packet_result {
    unsigned short buttons;
    bool owns_motion;
};
class pie_runtime {
public:
    explicit pie_runtime(const configuration& config) : settings_(config.pie), mapping_(settings_) {}
    bool configured() const noexcept { return settings_.trigger.kind != input_kind::none; }
    bool trigger_key(key_code code) const noexcept {
        return settings_.trigger.kind == input_kind::keyboard && settings_.trigger.code == code;
    }
    bool opened() const noexcept { return open_; }
    std::uintptr_t owner() const noexcept { return owner_; }
    void start() {
        if (!configured()) return;
        initially_held_ = (GetAsyncKeyState(trigger_vk()) & 0x8000) != 0;
        for (const auto code : pie_arrow_keys)
            if (GetAsyncKeyState(static_cast<int>(MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX))) & 0x8000)
                mapping_.seed_physical(code);
        if (settings_.visual_enabled) {
            try { overlay_.start(); }
            catch (const std::exception& error) {
                std::cerr << "Pie visualization unavailable; input remains active: " << error.what() << '\n';
            }
        }
        std::cout << "Pie: hold " << format_input_binding(settings_.trigger)
            << ", move to select, release to send UP/DOWN/LEFT/RIGHT ("
            << settings_.key_hold_ms << " ms). Main toggle must be ON.\n";
    }
    template<class Sink> bool physical(key_event event, Sink&& send) {
        return configured() && mapping_.physical(event, send);
    }
    // Called before testing normal XY deadlines; arrows continue while another menu opens.
    template<class Sink> void tick(time_point now, Sink&& send) {
        if (!configured()) return;
        const auto due = mapping_.deadline();
        if ((open_ && now >= focus_due_) || (due && now >= *due)) {
            maintenance(send);
            focus_due_ = now + milliseconds(4);
        }
        mapping_.tick(now, send);
    }
    std::optional<time_point> deadline() const {
        if (!configured()) return std::nullopt;
        auto due = mapping_.deadline();
        if (open_ && (!due || focus_due_ < *due)) due = focus_due_;
        return due;
    }
    template<class Sink> void cancel(Sink&& send) {
        open_ = false; owner_ = 0; foreground_ = nullptr; output_foreground_ = nullptr;
        publish();
        mapping_.cancel(send);
    }
    template<class Sink> void maintenance(Sink&& send) {
        if (!configured()) return;
        if ((open_ && GetForegroundWindow() != foreground_) ||
            (mapping_.deadline() && GetForegroundWindow() != output_foreground_)) cancel(send);
        if (initially_held_) {
            if (!(GetAsyncKeyState(trigger_vk()) & 0x8000)) initially_held_ = false;
        }
    }
    template<class Sink> void remove_mouse(std::uintptr_t device, Sink&& send) {
        if (settings_.trigger.kind != input_kind::mouse) return;
        remove_device(device, send);
    }
    template<class Sink> void remove_keyboard(std::uintptr_t device, Sink&& send) {
        if (settings_.trigger.kind != input_kind::keyboard) return;
        remove_device(device, send);
    }
    template<class Sink> void remove_device(std::uintptr_t device, Sink&& send) {
        for (auto& entry : devices_) if (entry.used && entry.device == device) entry = {};
        if (open_ && owner_ == device) cancel(send);
    }
    // Raw Input supplies real device identities in user mode; Interception does
    // the same in kernel mode. Repeated DOWN never opens or confirms twice.
    template<class Sink, class ReleaseXY>
    bool keyboard(std::uintptr_t device, key_code code, bool down, bool allowed,
        time_point now, int output_keyboard, Sink&& send, ReleaseXY&& release_xy) {
        if (!trigger_key(code)) return false;
        auto* entry = device_entry(device);
        if (!entry) return false;
        if (down) {
            if (!entry->down) {
                entry->down = true;
                entry->consumed = allowed && !initially_held_;
                if (entry->consumed && !open_) begin(device, now, send, release_xy);
            }
            publish();
            return entry->consumed;
        }
        const bool consumed = entry->consumed;
        if (open_ && owner_ == device) finish(allowed, now, output_keyboard, send, release_xy);
        *entry = {};
        initially_held_ = false;
        publish();
        return consumed;
    }
    template<class Sink, class ReleaseXY>
    pie_packet_result packet(std::uintptr_t device, unsigned short flags, std::int32_t x, std::int32_t y,
        bool absolute, bool allowed, time_point now, int keyboard, Sink&& send, ReleaseXY&& release_xy) {
        if (!configured()) return {flags, false};
        if (settings_.trigger.kind == input_kind::keyboard) {
            // A keyboard-triggered menu accepts relative movement from any mouse.
            if (open_ && !absolute) mapping_.motion(x, y);
            publish();
            return {flags, open_};
        }
        bool owns_motion = open_;
        const auto down_bit = static_cast<unsigned short>(1u << (settings_.trigger.code * 2));
        const auto up_bit = static_cast<unsigned short>(down_bit << 1);
        device_state* entry = nullptr;
        if (flags & (down_bit | up_bit)) {
            entry = device_entry(device);
        }
        if (entry && (flags & down_bit)) {
            if (!entry->down) {
                entry->down = true;
                entry->consumed = allowed && !initially_held_;
                if (entry->consumed && !open_) {
                    begin(device, now, send, release_xy);
                    owns_motion = owns_motion || open_;
                }
            }
            if (entry->consumed) flags = static_cast<unsigned short>(flags & ~down_bit);
        }
        if (open_ && owner_ == device && !absolute) mapping_.motion(x, y);
        if (entry && (flags & up_bit)) {
            if (entry->consumed) flags = static_cast<unsigned short>(flags & ~up_bit);
            if (open_ && owner_ == device) {
                finish(allowed, now, keyboard, send, release_xy);
            }
            *entry = {};
            initially_held_ = false;
        }
        publish();
        return {flags, owns_motion};
    }
private:
    int trigger_vk() const noexcept {
        constexpr std::array buttons{VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2};
        return settings_.trigger.kind == input_kind::mouse ? buttons[settings_.trigger.code]
            : static_cast<int>(MapVirtualKeyW(settings_.trigger.code, MAPVK_VSC_TO_VK_EX));
    }
    struct device_state { std::uintptr_t device{}; bool used = false, down = false, consumed = false; };
    device_state* device_entry(std::uintptr_t device) noexcept {
        for (auto& item : devices_) if (item.used && item.device == device) return &item;
        for (auto& item : devices_) if (!item.used) {
            item = {device, true, false, false}; return &item;
        }
        return nullptr;
    }
    template<class Sink, class ReleaseXY>
    void begin(std::uintptr_t device, time_point now, Sink&& send, ReleaseXY&& release_xy) {
        release_xy();
        foreground_ = GetForegroundWindow();
        if (mapping_.deadline() && output_foreground_ != foreground_) mapping_.cancel_output(send);
        mapping_.begin();
        owner_ = device; open_ = foreground_ != nullptr;
        focus_due_ = now + milliseconds(4);
    }
    template<class Sink, class ReleaseXY>
    void finish(bool allowed, time_point now, int keyboard, Sink&& send, ReleaseXY&& release_xy) {
        // Check focus at the actual commit, not only at maintenance frequency.
        if (allowed && foreground_ == GetForegroundWindow()) {
            if (mapping_.deadline() && output_foreground_ != foreground_) mapping_.cancel_output(send);
            output_foreground_ = foreground_;
            mapping_.confirm(now, keyboard, send);
        } else mapping_.cancel(send);
        open_ = false; owner_ = 0; foreground_ = nullptr;
        release_xy();
    }
    void publish() noexcept {
        const auto state = open_ ? 1u + static_cast<unsigned>(mapping_.selected()) : 0u;
        if (state == published_ && foreground_ == published_foreground_) return;
        published_ = state; published_foreground_ = foreground_;
        overlay_.publish(open_, mapping_.selected(), foreground_);
    }
    pie_settings settings_;
    pie_mapping mapping_;
    pie_overlay overlay_;
    // No hot-path device allocation; excess simultaneous button holders pass through.
    std::array<device_state, 64> devices_{};
    bool open_ = false, initially_held_ = false;
    std::uintptr_t owner_ = 0;
    HWND foreground_{}, output_foreground_{}, published_foreground_{};
    unsigned published_ = 0;
    time_point focus_due_{};
};
}
