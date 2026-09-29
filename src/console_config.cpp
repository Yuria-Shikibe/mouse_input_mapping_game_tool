#include "console_config.hpp"
#include "config_detail.hpp"
#include <windows.h>
#include <format>
#include <iostream>
#include <stdexcept>
#include <charconv>

namespace mouse_mapping {
using namespace config_detail;

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
                     "Press another key to change the selection. Esc selects NONE; Enter keeps the displayed key.\n"
                     "To bind Enter itself, use --configure-text and type ENTER.\n";
    } else {
        std::cout << "Text input mode (IDE/redirected terminal): type a key NAME such as A, LEFT, F8, or NONE.\n"
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
                    if (key.wVirtualKeyCode == VK_ESCAPE) {
                        value = unbound_key;
                    } else {
                        const auto code = static_cast<key_code>(key.wVirtualScanCode | ((key.dwControlKeyState & ENHANCED_KEY) ? 0xe000 : 0));
                        if (key.wVirtualKeyCode == VK_PAUSE || key.wVirtualKeyCode == VK_SNAPSHOT || !supported_key(code)) {
                            std::cout << "Unsupported key; choose another.\n" << std::flush;
                            continue;
                        }
                        value = code;
                    }
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
    for (std::size_t i = 0; i < defaults.bypass_keys.size(); ++i) {
        for (;;) {
            std::cout << "Bypass key " << i + 1 << " (key name / MOUSE_LEFT/RIGHT/MIDDLE/X1/X2 / NONE; Enter keeps "
                << format_input_binding(defaults.bypass_keys[i]) << "): " << std::flush;
            const auto answer = trim(read_setting());
            if (answer.empty()) break;
            try {
                const auto binding = parse_input_binding(answer);
                if ((binding.kind == input_kind::keyboard && binding.code == defaults.toggle_key)
                    || (binding.kind != input_kind::none && binding == defaults.bypass_keys[1 - i]))
                    throw std::runtime_error("Bypass keys must differ from each other and toggle key");
                defaults.bypass_keys[i] = binding;
                break;
            } catch (const std::exception& error) { std::cout << error.what() << '\n'; }
        }
    }
    if (!defaults.map_y) {
        defaults.kernel_y_enabled = ask_switch("Kernel Y mapping enabled", defaults.kernel_y_enabled);
        defaults.kernel_y_block = ask_switch("Kernel Y original input blocked", defaults.kernel_y_block);
        if (defaults.kernel_y_enabled) {
            for (;;) {
                defaults.kernel_y.up_key = ask("Kernel Y- key", defaults.kernel_y.up_key);
                defaults.kernel_y.down_key = ask("Kernel Y+ key", defaults.kernel_y.down_key);
                try { validate(defaults); break; }
                catch (const std::exception& error) { std::cout << error.what() << '\n'; }
            }
            defaults.kernel_y.pulse_enabled = ask_switch("Kernel Y pulse enabled", defaults.kernel_y.pulse_enabled);
            defaults.kernel_y.hold_ratio = ask_ratio("Kernel Y hold ratio", defaults.kernel_y.hold_ratio);
        }
    }
    defaults.chord.enabled = ask_switch("One key to two keys enabled", defaults.chord.enabled);
    if (defaults.chord.enabled) {
        defaults.chord.trigger = ask("Chord trigger (original input preserved)", defaults.chord.trigger);
        defaults.chord.first = ask("Chord first output", defaults.chord.first);
        defaults.chord.second = ask("Chord second output", defaults.chord.second);
    }
    defaults.x_pulse_enabled = ask_switch("X pulse enabled", defaults.x_pulse_enabled);
    defaults.x_keyboard_override_enabled = ask_switch(
        "X keyboard override enabled", defaults.x_keyboard_override_enabled);
    defaults.x_hold_ratio = ask_ratio("X hold ratio", defaults.x_hold_ratio);
    if (defaults.map_y) {
        defaults.y_pulse_enabled = ask_switch("Y pulse enabled", defaults.y_pulse_enabled);
        defaults.y_hold_ratio = ask_ratio("Y hold ratio", defaults.y_hold_ratio);
    }
    auto ask_axis = [&](bool y) {
        const auto axis = y ? "Y" : "X";
        auto& smoothing = y ? (defaults.map_y ? defaults.y_smoothing_factor : defaults.kernel_y.smoothing_factor) : defaults.x_smoothing_factor;
        const auto smoothing_label = std::format("{} smoothing factor (1=raw, 0=no axis mapping)", axis);
        smoothing = ask_ratio(smoothing_label.c_str(), smoothing);
        auto& start = y ? (defaults.map_y ? defaults.y_start_counts : defaults.kernel_y.start_counts) : defaults.x_start_counts;
        auto& reverse = y ? (defaults.map_y ? defaults.y_reverse_counts : defaults.kernel_y.reverse_counts) : defaults.x_reverse_counts;
        auto ask_count = [&](const char* label, int current, int minimum) {
            for (;;) {
                std::cout << axis << " " << label << " (" << minimum << "..10000, Enter keeps " << current << "): " << std::flush;
                const auto text = trim(read_setting());
                if (text.empty() && current >= minimum) return current;
                int value = 0;
                const auto [end, error] = std::from_chars(text.data(), text.data()+text.size(), value);
                if (error == std::errc{} && end == text.data()+text.size() && value >= minimum && value <= 10000) return value;
                std::cout << "Invalid threshold\n";
            }
        };
        start = ask_count("start counts", start, 1);
        reverse = ask_count("reverse counts", reverse, start);
        if (!(y ? (defaults.map_y ? defaults.y_pulse_enabled : defaults.kernel_y.pulse_enabled) : defaults.x_pulse_enabled)) return;
        auto& curve = y ? (defaults.map_y ? defaults.y_curve : defaults.kernel_y.curve) : defaults.x_curve;
        const auto curve_switch_label = std::format("{} sensitivity curve enabled", axis);
        curve.enabled = ask_switch(curve_switch_label.c_str(), curve.enabled);
        if (!curve.enabled) return;
        for (;;) {
            std::cout << axis << " full speed (1..1000000 counts/s, Enter keeps " << curve.full_speed << "): " << std::flush;
            const auto text = trim(read_setting());
            if (text.empty()) break;
            try {
                const auto value = parse_number(text);
                if (value < 1 || value > 1000000) throw std::runtime_error("Full speed must be 1..1000000");
                curve.full_speed = value; break;
            } catch (const std::exception& error) { std::cout << error.what() << '\n'; }
        }
        for (;;) {
            std::cout << axis << " curve points (input:output,..., Enter keeps " << format_curve(curve.points) << "): " << std::flush;
            const auto text = trim(read_setting());
            if (text.empty()) break;
            try { curve.points = parse_curve(text); break; }
            catch (const std::exception& error) { std::cout << error.what() << '\n'; }
        }
    };
    ask_axis(false);
    if (y_enabled(defaults)) ask_axis(true);
    validate(defaults);
    return defaults;
}

} // namespace mouse_mapping
