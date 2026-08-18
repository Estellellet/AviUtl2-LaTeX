#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace aviutl2_latex {

enum class JapaneseFontSource : int {
    Default = 0,
    InstalledFamily = 1,
    FontFile = 2
};

enum class JapaneseSpacingMode : int {
    Auto = 0,
    Uniform = 1,
    FontMetrics = 2
};

struct JapaneseFontConfig {
    JapaneseFontSource source = JapaneseFontSource::Default;
    std::wstring display_name;
    std::wstring fontspec_family_name;
    std::filesystem::path file_path;
    bool needs_migration = false;
};

struct LegacyJapaneseFontValues {
    std::optional<std::wstring> source_value;
    std::optional<std::wstring> display_name;
    std::optional<std::wstring> fontspec_family_name;
    std::optional<std::filesystem::path> file_path;
};

struct DocumentJapaneseFontPreamble {
    bool japanese_enabled = false;
    JapaneseFontSource source = JapaneseFontSource::Default;
    JapaneseSpacingMode spacing_requested = JapaneseSpacingMode::Auto;
    JapaneseSpacingMode spacing_effective = JapaneseSpacingMode::Auto;
    bool spacing_options_applied = false;
    std::wstring jfm = L"default";
    std::wstring kerning = L"default";
    bool palt = false;
    bool kern_feature = false;
    std::wstring generated_script = L"not-specified";
    std::wstring generated_raw_features = L"not-specified";
    std::wstring normalized_font_directory;
    std::wstring normalized_font_filename;
    std::uintmax_t font_file_size = 0;
    std::wstring font_file_last_write_time = L"not-applicable";
    std::wstring generated_setmainjfont;
    std::wstring preamble;
    std::wstring cache_material;
    std::wstring error_message;

    bool valid() const noexcept {
        return error_message.empty();
    }
};

std::optional<JapaneseFontSource> parse_japanese_font_source(
    std::wstring_view serialized_value);

std::optional<JapaneseSpacingMode> parse_japanese_spacing_mode(
    std::wstring_view serialized_value);

JapaneseSpacingMode resolve_japanese_spacing_mode(
    std::wstring_view serialized_value,
    JapaneseSpacingMode fallback = JapaneseSpacingMode::Auto);

const wchar_t* japanese_font_source_name(JapaneseFontSource source) noexcept;
const wchar_t* japanese_spacing_mode_name(JapaneseSpacingMode mode) noexcept;

bool is_saved_japanese_font_config_valid(
    const JapaneseFontConfig& configuration);

JapaneseFontConfig resolve_japanese_font_config(
    const std::optional<JapaneseFontConfig>& current,
    const LegacyJapaneseFontValues& legacy);

std::wstring japanese_font_display_value(
    const JapaneseFontConfig& configuration);

DocumentJapaneseFontPreamble build_document_japanese_font_preamble(
    bool japanese_enabled,
    const JapaneseFontConfig& font,
    JapaneseSpacingMode spacing_mode);

} // namespace aviutl2_latex
