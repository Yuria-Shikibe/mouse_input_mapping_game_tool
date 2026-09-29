#include "input_preview.hpp"
#include "xy_mapping.hpp"
#include "bypass.hpp"
#include <mmsystem.h>
#include <stdexcept>
#include <system_error>

namespace mouse_mapping {
namespace {
constexpr wchar_t preview_class[] = L"MouseMappingLocalPreviewInput";
void discard_key(key_event) noexcept {}
[[noreturn]] void preview_error(const char* operation) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), operation);
}
struct input_window {
    HWND window = nullptr;
    bool registered = false, timer = false;
    ~input_window() {
        if (registered) {
            RAWINPUTDEVICE devices[]{{1, 2, RIDEV_REMOVE, nullptr}, {1, 6, RIDEV_REMOVE, nullptr}};
            RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
        }
        if (window) DestroyWindow(window);
        if (timer) timeEndPeriod(1);
    }
};
}

input_preview::~input_preview() { stop(); }
void input_preview::start(const configuration& config, HWND owner) {
    validate(config);
    stop();
    {
        const std::lock_guard lock(mutex_);
        events_.clear(); error_.clear();
    }
    state_ = 0; sequence_ = 0; recording_failed_ = false;
    running_ = true;
    try { worker_ = std::jthread([this, config, owner](std::stop_token stop) { run(stop, config, owner); }); }
    catch (...) { running_ = false; throw; }
}
void input_preview::stop() noexcept {
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
}
void input_preview::poll(std::vector<monitor_event>& events, std::string& error) {
    events.clear();
    const std::lock_guard lock(mutex_);
    events.assign(events_.begin(), events_.end());
    events_.clear();
    error = error_;
    if (recording_failed_) error = "Cannot record local preview input";
}
void input_preview::record(monitor_event_kind kind, std::int32_t x, std::int32_t y) noexcept {
    try {
        const monitor_event event{++sequence_, monitor_now(), x, y, kind, state_};
        const std::lock_guard lock(mutex_);
        if (events_.size() >= monitor_capacity) events_.pop_front();
        events_.push_back(event);
    } catch (...) { recording_failed_ = true; }
}
direction_observer input_preview::observer(bool y) noexcept {
    return {y ? &y_context_ : &x_context_, [](void* pointer, direction value) noexcept {
        auto& context = *static_cast<observer_context*>(pointer);
        auto& self = *context.owner;
        const auto shift = context.y ? 2u : 0u;
        const auto bits = value == direction::left ? 1u : value == direction::right ? 2u : 0u;
        self.state_ = (self.state_ & ~(3u << shift)) | (bits << shift);
        self.record(monitor_event_kind::intent);
    }};
}
void input_preview::run(std::stop_token stop, configuration config, HWND owner) noexcept {
    try {
        input_window input;
        WNDCLASSW type{};
        type.lpfnWndProc = DefWindowProcW;
        type.hInstance = GetModuleHandleW(nullptr);
        type.lpszClassName = preview_class;
        if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) preview_error("Register preview window");
        input.window = CreateWindowExW(0, preview_class, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, type.hInstance, nullptr);
        if (!input.window) preview_error("Create preview input window");
        RAWINPUTDEVICE devices[]{{1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, input.window},
            {1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, input.window}};
        if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) preview_error("Register preview input");
        input.registered = true;
        input.timer = timeBeginPeriod(1) == TIMERR_NOERROR;
        xy_mapping mapping(config);
        mapping.observe(observer(false), observer(true));
        bypass_state bypass(config);
        bypass.seed();
        bool bypassed = false;
        const auto sync_bypass = [&](bool packet = false) {
            const bool next = bypass.held() || packet;
            if (next && !bypassed) mapping.release(discard_key);
            if (next != bypassed) {
                state_ = (state_ & ~64u) | (next ? 64u : 0u);
                record(monitor_event_kind::snapshot);
            }
            bypassed = next;
        };
        sync_bypass();
        running_ = true;
        bool focused = false;
        double last_snapshot = 0;
        while (!stop.stop_requested() && !recording_failed_) {
            bypass.refresh_initial();
            sync_bypass();
            const bool foreground = GetForegroundWindow() == owner && !IsIconic(owner);
            if (foreground != focused) {
                mapping.release(discard_key);
                focused = foreground;
                state_ = (state_ & ~16u) | (focused ? 16u : 0u);
                record(monitor_event_kind::snapshot);
            }
            DWORD wait_ms = 4;
            if (focused && !bypassed) if (const auto due = mapping.deadline()) {
                const auto remaining = std::chrono::duration_cast<milliseconds>(*due - clock_type::now()).count();
                wait_ms = static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 1, 4));
            }
            if (MsgWaitForMultipleObjectsEx(0, nullptr, wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE) == WAIT_FAILED)
                preview_error("Wait for preview input");
            MSG message{};
            for (int count = 0; count < 256 && !stop.stop_requested() && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                if (message.message == WM_INPUT) {
                    RAWINPUT raw{};
                    UINT bytes = sizeof(raw);
                    if (GetRawInputData(reinterpret_cast<HRAWINPUT>(message.lParam), RID_INPUT, &raw, &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1))
                        preview_error("Read preview mouse");
                    if (raw.header.dwType == RIM_TYPEKEYBOARD) {
                        const auto& key = raw.data.keyboard;
                        if (!(key.Flags & RI_KEY_E1)) {
                            bypass.keyboard(reinterpret_cast<std::uintptr_t>(raw.header.hDevice),
                                static_cast<key_code>(key.MakeCode | ((key.Flags & RI_KEY_E0) ? 0xe000 : 0)),
                                !(key.Flags & RI_KEY_BREAK));
                            sync_bypass();
                        }
                    }
                    if (raw.header.dwType == RIM_TYPEMOUSE) {
                        const auto& mouse = raw.data.mouse;
                        const bool was_bypassed = bypass.held();
                        const bool down = bypass.mouse(reinterpret_cast<std::uintptr_t>(raw.header.hDevice), mouse.usButtonFlags);
                        const bool packet = was_bypassed || down || bypass.held();
                        sync_bypass(packet);
                        if (focused && GetForegroundWindow() == owner) {
                        if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) {
                            if (!(state_ & 32)) { state_ |= 32; record(monitor_event_kind::absolute); }
                        } else {
                            if (mouse.lLastX || mouse.lLastY) record(monitor_event_kind::motion, mouse.lLastX, mouse.lLastY);
                            if (!packet) mapping.update(mouse.lLastX, mouse.lLastY, clock_type::now(), discard_key);
                        }
                        }
                        sync_bypass();
                    }
                } else if (message.message == WM_INPUT_DEVICE_CHANGE && message.wParam == GIDC_REMOVAL) {
                    mapping.remove_mouse(static_cast<std::uintptr_t>(message.lParam), discard_key);
                    bypass.remove_mouse(static_cast<std::uintptr_t>(message.lParam));
                    bypass.remove_keyboard(static_cast<std::uintptr_t>(message.lParam));
                    sync_bypass();
                }
                DispatchMessageW(&message); // Includes required WM_INPUT cleanup.
                if (focused && !bypassed) mapping.tick(clock_type::now(), discard_key);
            }
            if (focused && !bypassed) mapping.tick(clock_type::now(), discard_key);
            const auto now = monitor_now();
            if (now - last_snapshot >= 0.033) { record(monitor_event_kind::snapshot); last_snapshot = now; }
        }
        mapping.release(discard_key);
    } catch (const std::exception& error) {
        try { const std::lock_guard lock(mutex_); error_ = error.what(); } catch (...) {}
    } catch (...) {
        recording_failed_ = true;
    }
    state_ = 0;
    record(monitor_event_kind::stopped);
    running_ = false;
}
}
