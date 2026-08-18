#include "JapaneseFontConfig.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

namespace aviutl2_latex {
namespace {

std::wstring trim_copy(std::wstring_view value) {
    std::size_t begin = 0;
    while (begin < value.size() && std::iswspace(value[begin])) {
        ++begin;
    }
    std::size_t end = value.size();
    while (end > begin && std::iswspace(value[end - 1])) {
        --end;
    }
    return std::wstring(value.substr(begin, end - begin));
}

std::wstring lowercase_ascii_copy(std::wstring value) {
    // Supported font extensions are ASCII. Keep this conversion independent
    // from the process locale so it behaves identically on hosted CI runners.
    std::transform(value.begin(), value.end(), value.begin(), [](wchar_t value) {
        if (value >= L'A' && value <= L'Z') {
            return static_cast<wchar_t>(value + (L'a' - L'A'));
        }
        return value;
    });
    return value;
}

bool contains_unsafe_font_name_character(std::wstring_view value) {
    return value.find_first_of(L"\r\n{}%#\\&$^~_") != std::wstring_view::npos;
}

bool contains_unsafe_font_path_character(std::wstring_view value) {
    // A backslash is valid in the incoming Windows path. It is normalized to
    // a forward slash before the path is embedded in TeX.
    return value.find_first_of(L"\r\n{}%#&$^~") != std::wstring_view::npos;
}

bool optional_text_present(const std::optional<std::wstring>& value) {
    return value.has_value() && !trim_copy(*value).empty();
}

bool optional_path_present(const std::optional<std::filesystem::path>& value) {
    return value.has_value() && !value->empty();
}

void configure_spacing(
    DocumentJapaneseFontPreamble& result,
    JapaneseSpacingMode requested) {
    result.spacing_requested = requested;
    if (result.source == JapaneseFontSource::Default) {
        return;
    }

    result.spacing_effective = requested == JapaneseSpacingMode::Auto
        ? JapaneseSpacingMode::FontMetrics
        : requested;
    result.spacing_options_applied = true;
    if (result.spacing_effective == JapaneseSpacingMode::Uniform) {
        result.jfm = L"ujis";
        result.kerning = L"off";
        return;
    }

    result.jfm = L"propw";
    result.kerning = L"on";
    result.palt = true;
    result.kern_feature = true;
    result.generated_script = L"Default";
    result.generated_raw_features = L"+palt,+kern";
}

std::wstring spacing_options(
    const DocumentJapaneseFontPreamble& result,
    bool include_leading_comma) {
    if (!result.spacing_options_applied) {
        return {};
    }
    std::wstring options;
    if (include_leading_comma) {
        options += L",\n";
    }
    if (result.spacing_effective == JapaneseSpacingMode::Uniform) {
        options +=
            L"  YokoFeatures={JFM=ujis},\n"
            L"  Kerning=Off\n";
    } else {
        options +=
            L"  YokoFeatures={JFM=propw},\n"
            L"  Kerning=On,\n"
            L"  Script=Default,\n"
            L"  RawFeature={+palt,+kern}\n";
    }
    return options;
}

void append_spacing_cache_material(DocumentJapaneseFontPreamble& result) {
    result.cache_material +=
        L"\njapanese-spacing-requested=" +
            std::wstring(japanese_spacing_mode_name(result.spacing_requested)) +
        L"\njapanese-spacing-effective=" +
            std::wstring(result.spacing_options_applied
                ? japanese_spacing_mode_name(result.spacing_effective)
                : L"default") +
        L"\njapanese-jfm=" + result.jfm +
        L"\njapanese-kerning=" + result.kerning +
        L"\njapanese-palt=" + std::wstring(result.palt ? L"on" : L"off") +
        L"\njapanese-kern-feature=" +
            std::wstring(result.kern_feature ? L"on" : L"off");
}

} // namespace

std::optional<JapaneseFontSource> parse_japanese_font_source(
    std::wstring_view serialized_value) {
    const std::wstring value = trim_copy(serialized_value);
    if (value == L"0" || value == L"既定" || value == L"default" ||
        value == L"Default") {
        return JapaneseFontSource::Default;
    }
    if (value == L"1" || value == L"フォント名" ||
        value == L"installed-family" || value == L"InstalledFamily" ||
        value == L"font-name") {
        return JapaneseFontSource::InstalledFamily;
    }
    if (value == L"2" || value == L"フォントファイル" ||
        value == L"font-file" || value == L"FontFile") {
        return JapaneseFontSource::FontFile;
    }
    return std::nullopt;
}

std::optional<JapaneseSpacingMode> parse_japanese_spacing_mode(
    std::wstring_view serialized_value) {
    const std::wstring value = trim_copy(serialized_value);
    if (value == L"0" || value == L"自動" || value == L"auto" ||
        value == L"Auto") {
        return JapaneseSpacingMode::Auto;
    }
    if (value == L"1" || value == L"均等" || value == L"uniform" ||
        value == L"Uniform") {
        return JapaneseSpacingMode::Uniform;
    }
    if (value == L"2" || value == L"フォント準拠" ||
        value == L"font-metrics" || value == L"FontMetrics") {
        return JapaneseSpacingMode::FontMetrics;
    }
    // v0.1.x briefly exposed Custom. It was removed and is deliberately read
    // as Auto without rewriting the saved object.
    if (value == L"3" || value == L"カスタム" || value == L"custom" ||
        value == L"Custom") {
        return JapaneseSpacingMode::Auto;
    }
    return std::nullopt;
}

JapaneseSpacingMode resolve_japanese_spacing_mode(
    std::wstring_view serialized_value,
    JapaneseSpacingMode fallback) {
    const auto parsed = parse_japanese_spacing_mode(serialized_value);
    return parsed.value_or(fallback);
}

const wchar_t* japanese_font_source_name(JapaneseFontSource source) noexcept {
    switch (source) {
    case JapaneseFontSource::InstalledFamily:
        return L"installed-family";
    case JapaneseFontSource::FontFile:
        return L"font-file";
    default:
        return L"default";
    }
}

const wchar_t* japanese_spacing_mode_name(JapaneseSpacingMode mode) noexcept {
    switch (mode) {
    case JapaneseSpacingMode::Uniform:
        return L"uniform";
    case JapaneseSpacingMode::FontMetrics:
        return L"font-metrics";
    default:
        return L"auto";
    }
}

bool is_saved_japanese_font_config_valid(
    const JapaneseFontConfig& configuration) {
    switch (configuration.source) {
    case JapaneseFontSource::Default:
        return true;
    case JapaneseFontSource::InstalledFamily:
        return !trim_copy(configuration.fontspec_family_name).empty();
    case JapaneseFontSource::FontFile:
        return !configuration.file_path.empty();
    default:
        return false;
    }
}

JapaneseFontConfig resolve_japanese_font_config(
    const std::optional<JapaneseFontConfig>& current,
    const LegacyJapaneseFontValues& legacy) {
    if (current.has_value() && is_saved_japanese_font_config_valid(*current)) {
        JapaneseFontConfig result = *current;
        result.needs_migration = false;
        if (result.display_name.empty()) {
            result.display_name = japanese_font_display_value(result);
        }
        return result;
    }

    JapaneseFontConfig result;
    result.needs_migration = true;
    const bool source_missing = !legacy.source_value.has_value() ||
        trim_copy(*legacy.source_value).empty();
    const auto parsed_source = source_missing
        ? std::optional<JapaneseFontSource>{}
        : parse_japanese_font_source(*legacy.source_value);
    if (parsed_source.has_value()) {
        result.source = *parsed_source;
    } else if (source_missing) {
        // Infer only when the old mode is absent. An explicit Default must win
        // over stale family/file fields left by a prior selection.
        if (optional_path_present(legacy.file_path)) {
            result.source = JapaneseFontSource::FontFile;
        } else if (optional_text_present(legacy.fontspec_family_name)) {
            result.source = JapaneseFontSource::InstalledFamily;
        }
    }

    if (legacy.fontspec_family_name.has_value()) {
        result.fontspec_family_name = trim_copy(*legacy.fontspec_family_name);
    }
    if (legacy.file_path.has_value()) {
        result.file_path = *legacy.file_path;
    }
    if (legacy.display_name.has_value()) {
        result.display_name = trim_copy(*legacy.display_name);
    }
    if (!is_saved_japanese_font_config_valid(result)) {
        JapaneseFontConfig fallback;
        fallback.display_name = L"既定";
        fallback.needs_migration = true;
        return fallback;
    }
    if (result.display_name.empty()) {
        result.display_name = japanese_font_display_value(result);
    }
    return result;
}

std::wstring japanese_font_display_value(
    const JapaneseFontConfig& configuration) {
    if (configuration.source == JapaneseFontSource::Default) {
        return L"既定";
    }
    const std::wstring saved_display = trim_copy(configuration.display_name);
    if (!saved_display.empty() && saved_display != L"既定") {
        return saved_display;
    }
    if (configuration.source == JapaneseFontSource::InstalledFamily &&
        !trim_copy(configuration.fontspec_family_name).empty()) {
        return trim_copy(configuration.fontspec_family_name);
    }
    if (configuration.source == JapaneseFontSource::FontFile &&
        !configuration.file_path.empty()) {
        const std::wstring filename = configuration.file_path.filename().wstring();
        if (!filename.empty()) {
            return filename;
        }
    }
    return L"既定";
}

DocumentJapaneseFontPreamble build_document_japanese_font_preamble(
    bool japanese_enabled,
    const JapaneseFontConfig& font,
    JapaneseSpacingMode spacing_mode) {
    DocumentJapaneseFontPreamble result;
    result.japanese_enabled = japanese_enabled;
    result.source = font.source;
    result.spacing_requested = spacing_mode;
    result.cache_material = L"japanese_enabled=" +
        std::to_wstring(japanese_enabled ? 1 : 0);
    if (!japanese_enabled) {
        return result;
    }

    result.cache_material += L"\njapanese-font-source=" +
        std::wstring(japanese_font_source_name(font.source));
    configure_spacing(result, spacing_mode);
    append_spacing_cache_material(result);
    result.preamble =
        L"\\usepackage[no-math]{fontspec}\n"
        L"\\usepackage{luatexja}\n"
        L"\\usepackage{luatexja-fontspec}\n";
    if (font.source == JapaneseFontSource::Default) {
        return result;
    }

    if (font.source == JapaneseFontSource::InstalledFamily) {
        const std::wstring family = trim_copy(font.fontspec_family_name);
        if (family.empty()) {
            result.error_message = L"日本語フォント名が空です";
            return result;
        }
        if (contains_unsafe_font_name_character(family)) {
            result.error_message =
                L"日本語フォント名にTeXへ安全に渡せない文字が含まれています";
            return result;
        }
        if (result.spacing_options_applied) {
            result.generated_setmainjfont =
                L"\\setmainjfont[\n" + spacing_options(result, false) +
                L"]{" + family + L"}\n";
        } else {
            result.generated_setmainjfont =
                L"\\setmainjfont{" + family + L"}\n";
        }
        result.preamble += result.generated_setmainjfont;
        result.cache_material += L"\nfontspec-family-name=" + family;
        return result;
    }

    if (font.file_path.empty()) {
        result.error_message = L"日本語フォントファイルが空です";
        return result;
    }
    const std::wstring requested_path = font.file_path.wstring();
    if (contains_unsafe_font_path_character(requested_path)) {
        result.error_message =
            L"日本語フォントファイルのパスにTeXへ安全に渡せない文字が含まれています";
        return result;
    }

    std::error_code error;
    std::filesystem::path normalized =
        std::filesystem::absolute(font.file_path, error).lexically_normal();
    if (error) {
        result.error_message = L"日本語フォントファイルの絶対パスを取得できません";
        return result;
    }
    if (!std::filesystem::exists(normalized, error) || error ||
        !std::filesystem::is_regular_file(normalized, error) || error) {
        result.error_message =
            L"日本語フォントファイルが存在しないか、通常ファイルではありません";
        return result;
    }

    const std::wstring extension =
        lowercase_ascii_copy(normalized.extension().wstring());
    if (extension != L".otf" && extension != L".ttf" && extension != L".ttc") {
        result.error_message =
            L"日本語フォントファイルの拡張子は.otf、.ttf、.ttcのみ対応しています";
        return result;
    }

    result.font_file_size = std::filesystem::file_size(normalized, error);
    if (error) {
        result.error_message = L"日本語フォントファイルのサイズを取得できません";
        return result;
    }
    const auto last_write_time = std::filesystem::last_write_time(normalized, error);
    result.font_file_last_write_time = error
        ? L"unavailable"
        : std::to_wstring(last_write_time.time_since_epoch().count());
    error.clear();

    result.normalized_font_directory = normalized.parent_path().generic_wstring();
    if (!result.normalized_font_directory.empty() &&
        result.normalized_font_directory.back() != L'/') {
        result.normalized_font_directory.push_back(L'/');
    }
    result.normalized_font_filename = normalized.filename().generic_wstring();
    if (contains_unsafe_font_path_character(result.normalized_font_directory) ||
        contains_unsafe_font_path_character(result.normalized_font_filename)) {
        result.error_message =
            L"日本語フォントファイルのパスにTeXへ安全に渡せない文字が含まれています";
        return result;
    }

    result.generated_setmainjfont =
        L"\\setmainjfont[\n  Path={" + result.normalized_font_directory + L"}" +
        spacing_options(result, true) + L"]{" +
        result.normalized_font_filename + L"}\n";
    result.preamble += result.generated_setmainjfont;
    result.cache_material +=
        L"\nnormalized-font-file=" + result.normalized_font_directory +
            result.normalized_font_filename +
        L"\nfont-file-size=" + std::to_wstring(result.font_file_size) +
        L"\nfont-file-last-write-time=" + result.font_file_last_write_time;
    return result;
}

} // namespace aviutl2_latex
