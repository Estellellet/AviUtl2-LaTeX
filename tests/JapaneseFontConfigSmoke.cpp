#include "JapaneseFontConfig.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>

namespace {

using aviutl2_latex::DocumentJapaneseFontPreamble;
using aviutl2_latex::JapaneseFontConfig;
using aviutl2_latex::JapaneseFontSource;
using aviutl2_latex::JapaneseSpacingMode;
using aviutl2_latex::LegacyJapaneseFontValues;

int fail(const char* message) {
    std::cerr << message << '\n';
    return 1;
}

bool contains(std::wstring_view text, std::wstring_view value) {
    return text.find(value) != std::wstring_view::npos;
}

bool excludes_forbidden_font_commands(const DocumentJapaneseFontPreamble& value) {
    return !contains(value.preamble, L"\\setmainfont") &&
        !contains(value.preamble, L"\\setmathfont") &&
        !contains(value.preamble, L"\\setsansfont") &&
        !contains(value.preamble, L"\\setmonofont");
}

} // namespace

int main() {
    using namespace aviutl2_latex;

    if (parse_japanese_font_source(L"フォント名") !=
            JapaneseFontSource::InstalledFamily ||
        parse_japanese_font_source(L"2") != JapaneseFontSource::FontFile ||
        parse_japanese_spacing_mode(L"均等") != JapaneseSpacingMode::Uniform ||
        parse_japanese_spacing_mode(L"フォント準拠") !=
            JapaneseSpacingMode::FontMetrics ||
        resolve_japanese_spacing_mode(L"カスタム") !=
            JapaneseSpacingMode::Auto) {
        return fail("serialized Japanese font values were not parsed correctly");
    }

    JapaneseFontConfig installed;
    installed.source = JapaneseFontSource::InstalledFamily;
    installed.display_name = L"表示専用名";
    installed.fontspec_family_name = L"Yu Gothic";

    const auto automatic = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::Auto);
    if (!automatic.valid() ||
        !contains(automatic.preamble, L"\\usepackage[no-math]{fontspec}") ||
        !contains(automatic.generated_setmainjfont, L"YokoFeatures={JFM=propw}") ||
        !contains(automatic.generated_setmainjfont, L"Kerning=On") ||
        !contains(automatic.generated_setmainjfont, L"RawFeature={+palt,+kern}") ||
        !contains(automatic.generated_setmainjfont, L"{Yu Gothic}") ||
        !excludes_forbidden_font_commands(automatic)) {
        return fail("FontMetrics preamble is invalid or changes Latin/math fonts");
    }

    const auto uniform = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::Uniform);
    if (!uniform.valid() ||
        !contains(uniform.generated_setmainjfont, L"YokoFeatures={JFM=ujis}") ||
        !contains(uniform.generated_setmainjfont, L"Kerning=Off") ||
        contains(uniform.generated_setmainjfont, L"RawFeature") ||
        !excludes_forbidden_font_commands(uniform)) {
        return fail("Uniform preamble is invalid");
    }

    const auto metrics = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::FontMetrics);
    if (!metrics.valid() || automatic.generated_setmainjfont !=
            metrics.generated_setmainjfont) {
        return fail("Auto with an installed font must resolve to FontMetrics");
    }

    JapaneseFontConfig default_font;
    const auto default_result = build_document_japanese_font_preamble(
        true, default_font, JapaneseSpacingMode::Uniform);
    if (!default_result.valid() || !default_result.generated_setmainjfont.empty() ||
        !excludes_forbidden_font_commands(default_result)) {
        return fail("Default font must not be reset by generated TeX");
    }
    const auto disabled = build_document_japanese_font_preamble(
        false, installed, JapaneseSpacingMode::FontMetrics);
    if (!disabled.valid() || !disabled.preamble.empty()) {
        return fail("Japanese-disabled document must not receive a font preamble");
    }

    JapaneseFontConfig renamed_display = installed;
    renamed_display.display_name = L"別の表示名";
    const auto renamed_result = build_document_japanese_font_preamble(
        true, renamed_display, JapaneseSpacingMode::Auto);
    if (automatic.cache_material != renamed_result.cache_material ||
        contains(automatic.cache_material, L"表示専用名") ||
        japanese_font_display_value(installed) != L"表示専用名") {
        return fail("display-only name leaked into the render cache material");
    }

    LegacyJapaneseFontValues legacy_name;
    legacy_name.fontspec_family_name = L"Meiryo";
    const auto inferred_name = resolve_japanese_font_config(std::nullopt, legacy_name);
    if (inferred_name.source != JapaneseFontSource::InstalledFamily ||
        !inferred_name.needs_migration || inferred_name.display_name != L"Meiryo") {
        return fail("missing legacy source was not inferred from the family name");
    }

    LegacyJapaneseFontValues legacy_file;
    legacy_file.file_path = std::filesystem::path(L"legacy-font.otf");
    const auto inferred_file = resolve_japanese_font_config(std::nullopt, legacy_file);
    if (inferred_file.source != JapaneseFontSource::FontFile ||
        inferred_file.display_name != L"legacy-font.otf") {
        return fail("missing legacy source was not inferred from the file path");
    }

    LegacyJapaneseFontValues explicit_default = legacy_name;
    explicit_default.source_value = L"既定";
    explicit_default.file_path = std::filesystem::path(L"stale.otf");
    const auto resolved_default = resolve_japanese_font_config(
        std::nullopt, explicit_default);
    if (resolved_default.source != JapaneseFontSource::Default) {
        return fail("explicit legacy Default must beat stale font fields");
    }
    JapaneseFontConfig stale_default_display;
    stale_default_display.display_name = L"Yu Gothic";
    if (japanese_font_display_value(stale_default_display) != L"既定") {
        return fail("Default source retained a stale display-only font name");
    }
    JapaneseFontConfig stale_installed_display = installed;
    stale_installed_display.display_name = L"既定";
    if (japanese_font_display_value(stale_installed_display) != L"Yu Gothic") {
        return fail("installed family did not replace a stale Default display");
    }

    LegacyJapaneseFontValues invalid_mode = legacy_name;
    invalid_mode.source_value = L"unknown-mode";
    const auto invalid_resolved = resolve_japanese_font_config(
        std::nullopt, invalid_mode);
    if (invalid_resolved.source != JapaneseFontSource::Default) {
        return fail("only a missing legacy mode may be inferred");
    }

    LegacyJapaneseFontValues incomplete_legacy;
    incomplete_legacy.source_value = L"フォント名";
    const auto incomplete_resolved = resolve_japanese_font_config(
        std::nullopt, incomplete_legacy);
    if (incomplete_resolved.source != JapaneseFontSource::Default ||
        incomplete_resolved.display_name != L"既定") {
        return fail("invalid legacy font data did not fall back to Default");
    }

    JapaneseFontConfig current = installed;
    current.fontspec_family_name = L"Current Family";
    const auto preferred_current = resolve_japanese_font_config(current, legacy_name);
    if (preferred_current.fontspec_family_name != L"Current Family" ||
        preferred_current.needs_migration) {
        return fail("current hidden font data did not take priority over legacy data");
    }

    JapaneseFontConfig invalid_current;
    invalid_current.source = JapaneseFontSource::InstalledFamily;
    const auto legacy_fallback = resolve_japanese_font_config(
        invalid_current, legacy_name);
    if (legacy_fallback.fontspec_family_name != L"Meiryo" ||
        !legacy_fallback.needs_migration) {
        return fail("invalid current data did not fall back to valid legacy data");
    }

    const std::filesystem::path test_root =
        std::filesystem::temp_directory_path() /
        (L"AviUtl2LaTeX-JapaneseFontConfigSmoke-日本語 (A)[B]-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(test_root, error);
    error.clear();
    std::filesystem::create_directories(test_root, error);
    if (error) {
        return fail("could not create the font config smoke-test directory");
    }
    const std::filesystem::path font_path = test_root / L"テスト Font (A)[B].OTF";
    {
        std::ofstream font_file(font_path, std::ios::binary | std::ios::trunc);
        font_file << "font-identity";
        if (!font_file) {
            std::filesystem::remove_all(test_root, error);
            return fail("could not create the font identity fixture");
        }
    }

    JapaneseFontConfig file_font;
    file_font.source = JapaneseFontSource::FontFile;
    file_font.display_name = L"表示だけ";
    file_font.file_path = font_path;
    const auto file_result = build_document_japanese_font_preamble(
        true, file_font, JapaneseSpacingMode::FontMetrics);
    if (!file_result.valid() ||
        !contains(file_result.generated_setmainjfont, L"Path={") ||
        !contains(file_result.generated_setmainjfont, L"テスト Font (A)[B].OTF") ||
        !contains(file_result.cache_material, L"normalized-font-file=") ||
        !contains(file_result.cache_material, L"font-file-size=13") ||
        !contains(file_result.cache_material, L"font-file-last-write-time=") ||
        contains(file_result.cache_material, L"表示だけ") ||
        !excludes_forbidden_font_commands(file_result)) {
        std::filesystem::remove_all(test_root, error);
        return fail("safe font-file identity or TeX generation is invalid");
    }
    for (const wchar_t* extension : {L".ttf", L".ttc"}) {
        const std::filesystem::path candidate = test_root /
            (std::wstring(L"font fixture") + extension);
        std::ofstream(candidate, std::ios::binary | std::ios::trunc) << "x";
        JapaneseFontConfig extension_font = file_font;
        extension_font.file_path = candidate;
        if (!build_document_japanese_font_preamble(
                true, extension_font, JapaneseSpacingMode::Auto).valid()) {
            std::filesystem::remove_all(test_root, error);
            return fail("supported TTF/TTC extension was rejected");
        }
    }
    const std::filesystem::path unsupported_path = test_root / L"font.woff";
    std::ofstream(unsupported_path, std::ios::binary | std::ios::trunc) << "x";
    JapaneseFontConfig unsupported = file_font;
    unsupported.file_path = unsupported_path;
    if (build_document_japanese_font_preamble(
            true, unsupported, JapaneseSpacingMode::Auto).valid()) {
        std::filesystem::remove_all(test_root, error);
        return fail("unsupported font extension was accepted");
    }
    JapaneseFontConfig missing = file_font;
    missing.file_path = test_root / L"missing.otf";
    if (build_document_japanese_font_preamble(
            true, missing, JapaneseSpacingMode::Auto).valid()) {
        std::filesystem::remove_all(test_root, error);
        return fail("missing font file was accepted");
    }

    JapaneseFontConfig unsafe_name = installed;
    unsafe_name.fontspec_family_name = L"Bad#Font";
    if (build_document_japanese_font_preamble(
            true, unsafe_name, JapaneseSpacingMode::Auto).valid()) {
        std::filesystem::remove_all(test_root, error);
        return fail("unsafe family name was accepted");
    }
    JapaneseFontConfig unsafe_path = file_font;
    unsafe_path.file_path = test_root / L"Bad#Font.otf";
    if (build_document_japanese_font_preamble(
            true, unsafe_path, JapaneseSpacingMode::Auto).valid()) {
        std::filesystem::remove_all(test_root, error);
        return fail("unsafe font path was accepted");
    }

    std::filesystem::remove_all(test_root, error);
    return 0;
}
