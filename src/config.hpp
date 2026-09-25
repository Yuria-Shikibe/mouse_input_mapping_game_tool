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
    key_code up_key = 0x1f;   // Y-: S
    key_code down_key = 0x11; // Y+: W
    bool map_y = false;
    // LMB, RMB, CMB (middle), X1, X2. Used only by the user target.
    std::array<key_code, 5> mouse_keys{0x4a, 0x32, 0x14, 0x2a, 0x21}; // SUBTRACT M T LSHIFT F
    std::array<key_code, 2> wheel_keys{0x13, 0x13}; // Up/down R
    filter_settings filter;
    bool x_pulse_enabled = false;
    bool y_pulse_enabled = true;
    double x_hold_ratio = 1.0;
    double y_hold_ratio = 0.65;
    int pulse_period_ms = 30;
};

inline constexpr std::array<const char*, 5> mouse_key_fields{
    "lmb_key", "rmb_key", "cmb_key", "x1_key", "x2_key"};
inline constexpr std::array<const char*, 2> wheel_key_fields{"wheel_up_key", "wheel_down_key"};

key_code parse_key(std::string_view text);
std::string format_key(key_code code);
std::string key_name(key_code code);
std::string describe_key(key_code code);
void validate(const configuration& config);
configuration read_config(std::istream& input, bool map_y = false, bool require_bindings = false);
configuration load_config(const std::filesystem::path& path, bool map_y = false, bool require_bindings = false);
void write_config(std::ostream& output, const configuration& config);
void save_config(const std::filesystem::path& path, const configuration& config);
configuration configure(configuration defaults, bool text_mode = false);
std::filesystem::path executable_directory();

} // namespace mouse_mapping
