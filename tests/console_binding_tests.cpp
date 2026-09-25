#include "config.hpp"
#include <windows.h>
#include <array>
#include <iostream>
#include <string>
#include <vector>

using namespace mouse_mapping;

namespace {
int child_test() {
    const auto input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, 0, nullptr);
    if (input == INVALID_HANDLE_VALUE || !SetStdHandle(STD_INPUT_HANDLE, input)) return 2;
    DWORD original_mode = 0;
    if (!GetConsoleMode(input, &original_mode)) return 3;
    std::vector<INPUT_RECORD> records;
    const auto key = [&](WORD vk, WORD scan, bool down, bool extended = false) {
        INPUT_RECORD record{};
        record.EventType = KEY_EVENT;
        record.Event.KeyEvent = {down, 1, vk, scan, {}, extended ? ENHANCED_KEY : 0u};
        records.push_back(record);
    };
    const auto confirm = [&] {
        key(VK_RETURN, 0x1c, true);
        key(VK_RETURN, 0x1c, true); // A held Enter must not confirm the following field.
        key(VK_RETURN, 0x1c, false);
    };
    key(VK_PAUSE, 0x45, true); // Unsupported keys must not replace the selection.
    key(VK_F12, 0x58, true);
    key(VK_F12, 0x58, true);
    key(VK_F12, 0x58, false);
    confirm();
    key(VK_LEFT, 0x4b, true, true);
    key(VK_LEFT, 0x4b, false, true);
    confirm();
    key(VK_F8, 0x42, true);
    key(VK_F8, 0x42, false);
    confirm();
    DWORD written = 0;
    if (!WriteConsoleInputW(input, records.data(), static_cast<DWORD>(records.size()), &written)) {
        std::cerr << "WriteConsoleInput failed: " << GetLastError() << '\n';
        return 4;
    }
    const auto result = configure({});
    DWORD restored_mode = 0;
    GetConsoleMode(input, &restored_mode);
    CloseHandle(input);
    if (result.left_key != 0x58 || result.right_key != 0xe04b || result.toggle_key != 0x42) return 5;
    if (original_mode != restored_mode) return 6;
    std::cout << "Direct console key capture passed.\n";
    return 0;
}
}

int wmain(int argc, wchar_t**) {
    if (argc == 2) return child_test();
    // The child owns a separate hidden console. WriteConsoleInput affects only
    // that console's input queue; no real keyboard/mouse events are injected.
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
    HANDLE read_pipe = nullptr;
    HANDLE write_pipe = nullptr;
    if (!CreatePipe(&read_pipe, &write_pipe, &attributes, 16384)) return 10;
    SetHandleInformation(read_pipe, HANDLE_FLAG_INHERIT, 0);
    const auto null_input = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING, 0, nullptr);
    std::wstring executable(32768, L'\0');
    executable.resize(GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size())));
    auto command = L"\"" + executable + L"\" --child";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW | STARTF_USESTDHANDLES;
    startup.wShowWindow = SW_HIDE;
    startup.hStdInput = null_input;
    startup.hStdOutput = write_pipe;
    startup.hStdError = write_pipe;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NEW_CONSOLE, nullptr, nullptr, &startup, &process)) return 11;
    CloseHandle(write_pipe);
    CloseHandle(null_input);
    const auto waited = WaitForSingleObject(process.hProcess, 10000);
    if (waited != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 12);
        WaitForSingleObject(process.hProcess, 1000);
    }
    DWORD result = 12;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    std::array<char, 4096> buffer{};
    DWORD read = 0;
    while (ReadFile(read_pipe, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read)
        std::cout.write(buffer.data(), read);
    CloseHandle(read_pipe);
    return static_cast<int>(result);
}
