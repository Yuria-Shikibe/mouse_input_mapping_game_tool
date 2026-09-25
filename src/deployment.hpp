#pragma once

#include <filesystem>

namespace mouse_mapping {

enum class embedded_asset { library = 101, installer = 102 };

// Owns one extracted resource in a newly created, private temporary directory.
class embedded_file {
public:
    explicit embedded_file(embedded_asset asset, bool system_temporary = false);
    ~embedded_file();
    embedded_file(const embedded_file&) = delete;
    embedded_file& operator=(const embedded_file&) = delete;
    const std::filesystem::path& path() const { return path_; }
private:
    std::filesystem::path path_;
};

bool driver_registered();
bool ensure_driver();
unsigned long install_driver();

} // namespace mouse_mapping
