#pragma once
#include <string>
#include <vector>

namespace mouse_mapping {
// Presentation only: configuration names and persisted values stay in config_document.
enum class panel_color { general, x_axis, y_axis, pulse, curve, buttons, pie };
struct settings_panel {
    std::wstring title;
    panel_color color = panel_color::general;
    std::string toggle;
    std::vector<std::string> fields;
    std::vector<settings_panel> children;
};

inline settings_panel axis_panel(const wchar_t* title, const std::string& axis,
    std::vector<std::string> keys, panel_color color, std::string toggle = {}) {
    for (const auto* suffix : {"smoothing_factor", "start_counts", "reverse_counts"})
        keys.push_back(axis + suffix);
    if (axis == "x_") keys.push_back("x_keyboard_override_enabled");
    return {title, color, std::move(toggle), std::move(keys), {
        {L"脉冲模式", panel_color::pulse, axis + "pulse_enabled", {axis + "hold_ratio"}, {
            {L"灵敏度曲线", panel_color::curve, axis + "curve_enabled",
                {axis + "curve_full_speed", axis + "curve_points"}, {}}
        }}
    }};
}

inline std::vector<settings_panel> settings_panels() {
    return {
        {L"共享 · 开关与旁路", panel_color::general, {},
            {"toggle_key", "bypass_key_1", "bypass_key_2"}, {}},
        {L"共享 · 采样与时序", panel_color::general, {},
            {"window_ms", "release_ms", "pulse_period_ms"}, {}},
        axis_panel(L"共享 · X 轴映射", "x_", {"left_key", "right_key"}, panel_color::x_axis),
        axis_panel(L"用户态 · Y 轴映射", "y_", {"up_key", "down_key"}, panel_color::y_axis),
        axis_panel(L"内核态 · Y 轴映射", "kernel_y_",
            {"kernel_y_up_key", "kernel_y_down_key"}, panel_color::y_axis, "kernel_y_enabled"),
        {L"内核态 · 原始 Y 输入阻断（独立开关）", panel_color::y_axis, {}, {"kernel_y_block"}, {}},
        {L"用户态 · 鼠标五键", panel_color::buttons, {},
            {"lmb_key", "rmb_key", "cmb_key", "x1_key", "x2_key"}, {}},
        {L"用户态 · 滚轮映射", panel_color::buttons, {}, {"wheel_up_key", "wheel_down_key"}, {}},
        {L"共享 · 一键双键", panel_color::buttons, "chord_enabled",
            {"chord_trigger_key", "chord_first_key", "chord_second_key"}, {}},
        {L"共享 · Pie 方向菜单", panel_color::pie, {}, {"pie_trigger", "pie_visual_enabled"}, {
            {L"选区与输出", panel_color::pie, {},
                {"pie_deadzone_counts", "pie_radius_counts", "pie_hysteresis_degrees", "pie_key_hold_ms"}, {}}
        }}
    };
}
}
