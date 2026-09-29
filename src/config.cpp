#include "config.hpp"
#include "config_detail.hpp"

#include <windows.h>
#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cctype>
#include <format>
#include <fstream>
#include <set>
#include <system_error>

namespace mouse_mapping {
namespace config_detail {

constexpr std::pair<std::string_view, unsigned int> named_keys[] = {
    {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"UP", VK_UP}, {"DOWN", VK_DOWN},
    {"SPACE", VK_SPACE}, {"ENTER", VK_RETURN}, {"TAB", VK_TAB}, {"ESC", VK_ESCAPE},
    {"BACKSPACE", VK_BACK}, {"INSERT", VK_INSERT}, {"DELETE", VK_DELETE},
    {"HOME", VK_HOME}, {"END", VK_END}, {"PAGE_UP", VK_PRIOR}, {"PAGE_DOWN", VK_NEXT},
    {"LSHIFT", VK_LSHIFT}, {"RSHIFT", VK_RSHIFT}, {"LCTRL", VK_LCONTROL},
    {"RCTRL", VK_RCONTROL}, {"LALT", VK_LMENU}, {"RALT", VK_RMENU},
    {"LWIN", VK_LWIN}, {"RWIN", VK_RWIN}, {"APPS", VK_APPS},
    {"CAPS_LOCK", VK_CAPITAL}, {"NUM_LOCK", VK_NUMLOCK}, {"SCROLL_LOCK", VK_SCROLL},
    {"NUM0", VK_NUMPAD0}, {"NUM1", VK_NUMPAD1}, {"NUM2", VK_NUMPAD2},
    {"NUM3", VK_NUMPAD3}, {"NUM4", VK_NUMPAD4}, {"NUM5", VK_NUMPAD5},
    {"NUM6", VK_NUMPAD6}, {"NUM7", VK_NUMPAD7}, {"NUM8", VK_NUMPAD8}, {"NUM9", VK_NUMPAD9},
    {"ADD", VK_ADD}, {"SUBTRACT", VK_SUBTRACT}, {"MULTIPLY", VK_MULTIPLY},
    {"DIVIDE", VK_DIVIDE}, {"DECIMAL", VK_DECIMAL}
};

unsigned int scan_code_for_vk(unsigned int vk) {
    auto code = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    // Some layouts omit E0 for the dedicated navigation cluster.
    switch (vk) {
    case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN:
    case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END:
    case VK_PRIOR: case VK_NEXT: case VK_RCONTROL: case VK_RMENU:
    case VK_DIVIDE: case VK_LWIN: case VK_RWIN: case VK_APPS:
        if (code != 0) code |= 0xe000;
        break;
    default: break;
    }
    return code;
}

std::string trim(std::string_view value) {
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    return std::string(value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1));
}

int parse_integer(std::string_view value) {
    int result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size())
        throw std::runtime_error("Expected an integer");
    return result;
}

double parse_number(std::string_view value) {
    double result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result))
        throw std::runtime_error("Expected a finite number");
    return result;
}

double parse_ratio(std::string_view value) {
    double result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || !std::isfinite(result)
        || result < 0 || result > 1) throw std::runtime_error("Expected a ratio from 0 to 1");
    return result;
}

bool parse_switch(std::string_view value) {
    if (value == "0") return false;
    if (value == "1") return true;
    throw std::runtime_error("Expected 0 or 1");
}

bool supported_key(key_code code) {
    const auto prefix = code & 0xff00;
    const auto scan = code & 0xff;
    if ((prefix != 0 && prefix != 0xe000) || scan == 0 || scan > 0x7f) return false;
    const auto vk = MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX);
    // These keys use multi-packet sequences, which are intentionally unsupported.
    return vk != 0 && vk != VK_PAUSE && vk != VK_SNAPSHOT && vk != VK_CANCEL;
}

} // namespace

using namespace config_detail;

key_code parse_key(std::string_view text) {
    auto value = trim(text);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    unsigned int code = 0;
    if (value.starts_with("0X")) {
        const auto [end, error] = std::from_chars(value.data() + 2, value.data() + value.size(), code, 16);
        if (error != std::errc{} || end != value.data() + value.size() || code > 0xffff)
            throw std::runtime_error("Invalid hexadecimal scan code");
    } else if (value == "NUM_ENTER") {
        code = 0xe01c;
    } else {
        unsigned int vk = 0;
        if (value.size() == 1 && ((value[0] >= 'A' && value[0] <= 'Z') || (value[0] >= '0' && value[0] <= '9')))
            vk = static_cast<unsigned char>(value[0]);
        else if (value.size() >= 2 && value[0] == 'F' && std::isdigit(static_cast<unsigned char>(value[1]))) {
            const auto number = parse_integer(std::string_view(value).substr(1));
            if (number >= 1 && number <= 24) vk = VK_F1 + number - 1;
        } else {
            for (const auto& [name, named_vk] : named_keys) if (value == name) vk = named_vk;
        }
        if (vk != 0) code = scan_code_for_vk(vk);
    }
    if (!supported_key(static_cast<key_code>(code)))
        throw std::runtime_error("Unsupported key; use A-Z, 0-9, F1-F24, LEFT/RIGHT, or a standard key name");
    return static_cast<key_code>(code);
}

std::string format_key(key_code code) {
    return std::format("0x{:04x}", code);
}

std::string key_name(key_code code) {
    for (unsigned int vk = 'A'; vk <= 'Z'; ++vk)
        if (scan_code_for_vk(vk) == code) return std::string(1, static_cast<char>(vk));
    for (unsigned int vk = '0'; vk <= '9'; ++vk)
        if (scan_code_for_vk(vk) == code) return std::string(1, static_cast<char>(vk));
    for (unsigned int vk = VK_F1; vk <= VK_F24; ++vk)
        if (scan_code_for_vk(vk) == code) return std::format("F{}", vk - VK_F1 + 1);
    for (const auto& [name, vk] : named_keys)
        if (scan_code_for_vk(vk) == code) return std::string(name);
    if (code == 0xe01c) return "NUM_ENTER";
    const auto vk = MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX);
    const auto character = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7fffffff;
    if (character >= 33 && character <= 126) return std::string(1, static_cast<char>(character));
    return "SCAN_CODE";
}

std::string describe_key(key_code code) { return std::format("{} ({})", key_name(code), format_key(code)); }

static constexpr std::array mouse_binding_names{"MOUSE_LEFT", "MOUSE_RIGHT", "MOUSE_MIDDLE", "MOUSE_X1", "MOUSE_X2"};
static constexpr std::array mouse_binding_aliases{"LMB", "RMB", "CMB", "X1", "X2"};
input_binding parse_input_binding(std::string_view text) {
    auto value = trim(text);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (value == "NONE") return {};
    for (std::size_t i = 0; i < mouse_binding_names.size(); ++i)
        if (value == mouse_binding_names[i] || value == mouse_binding_aliases[i])
            return {input_kind::mouse, static_cast<key_code>(i)};
    return {input_kind::keyboard, parse_key(value)};
}
std::string format_input_binding(input_binding binding) {
    if (binding.kind == input_kind::none) return "NONE";
    if (binding.kind == input_kind::mouse && binding.code < mouse_binding_names.size()) return mouse_binding_names[binding.code];
    if (binding.kind == input_kind::keyboard) return format_key(binding.code);
    throw std::runtime_error("Invalid input binding");
}

void validate(const configuration& config) {
    const auto& chord = config.chord;
    for (const auto key : {chord.trigger, chord.first, chord.second})
        if (!supported_key(key)) throw std::runtime_error("Invalid chord scan code");
    if (chord.enabled) {
        if (chord.trigger == chord.first || chord.trigger == chord.second || chord.first == chord.second)
            throw std::runtime_error("Chord trigger and output keys must be different");
        for (const auto key : {chord.trigger, chord.first, chord.second}) {
            if (key == config.toggle_key) throw std::runtime_error("Chord keys conflict with toggle_key");
            for (const auto binding : config.bypass_keys)
                if (binding == input_binding{input_kind::keyboard, key})
                    throw std::runtime_error("Chord keys conflict with bypass keys");
        }
        if (chord.trigger == config.left_key || chord.trigger == config.right_key ||
            (y_enabled(config) && (chord.trigger == effective_config(config).up_key || chord.trigger == effective_config(config).down_key)))
            throw std::runtime_error("Chord trigger conflicts with an axis key");
        if (config.pie.trigger.kind != input_kind::none &&
            std::find(pie_arrow_keys.begin(), pie_arrow_keys.end(), chord.trigger) != pie_arrow_keys.end())
            throw std::runtime_error("Chord trigger conflicts with Pie arrow keys");
    }
    const auto& pie = config.pie;
    if ((pie.trigger.kind == input_kind::mouse && pie.trigger.code > 4) ||
        (pie.trigger.kind == input_kind::keyboard && !supported_key(pie.trigger.code)))
        throw std::runtime_error("pie_trigger must be NONE, a supported keyboard key or a mouse button");
    if (pie.deadzone_counts < 1 || pie.deadzone_counts > 10000 ||
        pie.radius_counts <= pie.deadzone_counts || pie.radius_counts > 100000 ||
        !std::isfinite(pie.hysteresis_degrees) || pie.hysteresis_degrees < 0 || pie.hysteresis_degrees > 20 ||
        pie.key_hold_ms < 1 || pie.key_hold_ms > 200)
        throw std::runtime_error("Invalid Pie deadzone, radius, hysteresis or key hold duration");
    if (pie.trigger.kind != input_kind::none) {
        for (const auto binding : config.bypass_keys)
            if (binding == pie.trigger) throw std::runtime_error("Pie trigger conflicts with a bypass key");
        const auto reserved = [](key_code key) {
            return std::find(pie_arrow_keys.begin(), pie_arrow_keys.end(), key) != pie_arrow_keys.end();
        };
        if (pie.trigger.kind == input_kind::keyboard) {
            const auto key = pie.trigger.code;
            const auto effective = effective_config(config);
            bool trigger_conflict = reserved(key) || key == config.toggle_key ||
                key == config.left_key || key == config.right_key ||
                (y_enabled(config) && (key == effective.up_key || key == effective.down_key));
            if (config.map_y) {
                for (const auto mapped : config.mouse_keys) trigger_conflict = trigger_conflict || mapped == key;
                for (const auto mapped : config.wheel_keys) trigger_conflict = trigger_conflict || mapped == key;
            }
            if (chord.enabled) trigger_conflict = trigger_conflict || key == chord.trigger || key == chord.first || key == chord.second;
            if (trigger_conflict) throw std::runtime_error("Pie keyboard trigger conflicts with toggle, mapped output, chord or arrow keys");
        }
        bool conflict = reserved(config.toggle_key) || reserved(config.left_key) || reserved(config.right_key);
        if (config.map_y) {
            conflict = conflict || reserved(config.up_key) || reserved(config.down_key);
            for (const auto key : config.mouse_keys) conflict = conflict || reserved(key);
            for (const auto key : config.wheel_keys) conflict = conflict || reserved(key);
        } else if (config.kernel_y_enabled) {
            conflict = conflict || reserved(config.kernel_y.up_key) || reserved(config.kernel_y.down_key);
        }
        // Bypass keys must remain genuine physical events, never routed Pie outputs.
        for (const auto binding : config.bypass_keys)
            conflict = conflict || (binding.kind == input_kind::keyboard && reserved(binding.code));
        if (conflict) throw std::runtime_error("Pie reserves UP/DOWN/LEFT/RIGHT; remove conflicting mappings, toggle or bypass keys");
    }
    for (const auto binding : config.bypass_keys) {
        if ((binding.kind == input_kind::keyboard && (!supported_key(binding.code) || binding.code == config.toggle_key))
            || (binding.kind == input_kind::mouse && binding.code > 4))
            throw std::runtime_error("Invalid bypass key or conflict with toggle_key");
    }
    if (config.bypass_keys[0].kind != input_kind::none && config.bypass_keys[0] == config.bypass_keys[1])
        throw std::runtime_error("Bypass keys must be different");
    const auto& ky = config.kernel_y;
    if (!supported_key(ky.up_key) || !supported_key(ky.down_key)) throw std::runtime_error("Invalid kernel Y keys");
    if (!config.map_y && config.kernel_y_enabled) {
        const std::set<key_code> keys{config.left_key, config.right_key, config.toggle_key, ky.up_key, ky.down_key};
        if (keys.size() != 5) throw std::runtime_error("Kernel direction and toggle keys must be different");
    }
    if (ky.start_counts < 1 || ky.reverse_counts < ky.start_counts || ky.reverse_counts > 10000
        || !std::isfinite(ky.smoothing_factor) || ky.smoothing_factor < 0 || ky.smoothing_factor > 1
        || !std::isfinite(ky.hold_ratio) || ky.hold_ratio < 0 || ky.hold_ratio > 1
        || !std::isfinite(ky.curve.full_speed) || ky.curve.full_speed < 1 || ky.curve.full_speed > 1000000)
        throw std::runtime_error("Invalid kernel Y parameters");
    validate_curve(ky.curve.points);
    for (const auto& [name, code] : {std::pair{"left_key", config.left_key},
            {"right_key", config.right_key}, {"toggle_key", config.toggle_key}})
        if (!supported_key(code)) throw std::runtime_error(std::format("{}: unsupported scan code", name));
    if (config.left_key == config.right_key || config.left_key == config.toggle_key || config.right_key == config.toggle_key)
        throw std::runtime_error("left_key, right_key and toggle_key must be different");
    if (config.map_y) {
        if (!supported_key(config.up_key) || !supported_key(config.down_key))
            throw std::runtime_error("up_key/down_key: unsupported scan code");
        std::set<key_code> keys{config.left_key, config.right_key, config.up_key, config.down_key, config.toggle_key};
        for (const auto code : config.mouse_keys) {
            if (!supported_key(code)) throw std::runtime_error("Mouse button binding: unsupported scan code");
            keys.insert(code);
        }
        if (keys.size() != 10) throw std::runtime_error("Direction, mouse button and toggle keys must be different");
        for (const auto code : config.wheel_keys) {
            if (!supported_key(code)) throw std::runtime_error("Mouse wheel binding: unsupported scan code");
            if (keys.contains(code)) throw std::runtime_error("Wheel keys must differ from direction, mouse button and toggle keys");
        }
    }
    for (const bool y : {false, true}) {
        const auto smoothing = y ? config.y_smoothing_factor : config.x_smoothing_factor;
        if (!std::isfinite(smoothing) || smoothing < 0 || smoothing > 1)
            throw std::runtime_error(y ? "y_smoothing_factor must be 0..1" : "x_smoothing_factor must be 0..1");
        const auto axis = config.axis_filter(y);
        if (axis.start_counts < 1 || axis.start_counts > 10000
            || axis.reverse_counts < axis.start_counts || axis.reverse_counts > 10000)
            throw std::runtime_error(y ? "Invalid Y start/reverse thresholds" : "Invalid X start/reverse thresholds");
        const auto& curve = y ? config.y_curve : config.x_curve;
        if (!std::isfinite(curve.full_speed) || curve.full_speed < 1 || curve.full_speed > 1000000)
            throw std::runtime_error(y ? "y_curve_full_speed must be 1..1000000" : "x_curve_full_speed must be 1..1000000");
        validate_curve(curve.points);
    }
    const auto& filter = config.filter;
    if (filter.window_ms < 1 || filter.window_ms > 1000) throw std::runtime_error("window_ms must be 1..1000");
    if (filter.start_counts < 1 || filter.start_counts > 10000) throw std::runtime_error("start_counts must be 1..10000");
    if (filter.reverse_counts < filter.start_counts || filter.reverse_counts > 10000)
        throw std::runtime_error("reverse_counts must be start_counts..10000");
    if (filter.release_ms < filter.window_ms || filter.release_ms > 2000)
        throw std::runtime_error("release_ms must be window_ms..2000");
    if (!std::isfinite(config.x_hold_ratio) || config.x_hold_ratio < 0 || config.x_hold_ratio > 1
        || !std::isfinite(config.y_hold_ratio) || config.y_hold_ratio < 0 || config.y_hold_ratio > 1)
        throw std::runtime_error("hold ratios must be 0..1");
    if (config.pulse_period_ms < 2 || config.pulse_period_ms > 1000)
        throw std::runtime_error("pulse_period_ms must be 2..1000");
}

configuration read_config(std::istream& input, bool map_y, bool require_bindings) {
    configuration config;
    config.map_y = map_y;
    std::set<std::string> seen;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line_number == 1 && line.starts_with("\xef\xbb\xbf")) line.erase(0, 3);
        line = trim(std::string_view(line).substr(0, line.find_first_of(";#")));
        if (line.empty() || line == "[mapping]") continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            throw std::runtime_error(std::format("Config line {}: expected key=value", line_number));
        const auto name = trim(std::string_view(line).substr(0, separator));
        const auto value = trim(std::string_view(line).substr(separator + 1));
        try {
            if (!seen.insert(name).second) throw std::runtime_error("Duplicate field");
            if (name == "left_key") config.left_key = parse_key(value);
            else if (name == "right_key") config.right_key = parse_key(value);
            else if (name == "chord_enabled") config.chord.enabled = parse_switch(value);
            else if (name == "chord_trigger_key") config.chord.trigger = parse_key(value);
            else if (name == "chord_first_key") config.chord.first = parse_key(value);
            else if (name == "chord_second_key") config.chord.second = parse_key(value);
            else if (name == "toggle_key") config.toggle_key = parse_key(value);
            else if (name == "up_key") config.up_key = parse_key(value);
            else if (name == "down_key") config.down_key = parse_key(value);
            else if (name == "pie_trigger") config.pie.trigger = parse_input_binding(value);
            else if (name == "pie_visual_enabled") config.pie.visual_enabled = parse_switch(value);
            else if (name == "pie_deadzone_counts") config.pie.deadzone_counts = parse_integer(value);
            else if (name == "pie_radius_counts") config.pie.radius_counts = parse_integer(value);
            else if (name == "pie_hysteresis_degrees") config.pie.hysteresis_degrees = parse_number(value);
            else if (name == "pie_key_hold_ms") config.pie.key_hold_ms = parse_integer(value);
            else if (name == "bypass_key_1") config.bypass_keys[0] = parse_input_binding(value);
            else if (name == "bypass_key_2") config.bypass_keys[1] = parse_input_binding(value);
            else if (name == "kernel_y_enabled") config.kernel_y_enabled = parse_switch(value);
            else if (name == "kernel_y_block") config.kernel_y_block = parse_switch(value);
            else if (name == "kernel_y_up_key") config.kernel_y.up_key = parse_key(value);
            else if (name == "kernel_y_down_key") config.kernel_y.down_key = parse_key(value);
            else if (name == "kernel_y_start_counts") config.kernel_y.start_counts = parse_integer(value);
            else if (name == "kernel_y_reverse_counts") config.kernel_y.reverse_counts = parse_integer(value);
            else if (name == "kernel_y_smoothing_factor") config.kernel_y.smoothing_factor = parse_ratio(value);
            else if (name == "kernel_y_hold_ratio") config.kernel_y.hold_ratio = parse_ratio(value);
            else if (name == "kernel_y_pulse_enabled") config.kernel_y.pulse_enabled = parse_switch(value);
            else if (name == "kernel_y_curve_enabled") config.kernel_y.curve.enabled = parse_switch(value);
            else if (name == "kernel_y_curve_full_speed") config.kernel_y.curve.full_speed = parse_number(value);
            else if (name == "kernel_y_curve_points") config.kernel_y.curve.points = parse_curve(value);
            else if (name == "window_ms") config.filter.window_ms = parse_integer(value);
            else if (name == "start_counts") config.filter.start_counts = parse_integer(value);
            else if (name == "reverse_counts") config.filter.reverse_counts = parse_integer(value);
            else if (name == "release_ms") config.filter.release_ms = parse_integer(value);
            else if (name == "x_pulse_enabled") config.x_pulse_enabled = parse_switch(value);
            else if (name == "x_keyboard_override_enabled") config.x_keyboard_override_enabled = parse_switch(value);
            else if (name == "y_pulse_enabled") config.y_pulse_enabled = parse_switch(value);
            else if (name == "x_hold_ratio") config.x_hold_ratio = parse_ratio(value);
            else if (name == "y_hold_ratio") config.y_hold_ratio = parse_ratio(value);
            else if (name == "x_smoothing_factor") config.x_smoothing_factor = parse_ratio(value);
            else if (name == "y_smoothing_factor") config.y_smoothing_factor = parse_ratio(value);
            else if (name == "x_start_counts") config.x_start_counts = parse_integer(value);
            else if (name == "x_reverse_counts") config.x_reverse_counts = parse_integer(value);
            else if (name == "x_curve_enabled") config.x_curve.enabled = parse_switch(value);
            else if (name == "x_curve_full_speed") config.x_curve.full_speed = parse_number(value);
            else if (name == "x_curve_points") config.x_curve.points = parse_curve(value);
            else if (name == "y_start_counts") config.y_start_counts = parse_integer(value);
            else if (name == "y_reverse_counts") config.y_reverse_counts = parse_integer(value);
            else if (name == "y_curve_enabled") config.y_curve.enabled = parse_switch(value);
            else if (name == "y_curve_full_speed") config.y_curve.full_speed = parse_number(value);
            else if (name == "y_curve_points") config.y_curve.points = parse_curve(value);
            else if (name == "pulse_period_ms") config.pulse_period_ms = parse_integer(value);
            else {
                bool found = false;
                for (std::size_t index = 0; index < mouse_key_fields.size(); ++index) {
                    if (name == mouse_key_fields[index]) {
                        config.mouse_keys[index] = parse_key(value);
                        found = true;
                        break;
                    }
                }
                for (std::size_t index = 0; index < wheel_key_fields.size(); ++index) {
                    if (name == wheel_key_fields[index]) {
                        config.wheel_keys[index] = parse_key(value);
                        found = true;
                        break;
                    }
                }
                if (!found) throw std::runtime_error("Unknown field");
            }
        } catch (const std::exception& error) {
            throw std::runtime_error(std::format("Config line {} ({}): {}", line_number, name, error.what()));
        }
    }
    if (input.bad()) throw std::runtime_error("Cannot read configuration");
    if (require_bindings) {
        for (const char* name : {"left_key", "right_key", "toggle_key"})
            if (!seen.contains(name)) throw std::runtime_error(std::format("Unbound key: {}; run --configure first", name));
        if (map_y) {
            for (const char* name : {"up_key", "down_key"})
                if (!seen.contains(name)) throw std::runtime_error(std::format("Unbound key: {}; run --configure first", name));
            for (const char* name : mouse_key_fields)
                if (!seen.contains(name)) throw std::runtime_error(std::format("Unbound key: {}; run --configure first", name));
            for (const char* name : wheel_key_fields)
                if (!seen.contains(name)) throw std::runtime_error(std::format("Unbound key: {}; run --configure first", name));
        }
    }
    if (!seen.contains("x_start_counts")) config.x_start_counts = config.filter.start_counts;
    if (!seen.contains("x_reverse_counts")) config.x_reverse_counts = config.filter.reverse_counts;
    if (!seen.contains("y_start_counts")) config.y_start_counts = config.filter.start_counts;
    if (!seen.contains("y_reverse_counts")) config.y_reverse_counts = config.filter.reverse_counts;
    validate(config);
    return config;
}

configuration load_config(const std::filesystem::path& path, bool map_y, bool require_bindings) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open configuration file");
    return read_config(input, map_y, require_bindings);
}

void write_config(std::ostream& output, const configuration& config) {
    validate(config);
    output << "; Keys are physical scan codes; 0xe0xx means an extended key. Key names are also accepted.\n"
        << "[mapping]\nleft_key=" << format_key(config.left_key) << " ; " << key_name(config.left_key)
        << "\nright_key=" << format_key(config.right_key) << " ; " << key_name(config.right_key);
    output << "\nchord_enabled=" << config.chord.enabled
        << "\nchord_trigger_key=" << format_key(config.chord.trigger)
        << "\nchord_first_key=" << format_key(config.chord.first)
        << "\nchord_second_key=" << format_key(config.chord.second);
    output << "\nup_key=" << format_key(config.up_key) << " ; Y-: " << key_name(config.up_key)
            << "\ndown_key=" << format_key(config.down_key) << " ; Y+: " << key_name(config.down_key);
    for (std::size_t index = 0; index < mouse_key_fields.size(); ++index)
        output << '\n' << mouse_key_fields[index] << '=' << format_key(config.mouse_keys[index])
            << " ; " << key_name(config.mouse_keys[index]);
    for (std::size_t index = 0; index < wheel_key_fields.size(); ++index)
        output << '\n' << wheel_key_fields[index] << '=' << format_key(config.wheel_keys[index])
            << " ; " << key_name(config.wheel_keys[index]);
    output << "\ntoggle_key=" << format_key(config.toggle_key) << " ; " << key_name(config.toggle_key)
        << "\nwindow_ms=" << config.filter.window_ms
        << "\nstart_counts=" << config.filter.start_counts
        << "\nreverse_counts=" << config.filter.reverse_counts
        << "\nrelease_ms=" << config.filter.release_ms
        << "\nx_pulse_enabled=" << (config.x_pulse_enabled ? 1 : 0)
        << "\nx_keyboard_override_enabled=" << (config.x_keyboard_override_enabled ? 1 : 0)
        << "\nx_hold_ratio=" << std::format("{:.17g}", config.x_hold_ratio);
    output << "\ny_pulse_enabled=" << (config.y_pulse_enabled ? 1 : 0)
        << "\ny_hold_ratio=" << std::format("{:.17g}", config.y_hold_ratio);
    output << "\n; Smoothing: lerp previous toward each nonzero axis input; 1 = raw, 0 = no axis mapping."
        << "\nx_smoothing_factor=" << std::format("{:.17g}", config.x_smoothing_factor)
        << "\ny_smoothing_factor=" << std::format("{:.17g}", config.y_smoothing_factor);
    output << "\n; Per-axis thresholds override legacy start_counts/reverse_counts."
        << "\n; Curves apply only to pulses: normalized speed -> hold ratio (0..1)."
        << "\n; Speed is abs(net counts in the last 30ms)/0.03s; full speed is counts/s."
        << "\n; Curve disabled preserves the fixed hold ratio. Points: input:output,...";
    output << "\nx_start_counts=" << config.x_start_counts
        << "\nx_reverse_counts=" << config.x_reverse_counts
        << "\nx_curve_enabled=" << (config.x_curve.enabled ? 1 : 0)
        << "\nx_curve_full_speed=" << std::format("{:.17g}", config.x_curve.full_speed)
        << "\nx_curve_points=" << format_curve(config.x_curve.points);
    output << "\ny_start_counts=" << config.y_start_counts
        << "\ny_reverse_counts=" << config.y_reverse_counts
        << "\ny_curve_enabled=" << (config.y_curve.enabled ? 1 : 0)
        << "\ny_curve_full_speed=" << std::format("{:.17g}", config.y_curve.full_speed)
        << "\ny_curve_points=" << format_curve(config.y_curve.points);
    output << "\npulse_period_ms=" << config.pulse_period_ms << '\n';
    output << "; Pie: hold keyboard key or mouse button, move to select, release to send an arrow key.\n"
        << "pie_trigger=" << format_input_binding(config.pie.trigger)
        << "\npie_visual_enabled=" << config.pie.visual_enabled
        << "\npie_deadzone_counts=" << config.pie.deadzone_counts
        << "\npie_radius_counts=" << config.pie.radius_counts
        << "\npie_hysteresis_degrees=" << std::format("{:.17g}", config.pie.hysteresis_degrees)
        << "\npie_key_hold_ms=" << config.pie.key_hold_ms << '\n';
    output << "bypass_key_1=" << format_input_binding(config.bypass_keys[0])
        << "\nbypass_key_2=" << format_input_binding(config.bypass_keys[1])
        << "\nkernel_y_enabled=" << config.kernel_y_enabled
        << "\nkernel_y_block=" << config.kernel_y_block;
    output << "\nkernel_y_up_key=" << format_key(config.kernel_y.up_key);
    output << "\nkernel_y_down_key=" << format_key(config.kernel_y.down_key);
    output << "\nkernel_y_start_counts=" << config.kernel_y.start_counts;
    output << "\nkernel_y_reverse_counts=" << config.kernel_y.reverse_counts;
    output << "\nkernel_y_smoothing_factor=" << std::format("{:.17g}", config.kernel_y.smoothing_factor);
    output << "\nkernel_y_hold_ratio=" << std::format("{:.17g}", config.kernel_y.hold_ratio);
    output << "\nkernel_y_pulse_enabled=" << config.kernel_y.pulse_enabled;
    output << "\nkernel_y_curve_enabled=" << config.kernel_y.curve.enabled;
    output << "\nkernel_y_curve_full_speed=" << std::format("{:.17g}", config.kernel_y.curve.full_speed);
    output << "\nkernel_y_curve_points=" << format_curve(config.kernel_y.curve.points);
    output << '\n';
}

void save_config(const std::filesystem::path& path, const configuration& config) {
    auto temporary = path;
    temporary += L".tmp.";
    temporary += std::to_wstring(GetCurrentProcessId());
    try {
        std::ofstream output(temporary, std::ios::trunc);
        if (!output) throw std::runtime_error("Cannot write configuration directory; use --config <writable-path>");
        write_config(output, config);
        output.close();
        if (!output) throw std::runtime_error("Cannot finish writing configuration");
        if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Cannot replace configuration");
    } catch (...) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        throw;
    }
}

std::filesystem::path default_config_path() {
    const auto directory = executable_directory();
    const auto shared = directory / L"config.ini";
    const auto legacy = directory / L"config.user.ini";
    // Both backends resolve the same file, including legacy user-only installs.
    return !std::filesystem::exists(shared) && std::filesystem::exists(legacy) ? legacy : shared;
}

std::filesystem::path executable_directory() {
    std::array<wchar_t, 512> buffer{};
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) throw std::runtime_error("Cannot determine executable directory");
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
}

} // namespace mouse_mapping
