#include "runtime.hpp"
#include "chord_mapping.hpp"
#include "xy_mapping.hpp"
#include "input_monitor.hpp"
#include "input_schedule.hpp"
#include "bypass.hpp"
#include "pie_runtime.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <system_error>

namespace mouse_mapping {
namespace {
std::atomic stopping = false;
std::atomic<HANDLE> shutdown_done = nullptr;
std::atomic<HANDLE> stop_event = nullptr;

void request_stop() noexcept {
    stopping = true;
    if (const auto event_handle = stop_event.load()) SetEvent(event_handle);
}

[[noreturn]] void fail(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}

void send_raw_key(key_event event) {
    if (!key_bound(event.code)) return;
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = event.code & 0xff;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (event.down ? 0 : KEYEVENTF_KEYUP)
        | ((event.code & 0xff00) == 0xe000 ? KEYEVENTF_EXTENDEDKEY : 0);
    if (SendInput(1, &input, sizeof(input)) != 1)
        fail("SendInput failed (check target integrity level / game input restrictions)");
}

bool key_is_down(key_code code) {
    return key_bound(code)
        && (GetAsyncKeyState(static_cast<int>(MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX))) & 0x8000) != 0;
}

chord_mapping* output_chord = nullptr;
void send_key(key_event event) {
    if (output_chord) output_chord->output(event, send_raw_key);
    else send_raw_key(event);
}

BOOL WINAPI console_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT
        && event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT) return FALSE;
    request_stop();
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT)
        if (const auto done = shutdown_done.load()) WaitForSingleObject(done, 2000);
    return TRUE;
}

class session;
session* active = nullptr;
LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam);
LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);

class session {
public:
    explicit session(const configuration& value) : config(value), bypass(value), pie(value), chord(value.chord) {}
    ~session() {
        try { chord.cancel(send_raw_key); }
        catch (const std::exception& error) { std::cerr << "Chord cleanup failed: " << error.what() << '\n'; }
        try { pie.cancel(send_key); }
        catch (const std::exception& error) { std::cerr << "Pie key cleanup failed: " << error.what() << '\n'; }
        try { if (mapping) mapping->release(send_key); }
        catch (const std::exception& error) { std::cerr << "Key cleanup failed: " << error.what() << '\n'; }
        monitor.stop();
        if (hook) UnhookWindowsHookEx(hook);
        if (window) {
            RAWINPUTDEVICE devices[]{{1, 2, RIDEV_REMOVE, nullptr}, {1, 6, RIDEV_REMOVE, nullptr}};
            RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE));
            DestroyWindow(window);
        }
        if (class_registered) UnregisterClassW(L"mouse_mapping_user_input", GetModuleHandleW(nullptr));
        if (done) SetEvent(done);
        if (handler_registered) SetConsoleCtrlHandler(console_handler, FALSE);
        shutdown_done = nullptr;
        stop_event = nullptr;
        // Keep the event valid until process teardown: a console callback may be waiting on it.
        if (console_changed) SetConsoleMode(console, console_mode);
        if (timer_started) timeEndPeriod(1);
        if (mutex) CloseHandle(mutex);
        output_chord = nullptr;
        active = nullptr;
    }
    void start() {
        // Same mutex as the kernel target: do not map the same input twice.
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\mouse_input_mapping_v1");
        if (!mutex) fail("Cannot create instance mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("Another mapping target is already running");
        monitor.start(config, true);
        active = this;
        output_chord = &chord;
        if (config.chord.enabled) for (const auto key : {config.chord.first, config.chord.second})
            if (key_is_down(key)) chord.seed(key);
        stopping = false;
        done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done) fail("Cannot create shutdown event");
        shutdown_done = done;
        wake = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!wake) fail("Cannot create input stop event");
        stop_event = wake;
        if (!SetConsoleCtrlHandler(console_handler, TRUE)) fail("Cannot register console handler");
        handler_registered = true;
        console = GetStdHandle(STD_INPUT_HANDLE);
        if (GetConsoleMode(console, &console_mode)) {
            if (!SetConsoleMode(console, (console_mode | ENABLE_EXTENDED_FLAGS | ENABLE_PROCESSED_INPUT) & ~ENABLE_QUICK_EDIT_MODE))
                fail("Cannot set console mode");
            console_changed = true;
        }
        timer_started = timeBeginPeriod(1) == TIMERR_NOERROR;
        WNDCLASSW wc{};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = L"mouse_mapping_user_input";
        if (!RegisterClassW(&wc)) fail("Cannot register Raw Input window class");
        class_registered = true;
        window = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
        if (!window) fail("Cannot create Raw Input window");
        RAWINPUTDEVICE devices[]{{1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window},
            {1, 6, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window}};
        if (!RegisterRawInputDevices(devices, 2, sizeof(RAWINPUTDEVICE))) fail("Cannot register Raw Input");
        hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, wc.hInstance, 0);
        if (!hook) fail("Cannot register keyboard hook");
        pie.start();
        bypass.seed();
        sync_bypass();
        std::cout << "User mode: disable mouse input in the game; original mouse input is not blocked.\n"
                     "OFF - press toggle to enable. Ctrl+C exits.\n" << std::flush;
    }
    void toggle() {
        if (mapping) {
            chord.cancel(send_raw_key);
            pie.cancel(send_key);
            mapping->release(send_key);
            mapping.reset();
        } else {
            const std::array keys{config.left_key, config.right_key, config.up_key, config.down_key,
                config.mouse_keys[0], config.mouse_keys[1], config.mouse_keys[2], config.mouse_keys[3], config.mouse_keys[4],
                config.wheel_keys[0], config.wheel_keys[1]};
            for (const auto key : keys)
                if (!bypass.held() && key_is_down(key)) {
                    std::cout << "OFF - release all mapped keyboard keys before enabling.\n" << std::flush;
                    return;
                }
            for (const auto button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
                if (!bypass.held() && (GetAsyncKeyState(button) & 0x8000)) {
                    std::cout << "OFF - release mouse buttons before enabling.\n" << std::flush;
                    return;
                }
            mapping = std::make_unique<xy_mapping>(config);
            mapping->observe(monitor.observer(false), monitor.observer(true));
            // Seed physical owners when enabling while a bypass key is held.
            if (bypass.held()) for (const auto key : keys)
                if (key_is_down(key))
                    mapping->physical({1, key, true}, [](key_event) {});
        }
        monitor.enabled(mapping != nullptr);
        PlaySoundW(mapping ? L"DeviceConnect" : L"DeviceDisconnect", nullptr,
            SND_ALIAS | SND_ASYNC | SND_NODEFAULT | SND_SYSTEM);
        std::cout << (mapping ? "ON\n" : "OFF\n") << std::flush;
    }
    configuration config;
    bypass_state bypass;
    pie_runtime pie;
    chord_mapping chord;
    bool bypassed = false;
    void sync_bypass(bool packet = false) {
        const bool next = bypass.held() || packet;
        if (next && !bypassed) {
            std::exception_ptr error;
            try { chord.cancel(send_raw_key); } catch (...) { error = std::current_exception(); }
            try { pie.cancel(send_key); } catch (...) { error = std::current_exception(); }
            try { if (mapping) mapping->release(send_key); } catch (...) { if (!error) error = std::current_exception(); }
            if (error) std::rethrow_exception(error);
        }
        if (next != bypassed) monitor.bypassed(next);
        bypassed = next;
    }
    bool mapping_active() const { return mapping && !bypassed; }
    monitor_publisher monitor;
    std::unique_ptr<xy_mapping> mapping;
    toggle_latch latch;
    std::exception_ptr failure;
    bool absolute_warning = false;
    HANDLE wake{}; // Kept valid through process teardown for console callbacks.
private:
    HANDLE mutex{}, done{}, console{};
    DWORD console_mode{};
    HWND window{};
    HHOOK hook{};
    bool handler_registered = false, console_changed = false, timer_started = false, class_registered = false;
};

LRESULT CALLBACK keyboard_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION && active && !stopping.load()) {
        const auto& event = *reinterpret_cast<const KBDLLHOOKSTRUCT*>(lparam);
        // Never feed our own SendInput output (or other injected events) back into ownership.
        if (!(event.flags & LLKHF_INJECTED) && event.vkCode != VK_PAUSE && event.vkCode != VK_SNAPSHOT) {
            const auto key = static_cast<key_code>(event.scanCode | ((event.flags & LLKHF_EXTENDED) ? 0xe000 : 0));
            const bool down = !(event.flags & LLKHF_UP);
            try {
                // The hook responds immediately. Raw Input subsequently supplies
                // the physical device identity and keeps overlapping keyboards held.
                active->bypass.keyboard(0, key, down);
                active->sync_bypass();
                if (active->chord.trigger_key(key)) {
                    active->chord.keyboard(1, key, down, active->mapping_active(), 1, send_raw_key);
                    return CallNextHookEx(nullptr, code, wparam, lparam);
                }
                if (key_bound(active->config.toggle_key) && key == active->config.toggle_key) {
                    if (active->latch.update(1, down)) active->toggle();
                    return 1;
                }
                if (!active->mapping || active->bypassed) {
                    active->pie.physical({1, key, down}, [](key_event) {});
                } else if (active->pie.physical({1, key, down}, send_key)) return 1;
                if (active->bypass.keyboard_binding(key)) {
                    // Restore keys themselves stay genuine keyboard events. Adopt
                    // physical ownership without sending a second copy via SendInput.
                    if (active->mapping) active->mapping->physical({1, key, down}, [](key_event) {});
                } else if (active->mapping && active->mapping->physical({1, key, down}, send_key)) return 1;
                else if (active->chord.output_key(key)) { send_key({1, key, down}); return 1; }
            } catch (...) {
                active->failure = std::current_exception();
                request_stop();
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_REMOVAL && active) {
        try {
            active->bypass.remove_mouse(static_cast<std::uintptr_t>(lparam));
            active->bypass.remove_keyboard(static_cast<std::uintptr_t>(lparam));
            active->pie.remove_mouse(static_cast<std::uintptr_t>(lparam), send_key);
            active->pie.remove_keyboard(static_cast<std::uintptr_t>(lparam), send_key);
            active->chord.remove_device(static_cast<std::uintptr_t>(lparam), send_raw_key);
            if (active->mapping) active->mapping->remove_mouse(static_cast<std::uintptr_t>(lparam), send_key);
            active->sync_bypass();
        }
        catch (...) { active->failure = std::current_exception(); request_stop(); }
    }
    if (message == WM_INPUT && active && !stopping.load()) {
        try {
            RAWINPUT input{};
            UINT bytes = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1))
                fail("Cannot read Raw Input");
            if (input.header.dwType == RIM_TYPEKEYBOARD && input.header.hDevice) {
                const auto& key = input.data.keyboard;
                if (!(key.Flags & RI_KEY_E1)) {
                    const auto code = static_cast<key_code>(key.MakeCode | ((key.Flags & RI_KEY_E0) ? 0xe000 : 0));
                    active->bypass.keyboard(reinterpret_cast<std::uintptr_t>(input.header.hDevice), code, !(key.Flags & RI_KEY_BREAK));
                    active->bypass.keyboard(0, code, false);
                    active->sync_bypass();
                    // User mode observes the physical trigger without consuming it.
                    active->pie.keyboard(reinterpret_cast<std::uintptr_t>(input.header.hDevice),
                        code, !(key.Flags & RI_KEY_BREAK), active->mapping_active(), clock_type::now(), 1,
                        send_key, [&] { if (active->mapping) active->mapping->release_motion(send_key); });
                }
            }
            if (input.header.dwType == RIM_TYPEMOUSE) {
                const auto& mouse = input.data.mouse;
                const bool was_bypassed = active->bypass.held();
                const bool bypass_down = active->bypass.mouse(reinterpret_cast<std::uintptr_t>(input.header.hDevice), mouse.usButtonFlags);
                const bool bypass_packet = was_bypassed || bypass_down || active->bypass.held();
                active->sync_bypass(bypass_packet);
                if (active->monitor.recording())
                    active->monitor.motion(mouse.lLastX, mouse.lLastY, (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0);
                const auto now = clock_type::now();
                active->chord.mouse(reinterpret_cast<std::uintptr_t>(input.header.hDevice), mouse.usButtonFlags,
                    active->mapping != nullptr && !bypass_packet, 1, send_raw_key);
                const auto pie_packet = active->pie.packet(reinterpret_cast<std::uintptr_t>(input.header.hDevice),
                    mouse.usButtonFlags, mouse.lLastX, mouse.lLastY, (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) != 0,
                    active->mapping != nullptr && !bypass_packet, now, 1, send_key,
                    [&] { if (active->mapping) active->mapping->release_motion(send_key); });
                if (!active->mapping || bypass_packet) {
                    active->sync_bypass();
                    return DefWindowProcW(window, message, wparam, lparam);
                }
                active->mapping->buttons(reinterpret_cast<std::uintptr_t>(input.header.hDevice), pie_packet.buttons, send_key);
                if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
                    active->mapping->wheel(reinterpret_cast<std::uintptr_t>(input.header.hDevice),
                        static_cast<std::int16_t>(mouse.usButtonData), now, send_key);
                if (!pie_packet.owns_motion && !(mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                    active->mapping->update(mouse.lLastX, mouse.lLastY, now, send_key);
                else if (!pie_packet.owns_motion && !active->absolute_warning) {
                    active->absolute_warning = true;
                    std::cout << "Absolute coordinates ignored; mouse buttons still map.\n";
                }
            }
        } catch (...) {
            active->failure = std::current_exception();
            request_stop();
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

void run(const configuration& config, const game_command* game, input_priority priority) {
    validate(config);
    session state(config);
    state.start();
    input_thread_priority thread_priority(priority);
    input_waiter waiter;
    std::unique_ptr<game_process> child;
    if (game) child = std::make_unique<game_process>(*game);
    auto maintenance = clock_type::now();
    while (!stopping.load()) {
        const auto now = clock_type::now();
        if (now >= maintenance) {
            state.monitor.heartbeat(now);
            state.bypass.refresh_initial();
            if (state.chord.held() && !(GetAsyncKeyState(binding_vk(state.config.chord.trigger)) & 0x8000))
                state.chord.reset(send_raw_key);
            state.sync_bypass();
            state.pie.maintenance(send_key);
            if (state.mapping) state.mapping->observe(state.monitor.observer(false), state.monitor.observer(true));
            maintenance = now + milliseconds(50);
        }
        state.pie.tick(now, send_key);
        if (state.mapping_active()) state.mapping->tick(now, send_key, state.pie.opened());
        auto wake_at = maintenance;
        if (const auto due = state.pie.deadline()) wake_at = std::min(wake_at, *due);
        if (state.mapping_active()) if (const auto due = state.mapping->deadline(state.pie.opened())) wake_at = std::min(wake_at, *due);
        const HANDLE game_handle = child ? child->handle() : nullptr;
        if (waiter.wait(state.wake, game_handle, wake_at)) {
            std::cout << "Daemon: game process exited; stopping mapping.\n" << std::flush;
            break;
        }
        MSG message{};
        const auto batch_end = clock_type::now() + std::chrono::microseconds(500);
        for (int count = 0; count < 32 && !stopping.load() && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) { request_stop(); break; }
            DispatchMessageW(&message);
            const auto processed = clock_type::now();
            if (!stopping.load()) {
                state.pie.tick(processed, send_key);
                if (state.mapping_active()) state.mapping->tick(processed, send_key, state.pie.opened());
            }
            if (processed >= batch_end) break;
        }
    }
    if (child && !child->exited())
        std::cout << "Daemon: mapping stopped; game process continues.\n" << std::flush;
    if (state.failure) std::rethrow_exception(state.failure);
    state.pie.cancel(send_key);
    if (state.mapping) state.mapping->release(send_key);
    std::cout << "OFF - mapping keys released.\n";
}
} // namespace mouse_mapping
