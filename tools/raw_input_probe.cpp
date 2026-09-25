#include <windows.h>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {
struct statistics {
    std::uint64_t mouse_packets = 0;
    std::uint64_t nonzero_x_packets = 0;
    std::uint64_t absolute_packets = 0;
    std::uint64_t button_packets = 0;
    std::uint64_t wheel_packets = 0;
    std::int64_t sum_x = 0;
    std::int64_t sum_y = 0;
} totals;
std::atomic<bool> stop_requested = false;
bool input_error = false;

BOOL WINAPI console_handler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        stop_requested = true;
        return TRUE;
    }
    return FALSE;
}

void report() {
    std::cout << "relative_packets=" << totals.mouse_packets << " nonzero_x_packets=" << totals.nonzero_x_packets
        << " sum_x=" << totals.sum_x << " sum_y=" << totals.sum_y
        << " buttons=" << totals.button_packets << " wheels=" << totals.wheel_packets
        << " absolute_packets=" << totals.absolute_packets << '\n' << std::flush;
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_INPUT) {
        RAWINPUT input{};
        UINT bytes = sizeof(input);
        if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &input, &bytes, sizeof(RAWINPUTHEADER)) == UINT(-1)) {
            input_error = true;
            std::cerr << "GetRawInputData failed: " << GetLastError() << '\n';
        } else if (input.header.dwType == RIM_TYPEMOUSE) {
            const auto& mouse = input.data.mouse;
            if (mouse.usFlags & MOUSE_MOVE_ABSOLUTE) ++totals.absolute_packets;
            else {
                ++totals.mouse_packets;
                if (mouse.lLastX != 0) ++totals.nonzero_x_packets;
                totals.sum_x += mouse.lLastX;
                totals.sum_y += mouse.lLastY;
            }
            if (mouse.usButtonFlags & 0x03ff) ++totals.button_packets;
            if (mouse.usButtonFlags & (RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL)) ++totals.wheel_packets;
        } else if (input.header.dwType == RIM_TYPEKEYBOARD) {
            const auto& keyboard = input.data.keyboard;
            if (keyboard.VKey == VK_F9 && !(keyboard.Flags & RI_KEY_BREAK)) {
                totals = {};
                std::cout << "Counters reset.\n";
            }
            std::cout << "key=0x" << std::hex << keyboard.MakeCode
                << ((keyboard.Flags & RI_KEY_E0) ? " E0" : "") << ((keyboard.Flags & RI_KEY_E1) ? " E1" : "")
                << ((keyboard.Flags & RI_KEY_BREAK) ? " UP" : " DOWN") << std::dec << '\n';
        }
        return DefWindowProcW(window, message, wparam, lparam);
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}

int wmain(int argc, wchar_t** argv) {
    try {
        int seconds = 0;
        bool expect_zero_x = false;
        for (int index = 1; index < argc; ++index) {
            const std::wstring_view argument(argv[index]);
            if (argument == L"--help") {
                std::cout << "Usage: raw_input_probe [--seconds N] [--expect-zero-x]\n"
                             "Reports background Raw Input mouse totals and keyboard edges.\n"
                             "F9 resets counters. Ctrl+C exits. No input is blocked.\n"
                             "--expect-zero-x requires relative mouse packets and zero nonzero-X packets.\n";
                return 0;
            }
            if (argument == L"--expect-zero-x") expect_zero_x = true;
            else if (argument == L"--seconds" && index + 1 < argc) {
                const std::wstring value(argv[++index]);
                std::size_t end = 0;
                seconds = std::stoi(value, &end);
                if (end != value.size() || seconds < 1 || seconds > 3600) throw std::runtime_error("--seconds must be 1..3600");
            } else throw std::runtime_error("Unknown option or missing argument; see --help");
        }
        const auto instance = GetModuleHandleW(nullptr);
        WNDCLASSW window_class{};
        window_class.lpfnWndProc = window_proc;
        window_class.hInstance = instance;
        window_class.lpszClassName = L"mouse_mapping_raw_input_probe";
        if (!RegisterClassW(&window_class)) throw std::runtime_error("Cannot register message window class");
        const auto window = CreateWindowExW(0, window_class.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, nullptr);
        if (!window) throw std::runtime_error("Cannot create message-only window");
        const std::array<RAWINPUTDEVICE, 2> devices{{{1, 2, RIDEV_INPUTSINK, window}, {1, 6, RIDEV_INPUTSINK, window}}};
        if (!RegisterRawInputDevices(devices.data(), static_cast<UINT>(devices.size()), sizeof(RAWINPUTDEVICE))) {
            DestroyWindow(window);
            throw std::runtime_error("Cannot register Raw Input devices");
        }
        SetConsoleCtrlHandler(console_handler, TRUE);
        const auto start = GetTickCount64();
        auto last_report = start;
        std::cout << "Listening to Raw Input. F9 resets counters; Ctrl+C exits.\n" << std::flush;
        while (!stop_requested.load() && (!seconds || GetTickCount64() - start < static_cast<ULONGLONG>(seconds) * 1000)) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            MSG message{};
            // Bound each batch so reporting and --seconds still work at high polling rates.
            for (int count = 0; count < 256 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++count) {
                TranslateMessage(&message);
                DispatchMessageW(&message);
            }
            if (GetTickCount64() - last_report >= 1000) {
                report();
                last_report = GetTickCount64();
            }
        }
        report();
        DestroyWindow(window);
        SetConsoleCtrlHandler(console_handler, FALSE);
        if (input_error) return 1;
        if (expect_zero_x && (totals.mouse_packets == 0 || totals.nonzero_x_packets != 0)) {
            std::cerr << "FAIL: need relative mouse traffic with no nonzero X packets.\n";
            return 1;
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }
}
