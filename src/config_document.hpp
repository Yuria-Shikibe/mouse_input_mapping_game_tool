#pragma once
#include "config.hpp"
#include <map>
#include <span>
#include <vector>

namespace mouse_mapping {
enum class field_kind { key, number, toggle, curve };
struct config_field {
    const char* name;
    const wchar_t* label;
    field_kind kind;
    bool user_only;
    const wchar_t* tooltip = nullptr;
};
std::span<const config_field> config_fields();

// A draft can contain invalid text. Only validated snapshots reach disk/runtime.
class config_document {
public:
    bool user_mode = true;
    bool dirty = false;
    std::filesystem::path path;
    std::map<std::string, std::string> values;
    void reset(bool user, const std::filesystem::path& file);
    void open(bool user, const std::filesystem::path& file);
    void defaults();
    void set(const std::string& name, const std::string& value);
    configuration snapshot() const;
    void save(const std::filesystem::path& file);
private:
    void assign(const configuration& config);
};

std::wstring quote_windows_argument(const std::wstring& value);
std::wstring windows_command_line(const std::vector<std::wstring>& arguments);
std::wstring runtime_command(const std::filesystem::path& directory,
    const config_document& document, bool steam);
std::filesystem::path runtime_executable(const std::filesystem::path& directory, bool user);
}
