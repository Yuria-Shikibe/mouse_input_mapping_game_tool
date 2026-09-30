#include "config.hpp"
#include "console_config.hpp"
#include "runtime.hpp"
#include <iostream>
#include <print>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    using namespace mouse_mapping;
    try {
        auto path = default_config_path();
        bool reconfigure = false, text_mode = false;
        game_command game;
        bool daemon = false;
        auto priority = input_priority::normal;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--help" || argument == L"-h") {
                std::cout << "Mouse XY -> keyboard mapping (user mode, no driver)\n"
                    "Usage: mouse_input_mapping_user [--configure | --configure-text] [--config <path>]\n"
                    "       mouse_input_mapping_user [--config <path>] --daemon <game.exe> [game arguments...]\n"
                    "--daemon launches and monitors a game; configure all keys beforehand.\n"
                    "--input-priority <normal|above-normal> sets input thread priority (default normal; before --daemon).\n"
                    "Default config: config.ini beside the EXE.\n"
                    "Defaults: X- A, X+ D, Y- S, Y+ W, toggle F8. Starts OFF.\n"
                    "Mouse buttons: LMB SUBTRACT, RMB M, CMB T, X1 LSHIFT, X2 F. Wheel up/down R.\n"
                    "X pulse defaults OFF; Y pulse defaults ON with 0.45 hold ratio.\n"
                    "Raw Input reads movement/buttons; SendInput sends keys. Disable mouse input in the game.\n"
                    "Original mouse input is NOT blocked. Absolute coordinates are ignored; buttons still map.\n"
                    "Hold either optional bypass key to suspend movement, button and wheel mappings.\n"
                    "Toggle is global; Ctrl+C exits. Release all mapping keys and mouse buttons before enabling.\n"
                    "No Interception DLL, driver installation, or administrator access is required.\n"
                    "SendInput can be rejected by a game or by Windows integrity-level restrictions.\n";
                return 0;
            }
            if (argument == L"--configure") reconfigure = true;
            else if (argument == L"--configure-text") { reconfigure = true; text_mode = true; }
            else if (argument == L"--config" && index + 1 < argc) path = std::filesystem::absolute(argv[++index]);
            else if (argument == L"--input-priority" && index + 1 < argc) priority = parse_input_priority(argv[++index]);
            else if (argument == L"--daemon") {
                daemon = true;
                for (++index; index < argc; ++index) game.emplace_back(argv[index]);
                break;
            }
            else throw std::runtime_error("Unknown option or missing argument; see --help");
        }
        if (daemon && (game.empty() || game.front().empty() || reconfigure))
            throw std::runtime_error("--daemon requires a game executable and cannot be combined with --configure");
        configuration config;
        config.map_y = true;
        if (daemon && !std::filesystem::exists(path))
            throw std::runtime_error("Daemon configuration missing; run --configure before launching the game");
        if (std::filesystem::exists(path)) config = load_config(path, true, daemon);
        if (reconfigure || !std::filesystem::exists(path)) {
            config = configure(config, text_mode);
            save_config(path, config);
            std::wcout << L"Configuration saved: " << path.native() << L'\n';
            if (reconfigure) return 0;
        }
        std::println("X-={} X+={} Y-={} Y+={} Toggle={}", describe_key(config.left_key),
            describe_key(config.right_key), describe_key(config.up_key), describe_key(config.down_key),
            describe_key(config.toggle_key));
        std::println("X={} ratio={} keyboard-override={} Y={} ratio={} period={}ms",
            config.x_pulse_enabled ? "pulse" : "hold", config.x_hold_ratio,
            config.x_keyboard_override_enabled ? "on" : "off", config.y_pulse_enabled ? "pulse" : "hold",
            config.y_hold_ratio, config.pulse_period_ms);
        std::println("X sensitivity={} full-speed={}counts/s start/reverse={}/{}",
            config.x_pulse_enabled && config.x_curve.enabled ? "curve" : "fixed", config.x_curve.full_speed,
            config.x_start_counts, config.x_reverse_counts);
        std::println("Y sensitivity={} full-speed={}counts/s start/reverse={}/{}",
            config.y_pulse_enabled && config.y_curve.enabled ? "curve" : "fixed", config.y_curve.full_speed,
            config.y_start_counts, config.y_reverse_counts);
        for (std::size_t index = 0; index < mouse_key_fields.size(); ++index)
            std::cout << mouse_key_fields[index] << '=' << describe_key(config.mouse_keys[index]) << '\n';
        for (std::size_t index = 0; index < wheel_key_fields.size(); ++index)
            std::cout << wheel_key_fields[index] << '=' << describe_key(config.wheel_keys[index]) << '\n';
        std::println("Bypass={} / {}", format_input_binding(config.bypass_keys[0]), format_input_binding(config.bypass_keys[1]));
        run(config, daemon ? &game : nullptr, priority);
        return 0;
    } catch (const std::exception& error) {
        std::println(std::cerr, "Error: {}", error.what());
        return 1;
    }
}
