#pragma once

#include "mapping.hpp"
#include <filesystem>
#include <iosfwd>
#include <string>
#include <string_view>

namespace mouse_mapping {

using std::literals::operator""sv;

enum class input_kind { none, keyboard, mouse };
struct input_binding {
    input_kind kind = input_kind::none;
    key_code code = 0; // Keyboard scan code, or mouse button index 0..4.
    bool operator==(const input_binding&) const noexcept = default;
};
struct y_settings {
    key_code up_key = 0x1f, down_key = 0x11;
    int start_counts = 2, reverse_counts = 4;
    double smoothing_factor = 0.85, hold_ratio = 0.45;
    bool pulse_enabled = true;
    sensitivity_settings curve{false, 1400, {{0, 0}, {0.32173913043478258, 0.080645161290322578}, {1, 1}}};
};
struct pie_settings {
    input_binding trigger{};
    bool visual_enabled = true;
    int deadzone_counts = 12, radius_counts = 80, key_hold_ms = 20;
    double hysteresis_degrees = 8;
};
inline constexpr std::array<key_code, 4> pie_arrow_keys{0xe048, 0xe050, 0xe04b, 0xe04d};
struct chord_settings {
    bool enabled = false;
    key_code trigger = 0x2a; // LSHIFT
    key_code first = 0x39, second = 0x2e; // SPACE + C
};
struct configuration {
    chord_settings chord;
    pie_settings pie;
    key_code left_key = 0x1e;
    key_code right_key = 0x20;
    key_code toggle_key = 0x42;
    key_code up_key = 0x1f;   // Y-: S
    key_code down_key = 0x11; // Y+: W
    bool map_y = false;
    bool kernel_y_enabled = false, kernel_y_block = false;
    y_settings kernel_y;
    std::array<input_binding, 2> bypass_keys{};
    // LMB, RMB, CMB (middle), X1, X2. Used only by the user target.
    std::array<key_code, 5> mouse_keys{0x4a, 0x32, 0x14, 0x2a, 0x21}; // SUBTRACT M T LSHIFT F
    std::array<key_code, 2> wheel_keys{0x13, 0x13}; // Up/down R
    int window_ms = 10, release_ms = 60;
    int x_start_counts = 3, x_reverse_counts = 6;
    int y_start_counts = 2, y_reverse_counts = 4;
    sensitivity_settings x_curve;
    sensitivity_settings y_curve{false, 1400, {{0, 0}, {0.32173913043478258, 0.080645161290322578}, {1, 1}}};
    double x_smoothing_factor = 1.0, y_smoothing_factor = 0.85;
    filter_settings axis_filter(bool y) const {
        return {window_ms, y ? y_start_counts : x_start_counts,
            y ? y_reverse_counts : x_reverse_counts, release_ms};
    }
    bool x_pulse_enabled = false;
    bool x_keyboard_override_enabled = true;
    bool y_pulse_enabled = true;
    double x_hold_ratio = 1.0;
    double y_hold_ratio = 0.45;
    int pulse_period_ms = 10;
};

// map_y identifies the user backend; effective parameters do not change that identity.
inline bool y_enabled(const configuration& config) { return config.map_y || config.kernel_y_enabled; }
inline configuration effective_config(configuration config) {
    if (!config.map_y) {
        const auto& y = config.kernel_y;
        config.up_key = y.up_key; config.down_key = y.down_key;
        config.y_start_counts = y.start_counts; config.y_reverse_counts = y.reverse_counts;
        config.y_smoothing_factor = y.smoothing_factor; config.y_hold_ratio = y.hold_ratio;
        config.y_pulse_enabled = y.pulse_enabled; config.y_curve = y.curve;
    }
    return config;
}

inline constexpr std::array mouse_key_fields{"lmb_key"sv, "rmb_key"sv, "cmb_key"sv, "x1_key"sv, "x2_key"sv};
inline constexpr std::array wheel_key_fields{"wheel_up_key"sv, "wheel_down_key"sv};

key_code parse_key(std::string_view text);
input_binding parse_input_binding(std::string_view text);
std::string format_input_binding(input_binding binding);
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
