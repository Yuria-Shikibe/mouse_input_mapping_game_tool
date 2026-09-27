#include "config_document.hpp"
#include <array>
#include <format>
#include <sstream>
#include <stdexcept>

namespace mouse_mapping {
std::span<const config_field> config_fields() {
    static constexpr auto fields = std::to_array<config_field>({
        {"left_key", L"X− 左移", field_kind::key, false},
        {"right_key", L"X+ 右移", field_kind::key, false},
        {"up_key", L"Y− 上移", field_kind::key, true},
        {"down_key", L"Y+ 下移", field_kind::key, true},
        {"lmb_key", L"鼠标左键", field_kind::key, true},
        {"rmb_key", L"鼠标右键", field_kind::key, true},
        {"cmb_key", L"鼠标中键", field_kind::key, true},
        {"x1_key", L"鼠标侧键 1", field_kind::key, true},
        {"x2_key", L"鼠标侧键 2", field_kind::key, true},
        {"wheel_up_key", L"滚轮向上", field_kind::key, true},
        {"wheel_down_key", L"滚轮向下", field_kind::key, true},
        {"toggle_key", L"映射开关", field_kind::key, false},
        {"window_ms", L"触发窗口（1–1000 ms）", field_kind::number, false, L"在此时间内累计鼠标净位移，达到启动或反向阈值才确认方向；过期位移丢弃，正反位移互相抵消。长按模式直接使用此值；脉冲模式实际使用 max(此值, 2 × 释放超时, 120 ms)。这是时间窗口，不是应用窗口。"},
        {"release_ms", L"释放超时（触发窗口–2000 ms）", field_kind::number, false, L"超过此时间未再次确认方向就释放合成方向键。值越小停止越快，但慢移可能断续；必须不小于触发窗口。脉冲模式超时还会清除待发脉冲。"},
        {"x_keyboard_override_enabled", L"X 实体键盘优先", field_kind::toggle, false, L"实体 X 左右映射键按住时暂停鼠标 X 合成输出。最后一个实体方向键松开后，恢复仍有效的鼠标方向。两个后端使用同一设置。"},
        {"x_pulse_enabled", L"X 脉冲模式", field_kind::toggle, false, L"关闭时持续按住 X 方向键；开启后由位移触发短按，移动越快脉冲越密，最多积压一次。停止移动不会无限重复。内核态开启后仍过滤原始 X。"},
        {"x_hold_ratio", L"X 按住比例（0–1）", field_kind::number, false, L"仅在 X 脉冲模式下生效。按住时间约为周期 × 比例；0 不输出，1 仍保留至少 1 ms 松开间隔。长按模式忽略此比例。"},
        {"y_pulse_enabled", L"Y 脉冲模式", field_kind::toggle, true, L"仅用户态支持：关闭时持续按住 Y 方向键；开启后由 Y 位移触发短按，与 X 独立累计和释放。"},
        {"y_hold_ratio", L"Y 按住比例（0–1）", field_kind::number, true, L"仅在 Y 脉冲模式下生效。0 不输出，1 仍保留至少 1 ms 松开间隔。长按模式忽略此比例。"},
        {"x_smoothing_factor", L"X 插值平滑（0–1）", field_kind::number, false, L"默认 1，直接使用最新输入；0 不产生该轴移动映射。越小越平滑，响应和转向延迟越明显。按该轴非零相对位移进行 lerp，效果与鼠标报告频率有关；长按和脉冲模式均生效。停止输入达到释放超时后清空平滑历史。"},
        {"x_start_counts", L"X 启动阈值（1–10000 counts）", field_kind::number, false, L"该轴独立的净位移启动阈值；越小越灵敏。长按和脉冲模式均使用。"},
        {"x_reverse_counts", L"X 反向阈值（启动阈值–10000）", field_kind::number, false, L"该轴反向所需净位移，不小于该轴启动阈值。"},
        {"x_curve_enabled", L"X 灵敏度曲线", field_kind::toggle, false, L"仅脉冲模式可用。启用后用速度曲线输出替代固定按住比例。"},
        {"x_curve_full_speed", L"X 满量程速度（counts/s）", field_kind::number, false, L"范围 1–1000000，默认 1000。该轴速度达到此值时曲线横坐标为 1；与鼠标 DPI 有关。使用最近 30 ms 的净位移估速。"},
        {"x_curve_points", L"X 速度 → 按住比例", field_kind::curve, false, L"横轴：速度 / 满量程速度；纵轴：脉冲按住比例。均为 0–1。曲线为 0 时不输出；为 1 时仍至少松开 1 ms。"},
        {"y_smoothing_factor", L"Y 插值平滑（0–1）", field_kind::number, true, L"仅用户态支持，与 X 独立。默认 0.85；1 直接使用最新输入，0 不产生该轴移动映射。越小越平滑，响应和转向延迟越明显。按该轴非零相对位移进行 lerp，效果与报告频率有关；长按和脉冲模式均生效。停止输入达到释放超时后清空历史。"},
        {"y_start_counts", L"Y 启动阈值（1–10000 counts）", field_kind::number, true, L"该轴独立的净位移启动阈值；越小越灵敏。长按和脉冲模式均使用。"},
        {"y_reverse_counts", L"Y 反向阈值（启动阈值–10000）", field_kind::number, true, L"该轴反向所需净位移，不小于该轴启动阈值。"},
        {"y_curve_enabled", L"Y 灵敏度曲线", field_kind::toggle, true, L"仅脉冲模式可用。启用后用速度曲线输出替代固定按住比例。"},
        {"y_curve_full_speed", L"Y 满量程速度（counts/s）", field_kind::number, true, L"范围 1–1000000，默认 1400。该轴速度达到此值时曲线横坐标为 1；与鼠标 DPI 有关。使用最近 30 ms 的净位移估速。"},
        {"y_curve_points", L"Y 速度 → 按住比例", field_kind::curve, true, L"横轴：速度 / 满量程速度；纵轴：脉冲按住比例。均为 0–1。曲线为 0 时不输出；为 1 时仍至少松开 1 ms。"},
        {"pulse_period_ms", L"共享脉冲周期（2–1000 ms）", field_kind::number, false, L"X/Y 共用的单次脉冲周期，范围 2–1000 ms。与按住比例一起决定按住和松开时间；至少保留 1 ms 松开。位移不足时不会按固定频率自动重复。"}
    });
    return fields;
}

void config_document::assign(const configuration& config) {
    std::ostringstream output;
    write_config(output, config);
    std::istringstream input(output.str());
    std::map<std::string, std::string> next;
    std::string line;
    while (std::getline(input, line)) {
        const auto equal = line.find('=');
        if (equal == std::string::npos) continue;
        auto value = line.substr(equal + 1);
        value = value.substr(0, value.find(';'));
        const auto last = value.find_last_not_of(" \t\r");
        value.resize(last == std::string::npos ? 0 : last + 1);
        next.emplace(line.substr(0, equal), value);
    }
    values = std::move(next);
}
void config_document::reset(bool user, const std::filesystem::path& file) {
    configuration config;
    config.map_y = user;
    assign(config);
    user_mode = user;
    path = file;
    dirty = false;
}
void config_document::open(bool user, const std::filesystem::path& file) {
    const auto absolute = std::filesystem::absolute(file);
    const auto config = load_config(absolute, true);
    assign(config);
    user_mode = user;
    path = absolute;
    dirty = false;
}
void config_document::defaults() {
    configuration config;
    config.map_y = user_mode;
    assign(config);
    dirty = true;
}
void config_document::set(const std::string& name, const std::string& value) {
    auto& current = values.at(name);
    if (current != value) { current = value; dirty = true; }
}
configuration config_document::snapshot() const {
    std::ostringstream output;
    output << "[mapping]\n";
    for (const auto& [name, value] : values) {
        // Draft text is one field, never an INI fragment (comments included).
        if (value.find_first_of("\r\n;#") != std::string::npos)
            throw std::runtime_error(std::format("{}: enter a single value without comments", name));
        output << name << '=' << value << '\n';
    }
    std::istringstream input(output.str());
    return read_config(input, true, true);
}
void config_document::save(const std::filesystem::path& file) {
    const auto absolute = std::filesystem::absolute(file);
    const auto config = snapshot();
    save_config(absolute, config);
    path = absolute;
    dirty = false;
}

std::wstring quote_windows_argument(const std::wstring& value) {
    std::wstring result;
    result.reserve(value.size() * 2 + 2);
    result.push_back(L'\"');
    std::size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') { ++slashes; continue; }
        result.append(ch == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result.push_back(ch);
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}
std::wstring windows_command_line(const std::vector<std::wstring>& arguments) {
    std::wstring result;
    std::size_t maximum_length = 0;
    for (const auto& argument : arguments) maximum_length += argument.size() * 2 + 3;
    result.reserve(maximum_length);
    for (const auto& argument : arguments) {
        if (!result.empty()) result.push_back(L' ');
        result.append(quote_windows_argument(argument));
    }
    return result;
}
std::filesystem::path runtime_executable(const std::filesystem::path& directory, bool user) {
    return directory / (user ? L"mouse_input_mapping_user.exe" : L"mouse_input_mapping_kernel.exe");
}
std::wstring runtime_command(const std::filesystem::path& directory,
    const config_document& document, bool steam) {
    if (document.path.empty() || document.dirty || !std::filesystem::is_regular_file(document.path))
        throw std::runtime_error("Save the configuration before generating a command");
    document.snapshot();
    auto result = windows_command_line({runtime_executable(directory, document.user_mode).wstring(),
        L"--config", std::filesystem::absolute(document.path).wstring()});
    if (steam) result.append(L" --daemon %command%");
    return result;
}
}
