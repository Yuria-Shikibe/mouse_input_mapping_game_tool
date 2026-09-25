#include "game_process.hpp"

#include <iostream>
#include <stdexcept>
#include <system_error>

namespace mouse_mapping {
namespace {
std::wstring quote_argument(const std::wstring& argument) {
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos && !argument.empty()) return argument;
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') { ++slashes; continue; }
        if (character == L'"') {
            result.append(slashes * 2 + 1, L'\\');
            result += character;
        } else {
            result.append(slashes, L'\\');
            result += character;
        }
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}
} // namespace

game_process::game_process(const game_command& command) {
    if (command.empty() || command.front().empty()) throw std::runtime_error("--daemon requires a game executable");
    std::wstring line;
    for (const auto& argument : command) {
        if (!line.empty()) line += L' ';
        line += quote_argument(argument);
    }
    std::vector<wchar_t> mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    // A detached child does not share this tool's console control events.
    if (!CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, FALSE, DETACHED_PROCESS,
            nullptr, nullptr, &startup, &information))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot launch game process");
    process_ = information.hProcess;
    CloseHandle(information.hThread);
    std::wcout << L"Daemon: launched " << command.front() << L" (PID " << information.dwProcessId << L").\n" << std::flush;
}

game_process::~game_process() { if (process_) CloseHandle(process_); }

bool game_process::exited() const {
    const auto result = WaitForSingleObject(process_, 0);
    if (result == WAIT_OBJECT_0) return true;
    if (result == WAIT_TIMEOUT) return false;
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot monitor game process");
}

} // namespace mouse_mapping
