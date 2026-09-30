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
    {"LEFT"sv, VK_LEFT}, {"RIGHT"sv, VK_RIGHT}, {"UP"sv, VK_UP}, {"DOWN"sv, VK_DOWN},
    {"SPACE"sv, VK_SPACE}, {"ENTER"sv, VK_RETURN}, {"TAB"sv, VK_TAB}, {"ESC"sv, VK_ESCAPE},
    {"BACKSPACE"sv, VK_BACK}, {"INSERT"sv, VK_INSERT}, {"DELETE"sv, VK_DELETE},
    {"HOME"sv, VK_HOME}, {"END"sv, VK_END}, {"PAGE_UP"sv, VK_PRIOR}, {"PAGE_DOWN"sv, VK_NEXT},
    {"LSHIFT"sv, VK_LSHIFT}, {"RSHIFT"sv, VK_RSHIFT}, {"LCTRL"sv, VK_LCONTROL},
    {"RCTRL"sv, VK_RCONTROL}, {"LALT"sv, VK_LMENU}, {"RALT"sv, VK_RMENU},
    {"LWIN"sv, VK_LWIN}, {"RWIN"sv, VK_RWIN}, {"APPS"sv, VK_APPS},
    {"CAPS_LOCK"sv, VK_CAPITAL}, {"NUM_LOCK"sv, VK_NUMLOCK}, {"SCROLL_LOCK"sv, VK_SCROLL},
    {"NUM0"sv, VK_NUMPAD0}, {"NUM1"sv, VK_NUMPAD1}, {"NUM2"sv, VK_NUMPAD2},
    {"NUM3"sv, VK_NUMPAD3}, {"NUM4"sv, VK_NUMPAD4}, {"NUM5"sv, VK_NUMPAD5},
    {"NUM6"sv, VK_NUMPAD6}, {"NUM7"sv, VK_NUMPAD7}, {"NUM8"sv, VK_NUMPAD8}, {"NUM9"sv, VK_NUMPAD9},
    {"ADD"sv, VK_ADD}, {"SUBTRACT"sv, VK_SUBTRACT}, {"MULTIPLY"sv, VK_MULTIPLY},
    {"DIVIDE"sv, VK_DIVIDE}, {"DECIMAL"sv, VK_DECIMAL}
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
    const auto first = value.find_first_not_of(" \t\r\n"sv);
    if (first == std::string_view::npos) return {};
    return std::string(value.substr(first, value.find_last_not_of(" \t\r\n"sv) - first + 1));
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
    if (value == "0"sv) return false;
    if (value == "1"sv) return true;
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
    if (value == "NONE"sv) return unbound_key;
    unsigned int code = 0;
    if (value.starts_with("0X"sv)) {
        const auto [end, error] = std::from_chars(value.data() + 2, value.data() + value.size(), code, 16);
        if (error != std::errc{} || end != value.data() + value.size() || code > 0xffff)
            throw std::runtime_error("Invalid hexadecimal scan code");
    } else if (value == "NUM_ENTER"sv) {
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
    if (!key_bound(code)) return std::string{"NONE"sv};
    return std::format("0x{:04x}", code);
}

std::string key_name(key_code code) {
    if (!key_bound(code)) return std::string{"NONE"sv};
    for (unsigned int vk = 'A'; vk <= 'Z'; ++vk)
        if (scan_code_for_vk(vk) == code) return std::string(1, static_cast<char>(vk));
    for (unsigned int vk = '0'; vk <= '9'; ++vk)
        if (scan_code_for_vk(vk) == code) return std::string(1, static_cast<char>(vk));
    for (unsigned int vk = VK_F1; vk <= VK_F24; ++vk)
        if (scan_code_for_vk(vk) == code) return std::format("F{}", vk - VK_F1 + 1);
    for (const auto& [name, vk] : named_keys)
        if (scan_code_for_vk(vk) == code) return std::string(name);
    if (code == 0xe01c) return std::string{"NUM_ENTER"sv};
    const auto vk = MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX);
    const auto character = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7fffffff;
    if (character >= 33 && character <= 126) return std::string(1, static_cast<char>(character));
    return std::string{"SCAN_CODE"sv};
}

std::string describe_key(key_code code) {
    return key_bound(code) ? std::format("{} ({})", key_name(code), format_key(code)) : std::string{"NONE"sv};
}

static constexpr std::array mouse_binding_names{
    "MOUSE_LEFT"sv, "MOUSE_RIGHT"sv, "MOUSE_MIDDLE"sv, "MOUSE_X1"sv, "MOUSE_X2"sv};
input_binding parse_input_binding(std::string_view text) {
    auto value = trim(text);
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); });
    if (value == "NONE"sv) return {};
    for (std::size_t i = 0; i < mouse_binding_names.size(); ++i)
        if (value == mouse_binding_names[i])
            return {input_kind::mouse, static_cast<key_code>(i)};
    return {input_kind::keyboard, parse_key(value)};
}
std::string format_input_binding(input_binding binding) {
    if (binding.kind == input_kind::none) return std::string{"NONE"sv};
    if (binding.kind == input_kind::mouse && binding.code < mouse_binding_names.size()) return std::string{mouse_binding_names[binding.code]};
    if (binding.kind == input_kind::keyboard) return format_key(binding.code);
    throw std::runtime_error("Invalid input binding");
}

bool supported_or_unbound(key_code code) {
    return !key_bound(code) || supported_key(code);
}

bool same_bound_key(key_code first, key_code second) {
    return key_bound(first) && key_bound(second) && first == second;
}

template<class Range>
bool unique_bound_keys(const Range& keys) {
    std::set<key_code> seen;
    for (const auto key : keys)
        if (mouse_mapping::key_bound(key) && !seen.insert(key).second) return false;
    return true;
}

bool unique_bound_keys(std::initializer_list<key_code> keys) {
    std::set<key_code> seen;
    for (const auto key : keys)
        if (key_bound(key) && !seen.insert(key).second) return false;
    return true;
}

void validate(const configuration& config) {
    const auto& chord = config.chord;
    if ((chord.trigger.kind == input_kind::keyboard && !supported_or_unbound(chord.trigger.code))
        || (chord.trigger.kind == input_kind::mouse && chord.trigger.code > 4))
        throw std::runtime_error("chord_trigger_key must be NONE, a supported keyboard key or a mouse button");
    for (const auto key : {chord.first, chord.second})
        if (!supported_or_unbound(key)) throw std::runtime_error("Invalid chord output scan code");
    if (chord.enabled) {
        if (!unique_bound_keys({chord.first, chord.second})
            || (chord.trigger.kind == input_kind::keyboard
                && (same_bound_key(chord.trigger.code, chord.first) || same_bound_key(chord.trigger.code, chord.second))))
            throw std::runtime_error("Chord trigger and output keys must be different");
        for (const auto key : {chord.first, chord.second}) {
            if (same_bound_key(key, config.toggle_key)) throw std::runtime_error("Chord keys conflict with toggle_key");
            for (const auto binding : config.bypass_keys)
                if (key_bound(key) && binding == input_binding{input_kind::keyboard, key})
                    throw std::runtime_error("Chord keys conflict with bypass keys");
        }
        if (chord.trigger.kind != input_kind::none) {
            if ((chord.trigger.kind == input_kind::keyboard && same_bound_key(chord.trigger.code, config.toggle_key))
                || std::find(config.bypass_keys.begin(), config.bypass_keys.end(), chord.trigger) != config.bypass_keys.end())
                throw std::runtime_error("Chord trigger conflicts with toggle_key or bypass keys");
            if (chord.trigger == config.pie.trigger)
                throw std::runtime_error("Chord trigger conflicts with Pie trigger");
            if (chord.trigger.kind == input_kind::keyboard) {
                const auto key = chord.trigger.code;
                if (same_bound_key(key, config.left_key) || same_bound_key(key, config.right_key) ||
                    (y_enabled(config) && (same_bound_key(key, effective_config(config).up_key)
                        || same_bound_key(key, effective_config(config).down_key))))
                    throw std::runtime_error("Chord trigger conflicts with an axis key");
                if (config.pie.trigger.kind != input_kind::none
                    && std::find(pie_arrow_keys.begin(), pie_arrow_keys.end(), key) != pie_arrow_keys.end())
                    throw std::runtime_error("Chord trigger conflicts with Pie arrow keys");
            }
        }
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
            bool trigger_conflict = reserved(key) || same_bound_key(key, config.toggle_key) ||
                same_bound_key(key, config.left_key) || same_bound_key(key, config.right_key) ||
                (y_enabled(config) && (same_bound_key(key, effective.up_key) || same_bound_key(key, effective.down_key)));
            if (config.map_y) {
                for (const auto mapped : config.mouse_keys) trigger_conflict = trigger_conflict || same_bound_key(mapped, key);
                for (const auto mapped : config.wheel_keys) trigger_conflict = trigger_conflict || same_bound_key(mapped, key);
            }
            if (chord.enabled) trigger_conflict = trigger_conflict
                || (chord.trigger.kind == input_kind::keyboard && same_bound_key(key, chord.trigger.code))
                || same_bound_key(key, chord.first) || same_bound_key(key, chord.second);
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
        if ((binding.kind == input_kind::keyboard && (!supported_key(binding.code) || same_bound_key(binding.code, config.toggle_key)))
            || (binding.kind == input_kind::mouse && binding.code > 4))
            throw std::runtime_error("Invalid bypass key or conflict with toggle_key");
    }
    if (config.bypass_keys[0].kind != input_kind::none && config.bypass_keys[0] == config.bypass_keys[1])
        throw std::runtime_error("Bypass keys must be different");
    const auto& ky = config.kernel_y;
    if (!supported_or_unbound(ky.up_key) || !supported_or_unbound(ky.down_key)) throw std::runtime_error("Invalid kernel Y keys");
    if (ky.start_counts < 1 || ky.reverse_counts < ky.start_counts || ky.reverse_counts > 10000
        || !std::isfinite(ky.smoothing_factor) || ky.smoothing_factor < 0 || ky.smoothing_factor > 1
        || !std::isfinite(ky.hold_ratio) || ky.hold_ratio < 0 || ky.hold_ratio > 1
        || !std::isfinite(ky.curve.full_speed) || ky.curve.full_speed < 1 || ky.curve.full_speed > 1000000)
        throw std::runtime_error("Invalid kernel Y parameters");
    validate_curve(ky.curve.points);
    for (const auto& [name, code] : {std::pair{"left_key", config.left_key},
            {"right_key", config.right_key}, {"toggle_key", config.toggle_key}})
        if (!supported_or_unbound(code)) throw std::runtime_error(std::format("{}: unsupported scan code", name));
    if (config.map_y) {
        if (!supported_or_unbound(config.up_key) || !supported_or_unbound(config.down_key))
            throw std::runtime_error("up_key/down_key: unsupported scan code");
        for (const auto code : config.mouse_keys) {
            if (!supported_or_unbound(code)) throw std::runtime_error("Mouse button binding: unsupported scan code");
        }
        for (const auto code : config.wheel_keys) {
            if (!supported_or_unbound(code)) throw std::runtime_error("Mouse wheel binding: unsupported scan code");
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
    if (config.window_ms < 1 || config.window_ms > 1000) throw std::runtime_error("window_ms must be 1..1000");
    if (config.release_ms < config.window_ms || config.release_ms > 2000)
        throw std::runtime_error("release_ms must be window_ms..2000");
    if (!std::isfinite(config.x_hold_ratio) || config.x_hold_ratio < 0 || config.x_hold_ratio > 1
        || !std::isfinite(config.y_hold_ratio) || config.y_hold_ratio < 0 || config.y_hold_ratio > 1)
        throw std::runtime_error("hold ratios must be 0..1");
    if (config.pulse_period_ms < 2 || config.pulse_period_ms > 1000)
        throw std::runtime_error("pulse_period_ms must be 2..1000");
}

namespace {

struct entry {
    std::string_view name;
    void (*parser)(configuration&, std::string_view);
};

constexpr auto config_entries = [] {
    std::array entries{
        entry{"left_key"sv, [](configuration& config, std::string_view value) { config.left_key = parse_key(value); }},
        entry{"right_key"sv, [](configuration& config, std::string_view value) { config.right_key = parse_key(value); }},
        entry{"chord_enabled"sv, [](configuration& config, std::string_view value) { config.chord.enabled = parse_switch(value); }},
        entry{"chord_trigger_key"sv, [](configuration& config, std::string_view value) { config.chord.trigger = parse_input_binding(value); }},
        entry{"chord_first_key"sv, [](configuration& config, std::string_view value) { config.chord.first = parse_key(value); }},
        entry{"chord_second_key"sv, [](configuration& config, std::string_view value) { config.chord.second = parse_key(value); }},
        entry{"toggle_key"sv, [](configuration& config, std::string_view value) { config.toggle_key = parse_key(value); }},
        entry{"up_key"sv, [](configuration& config, std::string_view value) { config.up_key = parse_key(value); }},
        entry{"down_key"sv, [](configuration& config, std::string_view value) { config.down_key = parse_key(value); }},
        entry{"pie_trigger"sv, [](configuration& config, std::string_view value) { config.pie.trigger = parse_input_binding(value); }},
        entry{"pie_visual_enabled"sv, [](configuration& config, std::string_view value) { config.pie.visual_enabled = parse_switch(value); }},
        entry{"pie_deadzone_counts"sv, [](configuration& config, std::string_view value) { config.pie.deadzone_counts = parse_integer(value); }},
        entry{"pie_radius_counts"sv, [](configuration& config, std::string_view value) { config.pie.radius_counts = parse_integer(value); }},
        entry{"pie_hysteresis_degrees"sv, [](configuration& config, std::string_view value) { config.pie.hysteresis_degrees = parse_number(value); }},
        entry{"pie_key_hold_ms"sv, [](configuration& config, std::string_view value) { config.pie.key_hold_ms = parse_integer(value); }},
        entry{"bypass_key_1"sv, [](configuration& config, std::string_view value) { config.bypass_keys[0] = parse_input_binding(value); }},
        entry{"bypass_key_2"sv, [](configuration& config, std::string_view value) { config.bypass_keys[1] = parse_input_binding(value); }},
        entry{"kernel_y_enabled"sv, [](configuration& config, std::string_view value) { config.kernel_y_enabled = parse_switch(value); }},
        entry{"kernel_y_block"sv, [](configuration& config, std::string_view value) { config.kernel_y_block = parse_switch(value); }},
        entry{"kernel_y_up_key"sv, [](configuration& config, std::string_view value) { config.kernel_y.up_key = parse_key(value); }},
        entry{"kernel_y_down_key"sv, [](configuration& config, std::string_view value) { config.kernel_y.down_key = parse_key(value); }},
        entry{"kernel_y_start_counts"sv, [](configuration& config, std::string_view value) { config.kernel_y.start_counts = parse_integer(value); }},
        entry{"kernel_y_reverse_counts"sv, [](configuration& config, std::string_view value) { config.kernel_y.reverse_counts = parse_integer(value); }},
        entry{"kernel_y_smoothing_factor"sv, [](configuration& config, std::string_view value) { config.kernel_y.smoothing_factor = parse_ratio(value); }},
        entry{"kernel_y_hold_ratio"sv, [](configuration& config, std::string_view value) { config.kernel_y.hold_ratio = parse_ratio(value); }},
        entry{"kernel_y_pulse_enabled"sv, [](configuration& config, std::string_view value) { config.kernel_y.pulse_enabled = parse_switch(value); }},
        entry{"kernel_y_curve_enabled"sv, [](configuration& config, std::string_view value) { config.kernel_y.curve.enabled = parse_switch(value); }},
        entry{"kernel_y_curve_full_speed"sv, [](configuration& config, std::string_view value) { config.kernel_y.curve.full_speed = parse_number(value); }},
        entry{"kernel_y_curve_points"sv, [](configuration& config, std::string_view value) { config.kernel_y.curve.points = parse_curve(value); }},
        entry{"window_ms"sv, [](configuration& config, std::string_view value) { config.window_ms = parse_integer(value); }},
        entry{"release_ms"sv, [](configuration& config, std::string_view value) { config.release_ms = parse_integer(value); }},
        entry{"x_pulse_enabled"sv, [](configuration& config, std::string_view value) { config.x_pulse_enabled = parse_switch(value); }},
        entry{"x_keyboard_override_enabled"sv, [](configuration& config, std::string_view value) { config.x_keyboard_override_enabled = parse_switch(value); }},
        entry{"y_pulse_enabled"sv, [](configuration& config, std::string_view value) { config.y_pulse_enabled = parse_switch(value); }},
        entry{"x_hold_ratio"sv, [](configuration& config, std::string_view value) { config.x_hold_ratio = parse_ratio(value); }},
        entry{"y_hold_ratio"sv, [](configuration& config, std::string_view value) { config.y_hold_ratio = parse_ratio(value); }},
        entry{"x_smoothing_factor"sv, [](configuration& config, std::string_view value) { config.x_smoothing_factor = parse_ratio(value); }},
        entry{"y_smoothing_factor"sv, [](configuration& config, std::string_view value) { config.y_smoothing_factor = parse_ratio(value); }},
        entry{"x_start_counts"sv, [](configuration& config, std::string_view value) { config.x_start_counts = parse_integer(value); }},
        entry{"x_reverse_counts"sv, [](configuration& config, std::string_view value) { config.x_reverse_counts = parse_integer(value); }},
        entry{"x_curve_enabled"sv, [](configuration& config, std::string_view value) { config.x_curve.enabled = parse_switch(value); }},
        entry{"x_curve_full_speed"sv, [](configuration& config, std::string_view value) { config.x_curve.full_speed = parse_number(value); }},
        entry{"x_curve_points"sv, [](configuration& config, std::string_view value) { config.x_curve.points = parse_curve(value); }},
        entry{"y_start_counts"sv, [](configuration& config, std::string_view value) { config.y_start_counts = parse_integer(value); }},
        entry{"y_reverse_counts"sv, [](configuration& config, std::string_view value) { config.y_reverse_counts = parse_integer(value); }},
        entry{"y_curve_enabled"sv, [](configuration& config, std::string_view value) { config.y_curve.enabled = parse_switch(value); }},
        entry{"y_curve_full_speed"sv, [](configuration& config, std::string_view value) { config.y_curve.full_speed = parse_number(value); }},
        entry{"y_curve_points"sv, [](configuration& config, std::string_view value) { config.y_curve.points = parse_curve(value); }},
        entry{"pulse_period_ms"sv, [](configuration& config, std::string_view value) { config.pulse_period_ms = parse_integer(value); }},
        entry{"lmb_key"sv, [](configuration& config, std::string_view value) { config.mouse_keys[0] = parse_key(value); }},
        entry{"rmb_key"sv, [](configuration& config, std::string_view value) { config.mouse_keys[1] = parse_key(value); }},
        entry{"cmb_key"sv, [](configuration& config, std::string_view value) { config.mouse_keys[2] = parse_key(value); }},
        entry{"x1_key"sv, [](configuration& config, std::string_view value) { config.mouse_keys[3] = parse_key(value); }},
        entry{"x2_key"sv, [](configuration& config, std::string_view value) { config.mouse_keys[4] = parse_key(value); }},
        entry{"wheel_up_key"sv, [](configuration& config, std::string_view value) { config.wheel_keys[0] = parse_key(value); }},
        entry{"wheel_down_key"sv, [](configuration& config, std::string_view value) { config.wheel_keys[1] = parse_key(value); }},
    };
    std::ranges::sort(entries, {}, &entry::name);
    return entries;
}();

static_assert(std::ranges::is_sorted(config_entries, {}, &entry::name));
static_assert(std::ranges::adjacent_find(config_entries, {}, &entry::name) == config_entries.end());

const entry* find_entry(std::string_view name) {
    const auto found = std::ranges::lower_bound(config_entries, name, {}, &entry::name);
    return found != config_entries.end() && found->name == name ? std::to_address(found) : nullptr;
}

} // namespace

configuration read_config(std::istream& input, bool map_y, bool require_bindings) {
    configuration config;
    config.map_y = map_y;
    std::set<std::string, std::less<>> seen;
    std::string line;
    int line_number = 0;
    while (std::getline(input, line)) {
        ++line_number;
        if (line_number == 1 && line.starts_with("\xef\xbb\xbf"sv)) line.erase(0, 3);
        line = trim(std::string_view(line).substr(0, line.find_first_of(";#"sv)));
        if (line.empty() || line == "[mapping]"sv) continue;
        const auto separator = line.find('=');
        if (separator == std::string::npos)
            throw std::runtime_error(std::format("Config line {}: expected key=value", line_number));
        const auto name = trim(std::string_view(line).substr(0, separator));
        const auto value = trim(std::string_view(line).substr(separator + 1));
        try {
            if (!seen.insert(name).second) throw std::runtime_error("Duplicate field");
            const auto* entry = find_entry(name);
            if (!entry) throw std::runtime_error("Unknown field");
            entry->parser(config, value);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::format("Config line {} ({}): {}", line_number, name, error.what()));
        }
    }
    if (input.bad()) throw std::runtime_error("Cannot read configuration");
    if (require_bindings) {
        for (const std::string_view name : {"left_key"sv, "right_key"sv, "toggle_key"sv})
            if (!seen.contains(name)) throw std::runtime_error(std::format("Unbound key: {}; run --configure first", name));
        if (map_y) {
            for (const std::string_view name : {"up_key"sv, "down_key"sv})
                if (!seen.contains(name)) throw std::runtime_error(std::format("Unbound key: {}; run --configure first", name));
            for (const std::string_view name : mouse_key_fields)
                if (!seen.contains(name)) throw std::runtime_error(std::format("Unbound key: {}; run --configure first", name));
            for (const std::string_view name : wheel_key_fields)
                if (!seen.contains(name)) throw std::runtime_error(std::format("Unbound key: {}; run --configure first", name));
        }
    }
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
    output << "; Keys are physical scan codes; 0xe0xx means an extended key. Key names and NONE are also accepted.\n"
        << "[mapping]\nleft_key=" << format_key(config.left_key) << " ; " << key_name(config.left_key)
        << "\nright_key=" << format_key(config.right_key) << " ; " << key_name(config.right_key);
    output << "\nchord_enabled=" << config.chord.enabled
        << "\nchord_trigger_key=" << format_input_binding(config.chord.trigger)
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
        << "\nwindow_ms=" << config.window_ms
        << "\nrelease_ms=" << config.release_ms
        << "\nx_pulse_enabled=" << (config.x_pulse_enabled ? 1 : 0)
        << "\nx_keyboard_override_enabled=" << (config.x_keyboard_override_enabled ? 1 : 0)
        << "\nx_hold_ratio=" << std::format("{:.17g}", config.x_hold_ratio);
    output << "\ny_pulse_enabled=" << (config.y_pulse_enabled ? 1 : 0)
        << "\ny_hold_ratio=" << std::format("{:.17g}", config.y_hold_ratio);
    output << "\n; Smoothing: lerp previous toward each nonzero axis input; 1 = raw, 0 = no axis mapping."
        << "\nx_smoothing_factor=" << std::format("{:.17g}", config.x_smoothing_factor)
        << "\ny_smoothing_factor=" << std::format("{:.17g}", config.y_smoothing_factor);
    output << "\n; Independent start/reverse thresholds for each axis."
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
    return executable_directory() / L"config.ini";
}

std::filesystem::path executable_directory() {
    std::array<wchar_t, 512> buffer{};
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) throw std::runtime_error("Cannot determine executable directory");
    return std::filesystem::path(std::wstring_view(buffer.data(), length)).parent_path();
}

} // namespace mouse_mapping
