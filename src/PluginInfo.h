#pragma once

#include <string>

enum class UserErrorCategory {
    None,
    EnvironmentNotConfigured,
    LuaLaTeXNotFound,
    MuToolNotFound,
    LuaLaTeXLaunchFailed,
    MuToolLaunchFailed,
    LatexCompileFailed,
    PdfRenderFailed,
    ImageLoadFailed,
    ImageTooLarge,
    FontNotFound,
    FontFileNotFound,
    CacheCorrupted,
    InvalidSetting,
    TimedOut,
    Cancelled,
    Unknown
};

struct LastOperationInfo {
    std::wstring status = L"未実行";
    std::wstring failed_stage = L"なし";
    std::wstring error_summary;
    std::wstring template_name = L"未確認";
    std::wstring font_display_name;
    std::wstring font_file_name;
    std::wstring timestamp = L"未実行";
    int render_dpi = 0;
    int exit_code = -1;
    bool cache_used = false;
    bool cache_known = false;
    UserErrorCategory error_category = UserErrorCategory::None;
};

struct InformationDialogSnapshot {
    LastOperationInfo last_operation;
};

const wchar_t* user_error_category_name(UserErrorCategory value);
const wchar_t* user_error_message(UserErrorCategory value);
UserErrorCategory classify_user_error(
    const std::wstring& failed_stage,
    const std::wstring& log_text);
LastOperationInfo inspect_latest_compile_failure(const LastOperationInfo& base);
std::wstring current_local_timestamp();
std::wstring current_windows_version();
std::wstring build_information_overview(
    const InformationDialogSnapshot& snapshot);
std::wstring build_diagnostic_report(
    const InformationDialogSnapshot& snapshot);

