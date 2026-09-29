#include "deployment.hpp"
#include "runtime.hpp"

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <objbase.h>
#include <array>
#include <format>
#include <iostream>
#include <system_error>
#include <vector>
#include <fstream>

namespace mouse_mapping {
namespace {

[[noreturn]] void fail_windows(const char* message) {
    throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), message);
}

class handle_owner {
public:
    explicit handle_owner(HANDLE handle) : handle_(handle) {}
    ~handle_owner() { if (handle_ && handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    handle_owner(const handle_owner&) = delete;
    handle_owner& operator=(const handle_owner&) = delete;
    HANDLE get() const { return handle_; }
private:
    HANDLE handle_;
};

bool elevated() {
    HANDLE raw_token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &raw_token)) fail_windows("Cannot inspect administrator token");
    handle_owner token(raw_token);
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    if (!GetTokenInformation(token.get(), TokenElevation, &elevation, sizeof(elevation), &bytes))
        fail_windows("Cannot inspect administrator token");
    return elevation.TokenIsElevated != 0;
}

DWORD wait_for_process(HANDLE process) {
    if (WaitForSingleObject(process, INFINITE) != WAIT_OBJECT_0) fail_windows("Cannot wait for driver setup");
    DWORD code = 0;
    if (!GetExitCodeProcess(process, &code)) fail_windows("Cannot read driver setup result");
    return code;
}

bool has_filter(const wchar_t* class_key, const wchar_t* name) {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, class_key, L"UpperFilters", RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS)
        return false;
    std::vector<wchar_t> values(bytes / sizeof(wchar_t) + 2, L'\0');
    if (RegGetValueW(HKEY_LOCAL_MACHINE, class_key, L"UpperFilters", RRF_RT_REG_MULTI_SZ, nullptr, values.data(), &bytes) != ERROR_SUCCESS)
        return false;
    for (const auto* value = values.data(); *value; value += std::wcslen(value) + 1)
        if (_wcsicmp(value, name) == 0) return true;
    return false;
}

bool has_service(const wchar_t* path) {
    DWORD type = 0;
    DWORD bytes = sizeof(type);
    return RegGetValueW(HKEY_LOCAL_MACHINE, path, L"Type", RRF_RT_REG_DWORD, nullptr, &type, &bytes) == ERROR_SUCCESS
        && type == SERVICE_KERNEL_DRIVER;
}

} // namespace

embedded_file::embedded_file(embedded_asset asset, bool system_temporary) {
    const auto resource = FindResourceW(nullptr, MAKEINTRESOURCEW(static_cast<int>(asset)), RT_RCDATA);
    if (!resource) fail_windows("Embedded Interception resource is missing; rebuild the executable");
    const auto size = SizeofResource(nullptr, resource);
    const auto loaded = LoadResource(nullptr, resource);
    const auto data = loaded ? LockResource(loaded) : nullptr;
    if (!data || size == 0) throw std::runtime_error("Cannot read embedded Interception resource");
    std::array<wchar_t, 32768> buffer{};
    const auto length = system_temporary
        ? GetWindowsDirectoryW(buffer.data(), static_cast<UINT>(buffer.size()))
        : GetTempPathW(static_cast<DWORD>(buffer.size()), buffer.data());
    if (length == 0 || length >= buffer.size()) fail_windows("Cannot find temporary directory");
    std::filesystem::path root(buffer.data());
    if (system_temporary) root /= L"Temp";
    GUID id{};
    if (FAILED(CoCreateGuid(&id))) throw std::runtime_error("Cannot create resource directory identifier");
    std::array<wchar_t, 40> id_text{};
    StringFromGUID2(id, id_text.data(), static_cast<int>(id_text.size()));
    const auto directory = root / (std::wstring(L"mouse_input_mapping_") + id_text.data());
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
            L"D:P(A;OICI;FA;;;SY)(A;OICI;FA;;;BA)(A;OICI;FA;;;OW)", SDDL_REVISION_1, &descriptor, nullptr))
        fail_windows("Cannot create resource directory permissions");
    SECURITY_ATTRIBUTES attributes{sizeof(attributes), descriptor, FALSE};
    const auto created = CreateDirectoryW(directory.c_str(), &attributes);
    const auto create_error = GetLastError();
    LocalFree(descriptor);
    if (!created) throw std::system_error(static_cast<int>(create_error), std::system_category(), "Cannot create private resource directory");
    path_ = directory / (asset == embedded_asset::library ? L"interception.dll" : L"install-interception.exe");
    try {
        handle_owner output(CreateFileW(path_.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY, nullptr));
        if (output.get() == INVALID_HANDLE_VALUE) fail_windows("Cannot extract Interception resource");
        DWORD written = 0;
        if (!WriteFile(output.get(), data, size, &written, nullptr) || written != size)
            fail_windows("Cannot write Interception resource");
    } catch (...) {
        DeleteFileW(path_.c_str());
        RemoveDirectoryW(directory.c_str());
        throw;
    }
}

embedded_file::~embedded_file() {
    DeleteFileW(path_.c_str());
    RemoveDirectoryW(path_.parent_path().c_str());
}

bool driver_registered() {
    return has_service(L"SYSTEM\\CurrentControlSet\\Services\\keyboard")
        && has_service(L"SYSTEM\\CurrentControlSet\\Services\\mouse")
        && has_filter(L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e96b-e325-11ce-bfc1-08002be10318}", L"keyboard")
        && has_filter(L"SYSTEM\\CurrentControlSet\\Control\\Class\\{4d36e96f-e325-11ce-bfc1-08002be10318}", L"mouse");
}

unsigned long install_driver() {
    if (!elevated()) {
        std::wstring executable(32768, L'\0');
        const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        if (!length || length >= executable.size()) fail_windows("Cannot locate setup executable");
        executable.resize(length);
        const auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW execution{};
        execution.cbSize = sizeof(execution);
        execution.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        execution.hwnd = GetConsoleWindow();
        execution.lpVerb = L"runas";
        execution.lpFile = executable.c_str();
        execution.lpParameters = L"--install-driver";
        execution.nShow = SW_HIDE;
        const bool started = ShellExecuteExW(&execution) != FALSE;
        const auto error = GetLastError();
        if (SUCCEEDED(initialized)) CoUninitialize();
        if (!started) {
            if (error == ERROR_CANCELLED) return ERROR_CANCELLED;
            throw std::system_error(static_cast<int>(error), std::system_category(), "Cannot start administrator driver setup");
        }
        handle_owner process(execution.hProcess);
        return wait_for_process(process.get());
    }
    // Extract only the executable's own installer after elevation. Do not run an
    // arbitrary installer from the working directory with administrator rights.
    embedded_file installer(embedded_asset::installer, true);
    auto command = L"\"" + installer.path().wstring() + L"\" /install";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(installer.path().c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, installer.path().parent_path().c_str(), &startup, &process))
        fail_windows("Cannot start embedded driver installer");
    handle_owner process_handle(process.hProcess);
    handle_owner thread_handle(process.hThread);
    const auto result = wait_for_process(process_handle.get());
    if (result != ERROR_SUCCESS && result != ERROR_SUCCESS_REBOOT_REQUIRED) return result;
    if (!driver_registered()) throw std::runtime_error("Installer finished, but the mouse/keyboard drivers were not registered");
    return ERROR_SUCCESS_REBOOT_REQUIRED;
}

std::wstring startup_error_text(const char* message) {
    const auto size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, message, -1, nullptr, 0);
    const auto page = size ? CP_UTF8 : CP_ACP;
    const auto count = MultiByteToWideChar(page, 0, message, -1, nullptr, 0);
    if (!count) return L"无法读取详细错误。";
    std::wstring result(count, L'\0');
    MultiByteToWideChar(page, 0, message, -1, result.data(), count);
    result.pop_back();
    return result;
}

void report_startup(const std::wstring& message, bool interactive) {
    auto display = message;
    try {
        std::array<wchar_t, 32768> root{};
        const auto count = GetEnvironmentVariableW(L"LOCALAPPDATA", root.data(), static_cast<DWORD>(root.size()));
        const auto directory = count && count < root.size()
            ? std::filesystem::path(root.data()) / L"MouseInputMapping"
            : std::filesystem::temp_directory_path() / L"MouseInputMapping";
        std::filesystem::create_directories(directory);
        const auto log = directory / L"kernel-startup.log";
        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), nullptr, 0, nullptr, nullptr);
        std::string encoded(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, message.data(), static_cast<int>(message.size()), encoded.data(), bytes, nullptr, nullptr);
        std::ofstream output(log, std::ios::app | std::ios::binary);
        SYSTEMTIME time{};
        GetLocalTime(&time);
        output << std::format("\n[{:04}-{:02}-{:02} {:02}:{:02}:{:02}]\n", time.wYear, time.wMonth, time.wDay,
            time.wHour, time.wMinute, time.wSecond) << encoded << '\n';
        output.flush();
        if (!output) throw std::runtime_error("log write failed");
        display += L"\n\n诊断日志：" + log.wstring();
    } catch (...) {
        display += L"\n\n诊断日志写入失败，请保留此提示中的错误信息。";
    }
    // WriteConsole preserves Chinese in a console; redirected output is UTF-8.
    DWORD mode = 0, written = 0;
    const auto output = GetStdHandle(STD_ERROR_HANDLE);
    display += L"\n";
    if (GetConsoleMode(output, &mode))
        WriteConsoleW(output, display.data(), static_cast<DWORD>(display.size()), &written, nullptr);
    else {
        const auto bytes = WideCharToMultiByte(CP_UTF8, 0, display.data(), static_cast<int>(display.size()), nullptr, 0, nullptr, nullptr);
        std::string encoded(bytes, '\0');
        WideCharToMultiByte(CP_UTF8, 0, display.data(), static_cast<int>(display.size()), encoded.data(), bytes, nullptr, nullptr);
        std::cerr << encoded << std::flush;
    }
    if (interactive) MessageBoxW(GetConsoleWindow(), display.c_str(), L"鼠标映射 · 内核启动诊断", MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
}

std::wstring driver_diagnosis(unsigned long* device_error) {
    if (device_error) *device_error = ERROR_SUCCESS;
    // Probe without installing filters. The upstream DLL loses GetLastError while
    // cleaning up a failed context, so obtain the device error independently.
    for (int index = 0; index < 20; ++index) {
        const auto name = std::format(L"\\\\.\\interception{:02}", index);
        handle_owner device(CreateFileW(name.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr));
        if (device.get() != INVALID_HANDLE_VALUE) continue;
        const auto error = GetLastError();
        if (device_error) *device_error = error;
        auto detail = std::format(L"设备 {} 打开失败（Windows 错误 {}）。\n", name, error);
        if (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED)
            return detail + L"设备可能被其他映射工具占用，或访问受到限制。请先关闭其他实例及使用 Interception 的软件，再重试；必要时以管理员身份运行。此情况不应反复安装驱动。";
        if (!driver_registered())
            return detail + L"Interception 驱动未安装或注册不完整。可使用自动修复重新安装内置驱动。";
        std::array<wchar_t, MAX_PATH> system{};
        if (!GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()))) fail_windows("Cannot locate system drivers");
        if (!std::filesystem::exists(std::filesystem::path(system.data()) / L"drivers/keyboard.sys")
            || !std::filesystem::exists(std::filesystem::path(system.data()) / L"drivers/mouse.sys"))
            return detail + L"驱动注册记录存在，但驱动文件缺失。可使用自动修复恢复内置驱动。";
        return detail + L"驱动注册记录和文件存在，但设备尚不可用。若刚安装，请先重启 Windows（不是关闭后再开机）。若重启后仍失败，可尝试自动修复，并检查事件查看器中的 CodeIntegrity 日志。安全策略阻止加载时，重装未必有效；可改用同目录的 mouse_input_mapping_user.exe。";
    }
    return L"驱动设备可以打开，但驱动会话初始化失败。请关闭其他映射工具后重试；仍失败时可尝试自动修复。";
}

unsigned long ensure_driver(bool interactive) {
    if (driver_available()) return ERROR_SUCCESS;
    unsigned long device_error = 0;
    const auto diagnosis = driver_diagnosis(&device_error);
    if (device_error == ERROR_SHARING_VIOLATION || device_error == ERROR_ACCESS_DENIED) {
        report_startup(L"内核映射尚未启动。\n\n" + diagnosis, interactive);
        return device_error;
    }
    report_startup(L"内核映射尚未启动。\n\n" + diagnosis, false);
    if (!interactive) {
        report_startup(L"请运行 mouse_input_mapping_kernel.exe --install-driver 修复，完成后重启 Windows。", false);
        return ERROR_NOT_READY;
    }
    const auto prompt = diagnosis + L"\n\n是否现在自动修复？\n将请求管理员权限并运行内置驱动安装器，保留键位配置。完成后需要手动重启 Windows；程序不会自动重启或更改系统安全设置。\n\n选择“否”退出，可在准备好后重新运行。";
    if (MessageBoxW(GetConsoleWindow(), prompt.c_str(), L"内核映射不可用 · 自动修复",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND) != IDYES)
        return ERROR_NOT_READY;
    const auto result = install_driver();
    if (result == ERROR_CANCELLED) {
        report_startup(L"已取消管理员授权，驱动未修复，内核映射未启动。重新运行程序即可再次修复。", true);
        return result;
    }
    if (result != ERROR_SUCCESS && result != ERROR_SUCCESS_REBOOT_REQUIRED)
        throw std::runtime_error(std::format("Driver installation failed (exit {}). Retry --install-driver from an administrator terminal.", result));
    report_startup(L"驱动修复已完成，键位配置已保留。\n请保存其他工作并手动重启 Windows，然后重新运行本程序。\n当前内核映射尚未启动。", true);
    return ERROR_SUCCESS_REBOOT_REQUIRED;
}

} // namespace mouse_mapping
