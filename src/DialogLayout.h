#pragma once

#include <windows.h>

namespace dialog_layout {

constexpr UINT kDefaultDpi = 96;

struct MonitorWorkArea {
    RECT work{};
    UINT dpi = kDefaultDpi;
};

struct WindowPlacement {
    UINT dpi = kDefaultDpi;
    RECT work_area{};
    RECT desired_rect{};
    RECT final_rect{};
};

int scale_for_dpi(int logical_value, UINT dpi) noexcept;
UINT get_effective_window_dpi(HWND parent) noexcept;
MonitorWorkArea get_nearest_monitor_work_area(HWND parent) noexcept;
RECT clamp_window_rect_to_work_area(
    RECT desired, RECT work_area, int margin) noexcept;
RECT center_and_clamp_window_rect(
    const RECT& anchor, SIZE desired_size, const RECT& work_area,
    int margin) noexcept;
SIZE adjust_window_size_for_dpi(
    SIZE client_size, DWORD style, DWORD extended_style, UINT dpi) noexcept;
WindowPlacement calculate_initial_window_placement(
    HWND parent, SIZE logical_client_size, DWORD style,
    DWORD extended_style, int logical_margin) noexcept;
RECT clamp_dpi_changed_rect(
    const RECT& suggested, UINT dpi, int logical_margin) noexcept;
void debug_log_window_placement(
    const wchar_t* dialog_type, const WindowPlacement& placement,
    const RECT* client_rect = nullptr) noexcept;

}  // namespace dialog_layout
