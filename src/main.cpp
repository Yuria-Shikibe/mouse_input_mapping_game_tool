#include "config.hpp"
#include "runtime.hpp"
#include "deployment.hpp"

#include <windows.h>
#include <iostream>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    using namespace mouse_mapping;
    try {
        auto path = executable_directory() / L"config.ini";
        bool reconfigure = false;
        bool text_mode = false;
        bool check_only = false;
        bool install_only = false;
        game_command game;
        bool daemon = false;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--help" || argument == L"-h") {
                std::cout << "Mouse X -> keyboard mapping (kernel driver, Windows x64)\n"
                    "Usage: mouse_input_mapping_kernel [--configure] [--config <path>]\n"
                    "       mouse_input_mapping_kernel [--config <path>] --daemon <game.exe> [game arguments...]\n"
                    "       mouse_input_mapping_kernel --check\n\n"
                    "--configure  Press keys, confirm with Enter, save, and exit.\n"
                    "--configure-text  Type key names (for IDE consoles), confirm, and save.\n"
                    "--config     Configuration file (default: config.ini beside the EXE).\n"
                    "--check      Check the DLL/driver and list accessible input devices.\n\n"
                    "--install-driver  Install/repair the bundled driver (requests administrator access).\n\n"
                    "--daemon    Launch and monitor a game; configure keys and driver beforehand.\n\n"
                    "First run asks for keys. Default: A / D, toggle F8. Starts OFF.\n"
                    "X pulse defaults OFF; the configuration prompt accepts a 0..1 hold ratio.\n"
                    "Toggle is global and consumed; Ctrl+C exits from this console.\n"
                    "DLL and driver installer are built in. First run sets up a missing driver.\n"
                    "First-time driver setup needs administrator approval and one Windows restart.\n";
                return 0;
            }
            if (argument == L"--configure") reconfigure = true;
            else if (argument == L"--configure-text") { reconfigure = true; text_mode = true; }
            else if (argument == L"--check") check_only = true;
            else if (argument == L"--install-driver") install_only = true;
            else if (argument == L"--config" && index + 1 < argc) path = std::filesystem::absolute(argv[++index]);
            else if (argument == L"--daemon") {
                daemon = true;
                for (++index; index < argc; ++index) game.emplace_back(argv[index]);
                break;
            }
            else throw std::runtime_error("Unknown option or missing argument; see --help");
        }
        if (daemon && (game.empty() || game.front().empty() || reconfigure || check_only || install_only))
            throw std::runtime_error("--daemon requires a game executable and cannot be combined with setup/check options");
        if (install_only) {
            const auto result = install_driver();
            if (result == ERROR_SUCCESS_REBOOT_REQUIRED) std::cout << "Driver setup completed. Restart Windows once.\n";
            else if (result == ERROR_CANCELLED) std::cout << "Driver setup canceled.\n";
            return static_cast<int>(result);
        }
        if (check_only) { check_driver(); return 0; }
        configuration config;
        if (daemon && !std::filesystem::exists(path))
            throw std::runtime_error("Daemon configuration missing; run --configure before launching the game");
        if (std::filesystem::exists(path)) config = load_config(path, false, daemon);
        if (reconfigure || !std::filesystem::exists(path)) {
            config = configure(config, text_mode);
            save_config(path, config);
            std::wcout << L"Configuration saved: " << path.native() << L'\n';
            if (reconfigure) return 0;
        }
        std::cout << "Left=" << describe_key(config.left_key) << " Right=" << describe_key(config.right_key)
            << " Toggle=" << describe_key(config.toggle_key) << '\n';
        std::cout << "X=" << (config.x_pulse_enabled ? "pulse" : "hold")
            << " ratio=" << config.x_hold_ratio << " keyboard-override="
            << (config.x_keyboard_override_enabled ? "on" : "off")
            << " period=" << config.pulse_period_ms << "ms\n";
        if (daemon) {
            if (!driver_available()) throw std::runtime_error("Daemon driver unavailable; run this tool normally to install it, then restart Windows");
        } else if (!ensure_driver()) return 0;
        run(config, daemon ? &game : nullptr);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
