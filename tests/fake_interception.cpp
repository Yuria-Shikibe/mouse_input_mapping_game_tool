// Isolated test DLL: never opens a driver or generates real input.
#include <windows.h>
#include <interception.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <filesystem>
#include <vector>

namespace {
struct packet {
    int device = 0; // Zero means a timed pause.
    InterceptionMouseStroke mouse{};
    InterceptionKeyStroke key{};
};
std::vector<packet> inputs;
std::vector<packet> expected;
std::array<InterceptionFilter, 20> filters{};
std::size_t input_index = 0;
std::size_t output_index = 0;
bool failed = false;

packet key(unsigned int vk, bool down) {
    packet result;
    result.device = 1;
    const auto scan = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
    result.key.code = static_cast<unsigned short>(scan & 0xff);
    result.key.state = static_cast<unsigned short>((down ? 0 : INTERCEPTION_KEY_UP)
        | ((scan & 0xff00) == 0xe000 ? INTERCEPTION_KEY_E0 : 0));
    return result;
}
packet mouse(int x, int y, unsigned short state = 0, unsigned short flags = 0) {
    packet result;
    result.device = 11;
    result.mouse = {state, flags, 120, x, y, 0x1234abcd};
    return result;
}
}

extern "C" {
InterceptionContext interception_create_context() {
    inputs = {
        mouse(9, 1),
        key(VK_F8, true), key(VK_F8, true), key(VK_F8, false),
        mouse(-3, 4, INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN | INTERCEPTION_MOUSE_WHEEL, INTERCEPTION_MOUSE_MOVE_NOCOALESCE),
        mouse(1, 2), mouse(5, -2),
        key(VK_F24, true), packet{}, key(VK_F24, false),
        mouse(-3, 9), packet{},
        mouse(1234, 500, 0, INTERCEPTION_MOUSE_MOVE_ABSOLUTE),
        key(VK_F8, true), key(VK_F8, false), mouse(7, 3),
        key(VK_F8, true), key(VK_F8, false), mouse(3, 8)
    };
    expected = {
        mouse(9, 1), key(VK_F23, true),
        mouse(0, 4, INTERCEPTION_MOUSE_LEFT_BUTTON_DOWN | INTERCEPTION_MOUSE_WHEEL, INTERCEPTION_MOUSE_MOVE_NOCOALESCE),
        mouse(0, 2), key(VK_F23, false), key(VK_F24, true), mouse(0, -2),
        key(VK_F24, false), key(VK_F23, true), mouse(0, 9), key(VK_F23, false),
        mouse(1234, 500, 0, INTERCEPTION_MOUSE_MOVE_ABSOLUTE),
        mouse(7, 3), key(VK_F24, true), mouse(0, 8), key(VK_F24, false)
    };
    return &filters;
}

void interception_destroy_context(InterceptionContext) {
    if (std::any_of(filters.begin(), filters.end(), [](auto value) { return value != 0; })) failed = true;
    std::array<wchar_t, 32768> path{};
    const auto length = GetEnvironmentVariableW(L"MOUSE_MAPPING_TEST_REPORT", path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size()) return;
    std::ofstream output{std::filesystem::path(path.data())};
    output << (!failed && output_index == expected.size() && input_index == inputs.size() ? "PASS" : "FAIL")
        << " inputs=" << input_index << '/' << inputs.size()
        << " outputs=" << output_index << '/' << expected.size();
}

void interception_set_filter(InterceptionContext, InterceptionPredicate predicate, InterceptionFilter filter) {
    for (int device = 1; device <= 20; ++device) if (predicate(device)) filters[device - 1] = filter;
}
// Real driver regression: IOCTL_GET_FILTER may succeed without returning data,
// so the official library returns 0 even while input filtering is active.
InterceptionFilter interception_get_filter(InterceptionContext, InterceptionDevice) { return 0; }

InterceptionDevice interception_wait_with_timeout(InterceptionContext, unsigned long) {
    if (GetEnvironmentVariableW(L"MOUSE_MAPPING_TEST_IDLE", nullptr, 0) != 0) {
        Sleep(4);
        return 0;
    }
    // A zero readback must not prevent startup, but filters must still be set.
    if (std::any_of(filters.begin(), filters.end(), [](auto value) { return value != 0xffff; })) {
        failed = true;
        return 11;
    }
    if (input_index == inputs.size()) return 11; // Next receive simulates a driver failure.
    if (inputs[input_index].device == 0) {
        ++input_index;
        Sleep(75);
        return 0;
    }
    return inputs[input_index].device;
}

int interception_receive(InterceptionContext, InterceptionDevice device, InterceptionStroke* stroke, unsigned int count) {
    if (input_index == inputs.size() || count == 0) return 0;
    const auto& input = inputs[input_index++];
    if (input.device != device) { failed = true; return 0; }
    if (device <= 10) std::memcpy(stroke, &input.key, sizeof(input.key));
    else std::memcpy(stroke, &input.mouse, sizeof(input.mouse));
    return 1;
}

int interception_send(InterceptionContext, InterceptionDevice device, const InterceptionStroke* stroke, unsigned int count) {
    if (output_index >= expected.size() || count != 1) { failed = true; return 0; }
    const auto& wanted = expected[output_index];
    bool matches = wanted.device == device;
    if (device <= 10) {
        InterceptionKeyStroke actual{};
        std::memcpy(&actual, stroke, sizeof(actual));
        matches = matches && actual.code == wanted.key.code && actual.state == wanted.key.state;
    } else {
        InterceptionMouseStroke actual{};
        std::memcpy(&actual, stroke, sizeof(actual));
        matches = matches && actual.x == wanted.mouse.x && actual.y == wanted.mouse.y
            && actual.state == wanted.mouse.state && actual.flags == wanted.mouse.flags
            && actual.rolling == wanted.mouse.rolling && actual.information == wanted.mouse.information;
    }
    if (!matches) { failed = true; return 0; }
    ++output_index;
    return 1;
}
unsigned int interception_get_hardware_id(InterceptionContext, InterceptionDevice, void*, unsigned int) { return 0; }
}
