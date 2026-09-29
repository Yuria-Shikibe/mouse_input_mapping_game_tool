#include "pie_overlay.hpp"
#include <objidl.h>
#include <gdiplus.h>
#include <shellscalingapi.h>
#include <iostream>
#include <stdexcept>

namespace mouse_mapping {
namespace {
constexpr wchar_t window_class[] = L"mouse_mapping_pie_overlay";
constexpr auto frame_interval = std::chrono::microseconds(8334);
LRESULT CALLBACK overlay_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_NCHITTEST) return HTTRANSPARENT;
    if (message == WM_MOUSEACTIVATE) return MA_NOACTIVATE;
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_PAINT) {
        PAINTSTRUCT paint{}; BeginPaint(window, &paint); EndPaint(window, &paint); return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}
struct surfaces {
    std::array<HBITMAP, 5> images{};
    int size = 0;
    ~surfaces() { clear(); }
    void clear() { for (auto& item : images) { if (item) DeleteObject(item); item = nullptr; } }
    void create(UINT dpi) {
        clear();
        size = MulDiv(240, static_cast<int>(dpi), 96);
        for (unsigned state = 0; state < images.size(); ++state) {
            BITMAPINFO info{};
            info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            info.bmiHeader.biWidth = size; info.bmiHeader.biHeight = -size;
            info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
            info.bmiHeader.biCompression = BI_RGB;
            void* pixels = nullptr;
            images[state] = CreateDIBSection(nullptr, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
            if (!images[state]) throw std::runtime_error("Cannot allocate Pie image");
            Gdiplus::Bitmap image(size, size, size * 4, PixelFormat32bppPARGB, static_cast<BYTE*>(pixels));
            Gdiplus::Graphics g(&image);
            g.Clear(Gdiplus::Color(0, 0, 0, 0));
            g.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
            g.ScaleTransform(static_cast<float>(size) / 240.f, static_cast<float>(size) / 240.f);
            Gdiplus::SolidBrush base(Gdiplus::Color(218, 25, 29, 38));
            Gdiplus::SolidBrush highlight(Gdiplus::Color(242, 38, 139, 216));
            Gdiplus::Pen border(Gdiplus::Color(210, 163, 182, 206), 1.2f);
            g.FillEllipse(&base, 3.f, 3.f, 234.f, 234.f);
            constexpr std::array<float, 5> starts{0, 225, 45, 135, 315};
            if (state) g.FillPie(&highlight, 3.f, 3.f, 234.f, 234.f, starts[state], 90.f);
            g.DrawEllipse(&border, 3.f, 3.f, 234.f, 234.f);
            g.DrawLine(&border, 37.f, 37.f, 203.f, 203.f);
            g.DrawLine(&border, 203.f, 37.f, 37.f, 203.f);
            Gdiplus::SolidBrush center(Gdiplus::Color(255, 32, 37, 47));
            g.FillEllipse(&center, 92.f, 92.f, 56.f, 56.f);
            g.DrawEllipse(&border, 92.f, 92.f, 56.f, 56.f);
            // Vector arrows avoid font fallback and glyph rasterization on updates.
            Gdiplus::Pen arrow(Gdiplus::Color(255, 241, 247, 255), 3.f);
            arrow.SetStartCap(Gdiplus::LineCapRound); arrow.SetEndCap(Gdiplus::LineCapRound);
            for (int i = 0; i < 4; ++i) {
                const auto saved = g.Save();
                g.TranslateTransform(120.f, 120.f);
                g.RotateTransform(static_cast<float>(i * 90));
                g.DrawLine(&arrow, 0.f, -60.f, 0.f, -85.f);
                g.DrawLine(&arrow, -9.f, -76.f, 0.f, -85.f);
                g.DrawLine(&arrow, 9.f, -76.f, 0.f, -85.f);
                g.Restore(saved);
            }
            Gdiplus::Pen cancel(Gdiplus::Color(220, 159, 171, 190), 2.f);
            g.DrawLine(&cancel, 115.f, 115.f, 125.f, 125.f);
            g.DrawLine(&cancel, 125.f, 115.f, 115.f, 125.f);
        }
    }
};
struct graphics_session {
    ULONG_PTR token{};
    HWND window{};
    HDC dc{};
    bool registered = false;
    ~graphics_session() {
        if (window) DestroyWindow(window);
        if (dc) DeleteDC(dc);
        if (registered) UnregisterClassW(window_class, GetModuleHandleW(nullptr));
        if (token) Gdiplus::GdiplusShutdown(token);
    }
};
}

void pie_overlay::start() {
    if (thread_.joinable()) return;
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!wake_ || !stop_) throw std::runtime_error("Cannot create Pie overlay events");
    thread_ = std::thread([this] { run(); });
}
pie_overlay::~pie_overlay() {
    if (stop_) SetEvent(stop_);
    if (thread_.joinable()) thread_.join();
    if (stop_) CloseHandle(stop_);
    if (wake_) CloseHandle(wake_);
}
void pie_overlay::publish(bool visible, pie_direction selected, HWND foreground) noexcept {
    if (!wake_) return;
    // Atomic fields avoid data races even when the reader retries a publication.
    sequence_.fetch_add(1);
    foreground_.store(foreground);
    state_.store(visible ? 1u + static_cast<unsigned>(selected) : 0u);
    sequence_.fetch_add(1);
    if (!notified_.exchange(true)) SetEvent(wake_);
}
void pie_overlay::run() noexcept {
    try {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_NORMAL);
        SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        graphics_session session;
        Gdiplus::GdiplusStartupInput startup;
        if (Gdiplus::GdiplusStartup(&session.token, &startup, nullptr) != Gdiplus::Ok)
            throw std::runtime_error("Cannot initialize Pie graphics");
        WNDCLASSW wc{};
        wc.lpfnWndProc = overlay_proc; wc.hInstance = GetModuleHandleW(nullptr); wc.lpszClassName = window_class;
        session.registered = RegisterClassW(&wc) != 0;
        if (!session.registered) throw std::runtime_error("Cannot register Pie window");
        session.window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_NOACTIVATE |
            WS_EX_TRANSPARENT | WS_EX_TOOLWINDOW, window_class, L"Pie", WS_POPUP,
            0, 0, 0, 0, nullptr, nullptr, wc.hInstance, nullptr);
        session.dc = CreateCompatibleDC(nullptr);
        if (!session.window || !session.dc) throw std::runtime_error("Cannot create Pie window");
        surfaces cached;
        UINT cached_dpi = 96;
        cached.create(cached_dpi);
        const HANDLE events[]{stop_, wake_};
        unsigned long long drawn_sequence = ~0ull;
        bool dirty = false, visible = false;
        auto next_frame = clock_type::now();
        for (;;) {
            DWORD timeout = INFINITE;
            if (dirty) {
                const auto remaining = std::chrono::ceil<milliseconds>(next_frame - clock_type::now()).count();
                timeout = static_cast<DWORD>(std::clamp<long long>(remaining, 0, 9));
            }
            const auto wait = MsgWaitForMultipleObjectsEx(2, events, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            if (wait == WAIT_OBJECT_0) break;
            if (wait == WAIT_FAILED) throw std::runtime_error("Pie window wait failed");
            if (wait == WAIT_OBJECT_0 + 1) { notified_.store(false); dirty = true; }
            MSG message{};
            for (int i = 0; i < 32 && PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE); ++i) {
                if (message.message == WM_QUIT) return;
                if (message.message == WM_DISPLAYCHANGE || message.message == WM_DPICHANGED) {
                    dirty = true; drawn_sequence = ~0ull;
                }
                DispatchMessageW(&message);
            }
            if (!dirty) continue;
            const auto sequence = sequence_.load();
            const auto state = state_.load();
            const auto foreground = foreground_.load();
            if ((sequence & 1) || sequence != sequence_.load()) { next_frame = clock_type::now(); continue; }
            if (sequence == drawn_sequence) { dirty = false; continue; }
            const bool show = state != 0 && foreground && GetForegroundWindow() == foreground;
            if (show && visible && clock_type::now() < next_frame) continue;
            if (!show) {
                if (visible) ShowWindow(session.window, SW_HIDE);
                visible = false;
            } else {
                const auto monitor = MonitorFromWindow(foreground, MONITOR_DEFAULTTONEAREST);
                MONITORINFO info{sizeof(info)};
                if (!GetMonitorInfoW(monitor, &info)) throw std::runtime_error("Cannot locate Pie monitor");
                UINT dx = 96, dy = 96;
                if (FAILED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &dx, &dy))) dx = 96;
                if (dx != cached_dpi) { cached.create(dx); cached_dpi = dx; }
                const auto previous = SelectObject(session.dc, cached.images[state - 1]);
                POINT destination{(info.rcMonitor.left + info.rcMonitor.right - cached.size) / 2,
                    (info.rcMonitor.top + info.rcMonitor.bottom - cached.size) / 2};
                POINT origin{};
                SIZE size{cached.size, cached.size};
                BLENDFUNCTION blend{AC_SRC_OVER, 0, 255, AC_SRC_ALPHA};
                const auto ok = UpdateLayeredWindow(session.window, nullptr, &destination, &size,
                    session.dc, &origin, 0, &blend, ULW_ALPHA);
                SelectObject(session.dc, previous);
                if (!ok) throw std::runtime_error("Cannot present Pie menu");
                SetWindowPos(session.window, HWND_TOPMOST, 0, 0, 0, 0,
                    SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
                visible = true;
                next_frame = clock_type::now() + frame_interval;
            }
            drawn_sequence = sequence;
            dirty = false;
        }
    } catch (const std::exception& error) {
        // Presentation failure must never disable input or prevent key cleanup.
        std::cerr << "Pie visualization unavailable; input remains active: " << error.what() << '\n';
    }
}
}
