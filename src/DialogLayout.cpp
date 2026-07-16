#include "DialogLayout.h"

#include <algorithm>
#include <cwchar>
#include <limits>

namespace dialog_layout {
namespace {

using AdjustWindowRectExForDpiFunction = BOOL(WINAPI*)(
    LPRECT, DWORD, BOOL, DWORD, UINT);

bool valid_rect(const RECT& value) noexcept {
    return value.right > value.left && value.bottom > value.top;
}

int positive_extent(LONG high, LONG low) noexcept {
    const auto extent = static_cast<long long>(high) - static_cast<long long>(low);
    return static_cast<int>((std::clamp)(
        extent, 1LL, static_cast<long long>((std::numeric_limits<int>::max)())));
}

RECT monitor_work_area(HMONITOR monitor) noexcept {
    MONITORINFO information{sizeof(information)};
    if (monitor != nullptr && GetMonitorInfoW(monitor, &information) &&
        valid_rect(information.rcWork)) {
        return information.rcWork;
    }
    RECT fallback{};
    if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &fallback, 0) &&
        valid_rect(fallback)) {
        return fallback;
    }
    return RECT{0, 0, 1, 1};
}

HMONITOR nearest_monitor(HWND parent) noexcept {
    if (parent != nullptr && IsWindow(parent)) {
        return MonitorFromWindow(parent, MONITOR_DEFAULTTONEAREST);
    }
    POINT cursor{};
    if (!GetCursorPos(&cursor)) cursor = POINT{0, 0};
    return MonitorFromPoint(cursor, MONITOR_DEFAULTTONEAREST);
}

RECT centered_rect(const RECT& anchor, SIZE size) noexcept {
    const long long anchor_x = static_cast<long long>(anchor.left) +
        (static_cast<long long>(anchor.right) - anchor.left) / 2;
    const long long anchor_y = static_cast<long long>(anchor.top) +
        (static_cast<long long>(anchor.bottom) - anchor.top) / 2;
    const long long left = anchor_x - static_cast<long long>(size.cx) / 2;
    const long long top = anchor_y - static_cast<long long>(size.cy) / 2;
    return RECT{
        static_cast<LONG>((std::clamp)(left,
            static_cast<long long>((std::numeric_limits<LONG>::min)()),
            static_cast<long long>((std::numeric_limits<LONG>::max)()))),
        static_cast<LONG>((std::clamp)(top,
            static_cast<long long>((std::numeric_limits<LONG>::min)()),
            static_cast<long long>((std::numeric_limits<LONG>::max)()))),
        0,
        0};
}

}  // namespace

int scale_for_dpi(int logical_value, UINT dpi) noexcept {
    const UINT effective = dpi == 0 ? kDefaultDpi : dpi;
    return MulDiv(logical_value, static_cast<int>(effective),
        static_cast<int>(kDefaultDpi));
}

UINT get_effective_window_dpi(HWND parent) noexcept {
    if (parent != nullptr && IsWindow(parent)) {
        const UINT dpi = GetDpiForWindow(parent);
        if (dpi != 0) return dpi;
    }
    return kDefaultDpi;
}

MonitorWorkArea get_nearest_monitor_work_area(HWND parent) noexcept {
    return MonitorWorkArea{
        monitor_work_area(nearest_monitor(parent)),
        get_effective_window_dpi(parent)};
}

RECT clamp_window_rect_to_work_area(
    RECT desired, RECT work_area, int margin) noexcept {
    if (!valid_rect(work_area)) work_area = RECT{0, 0, 1, 1};
    margin = (std::max)(0, margin);
    LONG left_bound = work_area.left + margin;
    LONG top_bound = work_area.top + margin;
    LONG right_bound = work_area.right - margin;
    LONG bottom_bound = work_area.bottom - margin;
    if (right_bound <= left_bound || bottom_bound <= top_bound) {
        left_bound = work_area.left;
        top_bound = work_area.top;
        right_bound = work_area.right;
        bottom_bound = work_area.bottom;
    }
    const int available_width = positive_extent(right_bound, left_bound);
    const int available_height = positive_extent(bottom_bound, top_bound);
    const int width = (std::min)(
        positive_extent(desired.right, desired.left), available_width);
    const int height = (std::min)(
        positive_extent(desired.bottom, desired.top), available_height);
    const LONG left = (std::clamp)(
        desired.left, left_bound, right_bound - width);
    const LONG top = (std::clamp)(
        desired.top, top_bound, bottom_bound - height);
    return RECT{left, top, left + width, top + height};
}

RECT center_and_clamp_window_rect(
    const RECT& anchor, SIZE desired_size, const RECT& work_area,
    int margin) noexcept {
    desired_size.cx = (std::max)(1L, desired_size.cx);
    desired_size.cy = (std::max)(1L, desired_size.cy);
    RECT desired = centered_rect(anchor, desired_size);
    desired.right = desired.left + desired_size.cx;
    desired.bottom = desired.top + desired_size.cy;
    return clamp_window_rect_to_work_area(desired, work_area, margin);
}

SIZE adjust_window_size_for_dpi(
    SIZE client_size, DWORD style, DWORD extended_style, UINT dpi) noexcept {
    RECT bounds{0, 0, (std::max)(1L, client_size.cx),
        (std::max)(1L, client_size.cy)};
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto adjust_for_dpi = reinterpret_cast<AdjustWindowRectExForDpiFunction>(
        user32 == nullptr ? nullptr :
        GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    BOOL adjusted = FALSE;
    if (adjust_for_dpi != nullptr) {
        adjusted = adjust_for_dpi(
            &bounds, style, FALSE, extended_style,
            dpi == 0 ? kDefaultDpi : dpi);
    }
    if (!adjusted) {
        adjusted = AdjustWindowRectEx(&bounds, style, FALSE, extended_style);
    }
    if (!adjusted) return client_size;
    return SIZE{
        (std::max)(1L, bounds.right - bounds.left),
        (std::max)(1L, bounds.bottom - bounds.top)};
}

WindowPlacement calculate_initial_window_placement(
    HWND parent, SIZE logical_client_size, DWORD style,
    DWORD extended_style, int logical_margin) noexcept {
    const MonitorWorkArea monitor = get_nearest_monitor_work_area(parent);
    const SIZE scaled_client{
        scale_for_dpi(static_cast<int>(logical_client_size.cx), monitor.dpi),
        scale_for_dpi(static_cast<int>(logical_client_size.cy), monitor.dpi)};
    const SIZE desired_size = adjust_window_size_for_dpi(
        scaled_client, style, extended_style, monitor.dpi);
    RECT anchor = monitor.work;
    if (parent != nullptr && IsWindow(parent) && !IsIconic(parent)) {
        RECT parent_rect{};
        if (GetWindowRect(parent, &parent_rect) && valid_rect(parent_rect)) {
            anchor = parent_rect;
        }
    }
    RECT desired = centered_rect(anchor, desired_size);
    desired.right = desired.left + desired_size.cx;
    desired.bottom = desired.top + desired_size.cy;
    const RECT final_rect = clamp_window_rect_to_work_area(
        desired, monitor.work, scale_for_dpi(logical_margin, monitor.dpi));
    return WindowPlacement{
        monitor.dpi, monitor.work, desired, final_rect};
}

RECT clamp_dpi_changed_rect(
    const RECT& suggested, UINT dpi, int logical_margin) noexcept {
    HMONITOR monitor = MonitorFromRect(&suggested, MONITOR_DEFAULTTONEAREST);
    const RECT work = monitor_work_area(monitor);
    return clamp_window_rect_to_work_area(
        suggested, work, scale_for_dpi(logical_margin, dpi));
}

void debug_log_window_placement(
    const wchar_t* dialog_type, const WindowPlacement& placement,
    const RECT* client_rect) noexcept {
#if defined(_DEBUG)
    wchar_t message[768]{};
    const RECT client = client_rect == nullptr ? RECT{} : *client_rect;
    _snwprintf_s(message, _TRUNCATE,
        L"dialog_layout: type=%ls effective_dpi=%u "
        L"monitor_work_area=(%ld,%ld,%ld,%ld) "
        L"desired_window_rect=(%ld,%ld,%ld,%ld) "
        L"clamped_window_rect=(%ld,%ld,%ld,%ld) "
        L"client_rect=(%ld,%ld,%ld,%ld)\n",
        dialog_type == nullptr ? L"unknown" : dialog_type,
        placement.dpi,
        placement.work_area.left, placement.work_area.top,
        placement.work_area.right, placement.work_area.bottom,
        placement.desired_rect.left, placement.desired_rect.top,
        placement.desired_rect.right, placement.desired_rect.bottom,
        placement.final_rect.left, placement.final_rect.top,
        placement.final_rect.right, placement.final_rect.bottom,
        client.left, client.top, client.right, client.bottom);
    OutputDebugStringW(message);
#else
    (void)dialog_type;
    (void)placement;
    (void)client_rect;
#endif
}

}  // namespace dialog_layout
