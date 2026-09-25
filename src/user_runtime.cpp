#include "runtime.hpp"
#include "xy_mapping.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <atomic>
#include <exception>
#include <iostream>
#include <memory>
#include <system_error>

namespace mouse_mapping {
namespace {
std::atomic<bool> stopping = false;
std::atomic<HANDLE> shutdown_done = nullptr;

[[noreturn]] void fail(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}

void send_key(key_event event) {
    INPUT input{};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = event.code & 0xff;
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (event.down ? 0 : KEYEVENTF_KEYUP)
        | ((event.code & 0xff00) == 0xe000 ? KEYEVENTF_EXTENDEDKEY : 0);
    if (SendInput(1, &input, sizeof(input)) != 1)
        fail("SendInput failed (check target integrity level / game input restrictions)");
}

BOOL WINAPI console_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT
        && event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT) return FALSE;
    stopping = true;
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
    explicit session(const configuration& value) : config(value) {}
    ~session() {
        try { if (mapping) mapping->release(send_key); }
        catch (const std::exception& error) { std::cerr << "Key cleanup failed: " << error.what() << '\n'; }
        if (hook) UnhookWindowsHookEx(hook);
        if (window) {
            RAWINPUTDEVICE device{1, 2, RIDEV_REMOVE, nullptr};
            RegisterRawInputDevices(&device, 1, sizeof(device));
            DestroyWindow(window);
        }
        if (class_registered) UnregisterClassW(L"mouse_mapping_user_input", GetModuleHandleW(nullptr));
        if (done) SetEvent(done);
        if (handler_registered) SetConsoleCtrlHandler(console_handler, FALSE);
        shutdown_done = nullptr;
        // Keep the event valid until process teardown: a console callback may be waiting on it.
        if (console_changed) SetConsoleMode(console, console_mode);
        if (timer_started) timeEndPeriod(1);
        if (mutex) CloseHandle(mutex);
        active = nullptr;
    }
    void start() {
        // Same mutex as the kernel target: do not map the same input twice.
        mutex = CreateMutexW(nullptr, FALSE, L"Local\\mouse_input_mapping_v1");
        if (!mutex) fail("Cannot create instance mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) throw std::runtime_error("Another mapping target is already running");
        active = this;
        stopping = false;
        done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done) fail("Cannot create shutdown event");
        shutdown_done = done;
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
        RAWINPUTDEVICE device{1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) fail("Cannot register mouse Raw Input");
        hook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_proc, wc.hInstance, 0);
        if (!hook) fail("Cannot register keyboard hook");
        std::cout << "User mode: disable mouse input in the game; original mouse input is not blocked.\n"
                     "OFF - press toggle to enable. Ctrl+C exits.\n" << std::flush;
    }
    void toggle() {
        if (mapping) {
            mapping->release(send_key);
            mapping.reset();
        } else {
            const std::array keys{config.left_key, config.right_key, config.up_key, config.down_key,
                config.mouse_keys[0], config.mouse_keys[1], config.mouse_keys[2], config.mouse_keys[3], config.mouse_keys[4],
                config.wheel_keys[0], config.wheel_keys[1]};
            for (const auto key : keys)
                if (GetAsyncKeyState(static_cast<int>(MapVirtualKeyW(key, MAPVK_VSC_TO_VK_EX))) & 0x8000) {
                    std::cout << "OFF - release all mapped keyboard keys before enabling.\n" << std::flush;
                    return;
                }
            for (const auto button : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2})
                if (GetAsyncKeyState(button) & 0x8000) {
                    std::cout << "OFF - release mouse buttons before enabling.\n" << std::flush;
                    return;
                }
            mapping = std::make_unique<xy_mapping>(config);
        }
        PlaySoundW(mapping ? L"DeviceConnect" : L"DeviceDisconnect", nullptr,
            SND_ALIAS | SND_ASYNC | SND_NODEFAULT | SND_SYSTEM);
        std::cout << (mapping ? "ON\n" : "OFF\n") << std::flush;
    }
    configuration config;
    std::unique_ptr<xy_mapping> mapping;
    toggle_latch latch;
    std::exception_ptr failure;
    bool absolute_warning = false;
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
                if (key == active->config.toggle_key) {
                    if (active->latch.update(1, down)) active->toggle();
                    return 1;
                }
                if (active->mapping && active->mapping->physical({1, key, down}, send_key)) return 1;
            } catch (...) {
                active->failure = std::current_exception();
                stopping = true;
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_REMOVAL && active && active->mapping) {
        try { active->mapping->remove_mouse(static_cast<std::uintptr_t>(lparam), send_key); }
        catch (...) { active->failure = std::current_exception(); stopping = true; }
    }
    if (message == WM_INPUT && active && !stopping.load()) {
        try {
            RAWINPUT input{};
            UINT bytes = sizeof(input);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1))
                fail("Cannot read mouse Raw Input");
            if (input.header.dwType == RIM_TYPEMOUSE && active->mapping) {
                const auto& mouse = input.data.mouse;
                active->mapping->buttons(reinterpret_cast<std::uintptr_t>(input.header.hDevice), mouse.usButtonFlags, send_key);
                if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
                    active->mapping->wheel(reinterpret_cast<std::uintptr_t>(input.header.hDevice),
                        static_cast<std::int16_t>(mouse.usButtonData), clock_type::now(), send_key);
                if (!(mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                    active->mapping->update(mouse.lLastX, mouse.lLastY, clock_type::now(), send_key);
                else if (!active->absolute_warning) {
                    active->absolute_warning = true;
                    std::cout << "Absolute coordinates ignored; mouse buttons still map.\n";
                }
            }
        } catch (...) {
            active->failure = std::current_exception();
            stopping = true;
        }
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
} // namespace

void run(const configuration& config, const game_command* game) {
    validate(config);
    session state(config);
    state.start();
    std::unique_ptr<game_process> child;
    if (game) child = std::make_unique<game_process>(*game);
    while (!stopping.load()) {
        DWORD wait_ms = 4;
        if (state.mapping) if (const auto due = state.mapping->deadline()) {
            const auto remaining = std::chrono::duration_cast<milliseconds>(*due - clock_type::now()).count();
            wait_ms = static_cast<DWORD>(std::clamp<std::int64_t>(remaining, 1, 4));
        }
        const HANDLE game_handle = child ? child->handle() : nullptr;
        const DWORD wait_result = MsgWaitForMultipleObjectsEx(child ? 1 : 0, child ? &game_handle : nullptr,
            wait_ms, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        if (wait_result == WAIT_FAILED)
            fail("Cannot wait for input");
        if (child && wait_result == WAIT_OBJECT_0) {
            std::cout << "Daemon: game process exited; stopping mapping.\n" << std::flush;
            break;
        }
        MSG message{};
        for (int count = 0; count < 256 && !stopping.load() && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
            if (message.message == WM_QUIT) { stopping = true; break; }
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (state.mapping && !stopping.load()) state.mapping->tick(clock_type::now(), send_key);
    }
    if (child && !child->exited())
        std::cout << "Daemon: mapping stopped; game process continues.\n" << std::flush;
    if (state.failure) std::rethrow_exception(state.failure);
    if (state.mapping) state.mapping->release(send_key);
    std::cout << "OFF - mapping keys released.\n";
}
} // namespace mouse_mapping
