#include "config.hpp"

#include <windows.h>
#include <algorithm>
#include <charconv>
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
    const auto& filter = config.filter;
    if (filter.window_ms < 1 || filter.window_ms > 1000) throw std::runtime_error("window_ms must be 1..1000");
    if (filter.start_counts < 1 || filter.start_counts > 10000) throw std::runtime_error("start_counts must be 1..10000");
    if (filter.reverse_counts < filter.start_counts || filter.reverse_counts > 10000)
        throw std::runtime_error("reverse_counts must be start_counts..10000");
    if (filter.release_ms < filter.window_ms || filter.release_ms > 2000)
        throw std::runtime_error("release_ms must be window_ms..2000");
}

configuration read_config(std::istream& input) {
    configuration config;
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
            else if (name == "window_ms") config.filter.window_ms = parse_integer(value);
            else if (name == "start_counts") config.filter.start_counts = parse_integer(value);
            else if (name == "reverse_counts") config.filter.reverse_counts = parse_integer(value);
            else if (name == "release_ms") config.filter.release_ms = parse_integer(value);
            else throw std::runtime_error("Unknown field");
        } catch (const std::exception& error) {
            throw std::runtime_error("Config line " + std::to_string(line_number) + " (" + name + "): " + error.what());
        }
    }
    if (input.bad()) throw std::runtime_error("Cannot read configuration");
    validate(config);
    return config;
}

configuration load_config(const std::filesystem::path& path) {
    std::ifstream input(path);
    if (!input) throw std::runtime_error("Cannot open configuration file");
    return read_config(input);
}

void write_config(std::ostream& output, const configuration& config) {
    validate(config);
    output << "; Keys are physical scan codes; 0xe0xx means an extended key. Key names are also accepted.\n"
        << "[mapping]\nleft_key=" << format_key(config.left_key) << " ; " << key_name(config.left_key)
        << "\nright_key=" << format_key(config.right_key) << " ; " << key_name(config.right_key)
        << "\ntoggle_key=" << format_key(config.toggle_key) << " ; " << key_name(config.toggle_key)
        << "\nwindow_ms=" << config.filter.window_ms
        << "\nstart_counts=" << config.filter.start_counts
        << "\nreverse_counts=" << config.filter.reverse_counts
        << "\nrelease_ms=" << config.filter.release_ms << '\n';
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
        defaults.toggle_key = ask("Toggle key", defaults.toggle_key);
        try { validate(defaults); return defaults; }
        catch (const std::exception& error) { std::cout << error.what() << "; please enter keys again.\n"; }
    }
}

std::filesystem::path executable_directory() {
    std::wstring buffer(32768, L'\0');
    const auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    if (length == 0 || length >= buffer.size()) throw std::runtime_error("Cannot determine executable directory");
    buffer.resize(length);
    return std::filesystem::path(buffer).parent_path();
}

} // namespace mouse_mapping
