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
    filter_settings filter{10, 3, 6, 60};
    // Missing per-axis fields inherit legacy filter thresholds when loading.
    int x_start_counts = 3, x_reverse_counts = 6;
    int y_start_counts = 2, y_reverse_counts = 4;
    sensitivity_settings x_curve;
    sensitivity_settings y_curve{false, 1400, {{0, 0}, {0.32173913043478258, 0.080645161290322578}, {1, 1}}};
    double x_smoothing_factor = 1.0, y_smoothing_factor = 0.85;
    filter_settings axis_filter(bool y) const {
        auto result = filter;
        result.start_counts = y ? y_start_counts : x_start_counts;
        result.reverse_counts = y ? y_reverse_counts : x_reverse_counts;
        return result;
    }
    bool x_pulse_enabled = false;
    bool x_keyboard_override_enabled = true;
    bool y_pulse_enabled = true;
    double x_hold_ratio = 1.0;
    double y_hold_ratio = 0.45;
    int pulse_period_ms = 10;
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
std::filesystem::path executable_directory();
std::filesystem::path default_config_path();

} // namespace mouse_mapping
