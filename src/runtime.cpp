#include "runtime.hpp"
#include "chord_mapping.hpp"
#include "deployment.hpp"
#include "input_monitor.hpp"
#include "input_schedule.hpp"
#include "bypass.hpp"
#include "pie_runtime.hpp"
#include <windows.h>
#include <mmsystem.h>
#include <interception.h>

#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstring>
#include <format>
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
        // Use the known bundled version even if a stale/corrupt DLL sits beside the EXE.
        extracted_ = std::make_unique<embedded_file>(embedded_asset::library);
        const auto path = extracted_->path();
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
            throw std::runtime_error(std::format("Driver input send failed (device {})", device));
    }

    void send_key(key_event event) {
        if (!key_bound(event.code)) return;
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
    if (!key_bound(code)) return false;
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
                 "This checks connectivity only; verify actual X suppression in your target application.\n";
}

void run(const configuration& source, const game_command* game, input_priority priority) {
    validate(source);
    const auto config = effective_config(source);
    runtime_environment environment;
    input_thread_priority thread_priority(priority);
    driver_session session;
    monitor_publisher monitor;
    monitor.start(config, false);
    axis_mapping axis(config.axis_filter(false), config.left_key, config.right_key,
        config.x_pulse_enabled, config.x_hold_ratio, config.pulse_period_ms,
        config.x_keyboard_override_enabled, config.x_curve, config.x_smoothing_factor);
    axis.observe(monitor.observer(false));
    axis_mapping vertical(config.axis_filter(true), config.up_key, config.down_key,
        config.y_pulse_enabled, config.y_hold_ratio, config.pulse_period_ms,
        false, config.y_curve, config.y_smoothing_factor);
    vertical.observe(monitor.observer(true));
    bypass_state bypass(config);
    pie_runtime pie(config);
    toggle_latch toggle;
    bool enabled = false;
    bool absolute_warning = false;
    int output_keyboard = 0;
    chord_mapping chord(config.chord);
    const auto send_raw_key = [&](key_event event) { session.send_key(event); };
    const auto send_key = [&](key_event event) { chord.output(event, send_raw_key); };
    const auto release_xy = [&] {
        std::exception_ptr failure;
        try { axis.release(output_keyboard, send_key); } catch (...) { failure = std::current_exception(); }
        try { vertical.release(output_keyboard, send_key); } catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
    };
    const auto release = [&] {
        std::exception_ptr failure;
        try { chord.cancel(send_raw_key); } catch (...) { failure = std::current_exception(); }
        try { pie.cancel(send_key); } catch (...) { failure = std::current_exception(); }
        try { release_xy(); } catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
    };
    const auto tick = [&](time_point now) {
        pie.tick(now, send_key);
        if (!enabled || bypass.held() || pie.opened()) return;
        axis.tick(now, output_keyboard, send_key);
        if (config.kernel_y_enabled) vertical.tick(now, output_keyboard, send_key);
    };
    bool bypassed = false;
    const auto sync_bypass = [&](bool packet = false) {
        const bool next = bypass.held() || packet;
        if (next && !bypassed) release();
        if (next != bypassed) monitor.bypassed(next);
        bypassed = next;
    };
    session.enable_filters();
    pie.start();
    for (const auto key : {config.left_key, config.right_key})
        if (key_is_down(key)) axis.seed_physical(key);
    if (config.kernel_y_enabled) for (const auto key : {config.up_key, config.down_key})
        if (key_is_down(key)) vertical.seed_physical(key);
    if (config.chord.enabled) for (const auto key : {config.chord.first, config.chord.second})
        if (key_is_down(key)) chord.seed(key);
    bypass.seed();
    sync_bypass();
    std::cout << "OFF - press the toggle key to enable. Ctrl+C exits.\n" << std::flush;
    try {
        std::unique_ptr<game_process> child;
        if (game) child = std::make_unique<game_process>(*game);
        auto maintenance = clock_type::now();
        auto game_check = maintenance;
        auto device_check = maintenance;
        alignas(InterceptionMouseStroke) std::array<char, sizeof(InterceptionMouseStroke) * 32> buffer;
        for (;;) {
            auto now = clock_type::now();
            if (now >= maintenance) {
                monitor.heartbeat(now);
                axis.observe(monitor.observer(false));
                vertical.observe(monitor.observer(true));
                bypass.refresh_initial();
                sync_bypass();
                pie.maintenance(send_key);
                maintenance = now + milliseconds(50);
            }
            if (stop_requested.load()) {
                if (child) std::cout << "Daemon: mapping stopped; game process continues.\n" << std::flush;
                break;
            }
            if ((bypass.held() || pie.opened() || chord.held()) && now >= device_check) {
                device_check = now + milliseconds(500);
                for (int id = 1; id <= 20; ++id) {
                    if (!bypass.owns_device(id, is_mouse(id) != 0) &&
                        !(pie.opened() && pie.owner() == static_cast<std::uintptr_t>(id)) && !chord.owns_device(id)) continue;
                    std::array<wchar_t, 512> hardware{};
                    if (session.api.get_hardware_id(session.context, id, hardware.data(),
                        static_cast<unsigned int>(sizeof(hardware))) == 0) {
                        if (is_mouse(id)) { bypass.remove_mouse(id); pie.remove_mouse(id, send_key); }
                        else { bypass.remove_keyboard(id); pie.remove_keyboard(id, send_key); }
                        chord.remove_device(id, send_raw_key);
                    }
                }
                sync_bypass();
            }
            if (child && now >= game_check) {
                game_check = now + milliseconds(4);
                if (child->exited()) {
                    std::cout << "Daemon: game process exited; stopping mapping.\n" << std::flush;
                    break;
                }
            }
            tick(now);
            DWORD wait_ms = 4;
            if (const auto due = pie.deadline()) wait_ms = std::min(wait_ms, wait_milliseconds(*due, clock_type::now(), 4));
            if (enabled && !bypass.held() && !pie.opened())
                for (const auto due : {axis.deadline(), config.kernel_y_enabled ? vertical.deadline() : std::nullopt})
                    if (due) wait_ms = std::min(wait_ms, wait_milliseconds(*due, clock_type::now(), 4));
            if (child) wait_ms = std::min(wait_ms, wait_milliseconds(game_check, clock_type::now(), 4));
            const auto device = session.api.wait_with_timeout(session.context, wait_ms);
            if (device == 0) continue;
            const auto count = session.api.receive(session.context, device, reinterpret_cast<InterceptionStroke*>(buffer.data()), 32);
            if (count <= 0 || count > 32) throw std::runtime_error("Driver input receive failed");
            for (int index = 0; index < count; ++index) {
                now = clock_type::now();
                if (is_keyboard(device)) {
                    InterceptionKeyStroke stroke{};
                    std::memcpy(&stroke, buffer.data() + index * sizeof(stroke), sizeof(stroke));
                    const bool simple = (stroke.state & ~(INTERCEPTION_KEY_UP | INTERCEPTION_KEY_E0)) == 0;
                    const auto code = static_cast<key_code>(stroke.code | ((stroke.state & INTERCEPTION_KEY_E0) ? 0xe000 : 0));
                    const bool down = (stroke.state & INTERCEPTION_KEY_UP) == 0;
                    if (simple) { bypass.keyboard(device, code, down); sync_bypass(); }
                    if (simple && pie.trigger_key(code)) {
                        if (!pie.keyboard(device, code, down, enabled && !bypass.held(), now,
                            output_keyboard, send_key, release_xy)) session.forward(device, stroke);
                    } else if (simple && chord.trigger_key(code)) {
                        session.forward(device, stroke);
                        chord.keyboard(device, code, down, enabled && !bypass.held(), device, send_raw_key);
                    } else if (simple && key_bound(config.toggle_key) && code == config.toggle_key) {
                        if (toggle.update(device, down)) {
                            if (!enabled && !bypass.held() && (key_is_down(config.left_key) || key_is_down(config.right_key)
                                || (config.kernel_y_enabled && (key_is_down(config.up_key) || key_is_down(config.down_key))))) {
                                std::cout << "OFF - release mapped physical keys, then press toggle again.\n" << std::flush;
                                continue;
                            }
                            release();
                            enabled = !enabled;
                            monitor.enabled(enabled);
                            if (enabled) output_keyboard = device;
                            // Play asynchronously so feedback never waits for the sound to finish.
                            PlaySoundW(enabled ? L"DeviceConnect" : L"DeviceDisconnect", nullptr,
                                SND_ALIAS | SND_ASYNC | SND_NODEFAULT | SND_SYSTEM);
                            std::cout << (enabled ? "ON\n" : "OFF\n") << std::flush;
                        }
                    } else if (simple && pie.configured() &&
                        std::find(pie_arrow_keys.begin(), pie_arrow_keys.end(), code) != pie_arrow_keys.end()) {
                        if (enabled && !bypass.held()) pie.physical({device, code, down}, send_key);
                        else {
                            pie.physical({device, code, down}, [](key_event) {});
                            if (chord.output_key(code)) send_key({device, code, down});
                            else session.forward(device, stroke);
                        }
                    } else if (simple && bypass.keyboard_binding(code)) {
                        axis.physical({device, code, down}, [](key_event) {});
                        if (config.kernel_y_enabled) vertical.physical({device, code, down}, [](key_event) {});
                        session.forward(device, stroke);
                    } else if (!simple || !(axis.physical({device, code, down}, send_key)
                        || (config.kernel_y_enabled && vertical.physical({device, code, down}, send_key)))) {
                        if (simple && chord.output_key(code)) send_key({device, code, down});
                        else session.forward(device, stroke);
                    }
                } else {
                    InterceptionMouseStroke stroke{};
                    std::memcpy(&stroke, buffer.data() + index * sizeof(stroke), sizeof(stroke));
                    const bool was_bypassed = bypass.held();
                    const bool bypass_down = bypass.mouse(device, stroke.state);
                    const bool bypass_packet = was_bypassed || bypass_down || bypass.held();
                    sync_bypass(bypass_packet);
                    if (monitor.recording())
                        monitor.motion(stroke.x, stroke.y, (stroke.flags & INTERCEPTION_MOUSE_MOVE_ABSOLUTE) != 0);
                    chord.mouse(device, stroke.state, enabled && !bypass_packet, output_keyboard, send_raw_key);
                    const auto pie_packet = pie.packet(device, stroke.state, stroke.x, stroke.y,
                        (stroke.flags & INTERCEPTION_MOUSE_MOVE_ABSOLUTE) != 0, enabled && !bypass_packet,
                        now, output_keyboard, send_key, release_xy);
                    stroke.state = pie_packet.buttons;
                    if (pie_packet.owns_motion) {
                        stroke.x = stroke.y = 0;
                        // A zero absolute position is not a no-op: forward button-only
                        // packets as zero relative movement instead.
                        stroke.flags = static_cast<unsigned short>(stroke.flags &
                            ~(INTERCEPTION_MOUSE_MOVE_ABSOLUTE | INTERCEPTION_MOUSE_VIRTUAL_DESKTOP));
                    } else if (enabled && !bypass_packet && !(stroke.flags & INTERCEPTION_MOUSE_MOVE_ABSOLUTE)) {
                        axis.update(stroke.x, now, output_keyboard, send_key);
                        if (config.kernel_y_enabled) vertical.update(stroke.y, now, output_keyboard, send_key);
                        stroke.x = 0;
                        if (config.kernel_y_block) stroke.y = 0;
                    } else if (enabled && !absolute_warning && (stroke.flags & INTERCEPTION_MOUSE_MOVE_ABSOLUTE)) {
                        absolute_warning = true;
                        std::cout << "Absolute pointing device detected: passed through unchanged (unsupported).\n" << std::flush;
                    }
                    // Do not send an empty movement packet back through the driver.
                    if (!pie_packet.owns_motion || stroke.state != 0 || stroke.information != 0 ||
                        (stroke.flags & ~INTERCEPTION_MOUSE_MOVE_NOCOALESCE) != 0)
                        session.forward(device, stroke);
                    sync_bypass();
                }
                // Also tick under sustained traffic, where the wait never times out.
                tick(clock_type::now());
            }
        }
    } catch (...) {
        try { release(); }
        catch (const std::exception& error) { std::cerr << "Key cleanup failed: " << error.what() << '\n'; }
        throw;
    }
    release();
    std::cout << "OFF - mapping keys released.\n";
}

} // namespace mouse_mapping
