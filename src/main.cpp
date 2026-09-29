#include "config.hpp"
#include "console_config.hpp"
#include "runtime.hpp"
#include "deployment.hpp"

#include <windows.h>
#include <iostream>
#include <print>
#include <string_view>

int wmain(int argc, wchar_t** argv) {
    using namespace mouse_mapping;
    DWORD console_processes[2]{};
    bool interactive = argc == 1 || GetConsoleProcessList(console_processes, 2) == 1;
    // Automation/checks must never wait on a message box, including the hidden
    // elevated installation child. The visible parent reports its result.
    for (int index = 1; index < argc; ++index) {
        const std::wstring_view argument(argv[index]);
        if (argument == L"--check" || argument == L"--daemon" || argument == L"--install-driver"
            || argument == L"--help" || argument == L"-h" || argument == L"--non-interactive")
            interactive = false;
    }
    try {
        auto path = default_config_path();
        bool reconfigure = false;
        bool text_mode = false;
        bool check_only = false;
        bool install_only = false;
        game_command game;
        bool daemon = false;
        auto priority = input_priority::normal;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--help" || argument == L"-h") {
                std::cout << "Mouse X/Y -> keyboard mapping (kernel driver, Windows x64)\n"
                    "Usage: mouse_input_mapping_kernel [--configure] [--config <path>]\n"
                    "       mouse_input_mapping_kernel [--config <path>] --daemon <game.exe> [game arguments...]\n"
                    "       mouse_input_mapping_kernel --check\n\n"
                    "--configure  Press keys, confirm with Enter, save, and exit.\n"
                    "--configure-text  Type key names (for IDE consoles), confirm, and save.\n"
                    "--config     Configuration file (default: config.ini; fallback: config.user.ini beside the EXE).\n"
                    "--check      Check the DLL/driver and list accessible input devices.\n\n"
                    "--non-interactive  No repair dialogs; report failures to stderr and a startup log.\n"
                    "--install-driver  Install/repair the bundled driver (requests administrator access).\n\n"
                    "--daemon    Launch and monitor a game; configure keys and driver beforehand.\n\n"
                    "--input-priority <normal|above-normal>  Input thread priority (default normal; before --daemon).\n"
                    "First run asks for keys. Default: A / D, toggle F8. Starts OFF.\n"
                    "X pulse defaults OFF; the configuration prompt accepts a 0..1 hold ratio.\n"
                    "Kernel Y mapping and Y blocking are independent and default OFF.\n"
                    "Hold either optional bypass key to pass original input without mapping.\n"
                    "Toggle is global and consumed; Ctrl+C exits from this console.\n"
                    "DLL and driver installer are built in. First run sets up a missing driver.\n"
                    "First-time driver setup needs administrator approval and one Windows restart.\n";
                return 0;
            }
            if (argument == L"--configure") reconfigure = true;
            else if (argument == L"--configure-text") { reconfigure = true; text_mode = true; }
            else if (argument == L"--check") check_only = true;
            else if (argument == L"--install-driver") install_only = true;
            else if (argument == L"--non-interactive") {}
            else if (argument == L"--config" && index + 1 < argc) path = std::filesystem::absolute(argv[++index]);
            else if (argument == L"--input-priority" && index + 1 < argc) priority = parse_input_priority(argv[++index]);
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
            if (result == ERROR_SUCCESS_REBOOT_REQUIRED)
                report_startup(L"驱动安装/修复完成。请手动重启 Windows，然后重新启动内核映射。", false);
            else if (result == ERROR_CANCELLED)
                report_startup(L"已取消管理员授权，驱动未修复。可重新运行 --install-driver。", false);
            else if (result != ERROR_SUCCESS)
                report_startup(L"驱动安装失败，退出码：" + std::to_wstring(result)
                    + L"。请在管理员终端运行 --install-driver 重试。", false);
            return static_cast<int>(result);
        }
        if (check_only) {
            if (!driver_available()) {
                report_startup(driver_diagnosis() + L"\n修复命令：mouse_input_mapping_kernel.exe --install-driver", false);
                return ERROR_NOT_READY;
            }
            check_driver(); return 0;
        }
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
        std::println("Left={} Right={} Toggle={}", describe_key(config.left_key), describe_key(config.right_key),
            describe_key(config.toggle_key));
        std::println("X={} ratio={} keyboard-override={} period={}ms",
            config.x_pulse_enabled ? "pulse" : "hold", config.x_hold_ratio,
            config.x_keyboard_override_enabled ? "on" : "off", config.pulse_period_ms);
        std::println("X sensitivity={} full-speed={}counts/s start/reverse={}/{}",
            config.x_pulse_enabled && config.x_curve.enabled ? "curve" : "fixed", config.x_curve.full_speed,
            config.x_start_counts, config.x_reverse_counts);
        std::println("Kernel Y mapping={} block={} Up={} Down={}; bypass={} / {}",
            config.kernel_y_enabled, config.kernel_y_block, describe_key(config.kernel_y.up_key),
            describe_key(config.kernel_y.down_key), format_input_binding(config.bypass_keys[0]), format_input_binding(config.bypass_keys[1]));
        if (daemon) {
            if (!driver_available()) throw std::runtime_error("Daemon driver unavailable; run this tool normally to install it, then restart Windows");
        } else {
            const auto result = ensure_driver(interactive);
            if (result != ERROR_SUCCESS) return static_cast<int>(result);
        }
        run(config, daemon ? &game : nullptr, priority);
        return 0;
    } catch (const std::exception& error) {
        report_startup(L"内核映射启动或运行失败。\n\n详细原因：" + startup_error_text(error.what())
            + L"\n\n配置错误请使用配置器检查或通过 --config 指定有效文件；驱动问题请先运行 --check 查看诊断，再使用 --install-driver 修复。", interactive);
        return 1;
    }
}
