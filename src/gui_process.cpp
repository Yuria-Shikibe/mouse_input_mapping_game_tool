#include "gui_process.hpp"
#include "config_document.hpp"
#include <stdexcept>
#include <system_error>

namespace mouse_mapping {
namespace {
struct handle {
    HANDLE value = nullptr;
    ~handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};
void fail(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}
}
gui_process::~gui_process() {
    if (process_) CloseHandle(process_);
    if (reader_) CloseHandle(reader_);
}
void gui_process::start(const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments, bool capture) {
    if (active()) throw std::runtime_error("A tool is still running");
    if (!std::filesystem::is_regular_file(executable))
        throw std::runtime_error("Executable missing; place the selected tool beside the configuration editor");
    std::vector command{executable.wstring()};
    command.insert(command.end(), arguments.begin(), arguments.end());
    auto line = windows_command_line(command);
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = capture ? sizeof(startup) : sizeof(STARTUPINFOW);
    handle reader, writer, input;
    HANDLE inherited[2]{};
    std::vector<unsigned char> attributes;
    struct attribute_cleanup {
        LPPROC_THREAD_ATTRIBUTE_LIST value = nullptr;
        ~attribute_cleanup() { if (value) DeleteProcThreadAttributeList(value); }
    } cleanup;
    if (capture) {
        SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
        if (!CreatePipe(&reader.value, &writer.value, &security, 0)) fail("CreatePipe");
        if (!SetHandleInformation(reader.value, HANDLE_FLAG_INHERIT, 0)) fail("SetHandleInformation");
        input.value = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (input.value == INVALID_HANDLE_VALUE) fail("Open NUL");
        SIZE_T size = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
        attributes.resize(size);
        startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &size)) fail("Initialize attributes");
        cleanup.value = startup.lpAttributeList;
        inherited[0] = writer.value;
        inherited[1] = input.value;
        if (!UpdateProcThreadAttribute(startup.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                inherited, sizeof(inherited), nullptr, nullptr)) fail("Set inherited handles");
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.StartupInfo.hStdOutput = startup.StartupInfo.hStdError = writer.value;
        startup.StartupInfo.hStdInput = input.value;
    }
    PROCESS_INFORMATION information{};
    const auto flags = capture ? CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT : CREATE_NEW_CONSOLE;
    if (!CreateProcessW(executable.c_str(), line.data(), nullptr, nullptr, capture, flags,
            nullptr, executable.parent_path().c_str(), &startup.StartupInfo, &information)) fail("Cannot launch tool");
    CloseHandle(information.hThread);
    process_ = information.hProcess;
    if (capture) {
        reader_ = reader.value;
        reader.value = nullptr;
    }
}
bool gui_process::poll(std::string& output, DWORD& exit_code) {
    if (!active()) return false;
    // Bound per-tick work so verbose children cannot starve the UI message loop.
    for (int block = 0; reader_ && block < 16; ++block) {
        DWORD available = 0;
        if (!PeekNamedPipe(reader_, nullptr, 0, nullptr, &available, nullptr)) {
            if (GetLastError() == ERROR_BROKEN_PIPE) break;
            fail("Cannot read tool output");
        }
        if (!available) break;
        char buffer[4096];
        DWORD count = 0;
        if (!ReadFile(reader_, buffer, (std::min)(available, DWORD{sizeof(buffer)}), &count, nullptr))
            fail("Cannot read tool output");
        output.append(buffer, count);
        if (output.size() > 131072) output.erase(0, output.size() - 131072);
    }
    const auto wait = WaitForSingleObject(process_, 0);
    if (wait == WAIT_FAILED) fail("Cannot monitor tool");
    if (wait != WAIT_OBJECT_0) return false;
    // Drain remaining bytes on subsequent ticks before announcing completion.
    DWORD remaining = 0;
    if (reader_ && PeekNamedPipe(reader_, nullptr, 0, nullptr, &remaining, nullptr) && remaining) return false;
    if (!GetExitCodeProcess(process_, &exit_code)) fail("Cannot read tool exit code");
    CloseHandle(process_); process_ = nullptr;
    if (reader_) CloseHandle(reader_);
    reader_ = nullptr;
    return true;
}
}
