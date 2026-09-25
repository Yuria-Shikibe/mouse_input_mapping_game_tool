#include "config.hpp"

#include <windows.h>
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cctype>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <set>
#include <sstream>
#include <system_error>

namespace mouse_mapping {
namespace {

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
    std::ostringstream output;
    output << "0x" << std::hex << std::setw(4) << std::setfill('0') << code;
    return output.str();
}

std::string key_name(key_code code) {
    for (unsigned int vk = 'A'; vk <= 'Z'; ++vk)
        if (scan_code_for_vk(vk) == code) return std::string(1, static_cast<char>(vk));
    for (unsigned int vk = '0'; vk <= '9'; ++vk)
        if (scan_code_for_vk(vk) == code) return std::string(1, static_cast<char>(vk));
    for (unsigned int vk = VK_F1; vk <= VK_F24; ++vk)
        if (scan_code_for_vk(vk) == code) return "F" + std::to_string(vk - VK_F1 + 1);
    for (const auto& [name, vk] : named_keys)
        if (scan_code_for_vk(vk) == code) return std::string(name);
    if (code == 0xe01c) return "NUM_ENTER";
    const auto vk = MapVirtualKeyW(code, MAPVK_VSC_TO_VK_EX);
    const auto character = MapVirtualKeyW(vk, MAPVK_VK_TO_CHAR) & 0x7fffffff;
    if (character >= 33 && character <= 126) return std::string(1, static_cast<char>(character));
    return "SCAN_CODE";
}

std::string describe_key(key_code code) { return key_name(code) + " (" + format_key(code) + ")"; }

void validate(const configuration& config) {
    for (const auto& [name, code] : {std::pair{"left_key", config.left_key},
            {"right_key", config.right_key}, {"toggle_key", config.toggle_key}})
        if (!supported_key(code)) throw std::runtime_error(std::string(name) + ": unsupported scan code");
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
        if (separator == std::string::npos) throw std::runtime_error("Config line " + std::to_string(line_number) + ": expected key=value");
        const auto name = trim(std::string_view(line).substr(0, separator));
        const auto value = trim(std::string_view(line).substr(separator + 1));
        try {
            if (!seen.insert(name).second) throw std::runtime_error("Duplicate field");
            if (name == "left_key") config.left_key = parse_key(value);
            else if (name == "right_key") config.right_key = parse_key(value);
            else if (name == "toggle_key") config.toggle_key = parse_key(value);
            else if (map_y && name == "up_key") config.up_key = parse_key(value);
            else if (map_y && name == "down_key") config.down_key = parse_key(value);
            else if (name == "window_ms") config.filter.window_ms = parse_integer(value);
            else if (name == "start_counts") config.filter.start_counts = parse_integer(value);
            else if (name == "reverse_counts") config.filter.reverse_counts = parse_integer(value);
            else if (name == "release_ms") config.filter.release_ms = parse_integer(value);
            else if (name == "x_pulse_enabled") config.x_pulse_enabled = parse_switch(value);
            else if (map_y && name == "y_pulse_enabled") config.y_pulse_enabled = parse_switch(value);
            else if (name == "x_hold_ratio") config.x_hold_ratio = parse_ratio(value);
            else if (map_y && name == "y_hold_ratio") config.y_hold_ratio = parse_ratio(value);
            else if (name == "pulse_period_ms") config.pulse_period_ms = parse_integer(value);
            else {
                bool found = false;
                if (map_y) for (std::size_t index = 0; index < mouse_key_fields.size(); ++index) {
                    if (name == mouse_key_fields[index]) {
                        config.mouse_keys[index] = parse_key(value);
                        found = true;
                        break;
                    }
                }
                if (map_y) for (std::size_t index = 0; index < wheel_key_fields.size(); ++index) {
                    if (name == wheel_key_fields[index]) {
                        config.wheel_keys[index] = parse_key(value);
                        found = true;
                        break;
                    }
                }
                if (!found) throw std::runtime_error("Unknown field");
            }
        } catch (const std::exception& error) {
            throw std::runtime_error("Config line " + std::to_string(line_number) + " (" + name + "): " + error.what());
        }
    }
    if (input.bad()) throw std::runtime_error("Cannot read configuration");
    if (require_bindings) {
        for (const char* name : {"left_key", "right_key", "toggle_key"})
            if (!seen.contains(name)) throw std::runtime_error(std::string("Unbound key: ") + name + "; run --configure first");
        if (map_y) {
            for (const char* name : {"up_key", "down_key"})
                if (!seen.contains(name)) throw std::runtime_error(std::string("Unbound key: ") + name + "; run --configure first");
            for (const char* name : mouse_key_fields)
                if (!seen.contains(name)) throw std::runtime_error(std::string("Unbound key: ") + name + "; run --configure first");
            for (const char* name : wheel_key_fields)
                if (!seen.contains(name)) throw std::runtime_error(std::string("Unbound key: ") + name + "; run --configure first");
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
    output << "; Keys are physical scan codes; 0xe0xx means an extended key. Key names are also accepted.\n"
        << "[mapping]\nleft_key=" << format_key(config.left_key) << " ; " << key_name(config.left_key)
        << "\nright_key=" << format_key(config.right_key) << " ; " << key_name(config.right_key);
    if (config.map_y)
        output << "\nup_key=" << format_key(config.up_key) << " ; Y-: " << key_name(config.up_key)
            << "\ndown_key=" << format_key(config.down_key) << " ; Y+: " << key_name(config.down_key);
    if (config.map_y) for (std::size_t index = 0; index < mouse_key_fields.size(); ++index)
        output << '\n' << mouse_key_fields[index] << '=' << format_key(config.mouse_keys[index])
            << " ; " << key_name(config.mouse_keys[index]);
    if (config.map_y) for (std::size_t index = 0; index < wheel_key_fields.size(); ++index)
        output << '\n' << wheel_key_fields[index] << '=' << format_key(config.wheel_keys[index])
            << " ; " << key_name(config.wheel_keys[index]);
    output << "\ntoggle_key=" << format_key(config.toggle_key) << " ; " << key_name(config.toggle_key)
        << "\nwindow_ms=" << config.filter.window_ms
        << "\nstart_counts=" << config.filter.start_counts
        << "\nreverse_counts=" << config.filter.reverse_counts
        << "\nrelease_ms=" << config.filter.release_ms
        << "\nx_pulse_enabled=" << (config.x_pulse_enabled ? 1 : 0)
        << "\nx_hold_ratio=" << std::setprecision(17) << config.x_hold_ratio;
    if (config.map_y) output << "\ny_pulse_enabled=" << (config.y_pulse_enabled ? 1 : 0)
        << "\ny_hold_ratio=" << config.y_hold_ratio;
    output << "\npulse_period_ms=" << config.pulse_period_ms << '\n';
}

void save_config(const std::filesystem::path& path, const configuration& config) {
    auto temporary = path;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
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

configuration configure(configuration defaults, bool text_mode) {
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD old_mode = 0;
    const bool live_console = !text_mode && GetConsoleMode(input, &old_mode);
    struct console_restore {
        HANDLE input;
        DWORD mode;
        bool active;
        ~console_restore() { if (active) SetConsoleMode(input, mode); }
    } restore{input, old_mode, live_console};
    if (live_console) {
        if (!SetConsoleMode(input, (old_mode | ENABLE_EXTENDED_FLAGS | ENABLE_PROCESSED_INPUT)
                & ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_QUICK_EDIT_MODE | ENABLE_VIRTUAL_TERMINAL_INPUT)))
            throw std::runtime_error("Cannot enable direct console key capture");
        std::cout << "Press the target key (including F1-F24), then Enter to confirm.\n"
                     "Press another key to change the selection. Enter keeps the displayed key.\n"
                     "To bind Enter itself, use --configure-text and type ENTER.\n";
    } else {
        std::cout << "Text input mode (IDE/redirected terminal): type a key NAME such as A, LEFT, F8.\n"
                     "Press Enter to display it, then Enter again to confirm.\n"
                     "Do not press function keys here; type their names, e.g. F8.\n";
    }
    auto ask = [&](const char* label, key_code value) {
        std::cout << label << " - selected: " << describe_key(value) << '\n' << std::flush;
        bool enter_down = false;
        for (;;) {
            bool confirmed = false;
            if (live_console) {
                INPUT_RECORD record{};
                DWORD read = 0;
                if (!ReadConsoleInputW(input, &record, 1, &read)) throw std::runtime_error("Cannot read console key event");
                if (record.EventType != KEY_EVENT) continue;
                const auto& key = record.Event.KeyEvent;
                if (key.wVirtualKeyCode == VK_RETURN) {
                    if (key.bKeyDown) { enter_down = true; continue; }
                    confirmed = enter_down; // Consume release/repeats before advancing to the next binding.
                } else {
                    if (!key.bKeyDown || enter_down) continue;
                    const auto code = static_cast<key_code>(key.wVirtualScanCode | ((key.dwControlKeyState & ENHANCED_KEY) ? 0xe000 : 0));
                    if (key.wVirtualKeyCode == VK_PAUSE || key.wVirtualKeyCode == VK_SNAPSHOT || !supported_key(code)) {
                        std::cout << "Unsupported key; choose another.\n" << std::flush;
                        continue;
                    }
                    value = code;
                }
            } else {
                std::cout << "Key name / Enter to confirm: " << std::flush;
                std::string answer;
                if (!std::getline(std::cin, answer)) throw std::runtime_error("Configuration input ended before confirmation");
                confirmed = trim(answer).empty();
                if (!confirmed) {
                    try { value = parse_key(answer); }
                    catch (const std::exception& error) { std::cout << error.what() << '\n'; continue; }
                }
            }
            if (confirmed) {
                std::cout << "Bound " << label << ": " << describe_key(value) << "\n\n";
                return value;
            }
            std::cout << "Selected: " << describe_key(value) << " - Enter to confirm.\n" << std::flush;
        }
    };
    for (;;) {
        defaults.left_key = ask("Left movement key", defaults.left_key);
        defaults.right_key = ask("Right movement key", defaults.right_key);
        if (defaults.map_y) {
            defaults.up_key = ask("Up movement key (Y-)", defaults.up_key);
            defaults.down_key = ask("Down movement key (Y+)", defaults.down_key);
            constexpr std::array labels{"Left mouse button (LMB)", "Right mouse button (RMB)",
                "Middle mouse button (CMB)", "Side mouse button 1 (X1)", "Side mouse button 2 (X2)"};
            for (std::size_t index = 0; index < labels.size(); ++index)
                defaults.mouse_keys[index] = ask(labels[index], defaults.mouse_keys[index]);
            defaults.wheel_keys[0] = ask("Wheel scroll up", defaults.wheel_keys[0]);
            defaults.wheel_keys[1] = ask("Wheel scroll down", defaults.wheel_keys[1]);
        }
        defaults.toggle_key = ask("Toggle key", defaults.toggle_key);
        try { validate(defaults); break; }
        catch (const std::exception& error) { std::cout << error.what() << "; please enter keys again.\n"; }
    }
    auto read_setting = [&]() {
        std::string answer;
        if (!live_console) {
            if (!std::getline(std::cin, answer)) throw std::runtime_error("Configuration input ended before confirmation");
            return answer;
        }
        for (;;) {
            INPUT_RECORD record{};
            DWORD read = 0;
            if (!ReadConsoleInputW(input, &record, 1, &read)) throw std::runtime_error("Cannot read console key event");
            if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
            const auto& key = record.Event.KeyEvent;
            if (key.wVirtualKeyCode == VK_RETURN) { std::cout << '\n'; return answer; }
            if (key.wVirtualKeyCode == VK_BACK) {
                if (!answer.empty()) answer.pop_back();
                continue;
            }
            const auto character = key.uChar.UnicodeChar;
            if (character >= 32 && character <= 126) answer.push_back(static_cast<char>(character));
        }
    };
    auto ask_switch = [&](const char* label, bool current) {
        for (;;) {
            std::cout << label << " (0/1, Enter keeps " << (current ? 1 : 0) << "): " << std::flush;
            const auto answer = read_setting();
            if (trim(answer).empty()) return current;
            try { return parse_switch(trim(answer)); }
            catch (const std::exception& error) { std::cout << error.what() << '\n'; }
        }
    };
    auto ask_ratio = [&](const char* label, double current) {
        for (;;) {
            std::cout << label << " (0..1, Enter keeps " << current << "): " << std::flush;
            const auto answer = read_setting();
            if (trim(answer).empty()) return current;
            try { return parse_ratio(trim(answer)); }
            catch (const std::exception& error) { std::cout << error.what() << '\n'; }
        }
    };
    defaults.x_pulse_enabled = ask_switch("X pulse enabled", defaults.x_pulse_enabled);
    defaults.x_hold_ratio = ask_ratio("X hold ratio", defaults.x_hold_ratio);
    if (defaults.map_y) {
        defaults.y_pulse_enabled = ask_switch("Y pulse enabled", defaults.y_pulse_enabled);
        defaults.y_hold_ratio = ask_ratio("Y hold ratio", defaults.y_hold_ratio);
    }
    return defaults;
}

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) throw std::runtime_error("Cannot determine executable directory");
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

} // namespace mouse_mapping
