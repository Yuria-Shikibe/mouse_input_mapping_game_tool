#include "config.hpp"
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

    toggle_latch toggle;
    require(toggle.update(1, true), "First toggle press ignored");
    require(!toggle.update(1, true), "Autorepeat toggled mapping");
    require(!toggle.update(1, false), "Key-up toggled mapping");
    require(toggle.update(1, true), "Second toggle press ignored");
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
        && result.toggle_key == config.toggle_key && result.filter.release_ms == 60, "Config round trip failed");
    const std::vector<std::string> invalid{
        "left_key=A\nright_key=A", "toggle_key=A", "unknown=1", "window_ms=0", "window_ms=1x",
        "start_counts=-1", "start_counts=7\nreverse_counts=6", "release_ms=20", "release_ms=2001",
        "left_key=0xe11d", "left_key=0x0000", "left_key=0x10000", "left_key=PAUSE",
        "toggle_key=Ctrl+F8", "left_key=A\nleft_key=D", "[invalid]", "left_key",
        "window_ms=99999999999999999999"
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
        test_config();
        std::cout << "Passed " << assertions << " assertions.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED after " << assertions << " assertions: " << error.what() << '\n';
        return 1;
    }
}
