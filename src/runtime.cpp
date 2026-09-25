#include "runtime.hpp"
#include "deployment.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <interception.h>

#include <array>
#include <atomic>
#include <bit>
#include <cstring>
#include <iostream>
#include <memory>
#include <system_error>

namespace mouse_mapping {
namespace {

[[noreturn]] void fail_windows(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}

class driver_api {
public:
    driver_api() {
        auto path = executable_directory() / L"interception.dll";
        if (!std::filesystem::exists(path)) {
            extracted_ = std::make_unique<embedded_file>(embedded_asset::library);
            path = extracted_->path();
        }
        module_ = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_) fail_windows("Cannot load Interception library");
        try {
            create_context = bind<decltype(create_context)>("interception_create_context");
            destroy_context = bind<decltype(destroy_context)>("interception_destroy_context");
            set_filter = bind<decltype(set_filter)>("interception_set_filter");
            wait_with_timeout = bind<decltype(wait_with_timeout)>("interception_wait_with_timeout");
            receive = bind<decltype(receive)>("interception_receive");
            send = bind<decltype(send)>("interception_send");
            get_hardware_id = bind<decltype(get_hardware_id)>("interception_get_hardware_id");
        } catch (...) { FreeLibrary(module_); throw; }
    }
    ~driver_api() { FreeLibrary(module_); }
    driver_api(const driver_api&) = delete;
    driver_api& operator=(const driver_api&) = delete;

    decltype(&interception_create_context) create_context{};
    decltype(&interception_destroy_context) destroy_context{};
    decltype(&interception_set_filter) set_filter{};
    decltype(&interception_wait_with_timeout) wait_with_timeout{};
    decltype(&interception_receive) receive{};
    decltype(&interception_send) send{};
    decltype(&interception_get_hardware_id) get_hardware_id{};
private:
    template<class function_type>
    function_type bind(const char* name) {
        const auto address = GetProcAddress(module_, name);
        if (!address) fail_windows(name);
        return std::bit_cast<function_type>(address);
    }
    HMODULE module_{};
    std::unique_ptr<embedded_file> extracted_;
};

int is_keyboard(int device) { return device >= 1 && device <= 10; }
int is_mouse(int device) { return device >= 11 && device <= 20; }

class driver_session {
public:
    driver_session() : context(api.create_context()) {
        if (!context) throw std::runtime_error(
            "Cannot open Interception driver. Start this EXE normally for automatic setup. "
            "If already installed, restart Windows; if still unavailable, Windows may have blocked the driver.");
    }
    ~driver_session() {
        api.set_filter(context, is_mouse, INTERCEPTION_FILTER_MOUSE_NONE);
        api.set_filter(context, is_keyboard, INTERCEPTION_FILTER_KEY_NONE);
        api.destroy_context(context);
    }
    driver_session(const driver_session&) = delete;
    driver_session& operator=(const driver_session&) = delete;

    void enable_filters() {
        api.set_filter(context, is_keyboard, INTERCEPTION_FILTER_KEY_ALL);
        api.set_filter(context, is_mouse, INTERCEPTION_FILTER_MOUSE_ALL);
        // Do not validate with interception_get_filter: the installed driver
        // can complete IOCTL_GET_FILTER successfully with zero output bytes.
        // The library then returns its initial value (0), even after setting
        // a filter. Follow the upstream samples and proceed to receive input.
    }

    template<class stroke_type>
    void forward(int device, const stroke_type& stroke) {
        if (api.send(context, device, reinterpret_cast<const InterceptionStroke*>(&stroke), 1) != 1)
            throw std::runtime_error("Driver input send failed (device " + std::to_string(device) + ")");
    }

    void send_key(key_event event) {
        InterceptionKeyStroke stroke{};
        stroke.code = event.code & 0xff;
        stroke.state = static_cast<unsigned short>((event.down ? INTERCEPTION_KEY_DOWN : INTERCEPTION_KEY_UP)
            | ((event.code & 0xff00) == 0xe000 ? INTERCEPTION_KEY_E0 : 0));
        forward(event.device, stroke);
    }

    driver_api api;
    InterceptionContext context{};
};

std::atomic<bool> stop_requested = false;
std::atomic<HANDLE> shutdown_complete = nullptr;

BOOL WINAPI console_handler(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT && event != CTRL_CLOSE_EVENT
        && event != CTRL_LOGOFF_EVENT && event != CTRL_SHUTDOWN_EVENT) return FALSE;
    stop_requested.store(true);
    // Windows terminates a closing console after the handler returns. Give the
    // input thread a bounded chance to release its keys first.
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        if (const auto done = shutdown_complete.load()) WaitForSingleObject(done, 2000);
    }
    return TRUE;
}

class runtime_environment {
public:
    runtime_environment() {
        instance_ = CreateMutexW(nullptr, FALSE, L"Local\\mouse_input_mapping_v1");
        if (!instance_) fail_windows("Cannot create instance mutex");
        if (GetLastError() == ERROR_ALREADY_EXISTS) {
            CloseHandle(instance_);
            throw std::runtime_error("Another mouse_input_mapping instance is already running");
        }
        done_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!done_) { CloseHandle(instance_); fail_windows("Cannot create shutdown event"); }
        stop_requested = false;
        shutdown_complete = done_;
        if (!SetConsoleCtrlHandler(console_handler, TRUE)) {
            shutdown_complete = nullptr;
            CloseHandle(done_);
            CloseHandle(instance_);
            fail_windows("Cannot register console shutdown handler");
        }
        input_ = GetStdHandle(STD_INPUT_HANDLE);
        has_console_ = GetConsoleMode(input_, &old_mode_) != FALSE;
        if (has_console_) SetConsoleMode(input_, (old_mode_ | ENABLE_EXTENDED_FLAGS | ENABLE_PROCESSED_INPUT) & ~ENABLE_QUICK_EDIT_MODE);
        timer_period_ = timeBeginPeriod(1) == TIMERR_NOERROR;
    }
    ~runtime_environment() {
        SetEvent(done_);
        SetConsoleCtrlHandler(console_handler, FALSE);
        shutdown_complete = nullptr;
        if (has_console_) SetConsoleMode(input_, old_mode_);
        if (timer_period_) timeEndPeriod(1);
        // done_ deliberately remains valid until process teardown: a console
        // handler may already be waiting on it on another system thread.
        CloseHandle(instance_);
    }
    runtime_environment(const runtime_environment&) = delete;
    runtime_environment& operator=(const runtime_environment&) = delete;
private:
    HANDLE instance_{};
    HANDLE done_{};
    HANDLE input_{};
    DWORD old_mode_{};
    bool has_console_ = false;
    bool timer_period_ = false;
};

bool key_is_down(key_code code) {
    const auto vk = MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX);
    return (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) != 0;
}

} // namespace

bool driver_available() {
    driver_api api;
    const auto context = api.create_context();
    if (!context) return false;
    api.destroy_context(context);
    return true;
}

void check_driver() {
    driver_session session;
    int keyboards = 0;
    int mice = 0;
    for (int device = 1; device <= 20; ++device) {
        std::array<wchar_t, 512> hardware_id{};
        if (session.api.get_hardware_id(session.context, device, hardware_id.data(),
                static_cast<unsigned int>(sizeof(hardware_id))) == 0) continue;
        hardware_id.back() = L'\0';
        if (is_keyboard(device)) ++keyboards;
        else ++mice;
        std::wcout << (is_keyboard(device) ? L"Keyboard " : L"Mouse ") << device << L": " << hardware_id.data() << L'\n';
    }
    if (keyboards == 0 || mice == 0) throw std::runtime_error("Driver opened, but no accessible keyboard or mouse was found");
    std::cout << "Driver is accessible. No input filters were enabled.\n"
                 "This checks connectivity only; verify actual X suppression with raw_input_probe.\n";
}

void run(const configuration& config) {
    validate(config);
    runtime_environment environment;
    driver_session session;
    motion_filter filter(config.filter);
    key_router router(config.left_key, config.right_key);
    toggle_latch toggle;
    bool enabled = false;
    bool absolute_warning = false;
    int output_keyboard = 0;
    const auto send_key = [&](key_event event) { session.send_key(event); };
    session.enable_filters();
    std::cout << "OFF - press the toggle key to enable. Ctrl+C exits.\n" << std::flush;
    try {
        for (;;) {
            if (stop_requested.load()) break;
            if (enabled) router.set_direction(filter.tick(clock_type::now()), output_keyboard, send_key);
            const auto device = session.api.wait_with_timeout(session.context, 4);
            if (device == 0) continue;
            alignas(InterceptionMouseStroke) std::array<char, sizeof(InterceptionMouseStroke) * 32> buffer{};
            const auto count = session.api.receive(session.context, device, reinterpret_cast<InterceptionStroke*>(buffer.data()), 32);
            if (count <= 0 || count > 32) throw std::runtime_error("Driver input receive failed");
            for (int index = 0; index < count; ++index) {
                if (is_keyboard(device)) {
                    InterceptionKeyStroke stroke{};
                    std::memcpy(&stroke, buffer.data() + index * sizeof(stroke), sizeof(stroke));
                    const bool simple = (stroke.state & ~(INTERCEPTION_KEY_UP | INTERCEPTION_KEY_E0)) == 0;
                    const auto code = static_cast<key_code>(stroke.code | ((stroke.state & INTERCEPTION_KEY_E0) ? 0xe000 : 0));
                    const bool down = (stroke.state & INTERCEPTION_KEY_UP) == 0;
                    if (simple && code == config.toggle_key) {
                        if (toggle.update(device, down)) {
                            if (!enabled && (key_is_down(config.left_key) || key_is_down(config.right_key))) {
                                std::cout << "OFF - release mapped physical keys, then press toggle again.\n" << std::flush;
                                continue;
                            }
                            router.set_direction(direction::idle, output_keyboard, send_key);
                            filter.reset();
                            enabled = !enabled;
                            if (enabled) output_keyboard = device;
                            // Play asynchronously so feedback never waits for the sound to finish.
                            PlaySoundW(enabled ? L"DeviceConnect" : L"DeviceDisconnect", nullptr,
                                SND_ALIAS | SND_ASYNC | SND_NODEFAULT | SND_SYSTEM);
                            std::cout << (enabled ? "ON\n" : "OFF\n") << std::flush;
                        }
                    } else if (!simple || !router.physical({device, code, down}, send_key)) {
                        session.forward(device, stroke);
                    }
                } else {
                    InterceptionMouseStroke stroke{};
                    std::memcpy(&stroke, buffer.data() + index * sizeof(stroke), sizeof(stroke));
                    if (enabled && !(stroke.flags & INTERCEPTION_MOUSE_MOVE_ABSOLUTE)) {
                        router.set_direction(filter.update(stroke.x, clock_type::now()), output_keyboard, send_key);
                        stroke.x = 0;
                    } else if (enabled && !absolute_warning && (stroke.flags & INTERCEPTION_MOUSE_MOVE_ABSOLUTE)) {
                        absolute_warning = true;
                        std::cout << "Absolute pointing device detected: passed through unchanged (unsupported).\n" << std::flush;
                    }
                    session.forward(device, stroke);
                }
                // Also tick under sustained traffic, where the wait never times out.
                if (enabled) router.set_direction(filter.tick(clock_type::now()), output_keyboard, send_key);
            }
        }
    } catch (...) {
        try { router.set_direction(direction::idle, output_keyboard, send_key); }
        catch (const std::exception& error) { std::cerr << "Key cleanup failed: " << error.what() << '\n'; }
        throw;
    }
    router.set_direction(direction::idle, output_keyboard, send_key);
    std::cout << "OFF - mapping keys released.\n";
}

} // namespace mouse_mapping
