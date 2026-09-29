#include "config_document.hpp"
#include "settings_layout.hpp"
#include "gui_process.hpp"
#include "curve_editor.hpp"
#include "input_monitor_view.hpp"
#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shobjidl.h>
#include <algorithm>
#include <array>
#include <memory>
#include <cwchar>
#include <stdexcept>
#include <utility>

namespace mouse_mapping {
namespace {
constexpr int backend_id = 100, new_id = 101, open_id = 102, save_id = 103,
    save_as_id = 104, defaults_id = 105, tab_id = 106, cancel_id = 108, clear_binding_id = 109,
    run_id = 110, copy_id = 111, steam_id = 112, check_id = 113, install_id = 114;
constexpr int edit_base = 1000, section_base = 3000;
constexpr std::array toolbar_specs{
    std::pair{new_id, L"新建"}, std::pair{open_id, L"打开…"}, std::pair{save_id, L"保存"},
    std::pair{save_as_id, L"另存为…"}, std::pair{defaults_id, L"恢复默认"}};
constexpr std::array tool_button_specs{
    std::pair{run_id, L"保存并启动"}, std::pair{copy_id, L"复制运行命令"}, std::pair{steam_id, L"复制 Steam 选项"},
    std::pair{check_id, L"检查驱动"}, std::pair{install_id, L"安装 / 修复驱动"}};

std::wstring wide(const std::string& text) {
    if (text.empty()) return {};
    auto encoding = CP_UTF8;
    auto count = MultiByteToWideChar(encoding, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (!count) {
        encoding = CP_ACP;
        count = MultiByteToWideChar(encoding, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    std::wstring result(count, L'\0');
    MultiByteToWideChar(encoding, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}
std::wstring window_text(HWND window) {
    std::wstring text(GetWindowTextLengthW(window) + 1, L'\0');
    text.resize(GetWindowTextW(window, text.data(), static_cast<int>(text.size())));
    return text;
}
std::string utf8(const std::wstring& text) {
    const auto count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string result(count, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}
template<class T> struct com_ptr {
    T* value = nullptr;
    ~com_ptr() { if (value) value->Release(); }
};
std::filesystem::path pick_file(HWND owner, bool save, const std::filesystem::path& current) {
    com_ptr<IFileDialog> dialog;
    const auto clsid = save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog;
    auto result = CoCreateInstance(clsid, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog.value));
    if (FAILED(result)) throw std::runtime_error("Cannot create Windows file dialog");
    const COMDLG_FILTERSPEC filters[]{{L"配置文件 (*.ini)", L"*.ini"}, {L"所有文件", L"*.*"}};
    dialog.value->SetFileTypes(2, filters);
    dialog.value->SetDefaultExtension(L"ini");
    DWORD options = 0;
    dialog.value->GetOptions(&options);
    dialog.value->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
        (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
    if (!current.empty()) {
        dialog.value->SetFileName(current.filename().c_str());
        com_ptr<IShellItem> folder;
        if (SUCCEEDED(SHCreateItemFromParsingName(current.parent_path().c_str(), nullptr, IID_PPV_ARGS(&folder.value))))
            dialog.value->SetFolder(folder.value);
    }
    result = dialog.value->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return {};
    if (FAILED(result)) throw std::runtime_error("Cannot select configuration file");
    com_ptr<IShellItem> item;
    if (FAILED(dialog.value->GetResult(&item.value))) throw std::runtime_error("Cannot obtain selected file");
    PWSTR name = nullptr;
    if (FAILED(item.value->GetDisplayName(SIGDN_FILESYSPATH, &name))) throw std::runtime_error("Cannot obtain file path");
    const std::filesystem::path file(name);
    CoTaskMemFree(name);
    return file;
}
void clipboard(HWND owner, const std::wstring& text) {
    const auto bytes = (text.size() + 1) * sizeof(wchar_t);
    const auto memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) throw std::runtime_error("Cannot allocate clipboard text");
    const auto destination = GlobalLock(memory);
    if (!destination) { GlobalFree(memory); throw std::runtime_error("Cannot lock clipboard text"); }
    memcpy(destination, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!OpenClipboard(owner)) { GlobalFree(memory); throw std::runtime_error("Clipboard is busy"); }
    EmptyClipboard();
    const auto success = SetClipboardData(CF_UNICODETEXT, memory);
    CloseClipboard();
    if (!success) { GlobalFree(memory); throw std::runtime_error("Cannot copy command"); }
}

LRESULT CALLBACK form_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR data);

constexpr wchar_t settings_form_class[] = L"MouseMappingSettingsForm";
void register_settings_form() {
    // STATIC uses CS_PARENTDC, which cannot be combined with WS_EX_COMPOSITED.
    // Give the scrollable form its own class so child compositing is supported.
    WNDCLASSEXW type{sizeof(type)};
    type.lpfnWndProc = DefWindowProcW;
    type.hInstance = GetModuleHandleW(nullptr);
    type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    type.lpszClassName = settings_form_class;
    if (!RegisterClassExW(&type) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("Cannot register settings form");
}

std::wstring binding_name(key_code code) {
    const auto name = key_name(code);
    if (name != "SCAN_CODE") return wide(name);
    wchar_t text[128]{};
    const auto bits = static_cast<LONG>((code & 0xff) << 16) | ((code & 0xff00) ? (1L << 24) : 0);
    if (GetKeyNameTextW(bits, text, 128)) return text;
    return wide(format_key(code));
}

struct field_control { HWND label{}, input{}; };
struct panel_control {
    settings_panel spec;
    HWND header{};
    RECT bounds{};
    int parent = -1;
    bool expanded = true;
    std::vector<std::size_t> fields;
};
class editor {
public:
    HWND window{}, backend{}, tabs{}, path_label{}, status{}, capture_label{}, cancel{}, clear_binding{}, dirty_label{}, form{};
    HWND explanation{}, command_label{}, command{}, output{}, tooltip{};
    HWND input_monitor{};
    HFONT font{};
    UINT dpi = 96;
    int page = 0;
    bool populating = false;
    int capture_index = -1;
    int swallowed_mouse = -1;
    key_code swallowed_key = 0;
    std::vector<panel_control> panels;
    int scroll_position = 0, wheel_remainder = 0;
    HBRUSH dirty_brush = CreateSolidBrush(RGB(255, 222, 150));
    config_document document;
    std::filesystem::path directory = executable_directory();
    std::vector<field_control> fields;
    std::array<HWND, toolbar_specs.size()> toolbar{};
    std::array<HWND, tool_button_specs.size()> tool_buttons{};
    gui_process tool;
    gui_process runtime_process;
    std::string tool_output;
    bool install_running = false;

    ~editor() { if (font) DeleteObject(font); if (dirty_brush) DeleteObject(dirty_brush); }
    int px(int value) const { return MulDiv(value, static_cast<int>(dpi), 96); }
    HWND control(const wchar_t* type, const wchar_t* text, DWORD style, int id = 0, DWORD ex = 0, HWND parent = nullptr) {
        const auto child = CreateWindowExW(ex, type, text, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS | style,
            0, 0, 0, 0, parent ? parent : window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), GetModuleHandleW(nullptr), nullptr);
        if (!child) {
            const auto error = GetLastError();
            throw std::runtime_error("Cannot create window control: " + utf8(type) +
                " (id=" + std::to_string(id) + ", Windows error=" + std::to_string(error) + ")");
        }
        SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
        return child;
    }
    void add_tooltip(HWND child, const wchar_t* text) {
        if (!text) return;
        TOOLINFOW info{sizeof(info)};
        info.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        info.hwnd = GetParent(child);
        info.uId = reinterpret_cast<UINT_PTR>(child);
        info.lpszText = const_cast<wchar_t*>(text);
        if (!SendMessageW(tooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info)))
            throw std::runtime_error("Cannot attach setting tooltip");
    }
    void place(HWND child, int x, int y, int width, int height, bool visible = true) {
        if (!child) return;
        // Do not paint intermediate positions. layout() invalidates the complete
        // sibling tree after all moves and visibility changes have finished.
        const UINT flags = SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOREDRAW | SWP_NOCOPYBITS |
            (visible ? SWP_SHOWWINDOW : SWP_HIDEWINDOW | SWP_NOMOVE | SWP_NOSIZE);
        SetWindowPos(child, nullptr, px(x), px(y), px(width), px(height), flags);
    }
    void set_status(const std::wstring& text) { SetWindowTextW(status, text.c_str()); }
    void error(const std::exception& exception) {
        MessageBoxW(window, wide(exception.what()).c_str(), L"操作未完成", MB_OK | MB_ICONERROR);
    }
    void title() {
        SetWindowTextW(dirty_label, document.dirty ? L"  ● 有未保存修改 — 请点击“保存”；重启运行程序后生效。" :
            document.path.empty() ? L"  新配置尚未保存 — 保存后可启动。" : L"  配置无未保存修改；已运行程序需重启才能载入新设置。");
        InvalidateRect(dirty_label, nullptr, TRUE);
        const auto name = document.path.empty() ? L"未保存配置" : document.path.filename().wstring();
        SetWindowTextW(window, (std::wstring(document.dirty ? L"* " : L"") + name + L" — 鼠标输入映射配置器").c_str());
        SetWindowTextW(path_label, (L"配置文件：" + (document.path.empty() ? std::wstring(L"尚未保存") : document.path.wstring())).c_str());
    }
    void update_font() {
        const auto next = CreateFontW(-MulDiv(9, static_cast<int>(dpi), 72), 0, 0, 0, FW_NORMAL,
            FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        if (!next) throw std::runtime_error("Cannot create UI font");
        EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(data), TRUE); return TRUE;
        }, reinterpret_cast<LPARAM>(next));
        if (font) DeleteObject(font);
        font = next;
    }
    void create() {
        dpi = GetDpiForWindow(window);
        update_font();
        tooltip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
            WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT, CW_USEDEFAULT,
            CW_USEDEFAULT, CW_USEDEFAULT, window, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (!tooltip) throw std::runtime_error("Cannot create setting tooltips");
        SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, px(440));
        SendMessageW(tooltip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 20000);
        backend = control(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, backend_id);
        SendMessageW(backend, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"用户态 · Raw Input"));
        SendMessageW(backend, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(L"内核态 · Interception"));
        for (std::size_t i = 0; i < toolbar_specs.size(); ++i)
            toolbar[i] = control(WC_BUTTONW, toolbar_specs[i].second, BS_PUSHBUTTON | WS_TABSTOP, toolbar_specs[i].first);
        path_label = control(WC_STATICW, L"", SS_PATHELLIPSIS);
        tabs = control(WC_TABCONTROLW, L"", WS_TABSTOP, tab_id);
        for (const wchar_t* text : {L"映射设置", L"启动与工具", L"XY 输入检查"}) {
            TCITEMW item{}; item.mask = TCIF_TEXT; item.pszText = const_cast<wchar_t*>(text);
            TabCtrl_InsertItem(tabs, TabCtrl_GetItemCount(tabs), &item);
        }
        dirty_label = control(WC_STATICW, L"", SS_LEFT | SS_CENTERIMAGE);
        register_settings_form();
        form = control(settings_form_class, L"", WS_CLIPCHILDREN | WS_VSCROLL, 0, WS_EX_CONTROLPARENT | WS_EX_COMPOSITED);
        if (!SetWindowSubclass(form, form_proc, 1, reinterpret_cast<DWORD_PTR>(this))) throw std::runtime_error("Cannot initialize settings panel");
        fields.reserve(config_fields().size());
        for (std::size_t i = 0; i < config_fields().size(); ++i) {
            const auto& field = config_fields()[i];
            field_control row;
            row.label = control(WC_STATICW, field.label, SS_LEFT | SS_NOTIFY, 0, 0, form);
            if (field.kind == field_kind::toggle)
                row.input = control(WC_BUTTONW, L"启用", BS_AUTOCHECKBOX | BS_NOTIFY | WS_TABSTOP, edit_base + static_cast<int>(i), 0, form);
            else if (field.kind == field_kind::key || field.kind == field_kind::input || field.kind == field_kind::curve)
                row.input = control(WC_BUTTONW, L"", BS_PUSHBUTTON | BS_NOTIFY | WS_TABSTOP, edit_base + static_cast<int>(i), 0, form);
            else {
                row.input = control(WC_EDITW, L"", ES_AUTOHSCROLL | WS_TABSTOP,
                    edit_base + static_cast<int>(i), WS_EX_CLIENTEDGE, form);
                SendMessageW(row.input, EM_SETLIMITTEXT, 128, 0);
            }
            add_tooltip(row.label, field.tooltip);
            add_tooltip(row.input, field.tooltip);
            fields.push_back(row);
        }
        create_panels();
        explanation = control(WC_STATICW, L"", SS_LEFT);
        command_label = control(WC_STATICW, L"运行命令 / Steam 启动选项（生成前请保存）：", SS_LEFT);
        command = control(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
        for (std::size_t i = 0; i < tool_button_specs.size(); ++i)
            tool_buttons[i] = control(WC_BUTTONW, tool_button_specs[i].second, BS_PUSHBUTTON | WS_TABSTOP,
                tool_button_specs[i].first);
        output = control(WC_EDITW, L"", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL | WS_TABSTOP, 0, WS_EX_CLIENTEDGE);
        SendMessageW(output, EM_SETLIMITTEXT, 262144, 0);
        capture_label = control(WC_STATICW, L"", SS_LEFT);
        clear_binding = control(WC_BUTTONW, L"清除绑定", BS_PUSHBUTTON | WS_TABSTOP, clear_binding_id);
        cancel = control(WC_BUTTONW, L"取消录入", WS_TABSTOP | BS_PUSHBUTTON, cancel_id);
        status = control(WC_STATICW, L"", SS_LEFT);
        input_monitor = create_input_monitor_view(window, font, [this] { return document.snapshot(); });
        // The tab paints a background underneath sibling form controls. Child
        // creation order does not put later-created controls above the tab.
        // Keep it behind them so WS_CLIPSIBLINGS excludes their drawing areas.
        SetWindowPos(tabs, HWND_BOTTOM, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_NOREDRAW);
        document.reset(true, default_config_path());
        if (std::filesystem::exists(document.path)) {
            try { document.open(true, document.path); }
            catch (const std::exception& exception) { document.path.clear(); error(exception); }
        }
        populate();
        SetTimer(window, 1, 100, nullptr);
    }
    std::size_t field_index(const std::string& name) const {
        const auto specs = config_fields();
        for (std::size_t i = 0; i < specs.size(); ++i)
            if (name == specs[i].name) return i;
        throw std::runtime_error("Unknown layout field: " + name);
    }
    void create_panel(settings_panel spec, int parent) {
        const auto index = panels.size();
        auto children = std::move(spec.children);
        panel_control panel{std::move(spec), {}, {}, parent};
        for (const auto& name : panel.spec.fields) panel.fields.push_back(field_index(name));
        panel.header = control(WC_BUTTONW, L"", BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP,
            section_base + static_cast<int>(index), 0, form);
        if (!panel.spec.toggle.empty())
            add_tooltip(panel.header, config_fields()[field_index(panel.spec.toggle)].tooltip);
        panels.push_back(std::move(panel));
        for (auto& child : children) create_panel(std::move(child), static_cast<int>(index));
    }
    void create_panels() {
        for (auto& spec : settings_panels()) create_panel(std::move(spec), -1);
        // Fail early if a future configuration field is omitted or shown twice.
        std::vector<int> uses(fields.size());
        for (const auto& panel : panels) {
            for (const auto index : panel.fields) ++uses[index];
            if (!panel.spec.toggle.empty()) {
                const auto index = field_index(panel.spec.toggle);
                ++uses[index];
                // The header owns this toggle; the original row never participates in layout.
                place(fields[index].label, 0, 0, 0, 0, false);
                place(fields[index].input, 0, 0, 0, 0, false);
            }
        }
        for (const auto count : uses)
            if (count != 1) throw std::runtime_error("Settings layout must contain every field exactly once");
        // Native dialog navigation follows the same order as the visible panels.
        HWND previous = HWND_TOP;
        const auto order = [&](HWND child) {
            SetWindowPos(child, previous, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
            previous = child;
        };
        for (const auto& panel : panels) {
            order(panel.header);
            for (const auto index : panel.fields) { order(fields[index].label); order(fields[index].input); }
        }
    }
    bool panel_open(const panel_control& panel) const {
        return panel.spec.toggle.empty() ? panel.expanded : document.values.at(panel.spec.toggle) == "1";
    }
    int panel_height(std::size_t index) const {
        const auto& panel = panels[index];
        int height = 34;
        if (!panel_open(panel)) return height;
        height += 8 + static_cast<int>(panel.fields.size()) * 32;
        for (std::size_t child = index + 1; child < panels.size(); ++child)
            if (panels[child].parent == static_cast<int>(index)) height += panel_height(child) + 8;
        return height + 6;
    }
    void place_panel(std::size_t index, int x, int y, int width, bool visible = true) {
        auto& panel = panels[index];
        const bool open = panel_open(panel);
        std::wstring caption = open ? L"▼  " : L"▶  ";
        caption += panel.spec.title;
        if (!panel.spec.toggle.empty()) caption += open ? L"  · 已启用（点击关闭）" : L"  · 已关闭（点击启用）";
        if (window_text(panel.header) != caption) SetWindowTextW(panel.header, caption.c_str());
        panel.bounds = visible ? RECT{px(x), px(y), px(x + width), px(y + panel_height(index))} : RECT{};
        place(panel.header, x + 1, y + 1, width - 2, 32, visible);
        const bool show_contents = visible && open;
        y += 42;
        for (const auto field : panel.fields) {
            place(fields[field].label, x + 12, y + 3, width - 210, 25, show_contents);
            place(fields[field].input, x + width - 186, y, 174, 26, show_contents);
            y += 32;
        }
        for (std::size_t child = index + 1; child < panels.size(); ++child) {
            if (panels[child].parent != static_cast<int>(index)) continue;
            place_panel(child, x + 12, y, width - 24, show_contents);
            y += panel_height(child) + 8;
        }
    }
    void layout_settings(int viewport) {
        int content = 0;
        for (std::size_t i = 0; i < panels.size(); ++i) {
            if (panels[i].parent == -1) content += panel_height(i) + 12;
        }
        scroll_position = std::clamp(scroll_position, 0, std::max(0, content - viewport));
        SCROLLINFO scroll{sizeof(scroll), SIF_RANGE | SIF_PAGE | SIF_POS};
        scroll.nMax = content - 1; scroll.nPage = static_cast<UINT>(viewport); scroll.nPos = scroll_position;
        SetScrollInfo(form, SB_VERT, &scroll, FALSE);
        RECT bounds{}; GetClientRect(form, &bounds);
        const int width = MulDiv(bounds.right, 96, static_cast<int>(dpi)) - 4;
        int y = -scroll_position;
        for (std::size_t i = 0; i < panels.size(); ++i) {
            if (panels[i].parent != -1) continue;
            place_panel(i, 0, y, width); y += panel_height(i) + 12;
        }
    }
    void paint_settings(HDC dc) const {
        RECT client{}; GetClientRect(form, &client);
        // Borders are pixels, not overlapping child windows: WS_CLIPCHILDREN
        // must only exclude actual controls, never the empty area inside a panel.
        FillRect(dc, &client, GetSysColorBrush(COLOR_BTNFACE));
        for (const auto& panel : panels)
            if (!IsRectEmpty(&panel.bounds)) FrameRect(dc, &panel.bounds, GetSysColorBrush(COLOR_BTNSHADOW));
    }
    bool draw_panel(const DRAWITEMSTRUCT& item) const {
        if (item.CtlID < section_base || item.CtlID >= section_base + panels.size()) return false;
        const auto& panel = panels[item.CtlID - section_base];
        constexpr std::array colors{RGB(226, 232, 240), RGB(214, 233, 252), RGB(215, 240, 228),
            RGB(255, 235, 199), RGB(235, 223, 250), RGB(217, 239, 242), RGB(249, 222, 231)};
        const bool high_contrast = [] {
            HIGHCONTRASTW value{sizeof(value)};
            return SystemParametersInfoW(SPI_GETHIGHCONTRAST, sizeof(value), &value, 0)
                && (value.dwFlags & HCF_HIGHCONTRASTON);
        }();
        const auto brush = CreateSolidBrush(high_contrast ? GetSysColor(COLOR_BTNFACE)
            : colors[static_cast<std::size_t>(panel.spec.color)]);
        FillRect(item.hDC, &item.rcItem, brush); DeleteObject(brush);
        SetBkMode(item.hDC, TRANSPARENT);
        SetTextColor(item.hDC, high_contrast ? GetSysColor(COLOR_BTNTEXT) : RGB(32, 45, 60));
        const auto old_font = SelectObject(item.hDC, font);
        auto text_rect = item.rcItem; InflateRect(&text_rect, -px(10), 0);
        if (item.itemState & ODS_SELECTED) OffsetRect(&text_rect, px(1), px(1));
        const auto caption = window_text(item.hwndItem);
        DrawTextW(item.hDC, caption.c_str(), -1, &text_rect, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(item.hDC, old_font);
        if (item.itemState & ODS_FOCUS) {
            auto focus = item.rcItem; InflateRect(&focus, -px(3), -px(3)); DrawFocusRect(item.hDC, &focus);
        }
        return true;
    }
    void layout() {
        RECT client{}; GetClientRect(window, &client);
        const auto width = MulDiv(client.right, 96, static_cast<int>(dpi));
        const auto height = MulDiv(client.bottom, 96, static_cast<int>(dpi));
        place(backend, 16, 14, 200, 220);
        for (std::size_t i = 0; i < toolbar.size(); ++i) place(toolbar[i], 228 + static_cast<int>(i) * 100, 13, 92, 28);
        place(path_label, 16, 49, width - 32, 23);
        place(dirty_label, 16, 76, width - 32, 30);
        place(tabs, 16, 114, width - 32, height - 210);
        const int viewport = std::max(100, height - 334);
        place(form, 28, 153, width - 56, viewport, page == 0);
        layout_settings(viewport);
        place(explanation, 30, page == 1 ? 154 : height - 172, width - 60, 66, page != 2);
        place(input_monitor, 30, 153, width - 60, height - 263, page == 2);
        place(command_label, 30, 231, width - 60, 22, page == 1);
        place(command, 30, 254, width - 60, 64, page == 1);
        for (std::size_t i = 0; i < tool_buttons.size(); ++i) {
            place(tool_buttons[i], 30 + static_cast<int>(i % 3) * 215,
                329 + static_cast<int>(i / 3) * 38, 200, 29, page == 1);
            EnableWindow(tool_buttons[i], (i != 3 && i != 4) || (!document.user_mode && !tool.active()));
        }
        place(output, 30, 410, width - 60, std::max(60, height - 524), page == 1);
        place(capture_label, 20, height - 84, width - 280, 32, capture_index >= 0);
        place(clear_binding, width - 240, height - 86, 104, 28,
            capture_index >= 0 && config_fields()[static_cast<std::size_t>(capture_index)].kind == field_kind::input);
        place(cancel, width - 124, height - 86, 104, 28, capture_index >= 0);
        place(status, 20, height - 48, width - 40, 42);
        // WS_CLIPCHILDREN on the main window excludes the controls from its
        // paint region; invalidate them explicitly, including their borders.
        RedrawWindow(window, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    void update_curve_controls() {
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const std::string name = config_fields()[i].name;
            if (!name.starts_with("kernel_y_") && !name.starts_with("x_") && !name.starts_with("y_")) continue;
            const auto axis = name.starts_with("kernel_y_") ? std::string("kernel_y_") : name.substr(0, 2);
            const bool pulse = document.values.at(axis + "pulse_enabled") == "1";
            const bool curve = document.values.at(axis + "curve_enabled") == "1";
            if (name == axis + "curve_enabled") EnableWindow(fields[i].input, pulse);
            else if (name == axis + "curve_points" || name == axis + "curve_full_speed")
                EnableWindow(fields[i].input, pulse && curve);
            else if (name == axis + "hold_ratio") EnableWindow(fields[i].input, pulse && !curve);
        }
    }
    void instructions() {
        const wchar_t* text = nullptr;
        if (page == 0) text = L"点击键位按钮后按下单键即可绑定；恢复键还支持鼠标五键和清除绑定。切换窗口取消录入。\n同色标题表示同类功能；点击功能标题启用 / 关闭并展开 / 收起，参数会保留。悬停参数查看说明。";
        else text = L"运行程序始终从 OFF 开始；GUI 关闭不影响独立终端。使用切换键启停映射，终端 Ctrl+C 退出。\nSteam 仅跟踪直接启动的游戏进程。驱动安装可能请求管理员权限并要求重启。";
        SetWindowTextW(explanation, text);
    }
    void populate() {
        populating = true;
        SendMessageW(backend, CB_SETCURSEL, document.user_mode ? 0 : 1, 0);
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto& field = config_fields()[i];
            const auto entry = document.values.find(field.name);
            if (entry == document.values.end()) continue;
            if (field.kind == field_kind::toggle)
                Button_SetCheck(fields[i].input, entry->second == "1" ? BST_CHECKED : BST_UNCHECKED);
            else {
                auto value = wide(entry->second);
                if (field.kind == field_kind::curve) value = L"编辑曲线…";
                if (field.kind == field_kind::input) {
                    try {
                        const auto binding = parse_input_binding(entry->second);
                        value = binding.kind == input_kind::keyboard ? binding_name(binding.code)
                            : binding.kind == input_kind::none ? L"未绑定" : wide(format_input_binding(binding));
                    } catch (...) {}
                }
                if (field.kind == field_kind::key) {
                    try { value = binding_name(parse_key(entry->second)); }
                    catch (...) { /* Keep invalid draft text visible. */ }
                }
                SetWindowTextW(fields[i].input, value.c_str());
            }
        }
        populating = false;
        update_curve_controls();
        SetWindowTextW(command, L"");
        stop_capture(); title(); instructions(); layout();
        set_status(L"保存会规范化 INI 并重写注释。配置更改需重启运行程序；GUI 不接管正在运行的映射。");
    }
    bool save(bool as) {
        auto file = document.path;
        // Validate before opening a save dialog; failures preserve the draft and disk.
        document.snapshot();
        if (as || file.empty()) file = pick_file(window, true,
            file.empty() ? directory / L"config.ini" : file);
        if (file.empty()) return false;
        document.save(file); title(); SetWindowTextW(command, L"");
        set_status(L"配置已保存。下次启动对应终端程序时生效。");
        return true;
    }
    bool may_discard() {
        if (!document.dirty) return true;
        const auto answer = MessageBoxW(window, L"当前配置有未保存修改，是否保存？\n选择“否”放弃修改，选择“取消”返回编辑。",
            L"未保存修改", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer == IDCANCEL) return false;
        return answer != IDYES || save(false);
    }
    void stop_capture() {
        if (capture_index >= 0) {
            const auto index = static_cast<std::size_t>(capture_index);
            const auto& field = config_fields()[index];
            const auto value = document.values.at(field.name);
            std::wstring label;
            if (field.kind == field_kind::input) {
                const auto binding = parse_input_binding(value);
                label = binding.kind == input_kind::keyboard ? binding_name(binding.code)
                    : binding.kind == input_kind::none ? L"未绑定" : wide(format_input_binding(binding));
            } else label = binding_name(parse_key(value));
            SetWindowTextW(fields[index].input, label.c_str());
        }
        capture_index = -1;
        ShowWindow(cancel, SW_HIDE); ShowWindow(capture_label, SW_HIDE);
        ShowWindow(clear_binding, SW_HIDE);
    }
    bool capture_message(const MSG& message) {
        if (GetForegroundWindow() != window) return false;
        int mouse_button = -1;
        bool mouse_down = false;
        switch (message.message) {
        case WM_LBUTTONDOWN: case WM_LBUTTONUP: mouse_button = 0; mouse_down = message.message == WM_LBUTTONDOWN; break;
        case WM_RBUTTONDOWN: case WM_RBUTTONUP: mouse_button = 1; mouse_down = message.message == WM_RBUTTONDOWN; break;
        case WM_MBUTTONDOWN: case WM_MBUTTONUP: mouse_button = 2; mouse_down = message.message == WM_MBUTTONDOWN; break;
        case WM_XBUTTONDOWN: case WM_XBUTTONUP:
            mouse_button = GET_XBUTTON_WPARAM(message.wParam) == XBUTTON1 ? 3 : 4;
            mouse_down = message.message == WM_XBUTTONDOWN; break;
        }
        if (mouse_button >= 0) {
            if (mouse_button == swallowed_mouse) {
                if (!mouse_down) { swallowed_mouse = -1; ReleaseCapture(); }
                return true;
            }
            if (capture_index >= 0 && mouse_down && message.hwnd != cancel && message.hwnd != clear_binding
                && config_fields()[static_cast<std::size_t>(capture_index)].kind == field_kind::input) {
                document.set(config_fields()[static_cast<std::size_t>(capture_index)].name,
                    format_input_binding({input_kind::mouse, static_cast<key_code>(mouse_button)}));
                swallowed_mouse = mouse_button;
                SetCapture(window);
                stop_capture(); title(); SetWindowTextW(command, L"");
                set_status(L"鼠标键已绑定；保存后下次启动生效。");
                return true;
            }
            return false;
        }
        const bool up = message.message == WM_KEYUP || message.message == WM_SYSKEYUP;
        if (!up && message.message != WM_KEYDOWN && message.message != WM_SYSKEYDOWN) return false;
        const auto code = static_cast<key_code>(((message.lParam >> 16) & 0xff) |
            ((message.lParam & (1LL << 24)) ? 0xe000 : 0));
        // Drain the bound key's repeats/release before dialog navigation can see it.
        if (swallowed_key) {
            if (up && code == swallowed_key) swallowed_key = 0;
            return true;
        }
        if (capture_index < 0) return false;
        if (up || (message.lParam & (1LL << 30))) return true;
        try {
            if (message.wParam == VK_PAUSE || message.wParam == VK_SNAPSHOT || message.wParam == VK_CANCEL)
                throw std::runtime_error("Pause / PrintScreen / Break are not supported");
            const auto captured = parse_key(format_key(code));
            const auto index = static_cast<std::size_t>(capture_index);
            document.set(config_fields()[index].name, format_key(captured));
            swallowed_key = captured;
            stop_capture();
            title(); SetWindowTextW(command, L"");
            set_status(L"已绑定 " + binding_name(captured) + L"。点击保存后，下次启动运行程序生效。");
            SetFocus(fields[index].input);
        } catch (const std::exception& exception) {
            SetWindowTextW(capture_label, wide(exception.what()).c_str());
        }
        return true;
    }
    void reveal(HWND child) {
        if (page != 0) return;
        RECT bounds{}, viewport{};
        GetWindowRect(child, &bounds); GetClientRect(form, &viewport);
        MapWindowPoints(nullptr, form, reinterpret_cast<POINT*>(&bounds), 2);
        if (bounds.top < 0) scroll_form(MulDiv(bounds.top, 96, static_cast<int>(dpi)) - 4);
        else if (bounds.bottom > viewport.bottom)
            scroll_form(MulDiv(bounds.bottom - viewport.bottom, 96, static_cast<int>(dpi)) + 4);
    }
    void scroll_form(int amount) {
        if (page != 0) return;
        scroll_position += amount;
        RECT client{}; GetClientRect(form, &client);
        layout_settings(MulDiv(client.bottom, 96, static_cast<int>(dpi)));
        RedrawWindow(form, nullptr, nullptr,
            RDW_INVALIDATE | RDW_ERASE | RDW_FRAME | RDW_ALLCHILDREN);
    }
    void check_executable(const std::filesystem::path& executable) {
        if (!std::filesystem::is_regular_file(executable))
            throw std::runtime_error("找不到工具，请将 EXE 放在配置器旁：\n" + utf8(executable.wstring()));
    }
    void action(int id, int notification) {
        if (populating) return;
        if (id >= edit_base && id < edit_base + static_cast<int>(fields.size())) {
            const auto index = static_cast<std::size_t>(id - edit_base);
            const auto& field = config_fields()[index];
            if ((field.kind == field_kind::number && notification == EN_SETFOCUS) ||
                (field.kind != field_kind::number && notification == BN_SETFOCUS)) {
                reveal(fields[index].input); return;
            }
            if (field.kind == field_kind::curve && notification == BN_CLICKED) {
                auto points = parse_curve(document.values.at(field.name));
                if (edit_curve(window, points, field.label)) {
                    document.set(field.name, format_curve(points));
                    title(); SetWindowTextW(command, L"");
                }
                return;
            }
            if ((field.kind == field_kind::key || field.kind == field_kind::input) && notification == BN_CLICKED) {
                stop_capture();
                capture_index = static_cast<int>(index);
                SetWindowTextW(fields[index].input, L"请按键…");
                SetWindowTextW(capture_label, field.kind == field_kind::input
                    ? L"按键盘或鼠标键绑定；也可清除绑定。" : L"按下单个目标键立即绑定；点击取消或切换窗口可退出录入。");
                layout(); SetFocus(window); return;
            }
            if (field.kind == field_kind::toggle && notification == BN_CLICKED)
                document.set(field.name, Button_GetCheck(fields[index].input) == BST_CHECKED ? "1" : "0");
            else if (field.kind == field_kind::number && notification == EN_CHANGE)
                document.set(field.name, utf8(window_text(fields[index].input)));
            else return;
            update_curve_controls();
            title(); SetWindowTextW(command, L"");
            if (field.kind == field_kind::toggle) layout();
            return;
        }
        if (id >= section_base && id < section_base + static_cast<int>(panels.size())) {
            auto& panel = panels[static_cast<std::size_t>(id - section_base)];
            if (notification == BN_SETFOCUS) { reveal(panel.header); return; }
            if (notification == BN_CLICKED) {
                stop_capture();
                if (panel.spec.toggle.empty()) panel.expanded = !panel.expanded;
                else {
                    document.set(panel.spec.toggle, panel_open(panel) ? "0" : "1");
                    update_curve_controls(); title(); SetWindowTextW(command, L"");
                }
                layout(); reveal(panel.header);
            }
            return;
        }
        if (id == cancel_id) { stop_capture(); return; }
        if (id == clear_binding_id && capture_index >= 0) {
            document.set(config_fields()[static_cast<std::size_t>(capture_index)].name, "NONE");
            stop_capture(); title(); SetWindowTextW(command, L""); return;
        }
        if (id == backend_id && notification == CBN_SELCHANGE) {
            const bool user = SendMessageW(backend, CB_GETCURSEL, 0, 0) == 0;
            // Restore selection before any operation that may cancel or throw.
            SendMessageW(backend, CB_SETCURSEL, document.user_mode ? 0 : 1, 0);
            if (user == document.user_mode) return;
            document.user_mode = user;
            populate(); return;
        }
        if (notification != BN_CLICKED) return;
        stop_capture();
        switch (id) {
        case new_id:
            if (may_discard()) { document.reset(document.user_mode, {}); populate(); }
            break;
        case open_id: {
            if (!may_discard()) break;
            const auto file = pick_file(window, false, document.path);
            if (!file.empty()) { document.open(document.user_mode, file); populate(); }
            break;
        }
        case save_id: save(false); break;
        case save_as_id: save(true); break;
        case defaults_id:
            if (MessageBoxW(window, L"将当前草稿恢复为程序默认值？文件只会在保存后改变。", L"恢复默认",
                    MB_YESNO | MB_ICONQUESTION) == IDYES) { document.defaults(); populate(); }
            break;
        case copy_id: case steam_id: {
            const auto line = runtime_command(directory, document, id == steam_id);
            SetWindowTextW(command, line.c_str()); clipboard(window, line);
            set_status(id == steam_id ? L"Steam 启动选项已复制，粘贴到游戏属性的启动选项。" : L"运行命令已复制，可粘贴到 cmd.exe；PowerShell 请在命令前加 &。");
            break;
        }
        case run_id: {
            if (runtime_process.active())
                throw std::runtime_error("由本窗口启动的运行程序仍在运行；请先在其终端退出，再重新启动。");
            const auto executable = runtime_executable(directory, document.user_mode);
            check_executable(executable);
            if (!save(false)) break;
            runtime_process.start(executable, {L"--config", document.path.wstring()}, false);
            set_status(L"已启动独立终端，默认 OFF。关闭本窗口不影响运行；终端 Ctrl+C 退出。");
            break;
        }
        case check_id: case install_id: {
            if (document.user_mode || tool.active()) break;
            const auto executable = runtime_executable(directory, false);
            check_executable(executable);
            install_running = id == install_id;
            tool.start(executable, {install_running ? L"--install-driver" : L"--check"}, true);
            tool_output.clear(); SetWindowTextW(output, L"正在运行…"); layout();
            break;
        }
        default: break;
        }
    }
    void poll() {
        if (runtime_process.active()) {
            DWORD exit_code = 0;
            std::string unused_output;
            if (runtime_process.poll(unused_output, exit_code)) {
                wchar_t code[32]{};
                swprintf_s(code, L"0x%08lX", exit_code);
                const auto note = std::wstring(L"运行程序已退出，退出码 ") + code + L"。";
                set_status(note);
                if (exit_code != 0)
                    MessageBoxW(window, (note + L"\n请确认 GUI 与两个终端来自同一次构建。旧终端可能无法读取新配置字段。"
                        L"\n可复制运行命令到已有终端查看具体错误。").c_str(), L"运行程序未正常结束", MB_OK | MB_ICONERROR);
            }
        }
        if (!tool.active()) return;
        DWORD code = 0;
        const auto previous = tool_output.size();
        const auto finished = tool.poll(tool_output, code);
        if (finished) {
            tool_output += "\r\nExit code: " + std::to_string(code);
            set_status(code == ERROR_SUCCESS_REBOOT_REQUIRED ? L"驱动安装完成，需要手动重启 Windows。" :
                code == ERROR_CANCELLED ? L"已取消驱动安装。" : code != 0 ? L"工具执行失败，请查看输出。" :
                install_running ? L"驱动安装程序已完成。" : L"驱动检查完成；不代表实际过滤或游戏兼容性已验证。");
            layout();
        }
        if (finished || previous != tool_output.size()) {
            auto text = wide(tool_output);
            // CLI output may use bare LF when redirected to a pipe.
            for (std::size_t i = 0; i < text.size(); ++i) if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) text.insert(i++, 1, L'\r');
            SetWindowTextW(output, text.c_str());
        }
    }
};

LRESULT CALLBACK form_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR, DWORD_PTR data) {
    const auto* app = reinterpret_cast<const editor*>(data);
    switch (message) {
    case WM_ERASEBKGND:
        // WM_PAINT fills every exposed pixel. Composite the form's children
        // together so background erasure and control painting are not separate frames.
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};
        const auto dc = BeginPaint(window, &paint);
        app->paint_settings(dc);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_PRINTCLIENT:
        app->paint_settings(reinterpret_cast<HDC>(wparam)); return 0;
    case WM_DRAWITEM: case WM_COMMAND: case WM_CTLCOLORSTATIC: case WM_VSCROLL: case WM_MOUSEWHEEL:
        return SendMessageW(GetParent(window), message, wparam, lparam);
    default:
        return DefSubclassProc(window, message, wparam, lparam);
    }
}

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* app = reinterpret_cast<editor*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        app = static_cast<editor*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        app->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(app));
    }
    if (!app) return DefWindowProcW(window, message, wparam, lparam);
    try {
        switch (message) {
        case WM_CREATE: app->create(); return 0;
        case WM_DRAWITEM:
            if (app->draw_panel(*reinterpret_cast<DRAWITEMSTRUCT*>(lparam))) return TRUE;
            break;
        case WM_CTLCOLORSTATIC:
            if (reinterpret_cast<HWND>(lparam) == app->dirty_label && (app->document.dirty || app->document.path.empty())) {
                const auto dc = reinterpret_cast<HDC>(wparam);
                SetTextColor(dc, RGB(112, 48, 0)); SetBkColor(dc, RGB(255, 222, 150));
                return reinterpret_cast<LRESULT>(app->dirty_brush);
            }
            break;
        case WM_MOUSEWHEEL:
            app->wheel_remainder += GET_WHEEL_DELTA_WPARAM(wparam);
            app->scroll_form(-90 * (app->wheel_remainder / WHEEL_DELTA));
            app->wheel_remainder %= WHEEL_DELTA;
            return 0;
        case WM_VSCROLL: {
            SCROLLINFO scroll{sizeof(scroll), SIF_ALL}; GetScrollInfo(app->form, SB_VERT, &scroll);
            int next = app->scroll_position;
            switch (LOWORD(wparam)) {
            case SB_LINEUP: next -= 30; break;
            case SB_LINEDOWN: next += 30; break;
            case SB_PAGEUP: next -= static_cast<int>(scroll.nPage); break;
            case SB_PAGEDOWN: next += static_cast<int>(scroll.nPage); break;
            case SB_THUMBTRACK: case SB_THUMBPOSITION: next = scroll.nTrackPos; break;
            case SB_TOP: next = 0; break;
            case SB_BOTTOM: next = scroll.nMax; break;
            default: return 0;
            }
            app->scroll_form(next - app->scroll_position); return 0;
        }
        case WM_COMMAND: app->action(LOWORD(wparam), HIWORD(wparam)); return 0;
        case WM_NOTIFY:
            if (reinterpret_cast<NMHDR*>(lparam)->idFrom == tab_id && reinterpret_cast<NMHDR*>(lparam)->code == TCN_SELCHANGE) {
                app->stop_capture(); app->page = TabCtrl_GetCurSel(app->tabs); app->instructions(); app->layout();
            }
            return 0;
        case WM_SIZE:
            if (wparam != SIZE_MINIMIZED && app->tabs) app->layout();
            return 0;
        case WM_GETMINMAXINFO: {
            auto* limits = reinterpret_cast<MINMAXINFO*>(lparam);
            limits->ptMinTrackSize = {app->px(780), app->px(660)}; return 0;
        }
        case WM_DPICHANGED: {
            app->dpi = HIWORD(wparam); app->update_font();
            SendMessageW(app->tooltip, TTM_SETMAXTIPWIDTH, 0, app->px(440));
            const auto* rect = reinterpret_cast<RECT*>(lparam);
            SetWindowPos(window, nullptr, rect->left, rect->top, rect->right - rect->left, rect->bottom - rect->top, SWP_NOZORDER | SWP_NOACTIVATE);
            app->layout(); return 0;
        }
        case WM_ACTIVATE: if (LOWORD(wparam) == WA_INACTIVE) { app->stop_capture(); app->swallowed_key = 0; } break;
        case WM_TIMER: app->poll(); return 0;
        case WM_CLOSE:
            if (app->tool.active()) {
                MessageBoxW(window, L"驱动工具仍在运行，请等待结果后关闭配置器。", L"工具正在运行", MB_OK | MB_ICONINFORMATION);
                return 0;
            }
            if (app->may_discard()) DestroyWindow(window);
            return 0;
        case WM_DESTROY: KillTimer(window, 1); PostQuitMessage(0); return 0;
        default: break;
        }
    } catch (const std::exception& exception) {
        app->error(exception);
        if (message == WM_CREATE) return -1;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
}

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int show) {
    using namespace mouse_mapping;
    const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return 1;
    int result = 1;
    try {
        INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_TAB_CLASSES | ICC_STANDARD_CLASSES};
        if (!InitCommonControlsEx(&controls)) throw std::runtime_error("Cannot initialize Windows controls");
        WNDCLASSEXW type{sizeof(type)};
        type.lpfnWndProc = window_proc; type.hInstance = instance;
        type.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        type.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
        type.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_BTNFACE + 1);
        type.lpszClassName = L"MouseMappingConfigEditor";
        if (!RegisterClassExW(&type)) throw std::runtime_error("Cannot register configuration window");
        editor app;
        const auto dpi = GetDpiForSystem();
        const auto window = CreateWindowExW(WS_EX_CONTROLPARENT, type.lpszClassName, L"鼠标输入映射配置器",
            WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
            MulDiv(860, static_cast<int>(dpi), 96), MulDiv(680, static_cast<int>(dpi), 96),
            nullptr, nullptr, instance, &app);
        if (!window) throw std::runtime_error("Cannot open configuration editor");
        ShowWindow(window, show); UpdateWindow(window);
        MSG message{};
        BOOL received;
        while ((received = GetMessageW(&message, nullptr, 0, 0)) > 0) {
            if (app.capture_message(message)) continue;
            if (!IsDialogMessageW(window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
        }
        result = received == -1 ? 1 : static_cast<int>(message.wParam);
    } catch (const std::exception& exception) {
        MessageBoxW(nullptr, wide(exception.what()).c_str(), L"配置器启动失败", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}
