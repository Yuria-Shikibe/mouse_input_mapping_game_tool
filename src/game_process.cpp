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
    std::vector mutable_line(line.begin(), line.end());
    mutable_line.push_back(L'\0');
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION information{};
    // Do not set KILL_ON_JOB_CLOSE or allow breakaway: descendants stay tracked,
    // but closing the tool must leave the game running.
    job_ = CreateJobObjectW(nullptr, nullptr);
    if (!job_)
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot create game process job");
    try {
        // Suspend until assigned so even a short-lived launcher cannot escape tracking.
        // A detached child does not share this tool's console control events.
        if (!CreateProcessW(nullptr, mutable_line.data(), nullptr, nullptr, FALSE,
                DETACHED_PROCESS | CREATE_SUSPENDED, nullptr, nullptr, &startup, &information))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot launch game process");
        if (!AssignProcessToJobObject(job_, information.hProcess))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot track game process tree");
        if (ResumeThread(information.hThread) == static_cast<DWORD>(-1))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot resume game process");
    } catch (...) {
        // Only the not-yet-started process is terminated on setup failure.
        if (information.hProcess) {
            TerminateProcess(information.hProcess, 1);
            CloseHandle(information.hProcess);
        }
        if (information.hThread) CloseHandle(information.hThread);
        CloseHandle(job_);
        job_ = nullptr;
        throw;
    }
    CloseHandle(information.hProcess);
    CloseHandle(information.hThread);
    std::wcout << L"Daemon: launched " << command.front() << L" (PID " << information.dwProcessId << L").\n" << std::flush;
}

game_process::~game_process() { if (job_) CloseHandle(job_); }

bool game_process::exited() const {
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
    if (!QueryInformationJobObject(job_, JobObjectBasicAccountingInformation,
            &accounting, sizeof(accounting), nullptr))
        throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot monitor game process tree");
    return accounting.ActiveProcesses == 0;
}

} // namespace mouse_mapping
