// Exercise the actual callbacks with synthetic Raw Input and an in-memory
// SendInput sink. No hook installation or real input injection takes place.
#include <windows.h>
#include <mmsystem.h>
#include <vector>
#include <stdexcept>
#include <iostream>

namespace {
RAWINPUT packet{};
std::vector<INPUT> sent;
bool reject_send = false, reject_read = false;
UINT WINAPI fake_send(UINT count, LPINPUT inputs, int) {
    if (reject_send) return 0;
    sent.insert(sent.end(), inputs, inputs + count);
    return count;
}
UINT WINAPI fake_read(HRAWINPUT, UINT, LPVOID data, PUINT size, UINT) {
    if (reject_read) return UINT(-1);
    if (*size < sizeof(packet)) return UINT(-1);
    *static_cast<RAWINPUT*>(data) = packet;
    *size = sizeof(packet);
    return sizeof(packet);
}
SHORT WINAPI fake_state(int) { return 0; }
BOOL WINAPI fake_sound(LPCWSTR, HMODULE, DWORD) { return TRUE; }
LRESULT WINAPI fake_default(HWND, UINT, WPARAM, LPARAM) { return 0; }
LRESULT WINAPI fake_next(HHOOK, int, WPARAM, LPARAM) { return 123; }
}
#define SendInput fake_send
#define GetRawInputData fake_read
#define GetAsyncKeyState fake_state
#define PlaySoundW fake_sound
#define DefWindowProcW fake_default
#define CallNextHookEx fake_next
#include "../src/user_runtime.cpp"
#undef SendInput
#undef GetRawInputData
#undef GetAsyncKeyState
#undef PlaySoundW
#undef DefWindowProcW
#undef CallNextHookEx

using namespace mouse_mapping;
namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
LRESULT key(key_code code, bool down, bool injected = false) {
    KBDLLHOOKSTRUCT event{};
    event.scanCode = code & 0xff;
    event.flags = (down ? 0 : LLKHF_UP) | ((code & 0xff00) ? LLKHF_EXTENDED : 0)
        | (injected ? LLKHF_INJECTED : 0);
    return keyboard_proc(HC_ACTION, down ? WM_KEYDOWN : WM_KEYUP, reinterpret_cast<LPARAM>(&event));
}
void mouse(LONG x, LONG y, USHORT flags = 0, USHORT mode = 0, USHORT wheel_data = 0) {
    packet = {};
    packet.header.dwType = RIM_TYPEMOUSE;
    packet.header.hDevice = reinterpret_cast<HANDLE>(11);
    packet.data.mouse.lLastX = x;
    packet.data.mouse.lLastY = y;
    packet.data.mouse.usButtonFlags = flags;
    packet.data.mouse.usButtonData = wheel_data;
    packet.data.mouse.usFlags = mode;
    window_proc(nullptr, WM_INPUT, RIM_INPUTSINK, 0);
}
}
int main() {
    try {
        configuration config;
        config.map_y = true;
        config.y_pulse_enabled = false;
        config.mouse_keys[4] = parse_key("RIGHT");
        session state(config);
        active = &state;
        stopping = false;
        mouse(10, 10, RI_MOUSE_LEFT_BUTTON_DOWN);
        require(sent.empty(), "OFF generated mapped keys");
        require(key(config.toggle_key, true) == 1 && state.mapping, "Toggle did not enable");
        key(config.toggle_key, true);
        require(state.mapping != nullptr, "Toggle repeat disabled mapping");
        key(config.toggle_key, false);
        mouse(-3, -3, RI_MOUSE_LEFT_BUTTON_DOWN | RI_MOUSE_BUTTON_5_DOWN);
        require(sent.size() == 4, "Combined XY/buttons raw packet lost events");
        require(sent[0].ki.wScan == config.mouse_keys[0] && sent[1].ki.wScan == 0x4d
            && (sent[1].ki.dwFlags & KEYEVENTF_EXTENDEDKEY), "Button scan code / E0 output wrong");
        require(key(config.mouse_keys[0], true, true) == 123, "Injected keyboard input was intercepted");
        require(sent.size() == 4, "Injected output fed back into mapping");
        require(key(config.mouse_keys[0], true) == 1, "Physical mapped key was not handled");
        mouse(0, 0, RI_MOUSE_LEFT_BUTTON_UP);
        require(sent.size() == 4, "Mouse up released physically held key");
        key(config.mouse_keys[0], false);
        require(sent.size() == 5 && (sent.back().ki.dwFlags & KEYEVENTF_KEYUP), "Physical release lost");
        window_proc(nullptr, WM_INPUT_DEVICE_CHANGE, GIDC_REMOVAL, 11);
        require(sent.size() == 6 && sent.back().ki.wScan == 0x4d, "Mouse unplug left side-key held");
        const auto before = sent.size();
        mouse(100, 100, RI_MOUSE_RIGHT_BUTTON_DOWN, MOUSE_MOVE_ABSOLUTE);
        require(sent.size() == before + 1 && sent.back().ki.wScan == config.mouse_keys[1], "Absolute device button mapping failed");
        key(config.toggle_key, true);
        key(config.toggle_key, false);
        require(!state.mapping && sent.size() == before + 4, "Disable did not release axes and buttons");
        require(key(config.left_key, true) == 123, "OFF intercepted normal keys");
        key(config.toggle_key, true);
        key(config.toggle_key, false);
        reject_send = true;
        mouse(0, 0, RI_MOUSE_MIDDLE_BUTTON_DOWN);
        require(stopping && state.failure, "SendInput failure escaped callback handling");
        reject_send = false;
        stopping = false;
        state.failure = nullptr;
        reject_read = true;
        mouse(0, 0);
        require(stopping && state.failure, "Raw Input read failure ignored");
        reject_read = false;
        stopping = false;
        state.failure = nullptr;
        const auto wheel_before = sent.size();
        const auto wheel_time = clock_type::now();
        mouse(0, 0, RI_MOUSE_WHEEL, 0, 120);
        require(sent.size() == wheel_before + 1 && sent[wheel_before].ki.wScan == config.wheel_keys[0],
            "Raw wheel up callback mapping failed");
        state.mapping->tick(wheel_time + milliseconds(11), send_key);
        require(sent.size() == wheel_before + 2 && (sent.back().ki.dwFlags & KEYEVENTF_KEYUP),
            "Raw wheel up release failed");
        mouse(0, 0, RI_MOUSE_WHEEL, 0, static_cast<USHORT>(-120));
        state.mapping->tick(wheel_time + milliseconds(22), send_key);
        require(sent.size() == wheel_before + 3 && sent[wheel_before + 2].ki.wScan == config.wheel_keys[1],
            "Raw wheel down callback mapping failed");
        std::cout << "User runtime callbacks passed (no real input generated).\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
