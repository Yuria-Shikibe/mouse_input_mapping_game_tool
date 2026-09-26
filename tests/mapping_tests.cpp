#include "config.hpp"
#include "xy_mapping.hpp"
#include <windows.h>
#include <functional>
#include <iostream>
#include <limits>
#include <sstream>
#include <vector>

using namespace mouse_mapping;
namespace {
int assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition) throw std::runtime_error(message);
}
template<class function_type>
void rejects(function_type&& action, const char* message) {
    bool failed = false;
    try { action(); } catch (const std::exception&) { failed = true; }
    require(failed, message);
}
time_point at(int ms) { return time_point{} + milliseconds(ms); }

void test_motion() {
    motion_filter filter;
    for (int time = 0; time < 1000; ++time)
        require(filter.update(time % 2 ? 1 : -1, at(time)) == direction::idle, "Alternating jitter activated mapping");
    filter.reset();
    require(filter.update(1, at(0)) == direction::idle, "First count should accumulate");
    require(filter.update(1, at(10)) == direction::idle, "Second count should accumulate");
    require(filter.update(1, at(20)) == direction::right, "Slow motion should activate at threshold");
    require(filter.update(3, at(60)) == direction::right, "Sustained motion should refresh hold");
    require(filter.tick(at(119)) == direction::right, "Released too early");
    require(filter.tick(at(120)) == direction::idle, "Must release at timeout even without packets");
    filter.reset();
    require(filter.update(-100, at(0)) == direction::left, "Fast motion should activate immediately");
    require(filter.update(2, at(1)) == direction::left, "Small reversal must not switch");
    require(filter.update(3, at(2)) == direction::left, "Reversal below threshold switched");
    require(filter.update(1, at(3)) == direction::right, "Reversal threshold should switch immediately");
    for (int time = 4; time < 63; ++time) filter.update(0, at(time));
    require(filter.tick(at(63)) == direction::idle, "Pure Y motion must not extend hold");
    filter.reset();
    filter.update(2, at(0));
    require(filter.update(1, at(30)) == direction::idle, "Expired samples must not accumulate");
    filter.reset();
    filter.update(3, at(0));
    for (int time = 1; time < 60; ++time) filter.update(time % 2 ? 1 : -1, at(time));
    require(filter.tick(at(60)) == direction::idle, "Jitter must not indefinitely extend hold");
    filter.reset();
    require(filter.update(std::numeric_limits<std::int32_t>::min(), at(0)) == direction::left, "Large negative delta overflow");
    require(filter.update(std::numeric_limits<std::int32_t>::max(), at(1)) == direction::right, "Large positive delta overflow");
    filter.reset();
    require(filter.tick(at(2)) == direction::idle, "Reset must clear held state");
    motion_filter high_rate;
    for (int sample = 0; sample < 8000; ++sample)
        high_rate.update(1, time_point{} + std::chrono::microseconds(sample * 125));
    require(high_rate.tick(at(1000)) == direction::right, "8 kHz sustained motion was lost");
    require(high_rate.tick(at(1060)) == direction::idle, "8 kHz motion failed to release");
}

void test_keys() {
    key_router router(0x1e, 0x20);
    std::vector<key_event> events;
    auto send = [&](key_event event) { events.push_back(event); };
    router.set_direction(direction::left, 1, send);
    router.set_direction(direction::left, 1, send);
    require(events.size() == 1 && events[0].down, "Mapping emitted repeats");
    router.set_direction(direction::right, 1, send);
    require(events.size() == 3 && !events[1].down && events[1].code == 0x1e
        && events[2].down && events[2].code == 0x20, "Switch must release before press");
    router.physical({2, 0x20, true}, send);
    router.set_direction(direction::idle, 1, send);
    require(events.size() == 3, "Mapping released physically held key");
    router.physical({2, 0x20, false}, send);
    require(events.size() == 4 && !events.back().down && events.back().device == 1, "Final release must match output device");
    require(!router.physical({1, 0x30, true}, send), "Unmapped key should pass through");
    events.clear();
    router.physical({1, 0x1e, true}, send);
    router.physical({2, 0x1e, true}, send);
    router.physical({1, 0x1e, false}, send);
    require(events.size() == 1, "Second physical keyboard lost ownership");
    router.physical({2, 0x1e, false}, send);
    require(events.size() == 2 && !events.back().down, "Physical keyboard merge release failed");
    router.physical({1, 0x1e, true}, send);
    router.physical({2, 0x1e, true}, send);
    router.physical({2, 0x1e, true}, send);
    require(events.back().device == 1, "Repeats must use the same output device as the original press");
    router.physical({1, 0x1e, false}, send);
    router.physical({2, 0x1e, false}, send);
    router.set_direction(direction::left, 1, send);
    router.physical({1, 0x1e, true}, send);
    router.physical({1, 0x1e, false}, send);
    require(events.back().down, "Physical key-up released active mapping");
    auto failed_send = [](key_event) { throw std::runtime_error("Simulated send failure"); };
    rejects([&] { router.set_direction(direction::idle, 1, failed_send); }, "Send failure hidden");
    router.set_direction(direction::idle, 1, send);
    require(!events.back().down, "Cleanup did not retry failed release");
    const auto count = events.size();
    for (int index = 0; index < 10; ++index) router.set_direction(direction::idle, 1, send);
    require(events.size() == count, "Idle cleanup emitted spurious key events");
    key_router failed_press(0x1e, 0x20);
    rejects([&] { failed_press.set_direction(direction::left, 1, failed_send); }, "Press failure hidden");
    failed_press.set_direction(direction::idle, 1, send);
    require(events.size() == count, "Failed press caused unowned release");
    router.physical({1, 0x1e, false}, send);
    require(events.size() == count + 1 && !events.back().down, "Pre-existing physical hold was not released");
    key_router extended(0xe04b, 0xe04d);
    extended.set_direction(direction::left, 3, send);
    extended.set_direction(direction::idle, 3, send);
    require(events.back().code == 0xe04b && !events.back().down, "Extended key lost scan code prefix");

    std::vector<key_event> exclusive_events;
    auto exclusive_send = [&](key_event event) { exclusive_events.push_back(event); };
    key_router exclusive(0x1e, 0x20, true);
    exclusive.set_direction(direction::right, 1, exclusive_send);
    exclusive.physical({2, 0x1e, true}, exclusive_send);
    require(exclusive_events.size() == 3 && exclusive_events[1].code == 0x20
        && !exclusive_events[1].down && exclusive_events[2].code == 0x1e
        && exclusive_events[2].down, "Physical left did not override mapped right");
    exclusive.physical({2, 0x1e, false}, exclusive_send);
    require(exclusive_events.size() == 5 && exclusive_events[3].code == 0x1e
        && !exclusive_events[3].down && exclusive_events[4].code == 0x20
        && exclusive_events[4].down && exclusive_events[4].device == 1,
        "Mapped direction did not resume on its original keyboard");
    exclusive.physical({2, 0x20, true}, exclusive_send);
    exclusive.physical({2, 0x20, false}, exclusive_send);
    require(exclusive_events.size() == 5, "Same-direction physical ownership emitted transitions");
    exclusive.physical({2, 0x1e, true}, exclusive_send);
    exclusive.physical({3, 0x1e, true}, exclusive_send);
    const auto first_release = exclusive_events.size();
    exclusive.physical({2, 0x1e, false}, exclusive_send);
    require(exclusive_events.size() == first_release, "First keyboard release ended shared override");
    exclusive.set_direction(direction::idle, 1, exclusive_send);
    exclusive.physical({3, 0x1e, false}, exclusive_send);
    require(exclusive_events.size() == first_release + 1 && !exclusive_events.back().down
        && exclusive_events.back().code == 0x1e, "Expired mapping resumed after keyboard override");

    toggle_latch toggle;
    require(toggle.update(1, true), "First toggle press ignored");
    require(!toggle.update(1, true), "Autorepeat toggled mapping");
    require(!toggle.update(1, false), "Key-up toggled mapping");
    require(toggle.update(1, true), "Second toggle press ignored");
}

void test_xy() {
    configuration config;
    config.map_y = true;
    config.y_pulse_enabled = false; // Existing continuous-hold behavior.
    validate(config);
    require(config.up_key == parse_key("S") && config.down_key == parse_key("W"), "Wrong Y defaults");
    std::stringstream serialized;
    write_config(serialized, config);
    const auto loaded = read_config(serialized, true);
    require(loaded.map_y && loaded.up_key == config.up_key && loaded.down_key == config.down_key
        && loaded.mouse_keys == config.mouse_keys && loaded.wheel_keys == config.wheel_keys
        && loaded.x_keyboard_override_enabled && !loaded.y_pulse_enabled,
        "XY/button config round trip failed");
    for (const auto* text : {"up_key=A", "down_key=S", "up_key=F8", "down_key=PAUSE",
            "lmb_key=A", "rmb_key=A", "cmb_key=F8", "x1_key=PAUSE", "x2_key=F\nx2_key=G",
            "wheel_up_key=A", "wheel_down_key=T", "wheel_up_key=PAUSE"})
        rejects([&] { std::istringstream input(text); read_config(input, true); }, "Invalid XY config accepted");
    std::istringstream shared_wheel("wheel_up_key=R\nwheel_down_key=R\n");
    const auto shared = read_config(shared_wheel, true);
    require(shared.wheel_keys[0] == shared.wheel_keys[1], "Shared wheel target rejected");
    std::istringstream legacy("left_key=W\n");
    const auto legacy_config = read_config(legacy);
    require(legacy_config.left_key == parse_key("W") && legacy_config.x_keyboard_override_enabled,
        "Legacy config did not receive compatible defaults");
    xy_mapping mapping(config);
    std::vector<key_event> events;
    auto send = [&](key_event event) { events.push_back(event); };
    mapping.update(-3, -3, at(0), send);
    require(events.size() == 2 && events[0].code == config.left_key && events[1].code == config.up_key
        && events[0].down && events[1].down, "Up-left diagonal failed");
    mapping.update(0, 6, at(10), send);
    require(events.size() == 4 && events[2].code == config.up_key && !events[2].down
        && events[3].code == config.down_key && events[3].down, "Y reversal order failed");
    mapping.tick(at(60), send);
    require(events.size() == 5 && events.back().code == config.left_key && !events.back().down, "Independent X timeout failed");
    mapping.tick(at(70), send);
    require(events.size() == 6 && events.back().code == config.down_key && !events.back().down, "Independent Y timeout failed");
    mapping.update(3, -3, at(100), send);
    mapping.physical({1, config.up_key, true}, send);
    mapping.release(send);
    require(events.size() == 9 && events.back().code == config.right_key && !events.back().down, "Cleanup released physical W");
    mapping.physical({1, config.up_key, false}, send);
    require(events.size() == 10 && !events.back().down, "Physical Y release failed");
    mapping.update(-3, 3, at(200), send);
    auto fail_x = [&](key_event event) {
        if (event.code == config.left_key) throw std::runtime_error("X output failure");
        send(event);
    };
    rejects([&] { mapping.release(fail_x); }, "Release error hidden");
    require(events.back().code == config.down_key && !events.back().down, "X failure skipped Y cleanup");
    mapping.release(send);
    require(events.back().code == config.left_key && !events.back().down, "Failed X release not retried");
}

void test_buttons() {
    configuration config;
    config.y_pulse_enabled = false;
    button_mapping buttons(config.mouse_keys);
    std::vector<key_event> events;
    auto send = [&](key_event event) { events.push_back(event); };
    for (std::size_t index = 0; index < 5; ++index) {
        events.clear();
        buttons.update(11, static_cast<unsigned short>(1u << (index * 2)), send);
        buttons.update(11, static_cast<unsigned short>(1u << (index * 2)), send);
        require(events.size() == 1 && events[0].down && events[0].code == config.mouse_keys[index], "Mouse button press/repeat failed");
        buttons.update(12, static_cast<unsigned short>(1u << (index * 2)), send);
        buttons.update(11, static_cast<unsigned short>(2u << (index * 2)), send);
        require(events.size() == 1, "One mouse released another mouse's hold");
        buttons.update(12, static_cast<unsigned short>(2u << (index * 2)), send);
        require(events.size() == 2 && !events.back().down, "Mouse button release failed");
    }
    events.clear();
    buttons.update(11, 0x155, send);
    require(events.size() == 5, "Simultaneous mouse buttons lost");
    buttons.physical({1, config.mouse_keys[0], true}, send);
    buttons.remove(11, send);
    require(events.size() == 9, "Unplug cleanup lost physical ownership");
    buttons.physical({1, config.mouse_keys[0], false}, send);
    require(events.size() == 10 && !events.back().down, "Physical mouse-mapped key not released");
    events.clear();
    buttons.update(11, RI_MOUSE_WHEEL | RI_MOUSE_HWHEEL, send);
    require(events.empty(), "Wheel generated button keys");
    buttons.update(11, RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_LEFT_BUTTON_UP, send);
    require(events.size() == 2 && events[0].down && !events[1].down, "Same-packet click lost");
    buttons.update(11, 0x155, send);
    const auto before = events.size();
    auto fail_first = [&](key_event event) {
        if (event.code == config.mouse_keys[0]) throw std::runtime_error("Failed LMB release");
        send(event);
    };
    rejects([&] { buttons.release(fail_first); }, "Mouse release error hidden");
    require(events.size() == before + 4, "Failed LMB cleanup skipped other buttons");
    buttons.release(send);
    require(events.size() == before + 5 && !events.back().down, "Mouse cleanup did not retry failure");
    xy_mapping combined(config);
    events.clear();
    combined.update(3, -3, at(0), send);
    combined.buttons(11, RI_MOUSE_LEFT_BUTTON_DOWN, send);
    combined.tick(at(100), send);
    require(events.size() == 5, "Axis timeout released mouse button");
    combined.release(send);
    require(events.size() == 6 && events.back().code == config.mouse_keys[0] && !events.back().down, "Toggle cleanup left mouse button held");
}

void test_wheel() {
    configuration config;
    config.map_y = true;
    xy_mapping mapping(config);
    std::vector<key_event> events;
    auto send = [&](key_event event) { events.push_back(event); };
    mapping.wheel(11, 60, at(0), send);
    require(events.empty(), "Partial wheel notch emitted a key");
    mapping.wheel(11, 60, at(0), send);
    require(events.size() == 1 && events[0].code == config.wheel_keys[0]
        && events[0].down, "Wheel up mapping failed");
    mapping.tick(at(9), send);
    require(events.size() == 1, "Wheel press released too early");
    mapping.tick(at(10), send);
    require(events.size() == 2 && !events[1].down, "Wheel press did not release");
    mapping.wheel(11, -240, at(20), send);
    mapping.tick(at(30), send);
    mapping.tick(at(40), send);
    mapping.tick(at(50), send);
    require(events.size() == 6 && events[2].code == config.wheel_keys[1]
        && !events[3].down && events[4].down && !events[5].down,
        "Multiple wheel down notches failed");
    mapping.wheel(11, 60, at(60), send);
    mapping.remove_mouse(11, send);
    mapping.wheel(11, 60, at(70), send);
    require(events.size() == 6, "Unplug retained partial wheel travel");
    mapping.physical({1, config.wheel_keys[0], true}, send);
    mapping.wheel(11, 120, at(80), send);
    mapping.tick(at(90), send);
    require(events.size() == 7, "Wheel pulse interrupted physical key ownership");
    mapping.physical({1, config.wheel_keys[0], false}, send);
    require(events.size() == 8 && !events.back().down, "Physical wheel target release failed");
    mapping.release(send);
}

void test_pulses() {
    configuration config;
    config.map_y = true;
    require(config.y_hold_ratio == 0.65 && config.mouse_keys[0] == parse_key("SUBTRACT")
        && config.mouse_keys[2] == parse_key("T") && config.wheel_keys[0] == parse_key("R")
        && config.wheel_keys[0] == config.wheel_keys[1], "User configuration defaults changed");
    config.y_hold_ratio = 1.0 / 3.0; // Exercise the 10 ms timing separately.
    xy_mapping mapping(config);
    std::vector<key_event> events;
    auto send = [&](key_event event) { events.push_back(event); };
    mapping.update(-10, -10, at(0), send);
    require(events.size() == 2 && events[0].code == config.left_key
        && events[1].code == config.up_key && events[1].down, "Default X hold/Y pulse failed");
    mapping.tick(at(9), send);
    require(events.size() == 2, "Y pulse released early");
    mapping.tick(at(10), send);
    require(events.size() == 3 && !events.back().down && events.back().code == config.up_key,
        "Y pulse did not release after 1/3 period");
    mapping.update(0, -10, at(20), send);
    require(events.size() == 3, "Y pulse ignored release gap");
    mapping.tick(at(30), send);
    require(events.size() == 4 && events.back().down, "Y pulse did not repeat");
    mapping.tick(at(40), send);
    mapping.tick(at(60), send);
    require(events.back().code == config.left_key && !events.back().down,
        "X hold did not time out independently");
    mapping.release(send);

    config.x_pulse_enabled = true;
    config.x_hold_ratio = 0;
    config.y_hold_ratio = 1;
    xy_mapping endpoints(config);
    events.clear();
    endpoints.update(100, 100, at(0), send);
    require(events.size() == 1 && events[0].code == config.down_key,
        "Zero ratio emitted X or full ratio failed to press Y");
    endpoints.tick(at(30), send);
    require(events.size() == 2 && !events.back().down, "Ratio 1 must still release");
    endpoints.release(send);

    config.y_hold_ratio = 0.25;
    config.x_pulse_enabled = false;
    xy_mapping quarter(config);
    events.clear();
    quarter.update(0, -10, at(0), send);
    quarter.tick(at(7), send);
    require(events.size() == 1, "Quarter pulse released too early");
    quarter.tick(at(8), send);
    require(events.size() == 2 && !events.back().down, "Quarter pulse hold duration wrong");
    quarter.release(send);

    axis_mapping slow(config.filter, config.left_key, config.right_key, true, 1.0 / 3.0, 30);
    axis_mapping fast(config.filter, config.left_key, config.right_key, true, 1.0 / 3.0, 30);
    int slow_presses = 0, fast_presses = 0;
    auto slow_send = [&](key_event event) { if (event.down) ++slow_presses; };
    auto fast_send = [&](key_event event) { if (event.down) ++fast_presses; };
    for (int t = 0; t <= 150; t += 10) {
        slow.update(2, at(t), 1, slow_send);
        fast.update(10, at(t), 1, fast_send);
        slow.tick(at(t), 1, slow_send);
        fast.tick(at(t), 1, fast_send);
    }
    require(fast_presses > slow_presses, "Faster displacement did not increase pulse density");
    slow.release(1, slow_send);
    fast.release(1, fast_send);
}

void test_config() {
    configuration config;
    config.left_key = parse_key("LEFT");
    config.right_key = parse_key("right");
    config.toggle_key = parse_key("F8");
    require(config.left_key == 0xe04b && config.right_key == 0xe04d, "Arrow keys must retain E0 prefix");
    require(parse_key("HOME") == 0xe047 && parse_key("DELETE") == 0xe053, "Navigation keys lost E0 prefix");
    require(parse_key("RCTRL") == 0xe01d && parse_key("DIVIDE") == 0xe035, "Right modifier/keypad divide lost E0 prefix");
    for (int number = 1; number <= 24; ++number) {
        const auto name = "F" + std::to_string(number);
        require(key_name(parse_key(name)) == name, "Function key name did not round trip");
    }
    for (const auto* name : {"A", "D", "LEFT", "RIGHT", "HOME", "DELETE", "RCTRL", "NUM0", "DIVIDE", "NUM_ENTER", "LWIN", "RWIN", "APPS"})
        require(key_name(parse_key(name)) == name, "Readable key name is ambiguous");
    require(describe_key(parse_key("F8")) == "F8 (0x0042)", "Function key summary is not readable");
    require(parse_key("0xe04b") == config.left_key, "Hex scan code parsing failed");
    std::stringstream serialized;
    write_config(serialized, config);
    const auto result = read_config(serialized);
    require(result.left_key == config.left_key && result.right_key == config.right_key
        && result.toggle_key == config.toggle_key && result.filter.release_ms == 60
        && result.x_keyboard_override_enabled, "Config round trip failed");
    std::istringstream override_disabled("x_keyboard_override_enabled=0\n");
    require(!read_config(override_disabled).x_keyboard_override_enabled,
        "X keyboard override could not be disabled");
    const std::vector<std::string> invalid{
        "left_key=A\nright_key=A", "toggle_key=A", "unknown=1", "window_ms=0", "window_ms=1x",
        "start_counts=-1", "start_counts=7\nreverse_counts=6", "release_ms=20", "release_ms=2001",
        "left_key=0xe11d", "left_key=0x0000", "left_key=0x10000", "left_key=PAUSE",
        "toggle_key=Ctrl+F8", "left_key=A\nleft_key=D", "[invalid]", "left_key",
        "window_ms=99999999999999999999", "x_pulse_enabled=2", "x_keyboard_override_enabled=2",
        "x_hold_ratio=-0.1", "x_hold_ratio=1.1",
        "x_hold_ratio=nan", "pulse_period_ms=1"
    };
    for (const auto& text : invalid) rejects([&] { std::istringstream input(text); read_config(input); }, "Invalid config accepted");
    std::istringstream bom("\xef\xbb\xbf[mapping]\nleft_key = LEFT ; comment\nright_key=RIGHT\n");
    require(read_config(bom).left_key == 0xe04b, "BOM/comments/whitespace parsing failed");
    const auto path = std::filesystem::temp_directory_path()
        / (L"mouse_mapping_test_" + std::to_wstring(GetCurrentProcessId()) + L".ini");
    try {
        save_config(path, config);
        require(load_config(path).right_key == config.right_key, "File persistence failed");
        config.filter.release_ms = 80;
        save_config(path, config);
        require(load_config(path).filter.release_ms == 80, "Atomic config replacement failed");
        std::filesystem::remove(path);
    } catch (...) { std::filesystem::remove(path); throw; }
}
}

int main() {
    try {
        test_motion();
        test_keys();
        test_xy();
        test_pulses();
        test_buttons();
        test_wheel();
        test_config();
        std::cout << "Passed " << assertions << " assertions.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED after " << assertions << " assertions: " << error.what() << '\n';
        return 1;
    }
}
