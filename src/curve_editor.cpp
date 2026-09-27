#include "curve_editor.hpp"
#include "config_detail.hpp"
#include <windowsx.h>
#include <array>
#include <format>

namespace mouse_mapping {
namespace {
constexpr wchar_t class_name[] = L"MouseMappingCurveEditor";
struct curve_editor {
    HWND window{}, x_edit{}, y_edit{};
    HFONT font{};
    UINT dpi = 96;
    curve_points points;
    std::size_t selected = 0;
    bool accepted = false, done = false, dragging = false;
    int px(int n) const { return MulDiv(n, static_cast<int>(dpi), 96); }
    RECT plot() const { return {px(52), px(40), px(512), px(350)}; }
    HWND control(const wchar_t* cls, const wchar_t* text, DWORD style, int id,
                 int x, int y, int w, int h) {
        auto child = CreateWindowExW(std::wstring_view(cls) == L"EDIT" ? WS_EX_CLIENTEDGE : 0,
            cls, text, WS_CHILD | WS_VISIBLE | style, px(x), px(y), px(w), px(h),
            window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return child;
    }
    void refresh() {
        auto precise = [](double value) {
            const auto text = std::format("{:.17g}", value);
            return std::wstring(text.begin(), text.end());
        };
        const auto x = precise(points[selected].x), y = precise(points[selected].y);
        SetWindowTextW(x_edit, x.c_str()); SetWindowTextW(y_edit, y.c_str());
        EnableWindow(x_edit, selected > 0 && selected + 1 < points.size());
        InvalidateRect(window, nullptr, FALSE);
    }
    void create() {
        dpi = GetDpiForWindow(window);
        font = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        control(L"STATIC", L"按住比例 0–1（纵轴）", 0, 0, 52, 10, 400, 24);
        control(L"STATIC", L"归一化速度 0–1（横轴）", 0, 0, 170, 355, 300, 24);
        control(L"STATIC", L"点击添加 / 拖动调整；← → 选点；Delete 删除内部点。", 0, 0, 22, 383, 520, 24);
        control(L"STATIC", L"输入", 0, 0, 22, 418, 40, 24);
        x_edit = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 10, 65, 415, 105, 26);
        control(L"STATIC", L"输出", 0, 0, 185, 418, 40, 24);
        y_edit = control(L"EDIT", L"", ES_AUTOHSCROLL | WS_TABSTOP, 11, 228, 415, 105, 26);
        control(L"BUTTON", L"应用坐标", BS_PUSHBUTTON | WS_TABSTOP, 12, 350, 415, 90, 28);
        control(L"BUTTON", L"删除点", BS_PUSHBUTTON | WS_TABSTOP, 13, 450, 415, 80, 28);
        control(L"BUTTON", L"恢复线性", BS_PUSHBUTTON | WS_TABSTOP, 14, 22, 460, 110, 30);
        control(L"BUTTON", L"确定", BS_DEFPUSHBUTTON | WS_TABSTOP, IDOK, 330, 460, 90, 30);
        control(L"BUTTON", L"取消", BS_PUSHBUTTON | WS_TABSTOP, IDCANCEL, 440, 460, 90, 30);
        refresh();
    }
    double number(HWND edit) {
        wchar_t text[128]{};
        GetWindowTextW(edit, text, 128);
        std::wstring value(text);
        if (std::any_of(value.begin(), value.end(), [](wchar_t c) { return c > 127; }))
            throw std::runtime_error("Use numeric coordinates from 0 to 1");
        std::string ascii;
        for (const auto ch : value) ascii.push_back(static_cast<char>(ch));
        return config_detail::parse_ratio(config_detail::trim(ascii));
    }
    void apply_coordinates() {
        auto next = points;
        next[selected] = {number(x_edit), number(y_edit)};
        validate_curve(next);
        points = std::move(next);
        refresh();
    }
    void remove() {
        if (selected == 0 || selected + 1 == points.size()) return;
        points.erase(points.begin() + static_cast<std::ptrdiff_t>(selected));
        --selected; refresh();
    }
    POINT position(curve_point p) const {
        const auto r = plot();
        return {r.left + static_cast<LONG>(std::lround(p.x*(r.right-r.left))),
            r.bottom - static_cast<LONG>(std::lround(p.y*(r.bottom-r.top)))};
    }
    curve_point coordinate(int x, int y) const {
        const auto r = plot();
        return {std::clamp(double(x-r.left)/(r.right-r.left), 0.0, 1.0),
            std::clamp(double(r.bottom-y)/(r.bottom-r.top), 0.0, 1.0)};
    }
    void mouse_down(int x, int y) {
        const auto r = plot();
        if (x < r.left-px(8) || x > r.right+px(8) || y < r.top-px(8) || y > r.bottom+px(8)) return;
        SetFocus(window);
        auto hit = points.size();
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto p = position(points[i]);
            if (std::abs(p.x-x) <= px(8) && std::abs(p.y-y) <= px(8)) { hit = i; break; }
        }
        if (hit == points.size()) {
            if (points.size() == maximum_curve_points) return;
            const auto p = coordinate(x, y);
            for (const auto point : points) if (std::abs(point.x - p.x) < minimum_curve_spacing) return;
            const auto at = std::lower_bound(points.begin(), points.end(), p.x,
                [](curve_point a, double b) { return a.x < b; });
            hit = static_cast<std::size_t>(at-points.begin());
            points.insert(at, p);
        }
        selected = hit; dragging = true; SetCapture(window); refresh();
    }
    void mouse_move(int x, int y) {
        if (!dragging) return;
        auto p = coordinate(x, y);
        if (selected == 0 || selected + 1 == points.size()) p.x = points[selected].x;
        else {
            const auto low = points[selected - 1].x + minimum_curve_spacing;
            const auto high = points[selected + 1].x - minimum_curve_spacing;
            p.x = low <= high ? std::clamp(p.x, low, high) : points[selected].x;
        }
        points[selected] = p; refresh();
    }
    void paint() {
        PAINTSTRUCT ps{}; const auto dc = BeginPaint(window, &ps);
        FillRect(dc, &ps.rcPaint, GetSysColorBrush(COLOR_BTNFACE));
        const auto r = plot(); FillRect(dc, &r, GetSysColorBrush(COLOR_WINDOW));
        const auto old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        for (int i = 0; i <= 4; ++i) {
            const std::array labels{L"0", L"0.25", L"0.5", L"0.75", L"1"};
            const auto label = labels[static_cast<std::size_t>(i)];
            TextOutW(dc, r.left-px(34), r.bottom-(r.bottom-r.top)*i/4-px(8), label, lstrlenW(label));
        }
        SelectObject(dc, old_font);
        const auto grid = CreatePen(PS_SOLID, 1, RGB(215,215,215));
        const auto old_pen = SelectObject(dc, grid);
        for (int i = 0; i <= 4; ++i) {
            const auto x = r.left+(r.right-r.left)*i/4, y = r.top+(r.bottom-r.top)*i/4;
            MoveToEx(dc, x, r.top, nullptr); LineTo(dc, x, r.bottom);
            MoveToEx(dc, r.left, y, nullptr); LineTo(dc, r.right, y);
        }
        const auto curve_pen = CreatePen(PS_SOLID, px(2), RGB(30,110,220)); SelectObject(dc, curve_pen);
        const sensitivity_curve curve(points);
        for (int x = 0; x <= r.right-r.left; ++x) {
            const auto p = position({double(x)/(r.right-r.left), curve.evaluate(double(x)/(r.right-r.left))});
            if (!x) MoveToEx(dc, p.x, p.y, nullptr); else LineTo(dc, p.x, p.y);
        }
        const auto brush = CreateSolidBrush(RGB(30,110,220)); const auto old_brush = SelectObject(dc, brush);
        for (std::size_t i = 0; i < points.size(); ++i) {
            const auto p = position(points[i]); const int radius = px(i == selected ? 6 : 4);
            Ellipse(dc, p.x-radius, p.y-radius, p.x+radius, p.y+radius);
        }
        SelectObject(dc, old_brush); SelectObject(dc, old_pen);
        DeleteObject(brush); DeleteObject(curve_pen); DeleteObject(grid);
        EndPaint(window, &ps);
    }
    ~curve_editor() { if (font) DeleteObject(font); }
};
LRESULT CALLBACK curve_proc(HWND window, UINT message, WPARAM wp, LPARAM lp) {
    auto* editor = reinterpret_cast<curve_editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        editor = static_cast<curve_editor*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
        editor->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(editor));
    }
    if (!editor) return DefWindowProcW(window, message, wp, lp);
    try {
        switch (message) {
        case WM_CREATE: editor->create(); return 0;
        case WM_PAINT: editor->paint(); return 0;
        case WM_DPICHANGED: {
            wchar_t x[128]{}, y[128]{};
            GetWindowTextW(editor->x_edit, x, 128); GetWindowTextW(editor->y_edit, y, 128);
            while (auto child = GetWindow(window, GW_CHILD)) DestroyWindow(child);
            if (editor->font) { DeleteObject(editor->font); editor->font = nullptr; }
            editor->dpi = HIWORD(wp);
            RECT bounds{0,0,editor->px(555),editor->px(510)};
            AdjustWindowRectExForDpi(&bounds, WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                FALSE, WS_EX_DLGMODALFRAME, editor->dpi);
            const auto* suggested = reinterpret_cast<RECT*>(lp);
            SetWindowPos(window, nullptr, suggested->left, suggested->top,
                bounds.right-bounds.left, bounds.bottom-bounds.top, SWP_NOZORDER | SWP_NOACTIVATE);
            editor->create();
            SetWindowTextW(editor->x_edit, x); SetWindowTextW(editor->y_edit, y);
            return 0;
        }
        case WM_LBUTTONDOWN: editor->mouse_down(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_MOUSEMOVE: editor->mouse_move(GET_X_LPARAM(lp), GET_Y_LPARAM(lp)); return 0;
        case WM_LBUTTONUP: editor->dragging = false; ReleaseCapture(); return 0;
        case WM_CAPTURECHANGED: editor->dragging = false; return 0;
        case WM_KEYDOWN:
            if (wp == VK_DELETE) editor->remove();
            else if (wp == VK_LEFT && editor->selected > 0) { --editor->selected; editor->refresh(); }
            else if (wp == VK_RIGHT && editor->selected + 1 < editor->points.size()) { ++editor->selected; editor->refresh(); }
            return 0;
        case WM_COMMAND:
            if (HIWORD(wp) != BN_CLICKED) break;
            switch (LOWORD(wp)) {
            case 12: editor->apply_coordinates(); return 0;
            case 13: editor->remove(); return 0;
            case 14: editor->points = {{0,0},{1,1}}; editor->selected = 0; editor->refresh(); return 0;
            case IDOK: editor->apply_coordinates(); editor->accepted = true; editor->done = true; return 0;
            case IDCANCEL: editor->done = true; return 0;
            }
            break;
        case WM_CLOSE: editor->done = true; return 0;
        }
    } catch (const std::exception& error) {
        MessageBoxA(window, error.what(), "Invalid curve", MB_OK | MB_ICONERROR);
        if (message == WM_CREATE) return -1;
        return 0;
    }
    return DefWindowProcW(window, message, wp, lp);
}
}
bool edit_curve(HWND owner, curve_points& points, const wchar_t* title) {
    WNDCLASSW wc{}; wc.lpfnWndProc = curve_proc; wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = class_name; wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = GetSysColorBrush(COLOR_BTNFACE);
    if (!RegisterClassW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register curve editor");
    curve_editor editor; editor.points = points; editor.dpi = GetDpiForWindow(owner);
    RECT rect{0, 0, editor.px(555), editor.px(510)};
    const DWORD style = WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN;
    AdjustWindowRectExForDpi(&rect, style, FALSE, WS_EX_DLGMODALFRAME, editor.dpi);
    const auto window = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, title, style,
        CW_USEDEFAULT, CW_USEDEFAULT, rect.right-rect.left, rect.bottom-rect.top, owner, nullptr, wc.hInstance, &editor);
    if (!window) throw std::runtime_error("Cannot create curve editor");
    EnableWindow(owner, FALSE); ShowWindow(window, SW_SHOW); SetFocus(window);
    MSG message{};
    while (!editor.done) {
        const auto status = GetMessageW(&message, nullptr, 0, 0);
        if (status <= 0) { if (!status) PostQuitMessage(static_cast<int>(message.wParam)); break; }
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE) { editor.done = true; continue; }
        if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    EnableWindow(owner, TRUE); DestroyWindow(window); SetActiveWindow(owner);
    if (editor.accepted) points = std::move(editor.points);
    return editor.accepted;
}
}
