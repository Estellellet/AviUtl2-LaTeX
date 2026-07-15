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
#include "LatexRenderer.h"
#include "PluginInfo.h"
#include "ToolSettings.h"

using Microsoft::WRL::ComPtr;

namespace {

HWND dialog_owner = nullptr;

constexpr int kEnvironmentClassAtom = 1;
constexpr int kFontClassAtom = 2;
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

int scale(HWND window, int value) {
    const UINT dpi = GetDpiForWindow(window);
    return MulDiv(value, dpi == 0 ? 96 : static_cast<int>(dpi), 96);
}

int owner_scale(int value) {
    const UINT dpi = dialog_owner != nullptr ? GetDpiForWindow(dialog_owner) : 96;
    return MulDiv(value, dpi == 0 ? 96 : static_cast<int>(dpi), 96);
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
    const int margin = scale(state.window, 12);
    const int label_width = scale(state.window, 110);
    const int edit_width = scale(state.window, 390);
    const int button_width = scale(state.window, 78);
    const int row = scale(state.window, 30);
    const int height = scale(state.window, 24);
    auto move = [&](HWND control, int x, int y, int w, int h) {
        MoveWindow(control, scale(state.window, x), scale(state.window, y),
            scale(state.window, w), scale(state.window, h), TRUE);
    };
    move(GetDlgItem(state.window, 1), 12, 14, 105, 24);
    move(state.environment, 120, 12, 180, 200);
    move(GetDlgItem(state.window, 2), 12, 47, 105, 24);
    move(state.lualatex, 120, 44, 370, 24);
    move(GetDlgItem(state.window, IDC_BROWSE_LUALATEX), 498, 44, 70, 24);
    move(GetDlgItem(state.window, 3), 12, 79, 105, 24);
    move(state.mutool, 120, 76, 370, 24);
    move(GetDlgItem(state.window, IDC_BROWSE_MUTOOL), 498, 76, 70, 24);
    move(GetDlgItem(state.window, IDC_AUTO_DETECT), 120, 110, 100, 26);
    move(GetDlgItem(state.window, IDC_DIAGNOSE), 228, 110, 100, 26);
    move(GetDlgItem(state.window, 4), 12, 145, 105, 24);
    move(state.diagnostic, 120, 143, 448, 170);
    move(GetDlgItem(state.window, IDC_SAVE), 410, 326, 76, 28);
    move(GetDlgItem(state.window, IDC_CANCEL), 492, 326, 76, 28);
    (void)margin; (void)label_width; (void)edit_width; (void)button_width; (void)row; (void)height;
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
        position_environment_controls(*state);
        return 0;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam);
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
        if (id == IDC_SAVE) {
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
        if (id == IDC_CANCEL) {
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
        state->search = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
            scale(window, 12), scale(window, 12), scale(window, 430), scale(window, 25), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_SEARCH)), GetModuleHandleW(nullptr), nullptr);
        state->list = CreateWindowExW(0, L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY,
            scale(window, 12), scale(window, 47), scale(window, 430), scale(window, 220), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_LIST)), GetModuleHandleW(nullptr), nullptr);
        state->preview = CreateWindowExW(0, L"AviUtl2LaTeX.FontPreview", L"",
            WS_CHILD | WS_VISIBLE,
            scale(window, 12), scale(window, 278), scale(window, 430), scale(window, 145), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_PREVIEW)),
            GetModuleHandleW(nullptr), state);
        CreateWindowExW(0, L"BUTTON", L"選択", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON,
            scale(window, 278), scale(window, 438), scale(window, 78), scale(window, 28), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_SELECT)), GetModuleHandleW(nullptr), nullptr);
        CreateWindowExW(0, L"BUTTON", L"キャンセル", WS_CHILD | WS_VISIBLE,
            scale(window, 364), scale(window, 438), scale(window, 78), scale(window, 28), window,
            reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_FONT_CANCEL)), GetModuleHandleW(nullptr), nullptr);
        refresh_font_list(*state); rebuild_font_preview(*state); return 0;
    }
    if (message == WM_COMMAND) {
        const int id = LOWORD(wparam), notification = HIWORD(wparam);
        if (id == IDC_FONT_SEARCH && notification == EN_CHANGE) {
            refresh_font_list(*state); rebuild_font_preview(*state); return 0;
        }
        if (id == IDC_FONT_LIST && notification == LBN_SELCHANGE) {
            rebuild_font_preview(*state); return 0;
        }
        if (id == IDC_FONT_SELECT || (id == IDC_FONT_LIST && notification == LBN_DBLCLK)) {
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
        if (id == IDC_FONT_CANCEL) { DestroyWindow(window); return 0; }
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) {
        reset_font_preview(*state);
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
    const int margin = scale(state.window, 12);
    const int width = scale(state.window, 696);
    const int text_height = scale(state.window, 456);
    MoveWindow(state.text, margin, margin, width, text_height, TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_INFO_COPY), margin,
        scale(state.window, 480), scale(state.window, 144), scale(state.window, 30), TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_INFO_LOG), scale(state.window, 164),
        scale(state.window, 480), scale(state.window, 112), scale(state.window, 30), TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_INFO_LOG_FOLDER), scale(state.window, 284),
        scale(state.window, 480), scale(state.window, 112), scale(state.window, 30), TRUE);
    MoveWindow(GetDlgItem(state.window, IDC_INFO_CLOSE), scale(state.window, 608),
        scale(state.window, 480), scale(state.window, 100), scale(state.window, 30), TRUE);
    MoveWindow(state.status, margin, scale(state.window, 520), width,
        scale(state.window, 24), TRUE);
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
        if (id == IDC_INFO_CLOSE) {
            DestroyWindow(window);
            return 0;
        }
    }
    if (message == WM_CLOSE) { DestroyWindow(window); return 0; }
    if (message == WM_DESTROY) { state->done = true; return 0; }
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
    state.window = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, L"AviUtl2 LaTeX 環境設定",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        owner_scale(610), owner_scale(410),
        dialog_owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window) modal_loop(state);
}

void show_information_dialog(const InformationDialogSnapshot& snapshot) {
    static constexpr wchar_t class_name[] = L"AviUtl2LaTeX.Information";
    if (!register_class(class_name, information_proc)) return;
    InformationWindow state;
    state.snapshot = snapshot;
    state.overview = build_information_overview(snapshot);
    state.report = build_diagnostic_report(snapshot);
    state.window = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name,
        L"AviUtl2 LaTeX 情報", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
        CW_USEDEFAULT, CW_USEDEFAULT, owner_scale(736), owner_scale(600),
        dialog_owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window) {
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
    state.window = CreateWindowExW(WS_EX_DLGMODALFRAME, class_name, L"フォント選択",
        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
        owner_scale(480), owner_scale(510),
        dialog_owner, nullptr, GetModuleHandleW(nullptr), &state);
    if (state.window) modal_loop(state);
    return state.selected;
}

std::optional<std::filesystem::path> show_font_file_dialog(const std::filesystem::path& current_file) {
    const COMDLG_FILTERSPEC filters[] = {{L"OpenType/TrueTypeフォント", L"*.otf;*.ttf;*.ttc"},
        {L"すべてのファイル", L"*.*"}};
    return open_file(dialog_owner, filters, 2, current_file);
}
