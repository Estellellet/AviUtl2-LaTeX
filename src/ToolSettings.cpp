#include "ToolSettings.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <new>
#include <sstream>
#include <system_error>
#include <utility>

#include "AppPaths.h"

namespace {

constexpr std::uintmax_t kMaximumSettingsFileSize = 1024ULL * 1024ULL;

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr) != size) return {};
    return result;
}

std::wstring from_utf8(const std::string& value) {
    if (value.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size) != size) return {};
    return result;
}

std::string json_escape(const std::wstring& value) {
    std::string result;
    for (const unsigned char c : to_utf8(value)) {
        switch (c) {
        case '\\': result += "\\\\"; break;
        case '"': result += "\\\""; break;
        case '\n': result += "\\n"; break;
        case '\r': result += "\\r"; break;
        case '\t': result += "\\t"; break;
        default: result.push_back(static_cast<char>(c)); break;
        }
    }
    return result;
}

bool json_string(const std::string& json, const std::string& key, std::wstring& value) {
    const std::string marker = "\"" + key + "\"";
    std::size_t position = json.find(marker);
    if (position == std::string::npos) return false;
    position = json.find(':', position + marker.size());
    if (position == std::string::npos) return false;
    position = json.find('"', position + 1);
    if (position == std::string::npos) return false;
    ++position;
    std::string decoded;
    bool escaped = false;
    for (; position < json.size(); ++position) {
        const char c = json[position];
        if (escaped) {
            switch (c) {
            case '\\': decoded.push_back('\\'); break;
            case '"': decoded.push_back('"'); break;
            case 'n': decoded.push_back('\n'); break;
            case 'r': decoded.push_back('\r'); break;
            case 't': decoded.push_back('\t'); break;
            default: return false;
            }
            escaped = false;
        } else if (c == '\\') {
            escaped = true;
        } else if (c == '"') {
            value = from_utf8(decoded);
            return !decoded.empty() ? !value.empty() : true;
        } else {
            decoded.push_back(c);
        }
    }
    return false;
}

std::wstring timestamp() {
    SYSTEMTIME time{};
    GetLocalTime(&time);
    wchar_t buffer[32]{};
    swprintf_s(buffer, L"%04u%02u%02u-%02u%02u%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute, time.wSecond);
    return buffer;
}

std::filesystem::path environment_path(const wchar_t* name) {
    const DWORD required = GetEnvironmentVariableW(name, nullptr, 0);
    if (required == 0) return {};
    std::wstring value(required, L'\0');
    const DWORD length = GetEnvironmentVariableW(name, value.data(), required);
    if (length == 0 || length >= required) return {};
    value.resize(length);
    return value;
}

std::filesystem::path search_path(const wchar_t* file) {
    const DWORD required = SearchPathW(nullptr, file, nullptr, 0, nullptr, nullptr);
    if (required == 0) return {};
    std::wstring value(static_cast<std::size_t>(required) + 1, L'\0');
    const DWORD length = SearchPathW(nullptr, file, nullptr,
        static_cast<DWORD>(value.size()), value.data(), nullptr);
    if (length == 0 || length >= value.size()) return {};
    value.resize(length);
    return value;
}

void resolve_one(const std::filesystem::path& explicit_path,
    const wchar_t* environment_name, const wchar_t* executable,
    std::filesystem::path& result, std::wstring& source) {
    if (is_valid_executable_file(explicit_path)) {
        result = explicit_path;
        source = L"settings.json";
        return;
    }
    if (!explicit_path.empty()) {
        source = L"configured-path-invalid";
        return;
    }
    const auto environment = environment_path(environment_name);
    if (is_valid_executable_file(environment)) {
        result = environment;
        source = L"environment";
        return;
    }
    const auto path = search_path(executable);
    if (is_valid_executable_file(path)) {
        result = path;
        source = L"PATH";
        return;
    }
    source = L"not-detected";
}

} // namespace

bool ResolvedTools::valid() const {
    return is_valid_executable_file(lualatex_path) && is_valid_executable_file(mutool_path);
}

bool is_valid_executable_file(const std::filesystem::path& path) {
    if (path.empty()) return false;
    std::error_code code;
    return std::filesystem::is_regular_file(path, code) && !code &&
        path.extension().wstring().size() == 4 &&
        _wcsicmp(path.extension().c_str(), L".exe") == 0;
}

const wchar_t* tex_environment_name(TexEnvironment value) {
    switch (value) {
    case TexEnvironment::MiKTeX: return L"MiKTeX";
    case TexEnvironment::TeXLive: return L"TeX Live";
    case TexEnvironment::Other: return L"その他";
    default: return L"自動";
    }
}

TexEnvironment tex_environment_from_name(const std::wstring& value) {
    if (value == L"MiKTeX") return TexEnvironment::MiKTeX;
    if (value == L"TeX Live") return TexEnvironment::TeXLive;
    if (value == L"その他") return TexEnvironment::Other;
    return TexEnvironment::Auto;
}

bool load_tool_settings(ToolSettings& settings, std::wstring& error) {
    AppPaths paths;
    if (!resolve_app_paths(paths, error) || !ensure_runtime_directories(paths, error)) return false;
    std::error_code code;
    const bool settings_exist = std::filesystem::exists(paths.settings_file, code);
    if (code) {
        error = L"settings.jsonの存在を確認できません";
        return false;
    }
    if (!settings_exist) return true;

    const std::uintmax_t file_size =
        std::filesystem::file_size(paths.settings_file, code);
    const bool invalid_size = code || file_size == 0 ||
        file_size > kMaximumSettingsFileSize;
    bool invalid_contents = invalid_size;
    ToolSettings loaded;
    try {
        if (!invalid_size) {
            std::ifstream input(paths.settings_file, std::ios::binary);
            std::string json(static_cast<std::size_t>(file_size), '\0');
            if (!input ||
                (file_size != 0 &&
                    !input.read(json.data(), static_cast<std::streamsize>(file_size))) ||
                static_cast<std::uintmax_t>(input.gcount()) != file_size ||
                input.peek() != std::char_traits<char>::eof()) {
                invalid_contents = true;
            } else {
                std::wstring environment;
                std::wstring lualatex;
                std::wstring mutool;
                invalid_contents =
                    !json_string(json, "tex_environment", environment) ||
                    !json_string(json, "lualatex_path", lualatex) ||
                    !json_string(json, "mutool_path", mutool);
                if (!invalid_contents) {
                    loaded.tex_environment = tex_environment_from_name(environment);
                    loaded.lualatex_path = std::move(lualatex);
                    loaded.mutool_path = std::move(mutool);
                    json_string(json, "last_lualatex_version", loaded.last_lualatex_version);
                    json_string(json, "last_mutool_version", loaded.last_mutool_version);
                    json_string(json, "last_diagnostic_summary", loaded.last_diagnostic_summary);
                    json_string(json, "last_diagnostic_details", loaded.last_diagnostic_details);
                }
            }
        }
    } catch (const std::bad_alloc&) {
        settings = {};
        error = L"settings.jsonを読み込むメモリが不足しています";
        return false;
    }

    if (invalid_contents) {
        const auto broken = paths.settings_file.wstring() + L".broken-" + timestamp();
        code.clear();
        std::filesystem::rename(paths.settings_file, broken, code);
        settings = {};
        error = invalid_size
            ? L"大きすぎるか壊れたsettings.jsonを退避し、既定値へ戻しました"
            : L"壊れたsettings.jsonを退避し、既定値へ戻しました";
        return true;
    }
    settings = std::move(loaded);
    return true;
}

bool save_tool_settings(const ToolSettings& settings, std::wstring& error) {
    AppPaths paths;
    if (!resolve_app_paths(paths, error) || !ensure_runtime_directories(paths, error)) return false;
    const auto temporary = paths.settings_file.wstring() + L".tmp";
    const std::string json =
        "{\n  \"schema_version\": 2,\n  \"tex_environment\": \"" +
        json_escape(tex_environment_name(settings.tex_environment)) +
        "\",\n  \"lualatex_path\": \"" + json_escape(settings.lualatex_path.wstring()) +
        "\",\n  \"mutool_path\": \"" + json_escape(settings.mutool_path.wstring()) +
        "\",\n  \"last_lualatex_version\": \"" + json_escape(settings.last_lualatex_version) +
        "\",\n  \"last_mutool_version\": \"" + json_escape(settings.last_mutool_version) +
        "\",\n  \"last_diagnostic_summary\": \"" + json_escape(settings.last_diagnostic_summary) +
        "\",\n  \"last_diagnostic_details\": \"" + json_escape(settings.last_diagnostic_details) + "\"\n}\n";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(json.data(), static_cast<std::streamsize>(json.size()));
        if (!output.good()) {
            error = L"settings.jsonの一時ファイルを書き込めません";
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), paths.settings_file.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        error = L"settings.jsonを置換できません";
        return false;
    }
    return true;
}

ResolvedTools resolve_external_tools(const ToolSettings& settings) {
    ResolvedTools resolved;
    resolved.settings = settings;
    resolve_one(settings.lualatex_path, L"AVIUTL2_LATEX_LUALATEX", L"lualatex.exe",
        resolved.lualatex_path, resolved.lualatex_source);
    resolve_one(settings.mutool_path, L"AVIUTL2_LATEX_MUTOOL", L"mutool.exe",
        resolved.mutool_path, resolved.mutool_source);
    return resolved;
}
