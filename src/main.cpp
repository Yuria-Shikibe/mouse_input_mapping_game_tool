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
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--help" || argument == L"-h") {
                std::cout << "Mouse X -> keyboard mapping (Windows x64)\n"
                    "Usage: mouse_input_mapping [--configure] [--config <path>]\n"
                    "       mouse_input_mapping --check\n\n"
                    "--configure  Press keys, confirm with Enter, save, and exit.\n"
                    "--configure-text  Type key names (for IDE consoles), confirm, and save.\n"
                    "--config     Configuration file (default: config.ini beside the EXE).\n"
                    "--check      Check the DLL/driver and list accessible input devices.\n\n"
                    "--install-driver  Install/repair the bundled driver (requests administrator access).\n\n"
                    "First run asks for keys. Default: A / D, toggle F8. Starts OFF.\n"
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
            else throw std::runtime_error("Unknown option or missing argument; see --help");
        }
        if (install_only) {
            const auto result = install_driver();
            if (result == ERROR_SUCCESS_REBOOT_REQUIRED) std::cout << "Driver setup completed. Restart Windows once.\n";
            else if (result == ERROR_CANCELLED) std::cout << "Driver setup canceled.\n";
            return static_cast<int>(result);
        }
        if (check_only) { check_driver(); return 0; }
        configuration config;
        if (std::filesystem::exists(path)) config = load_config(path);
        if (reconfigure || !std::filesystem::exists(path)) {
            config = configure(config, text_mode);
            save_config(path, config);
            std::wcout << L"Configuration saved: " << path.native() << L'\n';
            if (reconfigure) return 0;
        }
        std::cout << "Left=" << describe_key(config.left_key) << " Right=" << describe_key(config.right_key)
            << " Toggle=" << describe_key(config.toggle_key) << '\n';
        if (!ensure_driver()) return 0;
        run(config);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
