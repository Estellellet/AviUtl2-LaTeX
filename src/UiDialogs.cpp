#include "UiDialogs.h"

#include <windows.h>
#include <commctrl.h>
#include <dwrite.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <cwctype>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <system_error>
#include <thread>
#include <vector>

#include "EnvironmentDiagnostics.h"
#include "AppPaths.h"
#include "DialogLayout.h"
#include "LatexRenderer.h"
#include "PluginInfo.h"
#include "ToolSettings.h"

using Microsoft::WRL::ComPtr;

namespace {

HWND dialog_owner = nullptr;

constexpr int IDC_TEX_ENVIRONMENT = 100;
constexpr int IDC_LUALATEX = 101;
constexpr int IDC_BROWSE_LUALATEX = 102;
constexpr int IDC_MUTOOL = 103;
constexpr int IDC_BROWSE_MUTOOL = 104;
constexpr int IDC_AUTO_DETECT = 105;
constexpr int IDC_DIAGNOSE = 106;
constexpr int IDC_DIAGNOSTIC = 107;
constexpr int IDC_SAVE = 108;
constexpr int IDC_CANCEL = 109;
constexpr int IDC_FONT_SEARCH = 200;
constexpr int IDC_FONT_LIST = 201;
constexpr int IDC_FONT_PREVIEW = 202;
constexpr int IDC_FONT_SELECT = 203;
constexpr int IDC_FONT_CANCEL = 204;
constexpr int IDC_INFO_TEXT = 300;
constexpr int IDC_INFO_COPY = 301;
constexpr int IDC_INFO_LOG = 302;
constexpr int IDC_INFO_LOG_FOLDER = 303;
constexpr int IDC_INFO_CLOSE = 304;
constexpr int IDC_INFO_STATUS = 305;
constexpr UINT WM_APP_DIAGNOSTIC_FINISHED = WM_APP + 0x241;
constexpr UINT_PTR IDT_DIAGNOSTIC_COMPLETION = 0x241;
constexpr int kDialogWorkMargin = 12;
constexpr DWORD kResizableDialogStyle =
    WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_THICKFRAME | WS_MAXIMIZEBOX;

using SystemParametersInfoForDpiFunction = BOOL(WINAPI*)(
    UINT, UINT, PVOID, UINT, UINT);
using GetDpiForSystemFunction = UINT(WINAPI*)();

UINT valid_window_dpi(HWND window, UINT fallback = dialog_layout::kDefaultDpi) {
    const UINT dpi = window == nullptr ? 0 : GetDpiForWindow(window);
    return dpi == 0 ? fallback : dpi;
}

HFONT create_dialog_font(UINT dpi) {
    NONCLIENTMETRICSW metrics{sizeof(metrics)};
    const HMODULE user32 = GetModuleHandleW(L"user32.dll");
    const auto parameters_for_dpi =
        reinterpret_cast<SystemParametersInfoForDpiFunction>(
            user32 == nullptr ? nullptr :
            GetProcAddress(user32, "SystemParametersInfoForDpi"));
    if (parameters_for_dpi != nullptr && parameters_for_dpi(
            SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0,
            dpi == 0 ? dialog_layout::kDefaultDpi : dpi)) {
        return CreateFontIndirectW(&metrics.lfMessageFont);
    }
    if (!SystemParametersInfoW(
            SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0)) {
        return nullptr;
    }
    const auto get_system_dpi = reinterpret_cast<GetDpiForSystemFunction>(
        user32 == nullptr ? nullptr : GetProcAddress(user32, "GetDpiForSystem"));
    const UINT system_dpi = get_system_dpi == nullptr
        ? dialog_layout::kDefaultDpi
        : (std::max)(dialog_layout::kDefaultDpi, get_system_dpi());
    metrics.lfMessageFont.lfHeight = MulDiv(
        metrics.lfMessageFont.lfHeight,
        static_cast<int>(dpi == 0 ? dialog_layout::kDefaultDpi : dpi),
        static_cast<int>(system_dpi));
    return CreateFontIndirectW(&metrics.lfMessageFont);
}

BOOL CALLBACK apply_font_to_child(HWND child, LPARAM font_value) {
    SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font_value), TRUE);
    return TRUE;
}

void replace_dialog_font(HWND window, HFONT& owned_font, UINT dpi) {
    HFONT replacement = create_dialog_font(dpi);
    if (replacement == nullptr) return;
    EnumChildWindows(window, apply_font_to_child,
        reinterpret_cast<LPARAM>(replacement));
    HFONT previous = owned_font;
    owned_font = replacement;
    if (previous != nullptr) DeleteObject(previous);
}

int dialog_font_height(HWND window, HFONT font, UINT dpi) {
    HDC dc = GetDC(window);
    if (dc == nullptr) return dialog_layout::scale_for_dpi(16, dpi);
    HGDIOBJ previous = nullptr;
    if (font != nullptr) previous = SelectObject(dc, font);
    TEXTMETRICW metrics{};
    const bool measured = GetTextMetricsW(dc, &metrics) != FALSE;
    if (previous != nullptr) SelectObject(dc, previous);
    ReleaseDC(window, dc);
    return measured ? metrics.tmHeight : dialog_layout::scale_for_dpi(16, dpi);
}

int measured_button_width(
    HWND window, HFONT font, UINT dpi, const wchar_t* text, int minimum_logical) {
    SIZE extent{};
    HDC dc = GetDC(window);
    HGDIOBJ previous = nullptr;
    if (dc != nullptr && font != nullptr) previous = SelectObject(dc, font);
    const bool measured = dc != nullptr && GetTextExtentPoint32W(
        dc, text, static_cast<int>(wcslen(text)), &extent) != FALSE;
    if (previous != nullptr) SelectObject(dc, previous);
    if (dc != nullptr) ReleaseDC(window, dc);
    const int measured_width = measured
        ? extent.cx + dialog_layout::scale_for_dpi(24, dpi) : 0;
    return (std::max)(
        dialog_layout::scale_for_dpi(minimum_logical, dpi), measured_width);
}

void move_to_rect(HWND control, int left, int top, int width, int height) {
    if (control == nullptr) return;
    MoveWindow(control, left, top, (std::max)(1, width),
        (std::max)(1, height), TRUE);
}

void apply_minmax_limits(
    HWND window, MINMAXINFO& limits, SIZE minimum_logical, UINT dpi) {
    const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(window, GWL_STYLE));
    const DWORD extended = static_cast<DWORD>(
        GetWindowLongPtrW(window, GWL_EXSTYLE));
    const SIZE minimum_client{
        dialog_layout::scale_for_dpi(minimum_logical.cx, dpi),
        dialog_layout::scale_for_dpi(minimum_logical.cy, dpi)};
    const SIZE minimum_window = dialog_layout::adjust_window_size_for_dpi(
        minimum_client, style, extended, dpi);
    const auto monitor = dialog_layout::get_nearest_monitor_work_area(window);
    const LONG work_width = monitor.work.right - monitor.work.left;
    const LONG work_height = monitor.work.bottom - monitor.work.top;
    const int margin = dialog_layout::scale_for_dpi(kDialogWorkMargin, dpi);
    limits.ptMinTrackSize.x = (std::min)(
        minimum_window.cx, (std::max)(1L, work_width - margin * 2));
    limits.ptMinTrackSize.y = (std::min)(
        minimum_window.cy, (std::max)(1L, work_height - margin * 2));
    limits.ptMaxTrackSize.x = (std::max)(1L, work_width);
    limits.ptMaxTrackSize.y = (std::max)(1L, work_height);
}

dialog_layout::WindowPlacement reconcile_created_window(
    HWND window, SIZE logical_client_size, DWORD style,
    DWORD extended_style) {
    auto placement = dialog_layout::calculate_initial_window_placement(
        window, logical_client_size, style, extended_style, kDialogWorkMargin);
    const RECT rect = placement.final_rect;
    SetWindowPos(window, nullptr, rect.left, rect.top,
        rect.right - rect.left, rect.bottom - rect.top,
        SWP_NOACTIVATE | SWP_NOZORDER);
    return placement;
}

std::wstring control_text(HWND control) {
    const int length = GetWindowTextLengthW(control);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    GetWindowTextW(control, value.data(), length + 1);
    value.resize(static_cast<std::size_t>(length));
    return value;
}

std::optional<std::filesystem::path> open_file(
    HWND owner, const COMDLG_FILTERSPEC* filters, UINT filter_count,
    const std::filesystem::path& current) {
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_PPV_ARGS(&dialog)))) {
        if (SUCCEEDED(initialized)) CoUninitialize();
        return std::nullopt;
    }
    dialog->SetFileTypes(filter_count, filters);
    dialog->SetOptions(FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
    if (!current.empty()) {
        ComPtr<IShellItem> item;
        if (SUCCEEDED(SHCreateItemFromParsingName(current.c_str(), nullptr,
                IID_PPV_ARGS(&item)))) {
            dialog->SetFileName(current.filename().c_str());
            ComPtr<IShellItem> parent;
            if (SUCCEEDED(item->GetParent(&parent))) dialog->SetFolder(parent.Get());
        }
    }
    std::optional<std::filesystem::path> selected;
    if (SUCCEEDED(dialog->Show(owner))) {
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) &&
            SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
            selected = std::filesystem::path(path);
        }
        if (path != nullptr) CoTaskMemFree(path);
    }
    if (SUCCEEDED(initialized)) CoUninitialize();
    return selected;
}

struct DiagnosticCompletion {
    std::mutex mutex;
    std::optional<DiagnosticReport> report;
};

struct EnvironmentWindow {
    ToolSettings settings;
    HWND window = nullptr;
    HWND environment = nullptr;
    HWND lualatex = nullptr;
    HWND mutool = nullptr;
    HWND diagnostic = nullptr;
    std::jthread diagnostic_worker;
    std::shared_ptr<DiagnosticCompletion> diagnostic_completion;
    std::uint64_t diagnostic_generation = 0;
    HFONT ui_font = nullptr;
    UINT dpi = dialog_layout::kDefaultDpi;
    int scroll_offset = 0;
    int content_height = 0;
    int content_viewport_height = 0;
    bool diagnostic_running = false;
    bool close_requested = false;
    bool done = false;
};

void enable_environment_inputs(EnvironmentWindow& state, bool enabled) {
    for (HWND control : {
            state.environment,
            state.lualatex,
            state.mutool,
            GetDlgItem(state.window, IDC_BROWSE_LUALATEX),
            GetDlgItem(state.window, IDC_BROWSE_MUTOOL),
            GetDlgItem(state.window, IDC_AUTO_DETECT),
            GetDlgItem(state.window, IDC_DIAGNOSE),
            GetDlgItem(state.window, IDC_SAVE)}) {
        if (control != nullptr) {
            EnableWindow(control, enabled ? TRUE : FALSE);
        }
    }
}

void request_environment_window_close(EnvironmentWindow& state) {
    if (!state.diagnostic_running) {
        DestroyWindow(state.window);
        return;
    }
    state.close_requested = true;
    if (state.diagnostic_worker.joinable()) {
        state.diagnostic_worker.request_stop();
    }
    EnableWindow(GetDlgItem(state.window, IDC_CANCEL), FALSE);
    SetWindowTextW(state.diagnostic, L"診断を中止しています...");
}

void position_environment_controls(EnvironmentWindow& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int client_width = (std::max)(1L, client.right - client.left);
    const int client_height = (std::max)(1L, client.bottom - client.top);
    const int margin = dialog_layout::scale_for_dpi(12, state.dpi);
    const int gap = dialog_layout::scale_for_dpi(8, state.dpi);
    const int font_height = dialog_font_height(
        state.window, state.ui_font, state.dpi);
    const int control_height = (std::max)(
        dialog_layout::scale_for_dpi(24, state.dpi),
        font_height + dialog_layout::scale_for_dpi(8, state.dpi));
    const int button_height = (std::max)(
        dialog_layout::scale_for_dpi(28, state.dpi),
        font_height + dialog_layout::scale_for_dpi(10, state.dpi));
    const int footer_height = button_height + margin * 2;
    const int footer_top = (std::max)(0, client_height - footer_height);
    state.content_viewport_height = (std::max)(1, footer_top);
    const int content_width = (std::max)(1, client_width - margin * 2);
    const bool narrow = client_width <
        dialog_layout::scale_for_dpi(540, state.dpi);
    const int browse_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"参照", 70);
    const int detect_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"自動検出", 100);
    const int diagnose_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"環境確認", 100);
    const int minimum_diagnostic_height = dialog_layout::scale_for_dpi(
        narrow ? 90 : 100, state.dpi);

    struct Placement { HWND control; int x; int y; int width; int height; };
    std::vector<Placement> placements;
    auto add = [&](HWND control, int x, int y, int width, int height) {
        placements.push_back(Placement{control, x, y, width, height});
    };

    int diagnostic_top = 0;
    int diagnostic_x = margin;
    int diagnostic_width = content_width;
    if (!narrow) {
        const int label_width = (std::min)(
            dialog_layout::scale_for_dpi(105, state.dpi), content_width / 3);
        const int input_x = margin + label_width + gap;
        const int input_width = (std::max)(1,
            client_width - margin - input_x);
        int y = margin;
        add(GetDlgItem(state.window, 1), margin, y, label_width, control_height);
        add(state.environment, input_x, y,
            (std::min)(input_width, dialog_layout::scale_for_dpi(190, state.dpi)),
            control_height * 8);
        y += control_height + gap;
        const int path_width = (std::max)(1, input_width - browse_width - gap);
        add(GetDlgItem(state.window, 2), margin, y, label_width, control_height);
        add(state.lualatex, input_x, y, path_width, control_height);
        add(GetDlgItem(state.window, IDC_BROWSE_LUALATEX),
            input_x + path_width + gap, y, browse_width, control_height);
        y += control_height + gap;
        add(GetDlgItem(state.window, 3), margin, y, label_width, control_height);
        add(state.mutool, input_x, y, path_width, control_height);
        add(GetDlgItem(state.window, IDC_BROWSE_MUTOOL),
            input_x + path_width + gap, y, browse_width, control_height);
        y += control_height + gap;
        add(GetDlgItem(state.window, IDC_AUTO_DETECT),
            input_x, y, detect_width, button_height);
        add(GetDlgItem(state.window, IDC_DIAGNOSE),
            input_x + detect_width + gap, y, diagnose_width, button_height);
        y += button_height + gap;
        add(GetDlgItem(state.window, 4), margin, y, label_width, control_height);
        diagnostic_top = y;
        diagnostic_x = input_x;
        diagnostic_width = input_width;
    } else {
        int y = margin;
        add(GetDlgItem(state.window, 1), margin, y, content_width, control_height);
        y += control_height;
        add(state.environment, margin, y, content_width, control_height * 8);
        y += control_height + gap;
        add(GetDlgItem(state.window, 2), margin, y, content_width, control_height);
        y += control_height;
        const int path_width = (std::max)(1, content_width - browse_width - gap);
        add(state.lualatex, margin, y, path_width, control_height);
        add(GetDlgItem(state.window, IDC_BROWSE_LUALATEX),
            margin + path_width + gap, y, browse_width, control_height);
        y += control_height + gap;
        add(GetDlgItem(state.window, 3), margin, y, content_width, control_height);
        y += control_height;
        add(state.mutool, margin, y, path_width, control_height);
        add(GetDlgItem(state.window, IDC_BROWSE_MUTOOL),
            margin + path_width + gap, y, browse_width, control_height);
        y += control_height + gap;
        add(GetDlgItem(state.window, IDC_AUTO_DETECT),
            margin, y, detect_width, button_height);
        add(GetDlgItem(state.window, IDC_DIAGNOSE),
            margin + detect_width + gap, y, diagnose_width, button_height);
        y += button_height + gap;
        add(GetDlgItem(state.window, 4), margin, y, content_width, control_height);
        y += control_height;
        diagnostic_top = y;
    }

    const int diagnostic_height = (std::max)(minimum_diagnostic_height,
        state.content_viewport_height - diagnostic_top - margin);
    add(state.diagnostic, diagnostic_x, diagnostic_top,
        diagnostic_width, diagnostic_height);
    state.content_height = diagnostic_top + diagnostic_height + margin;
    const int maximum_scroll = (std::max)(
        0, state.content_height - state.content_viewport_height);
    state.scroll_offset = (std::clamp)(
        state.scroll_offset, 0, maximum_scroll);

    SCROLLINFO scroll{sizeof(scroll)};
    scroll.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
    scroll.nMin = 0;
    scroll.nMax = (std::max)(0, state.content_height - 1);
    scroll.nPage = static_cast<UINT>(state.content_viewport_height);
    scroll.nPos = state.scroll_offset;
    SetScrollInfo(state.window, SB_VERT, &scroll, TRUE);

    for (const auto& placement : placements) {
        move_to_rect(placement.control, placement.x,
            placement.y - state.scroll_offset,
            placement.width, placement.height);
    }

    const int cancel_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"キャンセル", 76);
    const int save_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"保存", 76);
    const int buttons_width = save_width + gap + cancel_width;
    const int button_y = footer_top + (footer_height - button_height) / 2;
    const int button_x = (std::max)(margin,
        client_width - margin - buttons_width);
    move_to_rect(GetDlgItem(state.window, IDC_SAVE),
        button_x, button_y, save_width, button_height);
    move_to_rect(GetDlgItem(state.window, IDC_CANCEL),
        button_x + save_width + gap, button_y, cancel_width, button_height);
}

void scroll_environment(EnvironmentWindow& state, int requested_position) {
    const int maximum_scroll = (std::max)(
        0, state.content_height - state.content_viewport_height);
    const int position = (std::clamp)(requested_position, 0, maximum_scroll);
    if (position == state.scroll_offset) return;
    state.scroll_offset = position;
    position_environment_controls(state);
}

void ensure_environment_control_visible(EnvironmentWindow& state, HWND control) {
    if (control == nullptr || control == GetDlgItem(state.window, IDC_SAVE) ||
        control == GetDlgItem(state.window, IDC_CANCEL)) return;
    RECT rect{};
    if (!GetWindowRect(control, &rect)) return;
    MapWindowPoints(nullptr, state.window, reinterpret_cast<POINT*>(&rect), 2);
    const int content_top = rect.top + state.scroll_offset;
    const int content_bottom = rect.bottom + state.scroll_offset;
    if (content_top < state.scroll_offset) {
        scroll_environment(state, content_top);
    } else if (content_bottom >
            state.scroll_offset + state.content_viewport_height) {
        scroll_environment(state,
            content_bottom - state.content_viewport_height);
    }
}

LRESULT CALLBACK environment_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<EnvironmentWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = reinterpret_cast<EnvironmentWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_CREATE) {
        state->dpi = valid_window_dpi(window);
        auto make = [&](const wchar_t* klass, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
                0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                GetModuleHandleW(nullptr), nullptr);
        };
        make(L"STATIC", L"TeX環境", 0, 1);
        state->environment = make(WC_COMBOBOXW, L"", CBS_DROPDOWNLIST | WS_TABSTOP, IDC_TEX_ENVIRONMENT);
        for (const auto* name : {L"自動", L"MiKTeX", L"TeX Live", L"その他"})
            SendMessageW(state->environment, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(name));
        SendMessageW(state->environment, CB_SETCURSEL,
            static_cast<WPARAM>(state->settings.tex_environment), 0);
        make(L"STATIC", L"LuaLaTeXパス", 0, 2);
        state->lualatex = make(L"EDIT", state->settings.lualatex_path.c_str(),
            WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_LUALATEX);
        make(L"BUTTON", L"参照", BS_PUSHBUTTON | WS_TABSTOP, IDC_BROWSE_LUALATEX);
        make(L"STATIC", L"MuPDFパス", 0, 3);
        state->mutool = make(L"EDIT", state->settings.mutool_path.c_str(),
            WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP, IDC_MUTOOL);
        make(L"BUTTON", L"参照", BS_PUSHBUTTON | WS_TABSTOP, IDC_BROWSE_MUTOOL);
        make(L"BUTTON", L"自動検出", BS_PUSHBUTTON | WS_TABSTOP, IDC_AUTO_DETECT);
        make(L"BUTTON", L"環境確認", BS_PUSHBUTTON | WS_TABSTOP, IDC_DIAGNOSE);
        make(L"STATIC", L"診断結果", 0, 4);
        state->diagnostic = make(L"EDIT", L"未確認", WS_BORDER | ES_MULTILINE |
            ES_AUTOVSCROLL | ES_READONLY | WS_VSCROLL, IDC_DIAGNOSTIC);
        make(L"BUTTON", L"保存", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_SAVE);
        make(L"BUTTON", L"キャンセル", BS_PUSHBUTTON | WS_TABSTOP, IDC_CANCEL);
        replace_dialog_font(window, state->ui_font, state->dpi);
        position_environment_controls(*state);
        return 0;
    }
    if (message == WM_SIZE) {
        position_environment_controls(*state);
        return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        apply_minmax_limits(window,
            *reinterpret_cast<MINMAXINFO*>(lparam), SIZE{420, 280}, state->dpi);
        return 0;
    }
    if (message == WM_DPICHANGED) {
        const UINT new_dpi = LOWORD(wparam) == 0
            ? dialog_layout::kDefaultDpi : LOWORD(wparam);
        const RECT suggested = *reinterpret_cast<const RECT*>(lparam);
        const RECT final_rect = dialog_layout::clamp_dpi_changed_rect(
            suggested, new_dpi, kDialogWorkMargin);
        state->dpi = new_dpi;
        SetWindowPos(window, nullptr, final_rect.left, final_rect.top,
            final_rect.right - final_rect.left,
            final_rect.bottom - final_rect.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        replace_dialog_font(window, state->ui_font, state->dpi);
        position_environment_controls(*state);
        return 0;
    }
    if (message == WM_VSCROLL) {
        SCROLLINFO scroll{sizeof(scroll)};
        scroll.fMask = SIF_ALL;
        GetScrollInfo(window, SB_VERT, &scroll);
        int next = state->scroll_offset;
        const int line = dialog_layout::scale_for_dpi(28, state->dpi);
        switch (LOWORD(wparam)) {
        case SB_LINEUP: next -= line; break;
        case SB_LINEDOWN: next += line; break;
        case SB_PAGEUP: next -= state->content_viewport_height; break;
        case SB_PAGEDOWN: next += state->content_viewport_height; break;
        case SB_THUMBPOSITION:
        case SB_THUMBTRACK: next = scroll.nTrackPos; break;
        case SB_TOP: next = 0; break;
        case SB_BOTTOM: next = state->content_height; break;
        default: return 0;
        }
        scroll_environment(*state, next);
        return 0;
    }
    if (message == WM_MOUSEWHEEL) {
        const int lines = GET_WHEEL_DELTA_WPARAM(wparam) / WHEEL_DELTA;
        scroll_environment(*state, state->scroll_offset - lines *
            dialog_layout::scale_for_dpi(36, state->dpi));
        return 0;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam);
        if (lparam != 0) {
            ensure_environment_control_visible(
                *state, reinterpret_cast<HWND>(lparam));
        }
        if (id == IDC_BROWSE_LUALATEX || id == IDC_BROWSE_MUTOOL) {
            const COMDLG_FILTERSPEC filters[] = {
                {id == IDC_BROWSE_LUALATEX ? L"lualatex.exe" : L"mutool.exe",
                 id == IDC_BROWSE_LUALATEX ? L"lualatex.exe" : L"mutool.exe"},
                {L"実行ファイル (*.exe)", L"*.exe"}, {L"すべてのファイル", L"*.*"}};
            HWND target = id == IDC_BROWSE_LUALATEX ? state->lualatex : state->mutool;
            if (const auto selected = open_file(window, filters, 3, control_text(target)))
                SetWindowTextW(target, selected->c_str());
            return 0;
        }
        if (id == IDC_AUTO_DETECT) {
            ToolSettings temporary = state->settings;
            const std::filesystem::path current_lualatex =
                control_text(state->lualatex);
            const std::filesystem::path current_mutool =
                control_text(state->mutool);
            temporary.lualatex_path = is_valid_executable_file(current_lualatex)
                ? current_lualatex
                : std::filesystem::path{};
            temporary.mutool_path = is_valid_executable_file(current_mutool)
                ? current_mutool
                : std::filesystem::path{};
            const auto resolved = resolve_external_tools(temporary);
            if (!resolved.lualatex_path.empty()) {
                SetWindowTextW(state->lualatex, resolved.lualatex_path.c_str());
            }
            if (!resolved.mutool_path.empty()) {
                SetWindowTextW(state->mutool, resolved.mutool_path.c_str());
            }
            const bool lualatex_found = !resolved.lualatex_path.empty();
            const bool mutool_found = !resolved.mutool_path.empty();
            const wchar_t* diagnostic_message = nullptr;
            if (lualatex_found && mutool_found) {
                diagnostic_message = L"LuaLaTeXとMuPDFを検出しました。保存前に環境確認を実行できます。";
            } else if (!lualatex_found && !mutool_found) {
                diagnostic_message = L"LuaLaTeXとMuPDFを検出できません。参照から指定してください。";
            } else if (!lualatex_found) {
                diagnostic_message = L"LuaLaTeXを検出できません。参照から指定してください。";
            } else {
                diagnostic_message = L"MuPDFを検出できません。参照から指定してください。";
            }
            SetWindowTextW(state->diagnostic, diagnostic_message);
            return 0;
        }
        if (id == IDC_DIAGNOSE) {
            if (state->diagnostic_running) {
                return 0;
            }
            ToolSettings temporary;
            temporary.tex_environment = static_cast<TexEnvironment>(SendMessageW(state->environment, CB_GETCURSEL, 0, 0));
            temporary.lualatex_path = control_text(state->lualatex);
            temporary.mutool_path = control_text(state->mutool);
            if (state->diagnostic_worker.joinable()) {
                state->diagnostic_worker.join();
            }
            const auto completion = std::make_shared<DiagnosticCompletion>();
            state->diagnostic_completion = completion;
            const std::uint64_t generation = ++state->diagnostic_generation;
            state->diagnostic_running = true;
            state->close_requested = false;
            enable_environment_inputs(*state, false);
            SetWindowTextW(state->diagnostic, L"確認中...");
            const HWND result_window = window;
            try {
                state->diagnostic_worker = std::jthread(
                    [temporary = std::move(temporary), completion,
                     result_window, generation](std::stop_token stop_token) {
                        DiagnosticReport report;
                        try {
                            report = run_environment_diagnostics(
                                temporary, stop_token);
                        } catch (const std::exception&) {
                            try {
                                report.summary = L"環境診断に失敗しました。";
                                report.details =
                                    L"診断ワーカーで予期しないエラーが発生しました。";
                            } catch (...) {
                                // Preserve an empty movable report even when
                                // allocating fallback text is impossible.
                            }
                        } catch (...) {
                            try {
                                report.summary = L"環境診断に失敗しました。";
                                report.details =
                                    L"診断ワーカーで予期しないエラーが発生しました。";
                            } catch (...) {
                            }
                        }
                        {
                            std::lock_guard completion_lock(completion->mutex);
                            completion->report = std::move(report);
                        }
                        // The shared completion object owns the result. A
                        // failed or stale post therefore cannot leak a heap
                        // payload or dereference destroyed UI state.
                        PostMessageW(
                            result_window,
                            WM_APP_DIAGNOSTIC_FINISHED,
                            static_cast<WPARAM>(generation),
                            0);
                    });
                // Polling the shared completion on the UI thread is a fallback
                // if PostMessageW cannot enqueue the completion message.
                SetTimer(window, IDT_DIAGNOSTIC_COMPLETION, 100, nullptr);
            } catch (const std::system_error&) {
                state->diagnostic_running = false;
                state->diagnostic_completion.reset();
                enable_environment_inputs(*state, true);
                SetWindowTextW(
                    state->diagnostic,
                    L"診断ワーカースレッドを開始できませんでした。");
            }
            return 0;
        }
        if (id == IDC_SAVE || id == IDOK) {
            ToolSettings value;
            value.tex_environment = static_cast<TexEnvironment>(SendMessageW(state->environment, CB_GETCURSEL, 0, 0));
            value.lualatex_path = control_text(state->lualatex);
            value.mutool_path = control_text(state->mutool);
            value.last_diagnostic_summary = state->settings.last_diagnostic_summary;
            value.last_diagnostic_details = state->settings.last_diagnostic_details;
            value.last_lualatex_version = state->settings.last_lualatex_version;
            value.last_mutool_version = state->settings.last_mutool_version;
            std::wstring error;
            if (!value.lualatex_path.empty() && !is_valid_executable_file(value.lualatex_path)) {
                SetWindowTextW(state->diagnostic, L"LuaLaTeXパスは通常の.exeファイルを指定してください。"); return 0;
            }
            if (!value.mutool_path.empty() && !is_valid_executable_file(value.mutool_path)) {
                SetWindowTextW(state->diagnostic, L"MuPDFパスは通常の.exeファイルを指定してください。"); return 0;
            }
            if (!save_tool_settings(value, error)) { SetWindowTextW(state->diagnostic, error.c_str()); return 0; }
            state->settings = std::move(value); state->done = true; DestroyWindow(window); return 0;
        }
        if (id == IDC_CANCEL || id == IDCANCEL) {
            request_environment_window_close(*state);
            return 0;
        }
    }
    if (message == WM_APP_DIAGNOSTIC_FINISHED ||
        (message == WM_TIMER && wparam == IDT_DIAGNOSTIC_COMPLETION)) {
        if (!state->diagnostic_running) {
            return 0;
        }
        if (message == WM_APP_DIAGNOSTIC_FINISHED &&
            static_cast<std::uint64_t>(wparam) != state->diagnostic_generation) {
            return 0;
        }
        std::optional<DiagnosticReport> report;
        if (state->diagnostic_completion) {
            std::lock_guard completion_lock(
                state->diagnostic_completion->mutex);
            if (state->diagnostic_completion->report.has_value()) {
                report = std::move(state->diagnostic_completion->report);
            }
        }
        if (!report.has_value()) {
            return 0;
        }
        KillTimer(window, IDT_DIAGNOSTIC_COMPLETION);
        if (state->diagnostic_worker.joinable()) {
            state->diagnostic_worker.join();
        }
        state->diagnostic_running = false;
        state->diagnostic_completion.reset();
        if (state->close_requested) {
            DestroyWindow(window);
            return 0;
        }
        const std::wstring text =
            report->summary + L"\r\n\r\n" + report->details;
        SetWindowTextW(state->diagnostic, text.c_str());
        state->settings.last_diagnostic_summary = report->summary;
        state->settings.last_diagnostic_details = report->details;
        state->settings.last_lualatex_version = report->lualatex_version;
        state->settings.last_mutool_version = report->mutool_version;
        enable_environment_inputs(*state, true);
        EnableWindow(GetDlgItem(window, IDC_CANCEL), TRUE);
        return 0;
    }
    if (message == WM_CLOSE) {
        request_environment_window_close(*state);
        return 0;
    }
    if (message == WM_DESTROY) {
        KillTimer(window, IDT_DIAGNOSTIC_COMPLETION);
        if (state->diagnostic_worker.joinable()) {
            state->diagnostic_worker.request_stop();
        }
        if (state->ui_font != nullptr) {
            DeleteObject(state->ui_font);
            state->ui_font = nullptr;
        }
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct FontEntry {
    std::wstring display_name;
    std::wstring dwrite_family_name;
    std::wstring fontspec_family_name;
    UINT32 family_index = (std::numeric_limits<UINT32>::max)();
    DWRITE_FONT_WEIGHT weight = DWRITE_FONT_WEIGHT_NORMAL;
    DWRITE_FONT_STRETCH stretch = DWRITE_FONT_STRETCH_NORMAL;
    DWRITE_FONT_STYLE style = DWRITE_FONT_STYLE_NORMAL;
    bool face_available = false;
    bool japanese = false;
    bool use_default = false;
};
struct FontWindow {
    HWND window = nullptr; HWND search = nullptr; HWND list = nullptr; HWND preview = nullptr;
    std::vector<FontEntry> fonts; std::vector<std::size_t> visible;
    ComPtr<IDWriteFactory> dwrite_factory;
    ComPtr<IDWriteFontCollection> font_collection;
    ComPtr<IDWriteGdiInterop> gdi_interop;
    ComPtr<IDWriteRenderingParams> rendering_params;
    ComPtr<IDWriteFontFace> preview_face;
    ComPtr<IDWriteTextFormat> preview_format;
    ComPtr<IDWriteTextLayout> preview_layout;
    ComPtr<IDWriteBitmapRenderTarget> preview_target;
    std::wstring preview_error;
    std::wstring preview_status;
    int preview_target_width = 0;
    int preview_target_height = 0;
    HFONT ui_font = nullptr;
    UINT dpi = dialog_layout::kDefaultDpi;
    std::wstring current;
    bool current_is_default = false;
    std::optional<SystemFontSelection> selected;
    bool done = false;
};

std::wstring localized_name(IDWriteLocalizedStrings* names, const wchar_t* preferred) {
    UINT32 index = 0; BOOL exists = FALSE;
    names->FindLocaleName(preferred, &index, &exists);
    if (!exists) names->FindLocaleName(L"en-us", &index, &exists);
    if (!exists) index = 0;
    UINT32 length = 0; names->GetStringLength(index, &length);
    std::wstring value(static_cast<std::size_t>(length) + 1, L'\0');
    names->GetString(index, value.data(), length + 1); value.resize(length); return value;
}

bool font_face_has_characters(
    IDWriteFontFace* face,
    const UINT32* code_points,
    UINT32 count) {
    if (face == nullptr || code_points == nullptr || count == 0) return false;
    std::vector<UINT16> glyphs(count);
    if (FAILED(face->GetGlyphIndices(code_points, count, glyphs.data()))) return false;
    return std::all_of(glyphs.begin(), glyphs.end(), [](UINT16 glyph) { return glyph != 0; });
}

std::wstring collection_family_name(
    IDWriteFontCollection* collection,
    IDWriteLocalizedStrings* names,
    UINT32 family_index,
    const std::wstring& display_name,
    const std::wstring& english_name) {
    const auto belongs_to_family = [&](const std::wstring& value) {
        if (value.empty()) return false;
        UINT32 found_index = 0;
        BOOL exists = FALSE;
        return SUCCEEDED(collection->FindFamilyName(value.c_str(), &found_index, &exists)) &&
            exists != FALSE && found_index == family_index;
    };
    if (belongs_to_family(display_name)) return display_name;
    if (belongs_to_family(english_name)) return english_name;
    UINT32 length = 0;
    if (SUCCEEDED(names->GetStringLength(0, &length))) {
        std::wstring first_name(static_cast<std::size_t>(length) + 1, L'\0');
        if (SUCCEEDED(names->GetString(0, first_name.data(), length + 1))) {
            first_name.resize(length);
            if (belongs_to_family(first_name)) return first_name;
        }
    }
    return display_name;
}

void load_system_fonts(FontWindow& state) {
    state.fonts.clear();
    FontEntry default_entry;
    default_entry.display_name = L"既定フォント";
    default_entry.dwrite_family_name = L"Segoe UI";
    default_entry.japanese = true;
    default_entry.use_default = true;
    state.fonts.push_back(std::move(default_entry));
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(state.dwrite_factory.GetAddressOf())))) return;
    if (FAILED(state.dwrite_factory->GetSystemFontCollection(
            &state.font_collection, FALSE))) return;
    state.dwrite_factory->GetGdiInterop(&state.gdi_interop);
    state.dwrite_factory->CreateRenderingParams(&state.rendering_params);
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (!GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH)) wcscpy_s(locale, L"en-us");
    for (UINT32 i = 0; i < state.font_collection->GetFontFamilyCount(); ++i) {
        ComPtr<IDWriteFontFamily> family; ComPtr<IDWriteLocalizedStrings> names;
        if (FAILED(state.font_collection->GetFontFamily(i, &family)) ||
            FAILED(family->GetFamilyNames(&names))) continue;
        FontEntry entry;
        entry.family_index = i;
        entry.display_name = localized_name(names.Get(), locale);
        entry.fontspec_family_name = localized_name(names.Get(), L"en-us");
        if (entry.fontspec_family_name.empty()) entry.fontspec_family_name = entry.display_name;
        entry.dwrite_family_name = collection_family_name(
            state.font_collection.Get(), names.Get(), i,
            entry.display_name, entry.fontspec_family_name);
        ComPtr<IDWriteFont> font;
        if (SUCCEEDED(family->GetFirstMatchingFont(DWRITE_FONT_WEIGHT_NORMAL,
                DWRITE_FONT_STRETCH_NORMAL, DWRITE_FONT_STYLE_NORMAL, &font))) {
            entry.weight = font->GetWeight();
            entry.stretch = font->GetStretch();
            entry.style = font->GetStyle();
            ComPtr<IDWriteFontFace> face;
            if (SUCCEEDED(font->CreateFontFace(&face))) {
                entry.face_available = true;
                static constexpr UINT32 japanese_test[] = {L'漢', L'あ', L'ア'};
                entry.japanese = font_face_has_characters(
                    face.Get(), japanese_test, static_cast<UINT32>(std::size(japanese_test)));
            }
        }
        state.fonts.push_back(std::move(entry));
    }
    std::sort(state.fonts.begin() + 1, state.fonts.end(), [](const auto& a, const auto& b) {
        return _wcsicmp(a.display_name.c_str(), b.display_name.c_str()) < 0;
    });
}

std::wstring lower(std::wstring value) { for (auto& c : value) c = static_cast<wchar_t>(towlower(c)); return value; }

void update_font_list_horizontal_extent(FontWindow& state) {
    if (state.list == nullptr) return;
    HDC dc = GetDC(state.list);
    if (dc == nullptr) return;
    HGDIOBJ previous = nullptr;
    if (state.ui_font != nullptr) previous = SelectObject(dc, state.ui_font);
    int maximum = 0;
    const int count = static_cast<int>(SendMessageW(state.list, LB_GETCOUNT, 0, 0));
    for (int index = 0; index < count; ++index) {
        const int length = static_cast<int>(
            SendMessageW(state.list, LB_GETTEXTLEN, index, 0));
        if (length < 0) continue;
        std::wstring text(static_cast<std::size_t>(length) + 1, L'\0');
        if (SendMessageW(state.list, LB_GETTEXT, index,
                reinterpret_cast<LPARAM>(text.data())) == LB_ERR) continue;
        SIZE extent{};
        if (GetTextExtentPoint32W(dc, text.c_str(), length, &extent)) {
            maximum = (std::max)(maximum, static_cast<int>(extent.cx));
        }
    }
    if (previous != nullptr) SelectObject(dc, previous);
    ReleaseDC(state.list, dc);
    SendMessageW(state.list, LB_SETHORIZONTALEXTENT,
        maximum + dialog_layout::scale_for_dpi(24, state.dpi), 0);
}

void refresh_font_list(FontWindow& state) {
    const std::wstring query = lower(control_text(state.search));
    SendMessageW(state.list, LB_RESETCONTENT, 0, 0); state.visible.clear();
    int selected = -1;
    for (std::size_t i = 0; i < state.fonts.size(); ++i) {
        const auto& font = state.fonts[i];
        if (!query.empty() && lower(font.display_name).find(query) == std::wstring::npos &&
            lower(font.dwrite_family_name).find(query) == std::wstring::npos &&
            lower(font.fontspec_family_name).find(query) == std::wstring::npos) continue;
        const std::wstring label = font.display_name +
            (font.japanese || font.use_default ? L"" : L"  [日本語なし]");
        const LRESULT row = SendMessageW(state.list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
        state.visible.push_back(i);
        if ((font.use_default && state.current_is_default) ||
            (!font.use_default && (_wcsicmp(font.fontspec_family_name.c_str(), state.current.c_str()) == 0 ||
            _wcsicmp(font.display_name.c_str(), state.current.c_str()) == 0))) selected = static_cast<int>(row);
    }
    if (selected >= 0) SendMessageW(state.list, LB_SETCURSEL, selected, 0);
    update_font_list_horizontal_extent(state);
}

void position_font_controls(FontWindow& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int client_width = (std::max)(1L, client.right - client.left);
    const int client_height = (std::max)(1L, client.bottom - client.top);
    const int margin = dialog_layout::scale_for_dpi(12, state.dpi);
    const int gap = dialog_layout::scale_for_dpi(10, state.dpi);
    const int font_height = dialog_font_height(
        state.window, state.ui_font, state.dpi);
    const int search_height = (std::max)(
        dialog_layout::scale_for_dpi(25, state.dpi),
        font_height + dialog_layout::scale_for_dpi(8, state.dpi));
    const int button_height = (std::max)(
        dialog_layout::scale_for_dpi(28, state.dpi),
        font_height + dialog_layout::scale_for_dpi(10, state.dpi));
    const int select_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"選択", 78);
    const int cancel_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"キャンセル", 78);
    const int footer_height = button_height + margin * 2;
    const int footer_top = (std::max)(0, client_height - footer_height);
    const int content_width = (std::max)(1, client_width - margin * 2);

    move_to_rect(state.search, margin, margin, content_width, search_height);
    const int body_top = margin + search_height + gap;
    const int body_bottom = (std::max)(body_top + 1, footer_top - gap);
    const int body_height = (std::max)(1, body_bottom - body_top);
    const int minimum_list = dialog_layout::scale_for_dpi(90, state.dpi);
    const int minimum_preview = dialog_layout::scale_for_dpi(64, state.dpi);
    const int preferred_preview = dialog_layout::scale_for_dpi(145, state.dpi);
    int preview_height = (std::clamp)(
        preferred_preview, minimum_preview,
        (std::max)(minimum_preview, body_height - minimum_list - gap));
    if (body_height < minimum_list + minimum_preview + gap) {
        preview_height = (std::max)(
            dialog_layout::scale_for_dpi(40, state.dpi), body_height / 3);
    }
    preview_height = (std::min)(preview_height, (std::max)(1, body_height - 1));
    const int list_height = (std::max)(1, body_height - preview_height - gap);
    move_to_rect(state.list, margin, body_top, content_width, list_height);
    move_to_rect(state.preview, margin, body_top + list_height + gap,
        content_width, preview_height);

    const int buttons_width = select_width + gap + cancel_width;
    const int button_x = (std::max)(margin,
        client_width - margin - buttons_width);
    const int button_y = footer_top + (footer_height - button_height) / 2;
    move_to_rect(GetDlgItem(state.window, IDC_FONT_SELECT),
        button_x, button_y, select_width, button_height);
    move_to_rect(GetDlgItem(state.window, IDC_FONT_CANCEL),
        button_x + select_width + gap, button_y, cancel_width, button_height);
    const int item_height = (std::max)(
        font_height + dialog_layout::scale_for_dpi(5, state.dpi),
        dialog_layout::scale_for_dpi(20, state.dpi));
    SendMessageW(state.list, LB_SETITEMHEIGHT, 0, item_height);
    update_font_list_horizontal_extent(state);
}

class BitmapTextRenderer final : public IDWriteTextRenderer {
public:
    BitmapTextRenderer(
        IDWriteBitmapRenderTarget* target,
        IDWriteRenderingParams* rendering_params,
        FLOAT pixels_per_dip,
        COLORREF color)
        : target_(target), rendering_params_(rendering_params),
          pixels_per_dip_(pixels_per_dip), color_(color) {}

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (object == nullptr) return E_POINTER;
        *object = nullptr;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IDWriteTextRenderer)) {
            *object = static_cast<IDWriteTextRenderer*>(this);
            AddRef();
            return S_OK;
        }
        if (iid == __uuidof(IDWritePixelSnapping)) {
            *object = static_cast<IDWritePixelSnapping*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(InterlockedIncrement(&references_));
    }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG references = static_cast<ULONG>(InterlockedDecrement(&references_));
        if (references == 0) delete this;
        return references;
    }
    HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(
        void*, BOOL* disabled) override {
        if (disabled == nullptr) return E_POINTER;
        *disabled = FALSE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetCurrentTransform(
        void*, DWRITE_MATRIX* transform) override {
        if (transform == nullptr) return E_POINTER;
        target_->GetCurrentTransform(transform);
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void*, FLOAT* pixels_per_dip) override {
        if (pixels_per_dip == nullptr) return E_POINTER;
        *pixels_per_dip = pixels_per_dip_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawGlyphRun(
        void*, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE measuring_mode,
        const DWRITE_GLYPH_RUN* glyph_run,
        const DWRITE_GLYPH_RUN_DESCRIPTION*, IUnknown*) override {
        return target_->DrawGlyphRun(
            x, y, measuring_mode, glyph_run, rendering_params_.Get(), color_);
    }
    HRESULT STDMETHODCALLTYPE DrawUnderline(
        void*, FLOAT, FLOAT, const DWRITE_UNDERLINE*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawStrikethrough(
        void*, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH*, IUnknown*) override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE DrawInlineObject(
        void*, FLOAT x, FLOAT y, IDWriteInlineObject* object,
        BOOL sideways, BOOL right_to_left, IUnknown* effect) override {
        if (object == nullptr) return E_INVALIDARG;
        return object->Draw(nullptr, this, x, y, sideways, right_to_left, effect);
    }

private:
    ~BitmapTextRenderer() = default;
    LONG references_ = 1;
    ComPtr<IDWriteBitmapRenderTarget> target_;
    ComPtr<IDWriteRenderingParams> rendering_params_;
    FLOAT pixels_per_dip_ = 1.0F;
    COLORREF color_ = RGB(0, 0, 0);
};

void reset_font_preview(FontWindow& state) {
    state.preview_face.Reset();
    state.preview_format.Reset();
    state.preview_layout.Reset();
    state.preview_target.Reset();
    state.preview_target_width = 0;
    state.preview_target_height = 0;
    state.preview_error.clear();
    state.preview_status.clear();
}

void rebuild_font_preview(FontWindow& state) {
    reset_font_preview(state);
    const int row = static_cast<int>(SendMessageW(state.list, LB_GETCURSEL, 0, 0));
    if (row < 0 || static_cast<std::size_t>(row) >= state.visible.size()) {
        state.preview_error = L"フォントを選択してください。";
        InvalidateRect(state.preview, nullptr, TRUE);
        return;
    }
    const auto& entry = state.fonts[state.visible[static_cast<std::size_t>(row)]];
    if (!state.dwrite_factory || !state.font_collection || !state.gdi_interop) {
        state.preview_error = L"プレビューを生成できません。";
        InvalidateRect(state.preview, nullptr, TRUE);
        return;
    }
    if (!entry.use_default && !entry.face_available) {
        state.preview_error = L"プレビューを生成できません。";
        InvalidateRect(state.preview, nullptr, TRUE);
        return;
    }

    UINT32 family_index = entry.family_index;
    if (entry.use_default) {
        BOOL exists = FALSE;
        if (FAILED(state.font_collection->FindFamilyName(
                entry.dwrite_family_name.c_str(), &family_index, &exists)) || exists == FALSE) {
            state.preview_error = L"プレビューを生成できません。";
            InvalidateRect(state.preview, nullptr, TRUE);
            return;
        }
    }
    ComPtr<IDWriteFontFamily> family;
    ComPtr<IDWriteFont> font;
    const DWRITE_FONT_WEIGHT requested_weight = entry.use_default
        ? DWRITE_FONT_WEIGHT_NORMAL : entry.weight;
    const DWRITE_FONT_STRETCH requested_stretch = entry.use_default
        ? DWRITE_FONT_STRETCH_NORMAL : entry.stretch;
    const DWRITE_FONT_STYLE requested_style = entry.use_default
        ? DWRITE_FONT_STYLE_NORMAL : entry.style;
    if (FAILED(state.font_collection->GetFontFamily(family_index, &family)) ||
        FAILED(family->GetFirstMatchingFont(
            requested_weight, requested_stretch, requested_style, &font)) ||
        FAILED(font->CreateFontFace(&state.preview_face))) {
        state.preview_error = L"プレビューを生成できません。";
        InvalidateRect(state.preview, nullptr, TRUE);
        return;
    }

    static constexpr wchar_t preview_text[] =
        L"数式と日本語 ABC 123\n漢字 ひらがな カタカナ\nE = mc²";
    std::wstring drawable_text = preview_text;
    bool missing_any = false;
    bool missing_japanese = false;
    for (auto& character : drawable_text) {
        if (character == L'\n' || iswspace(character)) continue;
        const UINT32 point = static_cast<UINT32>(character);
        UINT16 glyph = 0;
        if (FAILED(state.preview_face->GetGlyphIndices(&point, 1, &glyph)) || glyph == 0) {
            if (character == L'漢' || character == L'あ' || character == L'ア' ||
                (character >= 0x3040 && character <= 0x30ff) ||
                (character >= 0x4e00 && character <= 0x9fff)) {
                missing_japanese = true;
            }
            character = L' ';
            missing_any = true;
        }
    }
    if (missing_japanese) state.preview_status = L"日本語グリフなし";
    else if (missing_any) state.preview_status = L"一部グリフなし";

    RECT area{};
    GetClientRect(state.preview, &area);
    const UINT dpi = GetDpiForWindow(state.preview);
    const FLOAT pixels_per_dip = static_cast<FLOAT>(dpi == 0 ? 96 : dpi) / 96.0F;
    const FLOAT width = (std::max)(1.0F,
        static_cast<FLOAT>(area.right - area.left) / pixels_per_dip - 16.0F);
    const FLOAT height = (std::max)(1.0F,
        static_cast<FLOAT>(area.bottom - area.top) / pixels_per_dip -
        (state.preview_status.empty() ? 16.0F : 38.0F));
    if (FAILED(state.dwrite_factory->CreateTextFormat(
            entry.dwrite_family_name.c_str(), state.font_collection.Get(),
            font->GetWeight(), font->GetStyle(), font->GetStretch(), 24.0F,
            L"ja-jp", &state.preview_format)) ||
        FAILED(state.preview_format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER)) ||
        FAILED(state.preview_format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)) ||
        FAILED(state.preview_format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP)) ||
        FAILED(state.dwrite_factory->CreateTextLayout(
            drawable_text.c_str(), static_cast<UINT32>(drawable_text.size()),
            state.preview_format.Get(), width, height, &state.preview_layout))) {
        state.preview_error = L"プレビューを生成できません。";
    }
    InvalidateRect(state.preview, nullptr, TRUE);
}

bool ensure_preview_target(FontWindow& state, int width, int height) {
    if (width <= 0 || height <= 0 || !state.gdi_interop) return false;
    if (state.preview_target && state.preview_target_width == width &&
        state.preview_target_height == height) return true;
    state.preview_target.Reset();
    if (FAILED(state.gdi_interop->CreateBitmapRenderTarget(
            nullptr, static_cast<UINT32>(width), static_cast<UINT32>(height),
            &state.preview_target))) return false;
    state.preview_target_width = width;
    state.preview_target_height = height;
    const UINT dpi = GetDpiForWindow(state.preview);
    state.preview_target->SetPixelsPerDip(
        static_cast<FLOAT>(dpi == 0 ? 96 : dpi) / 96.0F);
    return true;
}

HRESULT draw_directwrite_layout(
    FontWindow& state,
    IDWriteTextLayout* layout,
    FLOAT x,
    FLOAT y,
    COLORREF color) {
    if (layout == nullptr || !state.preview_target) return E_INVALIDARG;
    const UINT dpi = GetDpiForWindow(state.preview);
    const FLOAT pixels_per_dip = static_cast<FLOAT>(dpi == 0 ? 96 : dpi) / 96.0F;
    auto* renderer = new (std::nothrow) BitmapTextRenderer(
        state.preview_target.Get(), state.rendering_params.Get(), pixels_per_dip, color);
    if (renderer == nullptr) return E_OUTOFMEMORY;
    const HRESULT result = layout->Draw(nullptr, renderer, x, y);
    renderer->Release();
    return result;
}

void draw_preview_message(
    FontWindow& state,
    const std::wstring& message,
    FLOAT top,
    FLOAT height,
    FLOAT font_size,
    COLORREF color) {
    ComPtr<IDWriteTextFormat> format;
    ComPtr<IDWriteTextLayout> layout;
    const FLOAT pixels_per_dip = state.preview_target->GetPixelsPerDip();
    const FLOAT width = static_cast<FLOAT>(state.preview_target_width) / pixels_per_dip;
    if (FAILED(state.dwrite_factory->CreateTextFormat(
            L"Segoe UI", state.font_collection.Get(), DWRITE_FONT_WEIGHT_NORMAL,
            DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, font_size,
            L"ja-jp", &format)) ||
        FAILED(format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER)) ||
        FAILED(format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER)) ||
        FAILED(state.dwrite_factory->CreateTextLayout(
            message.c_str(), static_cast<UINT32>(message.size()), format.Get(),
            width, height, &layout))) return;
    draw_directwrite_layout(state, layout.Get(), 0.0F, top, color);
}

void paint_font_preview(FontWindow& state, HDC destination) {
    RECT area{};
    GetClientRect(state.preview, &area);
    const int width = area.right - area.left;
    const int height = area.bottom - area.top;
    if (!ensure_preview_target(state, width, height)) {
        FillRect(destination, &area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
        return;
    }
    HDC memory = state.preview_target->GetMemoryDC();
    RECT memory_area{0, 0, width, height};
    FillRect(memory, &memory_area, reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1));
    FrameRect(memory, &memory_area, reinterpret_cast<HBRUSH>(COLOR_3DSHADOW + 1));
    const FLOAT pixels_per_dip = state.preview_target->GetPixelsPerDip();
    if (!state.preview_error.empty()) {
        draw_preview_message(state, state.preview_error, 0.0F,
            static_cast<FLOAT>(height) / pixels_per_dip, 15.0F, RGB(160, 0, 0));
    } else if (state.preview_layout) {
        draw_directwrite_layout(state, state.preview_layout.Get(), 8.0F, 8.0F, RGB(0, 0, 0));
        if (!state.preview_status.empty()) {
            draw_preview_message(state, state.preview_status,
                static_cast<FLOAT>(height) / pixels_per_dip - 25.0F,
                20.0F, 12.0F, RGB(160, 0, 0));
        }
    }
    BitBlt(destination, 0, 0, width, height, memory, 0, 0, SRCCOPY);
}

LRESULT CALLBACK font_preview_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<FontWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = reinterpret_cast<FontWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (message == WM_ERASEBKGND) return 1;
    if (message == WM_SIZE && state != nullptr) {
        state->preview_target.Reset();
        state->preview_target_width = 0;
        state->preview_target_height = 0;
        if (state->preview == window) rebuild_font_preview(*state);
        return 0;
    }
    if (message == WM_PAINT && state != nullptr) {
        PAINTSTRUCT paint{};
        HDC dc = BeginPaint(window, &paint);
        paint_font_preview(*state, dc);
        EndPaint(window, &paint);
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

LRESULT CALLBACK font_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<FontWindow*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { state = reinterpret_cast<FontWindow*>(reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams); state->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state)); }
    if (!state) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_CREATE) {
        state->dpi = valid_window_dpi(window);
        state->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE |
            WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_SEARCH)), GetModuleHandleW(nullptr), nullptr);
        state->list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE |
            WS_BORDER | WS_VSCROLL | WS_HSCROLL | WS_TABSTOP | LBS_NOTIFY,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_LIST)), GetModuleHandleW(nullptr), nullptr);
        state->preview = CreateWindowExW(0, L"AviUtl2LaTeX.FontPreview", L"",
            WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_PREVIEW)),
            GetModuleHandleW(nullptr), state);
        CreateWindowExW(0, L"BUTTON", L"選択", WS_CHILD | WS_VISIBLE |
            WS_TABSTOP | BS_DEFPUSHBUTTON,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_SELECT)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE |
            WS_TABSTOP,
            0, 0, 0, 0, window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_CANCEL)), GetModuleHandleW(nullptr), nullptr);
        replace_dialog_font(window, state->ui_font, state->dpi);
        position_font_controls(*state);
        refresh_font_list(*state);
        rebuild_font_preview(*state);
        return 0;
    }
    if (message == WM_SIZE) {
        position_font_controls(*state);
        return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        apply_minmax_limits(window,
            *reinterpret_cast<MINMAXINFO*>(lparam), SIZE{360, 330}, state->dpi);
        return 0;
    }
    if (message == WM_DPICHANGED) {
        const UINT new_dpi = LOWORD(wparam) == 0
            ? dialog_layout::kDefaultDpi : LOWORD(wparam);
        const RECT suggested = *reinterpret_cast<const RECT*>(lparam);
        const RECT final_rect = dialog_layout::clamp_dpi_changed_rect(
            suggested, new_dpi, kDialogWorkMargin);
        state->dpi = new_dpi;
        SetWindowPos(window, nullptr, final_rect.left, final_rect.top,
            final_rect.right - final_rect.left,
            final_rect.bottom - final_rect.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        replace_dialog_font(window, state->ui_font, state->dpi);
        position_font_controls(*state);
        rebuild_font_preview(*state);
        return 0;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam), notification = HIWORD(wparam);
        if (id == IDC_FONT_SEARCH && notification == EN_CHANGE) {
            refresh_font_list(*state); rebuild_font_preview(*state); return 0;
        }
        if (id == IDC_FONT_LIST && notification == LBN_SELCHANGE) {
            rebuild_font_preview(*state); return 0;
        }
        if (id == IDC_FONT_SELECT || id == IDOK ||
            (id == IDC_FONT_LIST && notification == LBN_DBLCLK)) {
            const int row = static_cast<int>(SendMessageW(state->list, LB_GETCURSEL, 0, 0));
            if (row >= 0 && static_cast<std::size_t>(row) < state->visible.size()) {
                const auto& entry = state->fonts[state->visible[static_cast<std::size_t>(row)]];
                state->selected = SystemFontSelection{
                    entry.use_default,
                    entry.use_default ? L"既定" : entry.display_name,
                    entry.fontspec_family_name
                };
            }
            DestroyWindow(window); return 0;
        }
        if (id == IDC_FONT_CANCEL || id == IDCANCEL) {
            DestroyWindow(window); return 0;
        }
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) {
        reset_font_preview(*state);
        if (state->ui_font != nullptr) {
            DeleteObject(state->ui_font);
            state->ui_font = nullptr;
        }
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

struct InformationWindow {
    InformationDialogSnapshot snapshot;
    std::wstring overview;
    std::wstring report;
    HWND window = nullptr;
    HWND text = nullptr;
    HWND status = nullptr;
    HFONT ui_font = nullptr;
    UINT dpi = dialog_layout::kDefaultDpi;
    bool done = false;
};

bool copy_unicode_text(HWND owner, const std::wstring& text) {
    if (!OpenClipboard(owner)) return false;
    struct ClipboardCloser { ~ClipboardCloser() { CloseClipboard(); } } closer;
    if (!EmptyClipboard()) return false;
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (memory == nullptr) return false;
    void* target = GlobalLock(memory);
    if (target == nullptr) {
        GlobalFree(memory);
        return false;
    }
    memcpy(target, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (SetClipboardData(CF_UNICODETEXT, memory) == nullptr) {
        GlobalFree(memory);
        return false;
    }
    // Ownership transfers to the system only after SetClipboardData succeeds.
    return true;
}

void position_information_controls(InformationWindow& state) {
    RECT client{};
    GetClientRect(state.window, &client);
    const int client_width = (std::max)(1L, client.right - client.left);
    const int client_height = (std::max)(1L, client.bottom - client.top);
    const int margin = dialog_layout::scale_for_dpi(12, state.dpi);
    const int gap = dialog_layout::scale_for_dpi(8, state.dpi);
    const int font_height = dialog_font_height(
        state.window, state.ui_font, state.dpi);
    const int button_height = (std::max)(
        dialog_layout::scale_for_dpi(30, state.dpi),
        font_height + dialog_layout::scale_for_dpi(10, state.dpi));
    const int status_height = (std::max)(
        dialog_layout::scale_for_dpi(24, state.dpi), font_height);
    const int copy_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"診断情報をコピー", 144);
    const int log_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"ログを開く", 112);
    const int folder_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"ログフォルダ", 112);
    const int close_width = measured_button_width(
        state.window, state.ui_font, state.dpi, L"閉じる", 100);
    const int available_width = (std::max)(1, client_width - margin * 2);
    const int single_row_width = copy_width + log_width + folder_width +
        close_width + gap * 3;
    const bool wrap_buttons = single_row_width > available_width;
    const int button_rows_height = wrap_buttons
        ? button_height * 2 + gap : button_height;
    const int footer_height = button_rows_height + gap + status_height + margin * 2;
    const int footer_top = (std::max)(margin,
        client_height - footer_height);
    move_to_rect(state.text, margin, margin, available_width,
        (std::max)(1, footer_top - margin - gap));

    if (!wrap_buttons) {
        int x = margin;
        move_to_rect(GetDlgItem(state.window, IDC_INFO_COPY),
            x, footer_top, copy_width, button_height);
        x += copy_width + gap;
        move_to_rect(GetDlgItem(state.window, IDC_INFO_LOG),
            x, footer_top, log_width, button_height);
        x += log_width + gap;
        move_to_rect(GetDlgItem(state.window, IDC_INFO_LOG_FOLDER),
            x, footer_top, folder_width, button_height);
        move_to_rect(GetDlgItem(state.window, IDC_INFO_CLOSE),
            client_width - margin - close_width, footer_top,
            close_width, button_height);
    } else {
        move_to_rect(GetDlgItem(state.window, IDC_INFO_COPY),
            margin, footer_top, copy_width, button_height);
        move_to_rect(GetDlgItem(state.window, IDC_INFO_LOG),
            (std::min)(client_width - margin - log_width,
                margin + copy_width + gap),
            footer_top, log_width, button_height);
        const int second_y = footer_top + button_height + gap;
        move_to_rect(GetDlgItem(state.window, IDC_INFO_LOG_FOLDER),
            margin, second_y, folder_width, button_height);
        move_to_rect(GetDlgItem(state.window, IDC_INFO_CLOSE),
            client_width - margin - close_width, second_y,
            close_width, button_height);
    }
    const int status_top = footer_top + button_rows_height + gap;
    move_to_rect(state.status, margin, status_top,
        available_width, status_height);
}

void set_information_status(InformationWindow& state, const wchar_t* value) {
    SetWindowTextW(state.status, value);
}

bool open_information_path(
    InformationWindow& state,
    const std::filesystem::path& path,
    const wchar_t* missing_message,
    const char* log_operation) {
    std::error_code code;
    if (!std::filesystem::exists(path, code) || code) {
        set_information_status(state, missing_message);
        return false;
    }
    const HINSTANCE result = ShellExecuteW(
        state.window, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        set_information_status(state, L"開けませんでした。");
        return false;
    }
    append_latest_log(std::string(log_operation) + ": yes\n");
    return true;
}

LRESULT CALLBACK information_proc(
    HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    auto* state = reinterpret_cast<InformationWindow*>(
        GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) {
        state = reinterpret_cast<InformationWindow*>(
            reinterpret_cast<CREATESTRUCTW*>(lparam)->lpCreateParams);
        state->window = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }
    if (state == nullptr) return DefWindowProcW(window, message, wparam, lparam);
    if (message == WM_CREATE) {
        state->dpi = valid_window_dpi(window);
        auto make = [&](const wchar_t* klass, const wchar_t* text, DWORD style, int id) {
            return CreateWindowExW(0, klass, text, WS_CHILD | WS_VISIBLE | style,
                0, 0, 0, 0, window,
                reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
                GetModuleHandleW(nullptr), nullptr);
        };
        state->text = make(L"EDIT", state->overview.c_str(), WS_BORDER | ES_LEFT |
            ES_MULTILINE | ES_AUTOVSCROLL | ES_AUTOHSCROLL | ES_READONLY |
            WS_VSCROLL | WS_HSCROLL, IDC_INFO_TEXT);
        make(L"BUTTON", L"診断情報をコピー", BS_PUSHBUTTON | WS_TABSTOP, IDC_INFO_COPY);
        make(L"BUTTON", L"ログを開く", BS_PUSHBUTTON | WS_TABSTOP, IDC_INFO_LOG);
        make(L"BUTTON", L"ログフォルダ", BS_PUSHBUTTON | WS_TABSTOP, IDC_INFO_LOG_FOLDER);
        make(L"BUTTON", L"閉じる", BS_DEFPUSHBUTTON | WS_TABSTOP, IDC_INFO_CLOSE);
        state->status = make(L"STATIC", L"", SS_LEFT, IDC_INFO_STATUS);
        replace_dialog_font(window, state->ui_font, state->dpi);
        position_information_controls(*state);
        return 0;
    }
    if (message == WM_SIZE) {
        position_information_controls(*state);
        return 0;
    }
    if (message == WM_GETMINMAXINFO) {
        apply_minmax_limits(window,
            *reinterpret_cast<MINMAXINFO*>(lparam), SIZE{440, 300}, state->dpi);
        return 0;
    }
    if (message == WM_DPICHANGED) {
        const UINT new_dpi = LOWORD(wparam) == 0
            ? dialog_layout::kDefaultDpi : LOWORD(wparam);
        const RECT suggested = *reinterpret_cast<const RECT*>(lparam);
        const RECT final_rect = dialog_layout::clamp_dpi_changed_rect(
            suggested, new_dpi, kDialogWorkMargin);
        state->dpi = new_dpi;
        SetWindowPos(window, nullptr, final_rect.left, final_rect.top,
            final_rect.right - final_rect.left,
            final_rect.bottom - final_rect.top,
            SWP_NOACTIVATE | SWP_NOZORDER);
        replace_dialog_font(window, state->ui_font, state->dpi);
        position_information_controls(*state);
        return 0;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam);
        if (id == IDC_INFO_COPY) {
            const bool copied = copy_unicode_text(window, state->report);
            set_information_status(*state,
                copied ? L"診断情報をコピーしました。" : L"診断情報をコピーできませんでした。");
            append_latest_log(std::string("diagnostic_report_copied: ") +
                (copied ? "yes\n" : "no\n"));
            return 0;
        }
        AppPaths paths;
        std::wstring error;
        if (id == IDC_INFO_LOG || id == IDC_INFO_LOG_FOLDER) {
            if (!resolve_app_paths(paths, error)) {
                set_information_status(*state, L"ログの保存場所を取得できません。");
                return 0;
            }
            if (id == IDC_INFO_LOG) {
                open_information_path(*state, paths.latest_log,
                    L"latest.logはまだありません。", "latest_log_opened");
            } else {
                if (!ensure_runtime_directories(paths, error)) {
                    set_information_status(*state, L"ログフォルダを作成できません。");
                    return 0;
                }
                open_information_path(*state, paths.logs_root,
                    L"ログフォルダがありません。", "log_directory_opened");
            }
            return 0;
        }
        if (id == IDC_INFO_CLOSE || id == IDOK || id == IDCANCEL) {
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) {
        if (state->ui_font != nullptr) {
            DeleteObject(state->ui_font);
            state->ui_font = nullptr;
        }
        state->done = true;
        return 0;
    }
    return DefWindowProcW(window, message, wparam, lparam);
}

ATOM register_class(const wchar_t* name, WNDPROC procedure) {
    WNDCLASSEXW window_class{sizeof(window_class)};
    window_class.lpfnWndProc = procedure; window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); window_class.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    window_class.lpszClassName = name;
    const ATOM atom = RegisterClassExW(&window_class);
    return atom != 0 ? atom : static_cast<ATOM>(GetLastError() == ERROR_CLASS_ALREADY_EXISTS ? 1 : 0);
}

template<typename State>
void modal_loop(State& state) {
    if (dialog_owner) EnableWindow(dialog_owner, FALSE);
    ShowWindow(state.window, SW_SHOW); UpdateWindow(state.window);
    MSG message{};
    while (!state.done && GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (message.message == WM_KEYDOWN && message.wParam == VK_ESCAPE &&
            (message.hwnd == state.window ||
             IsChild(state.window, message.hwnd))) {
            SendMessageW(state.window, WM_CLOSE, 0, 0);
            continue;
        }
        if (message.message == WM_KEYDOWN &&
            (message.wParam == VK_PRIOR || message.wParam == VK_NEXT) &&
            (GetWindowLongPtrW(state.window, GWL_STYLE) & WS_VSCROLL) != 0 &&
            (message.hwnd == state.window ||
             IsChild(state.window, message.hwnd))) {
            SendMessageW(state.window, WM_VSCROLL,
                message.wParam == VK_PRIOR ? SB_PAGEUP : SB_PAGEDOWN, 0);
            continue;
        }
        if (!IsDialogMessageW(state.window, &message)) { TranslateMessage(&message); DispatchMessageW(&message); }
    }
    if (dialog_owner) { EnableWindow(dialog_owner, TRUE); SetForegroundWindow(dialog_owner); }
}

} // namespace

void set_dialog_owner(HWND owner) { dialog_owner = owner; }

void show_environment_settings_dialog() {
    static constexpr wchar_t class_name[] = L"AviUtl2LaTeX.EnvironmentSettings";
    if (!register_class(class_name, environment_proc)) return;
    EnvironmentWindow state;
    std::wstring ignored; load_tool_settings(state.settings, ignored);
    constexpr DWORD style = kResizableDialogStyle | WS_VSCROLL;
    constexpr DWORD extended_style = WS_EX_DLGMODALFRAME;
    auto placement = dialog_layout::calculate_initial_window_placement(
        dialog_owner, SIZE{580, 365}, style, extended_style, kDialogWorkMargin);
    const RECT rect = placement.final_rect;
    state.window = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, L"AviUtl2 LaTeX 環境設定",
        style, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        dialog_owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window) {
        placement = reconcile_created_window(
            state.window, SIZE{580, 365}, style, extended_style);
        RECT client{};
        GetClientRect(state.window, &client);
        dialog_layout::debug_log_window_placement(
            L"environment_settings", placement, &client);
        modal_loop(state);
    }
}

void show_information_dialog(const InformationDialogSnapshot& snapshot) {
    static constexpr wchar_t class_name[] = L"AviUtl2LaTeX.Information";
    if (!register_class(class_name, information_proc)) return;
    InformationWindow state;
    state.snapshot = snapshot;
    state.overview = build_information_overview(snapshot);
    state.report = build_diagnostic_report(snapshot);
    constexpr DWORD style = kResizableDialogStyle;
    constexpr DWORD extended_style = WS_EX_DLGMODALFRAME;
    auto placement = dialog_layout::calculate_initial_window_placement(
        dialog_owner, SIZE{720, 560}, style, extended_style, kDialogWorkMargin);
    const RECT rect = placement.final_rect;
    state.window = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name,
        L"AviUtl2 LaTeX 情報", style,
        rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        dialog_owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window) {
        placement = reconcile_created_window(
            state.window, SIZE{720, 560}, style, extended_style);
        RECT client{};
        GetClientRect(state.window, &client);
        dialog_layout::debug_log_window_placement(
            L"information", placement, &client);
        append_latest_log("info_dialog_opened: yes\n");
        modal_loop(state);
    }
}

std::optional<SystemFontSelection> show_system_font_dialog(
    const std::wstring& current_font,
    bool current_is_default) {
    static constexpr wchar_t class_name[] = L"AviUtl2LaTeX.FontSelector";
    static constexpr wchar_t preview_class_name[] = L"AviUtl2LaTeX.FontPreview";
    if (!register_class(preview_class_name, font_preview_proc) ||
        !register_class(class_name, font_proc)) return std::nullopt;
    FontWindow state;
    state.current = current_font;
    state.current_is_default = current_is_default;
    load_system_fonts(state);
    constexpr DWORD style = kResizableDialogStyle;
    constexpr DWORD extended_style = WS_EX_DLGMODALFRAME;
    auto placement = dialog_layout::calculate_initial_window_placement(
        dialog_owner, SIZE{456, 470}, style, extended_style, kDialogWorkMargin);
    const RECT rect = placement.final_rect;
    state.window = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, L"フォント選択",
        style, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
        dialog_owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window) {
        placement = reconcile_created_window(
            state.window, SIZE{456, 470}, style, extended_style);
        RECT client{};
        GetClientRect(state.window, &client);
        dialog_layout::debug_log_window_placement(
            L"font_selector", placement, &client);
        modal_loop(state);
    }
    return state.selected;
}

std::optional<std::filesystem::path> show_font_file_dialog(const std::filesystem::path& current_file) {
    const COMDLG_FILTERSPEC filters[] = {{L"OpenType/TrueTypeフォント", L"*.otf;*.ttf;*.ttc"},
        {L"すべてのファイル", L"*.*"}};
    return open_file(dialog_owner, filters, 2, current_file);
}
