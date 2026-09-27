#pragma once
#include <windows.h>
#include <filesystem>
#include <string>
#include <vector>

namespace mouse_mapping {
// Non-owning lifetime policy: closing these handles never terminates a child.
class gui_process {
public:
    gui_process() = default;
    ~gui_process();
    gui_process(const gui_process&) = delete;
    gui_process& operator=(const gui_process&) = delete;
    void start(const std::filesystem::path& executable,
        const std::vector<std::wstring>& arguments, bool capture);
    bool poll(std::string& output, DWORD& exit_code);
    bool active() const { return process_ != nullptr; }
private:
    HANDLE process_ = nullptr;
    HANDLE reader_ = nullptr;
};
}
