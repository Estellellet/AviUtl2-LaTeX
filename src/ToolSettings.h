#pragma once

#include <filesystem>
#include <string>

enum class TexEnvironment {
    Auto,
    MiKTeX,
    TeXLive,
    Other
};

struct ToolSettings {
    TexEnvironment tex_environment = TexEnvironment::Auto;
    std::filesystem::path lualatex_path;
    std::filesystem::path mutool_path;
    std::wstring last_lualatex_version;
    std::wstring last_mutool_version;
    std::wstring last_diagnostic_summary;
    std::wstring last_diagnostic_details;
};

struct ResolvedTools {
    ToolSettings settings;
    std::filesystem::path lualatex_path;
    std::filesystem::path mutool_path;
    std::wstring lualatex_source;
    std::wstring mutool_source;

    bool valid() const;
};

bool load_tool_settings(ToolSettings& settings, std::wstring& error);
bool save_tool_settings(const ToolSettings& settings, std::wstring& error);
ResolvedTools resolve_external_tools(const ToolSettings& settings);
bool is_valid_executable_file(const std::filesystem::path& path);
const wchar_t* tex_environment_name(TexEnvironment value);
TexEnvironment tex_environment_from_name(const std::wstring& value);
