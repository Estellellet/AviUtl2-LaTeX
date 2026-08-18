#include "PluginInfo.h"

#include <windows.h>
#include <winternl.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <fstream>
#include <new>
#include <sstream>
#include <string_view>
#include <system_error>

#include "AppPaths.h"
#include "GeneratedVersion.h"
#include "PersistentRenderCache.h"
#include "ToolSettings.h"

namespace {

constexpr std::uintmax_t kMaximumLatestLogReadBytes = 4ULL * 1024ULL * 1024ULL;

std::wstring utf8_to_wide(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, value.data(),
        static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
        result.data(), size);
    return result;
}

std::wstring read_latest_log() {
    AppPaths paths;
    std::wstring error;
    if (!resolve_app_paths(paths, error)) return {};
    std::error_code code;
    const std::uintmax_t file_size = std::filesystem::file_size(paths.latest_log, code);
    if (code || file_size == 0) return {};

    const std::uintmax_t read_size = (std::min)(file_size, kMaximumLatestLogReadBytes);
    try {
        std::ifstream input(paths.latest_log, std::ios::binary);
        if (!input) return {};
        const bool truncated = file_size > read_size;
        if (truncated) {
            input.seekg(-static_cast<std::streamoff>(read_size), std::ios::end);
            if (!input) return {};
        }
        std::string bytes(static_cast<std::size_t>(read_size), '\0');
        input.read(bytes.data(), static_cast<std::streamsize>(read_size));
        const std::streamsize bytes_read = input.gcount();
        if (bytes_read <= 0) return {};
        bytes.resize(static_cast<std::size_t>(bytes_read));
        if (truncated) {
            const std::size_t first_newline = bytes.find('\n');
            if (first_newline != std::string::npos) {
                bytes.erase(0, first_newline + 1);
            }
        }
        return utf8_to_wide(bytes);
    } catch (const std::bad_alloc&) {
        return {};
    }
}

std::wstring value_after_last(const std::wstring& text, std::wstring_view key) {
    const std::size_t position = text.rfind(key);
    if (position == std::wstring::npos) return {};
    const std::size_t begin = position + key.size();
    const std::size_t end = text.find_first_of(L"\r\n", begin);
    return text.substr(begin, end == std::wstring::npos ? end : end - begin);
}

bool contains_ci(std::wstring value, std::wstring needle) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    std::transform(needle.begin(), needle.end(), needle.begin(), towlower);
    return value.find(needle) != std::wstring::npos;
}

std::wstring configured_state(const std::filesystem::path& path) {
    return path.empty() ? L"未設定" : L"設定済み";
}

std::wstring cache_text(const LastOperationInfo& operation) {
    if (!operation.cache_known) return L"未確認";
    return operation.cache_used ? L"使用" : L"未使用";
}

} // namespace

const wchar_t* user_error_category_name(UserErrorCategory value) {
    switch (value) {
    case UserErrorCategory::None: return L"None";
    case UserErrorCategory::EnvironmentNotConfigured: return L"EnvironmentNotConfigured";
    case UserErrorCategory::LuaLaTeXNotFound: return L"LuaLaTeXNotFound";
    case UserErrorCategory::MuToolNotFound: return L"MuToolNotFound";
    case UserErrorCategory::LuaLaTeXLaunchFailed: return L"LuaLaTeXLaunchFailed";
    case UserErrorCategory::MuToolLaunchFailed: return L"MuToolLaunchFailed";
    case UserErrorCategory::LatexCompileFailed: return L"LatexCompileFailed";
    case UserErrorCategory::PdfRenderFailed: return L"PdfRenderFailed";
    case UserErrorCategory::ImageLoadFailed: return L"ImageLoadFailed";
    case UserErrorCategory::ImageTooLarge: return L"ImageTooLarge";
    case UserErrorCategory::FontNotFound: return L"FontNotFound";
    case UserErrorCategory::FontFileNotFound: return L"FontFileNotFound";
    case UserErrorCategory::CacheCorrupted: return L"CacheCorrupted";
    case UserErrorCategory::InvalidSetting: return L"InvalidSetting";
    case UserErrorCategory::TimedOut: return L"TimedOut";
    case UserErrorCategory::Cancelled: return L"Cancelled";
    default: return L"Unknown";
    }
}

const wchar_t* user_error_message(UserErrorCategory value) {
    switch (value) {
    case UserErrorCategory::None: return L"なし";
    case UserErrorCategory::EnvironmentNotConfigured: return L"外部ツールが設定されていません。";
    case UserErrorCategory::LuaLaTeXNotFound: return L"LuaLaTeXが見つかりません。";
    case UserErrorCategory::MuToolNotFound: return L"MuPDFが見つかりません。";
    case UserErrorCategory::LuaLaTeXLaunchFailed: return L"LuaLaTeXを起動できません。";
    case UserErrorCategory::MuToolLaunchFailed: return L"MuPDFを起動できません。";
    case UserErrorCategory::LatexCompileFailed: return L"LaTeXのコンパイルに失敗しました。";
    case UserErrorCategory::PdfRenderFailed: return L"PDFから画像への変換に失敗しました。";
    case UserErrorCategory::ImageLoadFailed: return L"生成画像を読み込めません。";
    case UserErrorCategory::ImageTooLarge: return L"生成画像が上限を超えています。";
    case UserErrorCategory::FontNotFound: return L"指定されたフォントが見つかりません。";
    case UserErrorCategory::FontFileNotFound: return L"指定されたフォントファイルが見つかりません。";
    case UserErrorCategory::CacheCorrupted: return L"キャッシュが破損しています。再生成を試みました。";
    case UserErrorCategory::InvalidSetting: return L"設定値が正しくありません。";
    case UserErrorCategory::TimedOut: return L"外部処理がタイムアウトしました。";
    case UserErrorCategory::Cancelled: return L"処理が中止されました。";
    default: return L"詳細はログを確認してください。";
    }
}

UserErrorCategory classify_user_error(
    const std::wstring& failed_stage,
    const std::wstring& log_text) {
    if (contains_ci(log_text, L"timed_out: yes") || contains_ci(log_text, L"timeout: yes"))
        return UserErrorCategory::TimedOut;
    if (failed_stage == L"compile_timeout") return UserErrorCategory::TimedOut;
    if (contains_ci(log_text, L"cancelled: yes")) return UserErrorCategory::Cancelled;
    if (failed_stage == L"settings_load") return UserErrorCategory::EnvironmentNotConfigured;
    if (failed_stage == L"lualatex_not_found") return UserErrorCategory::LuaLaTeXNotFound;
    if (failed_stage == L"mutool_not_found") return UserErrorCategory::MuToolNotFound;
    if (failed_stage == L"lualatex_launch") return UserErrorCategory::LuaLaTeXLaunchFailed;
    if (failed_stage == L"mutool_launch") return UserErrorCategory::MuToolLaunchFailed;
    if (failed_stage == L"latex_compile") {
        if ((contains_ci(log_text, L"font") && contains_ci(log_text, L"not found")) ||
            contains_ci(log_text, L"font not loadable") ||
            contains_ci(log_text, L"metric data not found"))
            return UserErrorCategory::FontNotFound;
        return UserErrorCategory::LatexCompileFailed;
    }
    if (failed_stage == L"pdf_render") return UserErrorCategory::PdfRenderFailed;
    if (failed_stage == L"source_size_limit" ||
        failed_stage == L"source_memory_allocation")
        return UserErrorCategory::InvalidSetting;
    if (failed_stage == L"render_memory_limit" ||
        failed_stage.find(L"image_limit") != std::wstring::npos ||
        failed_stage.find(L"size_limit") != std::wstring::npos)
        return UserErrorCategory::ImageTooLarge;
    if (failed_stage == L"png_image_processing" ||
        failed_stage == L"image_memory_allocation" ||
        failed_stage == L"empty_step_layer") return UserErrorCategory::ImageLoadFailed;
    if (failed_stage == L"japanese_font_configuration") {
        if (contains_ci(log_text, L"font_file_exists: no") ||
            contains_ci(log_text, L"does not exist"))
            return UserErrorCategory::FontFileNotFound;
        return UserErrorCategory::InvalidSetting;
    }
    if (failed_stage.find(L"configuration") != std::wstring::npos ||
        failed_stage == L"step_count_limit") return UserErrorCategory::InvalidSetting;
    if (contains_ci(log_text, L"cache_invalid: yes") ||
        contains_ci(log_text, L"disk_cache_invalid: yes"))
        return UserErrorCategory::CacheCorrupted;
    return UserErrorCategory::Unknown;
}

LastOperationInfo inspect_latest_compile_failure(const LastOperationInfo& base) {
    LastOperationInfo result = base;
    const std::wstring log = read_latest_log();
    static constexpr std::wstring_view preferred_stages[] = {
        L"lualatex_not_found", L"mutool_not_found", L"lualatex_launch",
        L"mutool_launch", L"latex_compile", L"pdf_render",
        L"tikz_image_limit", L"png_file_size_limit", L"decoded_image_size_limit",
        L"png_image_processing", L"image_memory_allocation",
        L"source_size_limit", L"source_memory_allocation",
        L"render_memory_limit", L"compile_timeout",
        L"japanese_font_configuration",
        L"tikz_library_configuration", L"settings_load", L"step_count_limit",
        L"empty_step_layer", L"global_content_bounds", L"fixed_layout_composition"
    };
    for (const auto stage : preferred_stages) {
        if (log.find(std::wstring(L"failed_stage: ") + std::wstring(stage)) !=
            std::wstring::npos) {
            result.failed_stage = stage;
            break;
        }
    }
    if (result.failed_stage == L"なし") {
        const std::wstring stage = value_after_last(log, L"failed_stage: ");
        if (!stage.empty() && stage != L"none") result.failed_stage = stage;
    }
    const std::wstring exit = value_after_last(log, L"process_exit_code: ");
    if (!exit.empty() && exit != L"not-started") {
        try { result.exit_code = std::stoi(exit); } catch (...) { result.exit_code = -1; }
    }
    result.error_category = classify_user_error(result.failed_stage, log);
    result.error_summary = user_error_message(result.error_category);
    result.status = L"失敗";
    result.cache_known = true;
    result.cache_used = false;
    result.timestamp = current_local_timestamp();
    return result;
}

std::wstring current_local_timestamp() {
    SYSTEMTIME value{};
    GetLocalTime(&value);
    wchar_t buffer[40]{};
    swprintf_s(buffer, L"%04u-%02u-%02u %02u:%02u:%02u",
        value.wYear, value.wMonth, value.wDay,
        value.wHour, value.wMinute, value.wSecond);
    return buffer;
}

std::wstring current_windows_version() {
    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    const HMODULE module = GetModuleHandleW(L"ntdll.dll");
    const auto function = module == nullptr ? nullptr :
        reinterpret_cast<RtlGetVersionFunction>(GetProcAddress(module, "RtlGetVersion"));
    RTL_OSVERSIONINFOW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    if (function == nullptr || function(&version) != 0) return L"Windows / unknown build";
    std::wostringstream text;
    text << L"Windows " << version.dwMajorVersion << L'.' << version.dwMinorVersion
         << L" build " << version.dwBuildNumber;
    return text.str();
}

std::wstring build_information_overview(const InformationDialogSnapshot& snapshot) {
    ToolSettings settings;
    std::wstring ignored;
    load_tool_settings(settings, ignored);
    const auto& operation = snapshot.last_operation;
    std::wostringstream out;
    out << L"【プラグイン情報】\r\n"
        << L"製品: " << AVIUTL2_LATEX_PRODUCT_NAME << L"\r\n"
        << L"バージョン: " << AVIUTL2_LATEX_VERSION_W << L"\r\n"
#ifdef NDEBUG
        << L"ビルド: Release / x64\r\n"
#else
        << L"ビルド: Debug / x64\r\n"
#endif
        << L"ビルドID: " << AVIUTL2_LATEX_BUILD_ID_W << L"\r\n"
        << L"ビルド日時: " << AVIUTL2_LATEX_BUILD_TIMESTAMP_W << L"\r\n"
        << L"必要AviUtl2: " << AVIUTL2_LATEX_REQUIRED_AVIUTL2_W << L"\r\n"
        << L"キャッシュ: " << utf8_to_wide(kPersistentRenderCacheVersion) << L"\r\n\r\n"
        << L"【外部ツール】\r\n"
        << L"TeX環境: " << tex_environment_name(settings.tex_environment) << L"\r\n"
        << L"LuaLaTeX: " << configured_state(settings.lualatex_path) << L"\r\n"
        << L"LuaLaTeXバージョン: " << (settings.last_lualatex_version.empty() ? L"未確認" : settings.last_lualatex_version) << L"\r\n"
        << L"MuPDF: " << configured_state(settings.mutool_path) << L"\r\n"
        << L"MuPDFバージョン: " << (settings.last_mutool_version.empty() ? L"未確認" : settings.last_mutool_version) << L"\r\n"
        << L"最後の環境診断: " << (settings.last_diagnostic_summary.empty() ? L"未確認" : settings.last_diagnostic_summary) << L"\r\n"
        << (settings.last_diagnostic_details.empty() ? L"" : settings.last_diagnostic_details + L"\r\n")
        << L"\r\n【最後の処理】\r\n"
        << L"状態: " << operation.status << L"\r\n"
        << L"テンプレート: " << operation.template_name << L"\r\n"
        << L"描画DPI: " << (operation.render_dpi > 0 ? std::to_wstring(operation.render_dpi) : L"未確認") << L"\r\n"
        << L"キャッシュ: " << cache_text(operation) << L"\r\n"
        << L"段階: " << operation.failed_stage << L"\r\n"
        << L"終了コード: " << (operation.exit_code >= 0 ? std::to_wstring(operation.exit_code) : L"未確認") << L"\r\n"
        << L"エラー: " << (operation.error_summary.empty() ? L"なし" : operation.error_summary) << L"\r\n"
        << L"日時: " << operation.timestamp << L"\r\n";
    return out.str();
}

std::wstring build_diagnostic_report(const InformationDialogSnapshot& snapshot) {
    ToolSettings settings;
    std::wstring ignored;
    load_tool_settings(settings, ignored);
    const auto& operation = snapshot.last_operation;
    std::wostringstream out;
    out << L"AviUtl2 LaTeX diagnostic report\r\n"
        << L"Plugin version: " << AVIUTL2_LATEX_VERSION_W << L"\r\n"
#ifdef NDEBUG
        << L"Build: Release x64\r\n"
#else
        << L"Build: Debug x64\r\n"
#endif
        << L"Build ID: " << AVIUTL2_LATEX_BUILD_ID_W << L"\r\n"
        << L"Build timestamp: " << AVIUTL2_LATEX_BUILD_TIMESTAMP_W << L"\r\n"
        << L"Required AviUtl2: 2.00.3300\r\n"
        << L"Cache version: " << utf8_to_wide(kPersistentRenderCacheVersion) << L"\r\n\r\n"
        << L"OS: " << current_windows_version() << L"\r\nArchitecture: x64\r\n\r\n"
        << L"TeX environment: " << tex_environment_name(settings.tex_environment) << L"\r\n"
        << L"LuaLaTeX: " << configured_state(settings.lualatex_path) << L"\r\n"
        << L"LuaLaTeX version: " << (settings.last_lualatex_version.empty() ? L"unverified" : settings.last_lualatex_version) << L"\r\n"
        << L"MuPDF: " << configured_state(settings.mutool_path) << L"\r\n"
        << L"MuPDF version: " << (settings.last_mutool_version.empty() ? L"unverified" : settings.last_mutool_version) << L"\r\n\r\n"
        << L"Environment diagnostics:\r\n"
        << (settings.last_diagnostic_details.empty() ? L"未確認" : settings.last_diagnostic_details) << L"\r\n\r\n"
        << L"Last operation:\r\n"
        << L"Template: " << operation.template_name << L"\r\n"
        << L"DPI: " << (operation.render_dpi > 0 ? std::to_wstring(operation.render_dpi) : L"unverified") << L"\r\n"
        << L"Status: " << operation.status << L"\r\n"
        << L"Error category: " << user_error_category_name(operation.error_category) << L"\r\n"
        << L"Error summary: " << (operation.error_summary.empty() ? L"none" : operation.error_summary) << L"\r\n"
        << L"Failed stage: " << operation.failed_stage << L"\r\n"
        << L"Exit code: " << (operation.exit_code >= 0 ? std::to_wstring(operation.exit_code) : L"unverified") << L"\r\n"
        << L"Cache used: " << (operation.cache_known ? (operation.cache_used ? L"yes" : L"no") : L"unverified") << L"\r\n"
        << L"Timestamp: " << operation.timestamp << L"\r\n";
    if (!operation.font_display_name.empty())
        out << L"Font: " << operation.font_display_name << L"\r\n";
    if (!operation.font_file_name.empty())
        out << L"Font file: " << std::filesystem::path(operation.font_file_name).filename().wstring() << L"\r\n";
    return out.str();
}
