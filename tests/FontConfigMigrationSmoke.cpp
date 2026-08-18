#include "JapaneseFontConfig.h"
#include "JapaneseFontConfigTestSupport.h"

#include <filesystem>
#include <optional>
#include <string_view>

namespace {

using aviutl2_latex::JapaneseFontConfig;
using aviutl2_latex::JapaneseFontSource;
using aviutl2_latex::JapaneseSpacingMode;
using aviutl2_latex::LegacyJapaneseFontValues;

constexpr std::string_view kSuite = "FontConfigMigrationSmoke";

int fail(std::string_view check, std::string_view detail = {}) {
    return japanese_font_test::fail(kSuite, check, detail);
}

} // namespace

int main() {
    using namespace aviutl2_latex;

    if (parse_japanese_font_source(L"default") != JapaneseFontSource::Default ||
        parse_japanese_font_source(L"Default") != JapaneseFontSource::Default ||
        parse_japanese_font_source(L"font-name") !=
            JapaneseFontSource::InstalledFamily ||
        parse_japanese_font_source(L"InstalledFamily") !=
            JapaneseFontSource::InstalledFamily ||
        parse_japanese_font_source(L"font-file") !=
            JapaneseFontSource::FontFile ||
        parse_japanese_font_source(L"FontFile") !=
            JapaneseFontSource::FontFile) {
        return fail(
            "legacy source aliases",
            "a supported historical source representation was not parsed");
    }

    for (const std::wstring_view legacy_custom : {
             L"3", L"カスタム", L"custom", L"Custom"}) {
        if (resolve_japanese_spacing_mode(
                legacy_custom, JapaneseSpacingMode::Uniform) !=
            JapaneseSpacingMode::Auto) {
            return fail(
                "legacy Custom spacing fallback",
                "a removed Custom representation did not resolve to Auto");
        }
    }

    LegacyJapaneseFontValues legacy_name;
    legacy_name.fontspec_family_name = L"Meiryo";
    const auto inferred_name = resolve_japanese_font_config(
        std::nullopt, legacy_name);
    if (inferred_name.source != JapaneseFontSource::InstalledFamily ||
        inferred_name.fontspec_family_name != L"Meiryo" ||
        inferred_name.display_name != L"Meiryo" ||
        !inferred_name.needs_migration) {
        return fail(
            "missing legacy mode with family",
            "InstalledFamily was not inferred with its display and migration flag");
    }

    LegacyJapaneseFontValues legacy_file;
    legacy_file.file_path = std::filesystem::path(L"旧フォント.otf");
    const auto inferred_file = resolve_japanese_font_config(
        std::nullopt, legacy_file);
    if (inferred_file.source != JapaneseFontSource::FontFile ||
        inferred_file.file_path != *legacy_file.file_path ||
        inferred_file.display_name != L"旧フォント.otf" ||
        !inferred_file.needs_migration) {
        return fail(
            "missing legacy mode with file",
            "FontFile was not inferred with its filename and migration flag");
    }

    LegacyJapaneseFontValues both_values = legacy_name;
    both_values.file_path = std::filesystem::path(L"preferred-file.otf");
    const auto inferred_both = resolve_japanese_font_config(
        std::nullopt, both_values);
    if (inferred_both.source != JapaneseFontSource::FontFile ||
        inferred_both.file_path != *both_values.file_path ||
        !inferred_both.needs_migration) {
        return fail(
            "missing legacy mode with family and file",
            "the file source did not take deterministic precedence");
    }

    LegacyJapaneseFontValues explicit_default = legacy_name;
    explicit_default.source_value = L"既定";
    explicit_default.file_path = std::filesystem::path(L"stale.otf");
    const auto resolved_default = resolve_japanese_font_config(
        std::nullopt, explicit_default);
    if (resolved_default.source != JapaneseFontSource::Default ||
        resolved_default.display_name != L"既定" ||
        !resolved_default.needs_migration) {
        return fail(
            "explicit legacy Default",
            "stale family/file fields overrode the saved Default mode");
    }

    JapaneseFontConfig stale_default_display;
    stale_default_display.display_name = L"Yu Gothic";
    if (japanese_font_display_value(stale_default_display) != L"既定") {
        return fail(
            "Default display reconstruction",
            "a stale display-only family leaked into the Default UI value");
    }

    JapaneseFontConfig stale_installed_display;
    stale_installed_display.source = JapaneseFontSource::InstalledFamily;
    stale_installed_display.display_name = L"既定";
    stale_installed_display.fontspec_family_name = L"Yu Gothic";
    if (japanese_font_display_value(stale_installed_display) != L"Yu Gothic") {
        return fail(
            "InstalledFamily display reconstruction",
            "the family name did not replace a stale Default display value");
    }

    LegacyJapaneseFontValues invalid_mode = legacy_name;
    invalid_mode.source_value = L"unknown-mode";
    const auto invalid_resolved = resolve_japanese_font_config(
        std::nullopt, invalid_mode);
    if (invalid_resolved.source != JapaneseFontSource::Default ||
        invalid_resolved.display_name != L"既定" ||
        !invalid_resolved.needs_migration) {
        return fail(
            "invalid explicit legacy mode",
            "an unknown explicit mode incorrectly inferred a stale family");
    }

    LegacyJapaneseFontValues incomplete_legacy;
    incomplete_legacy.source_value = L"フォント名";
    const auto incomplete_resolved = resolve_japanese_font_config(
        std::nullopt, incomplete_legacy);
    if (incomplete_resolved.source != JapaneseFontSource::Default ||
        incomplete_resolved.display_name != L"既定" ||
        !incomplete_resolved.needs_migration) {
        return fail(
            "incomplete legacy InstalledFamily",
            "missing family data did not fall back to Default safely");
    }

    LegacyJapaneseFontValues explicit_display = legacy_name;
    explicit_display.source_value = L"フォント名";
    explicit_display.display_name = L"表示用 Meiryo";
    const auto preserved_display = resolve_japanese_font_config(
        std::nullopt, explicit_display);
    if (preserved_display.source != JapaneseFontSource::InstalledFamily ||
        preserved_display.display_name != L"表示用 Meiryo" ||
        !preserved_display.needs_migration) {
        return fail(
            "legacy display-name preservation",
            "a valid saved display name was not carried into hidden data");
    }

    JapaneseFontConfig current;
    current.source = JapaneseFontSource::InstalledFamily;
    current.display_name = L"Current Display";
    current.fontspec_family_name = L"Current Family";
    const auto preferred_current = resolve_japanese_font_config(
        current, legacy_name);
    if (preferred_current.source != JapaneseFontSource::InstalledFamily ||
        preferred_current.fontspec_family_name != L"Current Family" ||
        preferred_current.display_name != L"Current Display" ||
        preferred_current.needs_migration) {
        return fail(
            "current hidden-data priority",
            "valid current data was replaced by legacy compatibility fields");
    }

    JapaneseFontConfig current_without_display = current;
    current_without_display.display_name.clear();
    const auto reconstructed_current = resolve_japanese_font_config(
        current_without_display, legacy_name);
    if (reconstructed_current.display_name != L"Current Family" ||
        reconstructed_current.needs_migration) {
        return fail(
            "current hidden-data display reconstruction",
            "a missing display field was not rebuilt without marking migration");
    }

    JapaneseFontConfig invalid_current;
    invalid_current.source = JapaneseFontSource::InstalledFamily;
    const auto legacy_fallback = resolve_japanese_font_config(
        invalid_current, legacy_name);
    if (legacy_fallback.source != JapaneseFontSource::InstalledFamily ||
        legacy_fallback.fontspec_family_name != L"Meiryo" ||
        legacy_fallback.display_name != L"Meiryo" ||
        !legacy_fallback.needs_migration) {
        return fail(
            "invalid current-data fallback",
            "valid legacy data was not selected and marked for migration");
    }

    return 0;
}
