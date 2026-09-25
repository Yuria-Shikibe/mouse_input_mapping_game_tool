#include "deployment.hpp"
#include "runtime.hpp"

#include <windows.h>
#include <sddl.h>
#include <shellapi.h>
#include <objbase.h>
#include <array>
#include <iostream>
#include <system_error>
#include <vector>

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

bool ensure_driver() {
    if (driver_available()) return true;
    if (driver_registered()) {
        std::cout << "Driver is installed but not active. Restart Windows, then run this program again.\n"
                     "If you already restarted, use --check: Windows may have blocked the driver.\n";
        return false;
    }
    std::cout << "First-run setup: installing the bundled input driver.\n"
                 "Windows will request administrator permission. One restart is required.\n"
                 "Your key configuration is saved; no separate download or install command is needed.\n" << std::flush;
    const auto result = install_driver();
    if (result == ERROR_CANCELLED) {
        std::cout << "Setup canceled. Run this program again when ready to allow driver installation.\n";
        return false;
    }
    if (result != ERROR_SUCCESS && result != ERROR_SUCCESS_REBOOT_REQUIRED)
        throw std::runtime_error("Driver installation failed (exit " + std::to_string(result)
            + "). Run --install-driver from an Administrator terminal to retry. Windows has not been restarted.");
    std::cout << "Setup completed. Restart Windows once, then run this EXE again.\n"
                 "Configuration was preserved. This program will not restart Windows automatically.\n";
    return false;
}

} // namespace mouse_mapping
