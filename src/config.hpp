#pragma once

#include "mapping.hpp"
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

namespace mouse_mapping {

struct configuration {
    key_code left_key = 0x1e;
    key_code right_key = 0x20;
    key_code toggle_key = 0x42;
    filter_settings filter;
};

key_code parse_key(std::string_view text);
std::string format_key(key_code code);
std::string key_name(key_code code);
std::string describe_key(key_code code);
void validate(const configuration& config);
configuration read_config(std::istream& input);
configuration load_config(const std::filesystem::path& path);
void write_config(std::ostream& output, const configuration& config);
void save_config(const std::filesystem::path& path, const configuration& config);
configuration configure(configuration defaults, bool text_mode = false);
std::filesystem::path executable_directory();

} // namespace mouse_mapping
