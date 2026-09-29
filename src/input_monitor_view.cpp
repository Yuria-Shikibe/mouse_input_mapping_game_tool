#include "input_monitor_view.hpp"
#include "input_monitor.hpp"
#include "input_preview.hpp"
#include <windowsx.h>
#include <commctrl.h>
#include <algorithm>
#include <cmath>
#include <cwchar>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>

namespace mouse_mapping {
namespace {
constexpr wchar_t class_name[] = L"MouseMappingInputMonitor";
constexpr int duration_id = 1, pause_id = 2, clear_id = 3, source_id = 4, start_id = 5;
constexpr COLORREF colors[]{RGB(32, 111, 205), RGB(205, 105, 20), RGB(22, 132, 99), RGB(146, 71, 182)};
constexpr COLORREF fills[]{RGB(186, 214, 246), RGB(255, 219, 173), RGB(176, 231, 212), RGB(225, 199, 241)};
struct missing_interval { double begin, end; };

std::wstring error_text(const std::string& value) {
    const int count = MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}

std::wstring key_label(key_code code) {
    wchar_t label[128]{};
    const auto bits = static_cast<LONG>((code & 0xff) << 16) | ((code & 0xff00) ? (1L << 24) : 0);
    const auto name = key_name(code);
    if (name != "SCAN_CODE") return std::wstring(name.begin(), name.end());
    if (GetKeyNameTextW(bits, label, 128)) return label;
    const auto fallback = format_key(code);
    return std::wstring(fallback.begin(), fallback.end());
}
void fill(HDC dc, RECT rect, COLORREF color) {
    const auto brush = CreateSolidBrush(color);
    FillRect(dc, &rect, brush);
    DeleteObject(brush);
}
void line(HDC dc, int x1, int y1, int x2, int y2, COLORREF color, int width = 1) {
    const auto pen = CreatePen(PS_SOLID, width, color);
    const auto old = SelectObject(dc, pen);
    MoveToEx(dc, x1, y1, nullptr); LineTo(dc, x2, y2);
    SelectObject(dc, old); DeleteObject(pen);
}
void text(HDC dc, RECT rect, const std::wstring& value, COLORREF color = RGB(45, 49, 56)) {
    SetTextColor(dc, color);
    DrawTextW(dc, value.c_str(), static_cast<int>(value.size()), &rect, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}

class monitor_view {
public:
    HWND window{}, duration{}, pause{}, clear{}, source{}, start_button{};
    HFONT font{}; // Owned by the parent editor.
    UINT dpi = 96;
    monitor_reader reader;
    input_preview preview;
    std::function<configuration()> config_provider;
    monitor_info preview_info;
    bool local_mode = true, preview_active = false;
    std::string preview_error;
    std::vector<monitor_event> batch;
    std::deque<monitor_event> history, frozen;
    std::deque<missing_interval> gaps, frozen_gaps;
    double seconds = 5, frozen_end = 0;
    bool paused = false, failed = false;

    int px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    HWND control(const wchar_t* type, const wchar_t* label, DWORD style, int id) {
        const auto child = CreateWindowExW(0, type, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
            0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!child) throw std::runtime_error("Cannot create input monitor control");
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return child;
    }
    void create() {
        duration = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST, duration_id);
        for (const auto label : {L"最近 2 秒", L"最近 5 秒", L"最近 10 秒"})
            SendMessageW(duration, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
        SendMessageW(duration, CB_SETCURSEL, 1, 0);
        pause = control(L"BUTTON", L"暂停", BS_PUSHBUTTON, pause_id);
        clear = control(L"BUTTON", L"清空", BS_PUSHBUTTON, clear_id);
        source = control(L"COMBOBOX", L"", CBS_DROPDOWNLIST, source_id);
        SendMessageW(source, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"GUI 本地测试"));
        SendMessageW(source, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"运行程序监视"));
        SendMessageW(source, CB_SETCURSEL, 0, 0);
        start_button = control(L"BUTTON", L"开始测试", BS_PUSHBUTTON, start_id);
        SetTimer(window, 1, 250, nullptr);
        layout();
    }
    void layout() {
        dpi = GetDpiForWindow(window);
        MoveWindow(duration, 0, px(47), px(130), px(180), TRUE);
        MoveWindow(pause, px(144), px(47), px(84), px(27), TRUE);
        MoveWindow(clear, px(242), px(47), px(84), px(27), TRUE);
        MoveWindow(source, px(340), px(47), px(150), px(180), TRUE);
        MoveWindow(start_button, px(504), px(47), px(130), px(27), TRUE);
        InvalidateRect(window, nullptr, FALSE);
    }
    void reset() {
        history.clear(); gaps.clear(); frozen.clear(); frozen_gaps.clear();
        paused = false; frozen_end = 0;
        SetWindowTextW(pause, L"暂停");
    }
    void poll() {
        const bool watching = !local_mode && IsWindowVisible(window) && !IsIconic(GetAncestor(window, GA_ROOT));
        reader.subscribe(watching);
        if (!local_mode && !watching) return;
        bool session = false, gap = false;
        if (local_mode) {
            preview.poll(batch, preview_error);
            if (preview_active && !preview.running()) {
                preview_active = false;
                SetWindowTextW(start_button, L"开始测试");
            }
            if (!preview_error.empty()) failed = true;
        } else reader.poll(batch, session, gap);
        if (session) reset();
        if (gap && history.empty() && !batch.empty() && batch.front().sequence > 1)
            gaps.push_back({std::max(reader.info().started, batch.front().time - 10.0), batch.front().time});
        for (const auto& event : batch) {
            if (!history.empty() && (event.sequence != history.back().sequence + 1 || event.time - history.back().time > 2.0))
                gaps.push_back({history.back().time, event.time});
            history.push_back(event);
        }
        if (!history.empty()) {
            const double cutoff = history.back().time - 10.0;
            while (history.size() > 1 && (history[1].time < cutoff || history.size() > monitor_capacity)) history.pop_front();
            while (!gaps.empty() && gaps.front().end < history.front().time) gaps.pop_front();
            while (gaps.size() > monitor_capacity) gaps.pop_front();
        }
        if (IsWindowVisible(window)) InvalidateRect(window, nullptr, FALSE);
    }
    void stop_preview() noexcept {
        preview.stop();
        preview_active = false;
        if (start_button) SetWindowTextW(start_button, L"开始测试");
    }
    void action(int id, int notification) {
        if (id == source_id && notification == CBN_SELCHANGE) {
            reader.subscribe(false);
            stop_preview();
            preview.poll(batch, preview_error);
            batch.clear();
            local_mode = SendMessageW(source, CB_GETCURSEL, 0, 0) == 0;
            reset(); failed = false; preview_error.clear(); preview_info = {};
            EnableWindow(start_button, local_mode);
        } else if (id == start_id && notification == BN_CLICKED && local_mode) {
            if (preview_active) stop_preview();
            else {
                auto config = config_provider();
                config = effective_config(config);
                reset(); failed = false; preview_error.clear();
                preview_info.session = 1;
                preview_info.started = monitor_now();
                preview_info.user_mode = config.map_y;
                preview_info.y_enabled = y_enabled(config);
                preview_info.keys = {config.left_key, config.right_key, config.up_key, config.down_key};
                preview.start(config, GetAncestor(window, GA_ROOT));
                preview_active = true;
                SetWindowTextW(start_button, L"停止测试");
            }
        } else if (id == duration_id && notification == CBN_SELCHANGE) {
            const auto selection = SendMessageW(duration, CB_GETCURSEL, 0, 0);
            seconds = selection == 0 ? 2 : selection == 2 ? 10 : 5;
        } else if (id == pause_id && notification == BN_CLICKED) {
            if (!paused) {
                frozen = history; frozen_gaps = gaps;
                frozen_end = history.empty() ? monitor_now() : history.back().time;
            } else { frozen.clear(); frozen_gaps.clear(); }
            paused = !paused;
            SetWindowTextW(pause, paused ? L"继续" : L"暂停");
        } else if (id == clear_id && notification == BN_CLICKED) {
            // Preserve a fresh state anchor, never the original hold's start.
            // This keeps a currently held intent visible after clearing.
            monitor_event anchor;
            const bool has_anchor = !history.empty();
            if (has_anchor) { anchor = history.back(); anchor.kind = monitor_event_kind::snapshot; anchor.x = anchor.y = 0; }
            history.clear(); gaps.clear(); frozen.clear(); frozen_gaps.clear();
            if (has_anchor) history.push_back(anchor);
            if (paused) { frozen = history; frozen_end = has_anchor ? anchor.time : monitor_now(); }
        }
        InvalidateRect(window, nullptr, FALSE);
    }
    void draw(HDC dc, RECT client) {
        fill(dc, client, RGB(250, 251, 253));
        SetBkMode(dc, TRANSPARENT);
        const auto old_font = SelectObject(dc, font ? font : GetStockObject(DEFAULT_GUI_FONT));
        const auto& info = local_mode ? preview_info : reader.info();
        const auto& data = paused ? frozen : history;
        const auto& missing = paused ? frozen_gaps : gaps;
        const double end = paused ? frozen_end : data.empty() ? monitor_now() : data.back().time;
        const double begin = end - seconds;
        const auto state = history.empty() ? 0u : history.back().state;
        std::wstring heading = local_mode ?
            failed ? L"本地测试失败" : preview_active ? L"GUI 本地测试中" : L"GUI 本地测试 — 点击“开始测试”后移动鼠标" :
            failed ? L"监视读取失败；运行程序不受影响" :
            !info.session ? L"未连接 — 请从“启动与工具”启动新版运行程序" :
            reader.connected() ? L"已连接" : L"已断开 — 画面停留在最后可信时刻";
        if (info.session && !local_mode) heading += std::wstring(L"  ·  ") + (info.user_mode ? L"用户态" : L"内核态")
            + L"  ·  PID " + std::to_wstring(info.pid) + ((state & 16) ? L"  ·  映射 ON" : L"  ·  映射 OFF");
        if (local_mode && preview_active && !(state & 16)) heading += L"  ·  等待 GUI 获得焦点";
        if (state & 64) heading += L"  ·  按住恢复原始输入";
        if (paused) heading += L"  ·  画面已暂停";
        text(dc, {0, 0, client.right, px(24)}, heading);
        text(dc, {0, px(24), client.right, px(44)},
            local_mode && !preview_error.empty() ? error_text(preview_error) :
            (state & 32) ? L"已忽略绝对定位坐标；仅相对鼠标输入参与速度绘制。" :
            local_mode ? L"使用点击开始时的当前草稿（无需保存）；复用 XY 算法，仅绘图，不向系统发键。" :
            L"键名来自运行程序配置；负方向 = 左 / 上，正方向 = 右 / 下。", RGB(95, 100, 109));

        // Fixed 10 ms buckets avoid unstable event-to-event division at high
        // polling rates. Empty buckets return to zero rather than holding speed.
        const auto first_bucket = static_cast<std::int64_t>(std::floor(begin * 100.0));
        const int count = static_cast<int>(std::ceil(seconds * 100.0)) + 2;
        std::vector<std::array<double, 2>> speeds(static_cast<std::size_t>(count));
        for (const auto& event : data) {
            if (event.kind != monitor_event_kind::motion || event.time < begin || event.time > end) continue;
            const auto index = static_cast<std::int64_t>(std::floor(event.time * 100.0)) - first_bucket;
            if (index >= 0 && index < count) {
                speeds[static_cast<std::size_t>(index)][0] += static_cast<double>(event.x) * 100;
                speeds[static_cast<std::size_t>(index)][1] += static_cast<double>(event.y) * 100;
            }
        }
        double maximum = 100;
        for (const auto& speed : speeds) maximum = std::max({maximum, std::abs(speed[0]), std::abs(speed[1])});
        maximum = std::ceil(maximum / 100) * 100;
        const int top = px(85), bottom = client.bottom - px(43);
        const auto axis_height = std::max(px(65), (bottom - top) / 2);
        for (auto axis = 0; axis < 2; ++axis) {
            const auto y = top + axis * axis_height;
            const bool disabled = axis == 1 && info.session && !info.y_enabled;
            text(dc, {0, y, px(68), y + px(22)}, axis ? L"Y 轴" : L"X 轴");
            for (int sign = 0; sign < 2; ++sign) {
                const int index = axis * 2 + sign;
                const int x = px(78 + sign * 146);
                fill(dc, {x, y + px(5), x + px(12), y + px(17)}, disabled ? RGB(166, 171, 180) : colors[index]);
                const auto label = std::wstring(axis ? L"Y" : L"X") + (sign ? L"+  " : L"−  ")
                    + (info.session ? key_label(info.keys[static_cast<std::size_t>(index)]) : L"—");
                text(dc, {x + px(19), y, x + px(140), y + px(22)}, label,
                    disabled ? RGB(140, 145, 155) : colors[index]);
            }
            if (disabled) text(dc, {px(377), y, client.right, y + px(22)}, L"内核 Y 映射未开启", RGB(120, 124, 132));
            RECT plot{px(76), y + px(26), client.right - px(12), y + axis_height - px(25)};
            if (plot.right <= plot.left || plot.bottom <= plot.top) continue;
            const int middle = (plot.top + plot.bottom) / 2;
            const auto position = [&](double time) {
                return plot.left + static_cast<int>(std::clamp((time - begin) / seconds, 0.0, 1.0) * (plot.right - plot.left));
            };
            fill(dc, plot, RGB(240, 243, 248));
            // Each snapshot applies only until the next known record. Missing
            // sequences never extend a colored hold across an unknown interval.
            const auto draw_hold = [&](unsigned intent, double from, double until) {
                if (disabled || (intent != 1 && intent != 2) || until < begin || from > end || until <= from) return;
                const auto left = position(from);
                const auto minimum_right = std::min(left + 1, plot.right);
                const auto right = std::clamp(position(until), minimum_right, plot.right);
                if (left >= plot.right) return;
                fill(dc, {left, intent == 2 ? plot.top + 1 : middle + 1, right,
                    intent == 2 ? middle : plot.bottom}, fills[axis * 2 + (intent == 2 ? 1 : 0)]);
            };
            // Merge equal intent snapshots before drawing: a high-rate mouse
            // must not allocate a GDI brush for every raw packet in a hold.
            if (!disabled && !data.empty()) {
                unsigned intent = (data.front().state >> (axis * 2)) & 3;
                double from = data.front().time;
                for (std::size_t i = 1; i < data.size(); ++i) {
                    const auto& event = data[i];
                    const auto next = (event.state >> (axis * 2)) & 3;
                    const bool gap = event.sequence != data[i - 1].sequence + 1 || event.time - data[i - 1].time > 2.0;
                    if (gap || next != intent) {
                        draw_hold(intent, from, gap ? data[i - 1].time : event.time);
                        intent = next; from = event.time;
                    }
                }
                draw_hold(intent, from, end);
            }
            for (const auto& gap : missing) {
                if (gap.end < begin || gap.begin > end) continue;
                RECT region{position(gap.begin), plot.top, position(gap.end), plot.bottom};
                const auto brush = CreateHatchBrush(HS_BDIAGONAL, RGB(160, 164, 174));
                FillRect(dc, &region, brush); DeleteObject(brush);
            }
            for (int tick = 0; tick <= 5; ++tick) {
                const int x = plot.left + (plot.right - plot.left) * tick / 5;
                line(dc, x, plot.top, x, plot.bottom, RGB(214, 219, 228));
                wchar_t label[32]{};
                swprintf_s(label, L"%.1f s", -seconds * (5 - tick) / 5);
                text(dc, {x - px(20), plot.bottom + px(2), x + px(44), plot.bottom + px(22)}, label, RGB(115, 120, 130));
            }
            line(dc, plot.left, middle, plot.right, middle, RGB(158, 164, 176));
            const auto scale = std::to_wstring(static_cast<long long>(maximum));
            text(dc, {0, plot.top, px(72), plot.top + px(18)}, L"+" + scale, RGB(105, 110, 120));
            text(dc, {0, middle - px(9), px(72), middle + px(9)}, L"0", RGB(105, 110, 120));
            text(dc, {0, plot.bottom - px(18), px(72), plot.bottom}, L"−" + scale, RGB(105, 110, 120));
            const auto pen = CreatePen(PS_SOLID, std::max(1, px(2)), RGB(53, 60, 72));
            const auto old_pen = SelectObject(dc, pen);
            bool previous = false;
            std::size_t gap_index = 0;
            for (int i = 0; i < count; ++i) {
                const double bucket_begin = static_cast<double>(first_bucket + i) / 100.0;
                const double time = bucket_begin + 0.005;
                bool known = !data.empty() && time >= std::max(begin, data.front().time) && time <= end;
                while (gap_index < missing.size() && missing[gap_index].end <= bucket_begin) ++gap_index;
                if (gap_index < missing.size() && missing[gap_index].begin < bucket_begin + 0.01) known = false;
                if (!known) { previous = false; continue; }
                const int x = position(time);
                const int value_y = middle - static_cast<int>(speeds[static_cast<std::size_t>(i)][static_cast<std::size_t>(axis)]
                    / maximum * (plot.bottom - plot.top - px(4)) / 2);
                if (previous) LineTo(dc, x, value_y); else MoveToEx(dc, x, value_y, nullptr);
                previous = true;
            }
            SelectObject(dc, old_pen); DeleteObject(pen);
        }
        text(dc, {0, client.bottom - px(40), client.right, client.bottom - px(20)},
            L"折线：原始鼠标速度 counts/s（10 ms 分桶）；色块：鼠标映射意图，非系统 / 游戏收键确认。", RGB(95, 100, 109));
        text(dc, {0, client.bottom - px(20), client.right, client.bottom},
            !missing.empty() ? L"斜线区域为缺失数据；该区间的按住状态未知。" :
            local_mode ? L"切走页面自动停止；切换窗口暂停输入。修改参数后重新开始测试；运行中的内核过滤会影响采集。" :
            L"实体键盘不生成色块；键盘优先压制输出时，仍显示鼠标意图。", RGB(95, 100, 109));
        SelectObject(dc, old_font);
    }
    void paint() {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        RECT client{}; GetClientRect(window, &client);
        const auto buffer = CreateCompatibleDC(dc);
        const auto bitmap = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
        const auto old = buffer && bitmap ? SelectObject(buffer, bitmap) : nullptr;
        try {
            draw(old ? buffer : dc, client);
            if (old) BitBlt(dc, 0, 0, client.right, client.bottom, buffer, 0, 0, SRCCOPY);
        } catch (...) { failed = true; }
        if (old) SelectObject(buffer, old);
        if (bitmap) DeleteObject(bitmap);
        if (buffer) DeleteDC(buffer);
        EndPaint(window, &paint);
    }
};

LRESULT CALLBACK view_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* view = reinterpret_cast<monitor_view*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        view = static_cast<monitor_view*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        view->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(view));
    }
    if (!view) return DefWindowProcW(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_CREATE: view->create(); return 0;
        case WM_SIZE: if (view->duration) view->layout(); return 0;
        case WM_SETFONT:
            view->font = reinterpret_cast<HFONT>(wparam);
            for (auto child : {view->duration, view->pause, view->clear, view->source, view->start_button})
                if (child) SendMessageW(child, WM_SETFONT, wparam, lparam);
            view->layout(); return 0;
        case WM_SHOWWINDOW:
            if (!wparam) { view->stop_preview(); view->reader.subscribe(false); }
            SetTimer(window, 1, wparam ? 33 : 250, nullptr); break;
        case WM_TIMER: view->poll(); return 0;
        case WM_COMMAND: view->action(LOWORD(wparam), HIWORD(wparam)); return 0;
        case WM_ERASEBKGND: return 1;
        case WM_PAINT: view->paint(); return 0;
        case WM_DESTROY: view->stop_preview(); KillTimer(window, 1); return 0;
        case WM_NCDESTROY: SetWindowLongPtrW(window, GWLP_USERDATA, 0); break;
        default: break;
        }
    } catch (const std::exception& error) {
        view->stop_preview();
        view->failed = true;
        if (message == WM_CREATE) return -1;
        if (message == WM_COMMAND)
            MessageBoxW(window, error_text(error.what()).c_str(), L"操作未完成", MB_OK | MB_ICONERROR);
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    } catch (...) {
        view->stop_preview();
        view->failed = true;
        if (message == WM_CREATE) return -1;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

// Transfer ownership only after successful creation. Subclass cleanup also
// covers parent-driven DestroyWindow.
LRESULT CALLBACK lifetime_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR data) {
    const auto result = DefSubclassProc(window, message, wparam, lparam);
    if (message == WM_NCDESTROY) delete reinterpret_cast<monitor_view*>(data);
    return result;
}
}

HWND create_input_monitor_view(HWND parent, HFONT font, std::function<configuration()> config_provider) {
    WNDCLASSW type{};
    type.lpfnWndProc = view_proc;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = class_name;
    if (!RegisterClassW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register input monitor page");
    auto view = std::make_unique<monitor_view>();
    view->font = font;
    view->config_provider = std::move(config_provider);
    const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, class_name, L"", WS_CHILD | WS_CLIPCHILDREN | WS_CLIPSIBLINGS,
        0, 0, 0, 0, parent, nullptr, type.hInstance, view.get());
    if (!window) throw std::runtime_error("Cannot create input monitor page");
    if (!SetWindowSubclass(window, lifetime_proc, 1, reinterpret_cast<DWORD_PTR>(view.get()))) {
        DestroyWindow(window);
        throw std::runtime_error("Cannot attach input monitor lifetime");
    }
    view.release();
    return window;
}
} // namespace mouse_mapping
