#include "JapaneseFontConfig.h"
#include "JapaneseFontConfigTestSupport.h"

#include <filesystem>
#include <string>
#include <string_view>

namespace {

using aviutl2_latex::DocumentJapaneseFontPreamble;
using aviutl2_latex::JapaneseFontConfig;
using aviutl2_latex::JapaneseFontSource;
using aviutl2_latex::JapaneseSpacingMode;
using japanese_font_test::contains;

constexpr std::string_view kSuite = "JapaneseFontConfigSmoke";

int fail(std::string_view check, std::string_view detail = {}) {
    return japanese_font_test::fail(kSuite, check, detail);
}

bool excludes_forbidden_font_commands(
    const DocumentJapaneseFontPreamble& value) {
    return !contains(value.preamble, L"\\setmainfont") &&
        !contains(value.preamble, L"\\setmathfont") &&
        !contains(value.preamble, L"\\setsansfont") &&
        !contains(value.preamble, L"\\setmonofont") &&
        !contains(value.preamble, L"unicode-math");
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace aviutl2_latex;

    std::filesystem::path fixture_base;
    if (!japanese_font_test::read_explicit_fixture_base(
            argc, argv, kSuite, fixture_base)) {
        return 2;
    }

    if (parse_japanese_font_source(L"既定") != JapaneseFontSource::Default ||
        parse_japanese_font_source(L"0") != JapaneseFontSource::Default ||
        parse_japanese_font_source(L"フォント名") !=
            JapaneseFontSource::InstalledFamily ||
        parse_japanese_font_source(L"1") !=
            JapaneseFontSource::InstalledFamily ||
        parse_japanese_font_source(L"フォントファイル") !=
            JapaneseFontSource::FontFile ||
        parse_japanese_font_source(L"2") != JapaneseFontSource::FontFile ||
        parse_japanese_font_source(L"unknown").has_value()) {
        return fail(
            "current font-source parsing",
            "Default, InstalledFamily, FontFile, or invalid input was misparsed");
    }

    if (parse_japanese_spacing_mode(L"自動") != JapaneseSpacingMode::Auto ||
        parse_japanese_spacing_mode(L"0") != JapaneseSpacingMode::Auto ||
        parse_japanese_spacing_mode(L"均等") != JapaneseSpacingMode::Uniform ||
        parse_japanese_spacing_mode(L"1") != JapaneseSpacingMode::Uniform ||
        parse_japanese_spacing_mode(L"フォント準拠") !=
            JapaneseSpacingMode::FontMetrics ||
        parse_japanese_spacing_mode(L"2") !=
            JapaneseSpacingMode::FontMetrics ||
        parse_japanese_spacing_mode(L"unknown").has_value()) {
        return fail(
            "current spacing-mode parsing",
            "Auto, Uniform, FontMetrics, or invalid input was misparsed");
    }

    JapaneseFontConfig installed;
    installed.source = JapaneseFontSource::InstalledFamily;
    installed.display_name = L"表示専用名";
    installed.fontspec_family_name = L"Yu Gothic";

    const auto automatic = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::Auto);
    if (!automatic.valid()) {
        return fail("installed Auto preamble", "builder returned an error");
    }
    if (!contains(automatic.preamble, L"\\usepackage[no-math]{fontspec}") ||
        !contains(automatic.preamble, L"\\usepackage{luatexja}") ||
        !contains(automatic.preamble, L"\\usepackage{luatexja-fontspec}")) {
        return fail(
            "installed Auto preamble",
            "required Japanese packages or fontspec no-math are missing");
    }
    if (!contains(
            automatic.generated_setmainjfont,
            L"YokoFeatures={JFM=propw}") ||
        !contains(automatic.generated_setmainjfont, L"Kerning=On") ||
        !contains(
            automatic.generated_setmainjfont,
            L"RawFeature={+palt,+kern}") ||
        !contains(automatic.generated_setmainjfont, L"{Yu Gothic}")) {
        return fail(
            "installed Auto preamble",
            "FontMetrics setmainjfont options are incomplete");
    }
    if (!excludes_forbidden_font_commands(automatic)) {
        return fail(
            "installed Auto preamble",
            "Japanese configuration changed Latin or math fonts");
    }

    const auto uniform = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::Uniform);
    if (!uniform.valid() ||
        !contains(uniform.generated_setmainjfont, L"YokoFeatures={JFM=ujis}") ||
        !contains(uniform.generated_setmainjfont, L"Kerning=Off") ||
        contains(uniform.generated_setmainjfont, L"RawFeature") ||
        !excludes_forbidden_font_commands(uniform)) {
        return fail(
            "installed Uniform preamble",
            "Uniform JFM/Kerning options or font isolation are incorrect");
    }

    const auto metrics = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::FontMetrics);
    if (!metrics.valid() ||
        automatic.generated_setmainjfont != metrics.generated_setmainjfont) {
        return fail(
            "installed explicit FontMetrics preamble",
            "Auto with an installed font did not resolve to FontMetrics");
    }

    JapaneseFontConfig default_font;
    const auto default_result = build_document_japanese_font_preamble(
        true, default_font, JapaneseSpacingMode::Uniform);
    if (!default_result.valid() ||
        !default_result.generated_setmainjfont.empty() ||
        !contains(default_result.preamble, L"\\usepackage{luatexja}") ||
        !excludes_forbidden_font_commands(default_result)) {
        return fail(
            "default Japanese font preamble",
            "the default font was reset or required Japanese support is missing");
    }

    const auto disabled = build_document_japanese_font_preamble(
        false, installed, JapaneseSpacingMode::FontMetrics);
    if (!disabled.valid() || !disabled.preamble.empty() ||
        !disabled.generated_setmainjfont.empty()) {
        return fail(
            "Japanese-disabled preamble",
            "font commands were generated while Japanese support was disabled");
    }

    if (japanese_font_display_value(installed) != L"表示専用名") {
        return fail(
            "installed font display value",
            "the explicit display-only name was not returned");
    }

    japanese_font_test::ScopedFixtureDirectory fixtures(
        fixture_base, L"JapaneseFontConfigSmoke");
    if (!fixtures.valid()) {
        return fail("fixture directory", fixtures.error_message());
    }

    const std::string otf_fixture = "font-identity";
    const std::filesystem::path font_path =
        fixtures.path() / L"日本語 Font (A)[B].OTF";
    std::string fixture_error;
    if (!japanese_font_test::write_binary_fixture(
            font_path, otf_fixture, fixture_error)) {
        return fail("OTF fixture creation", fixture_error);
    }

    JapaneseFontConfig file_font;
    file_font.source = JapaneseFontSource::FontFile;
    file_font.display_name = L"表示だけ";
    file_font.file_path = font_path;
    const auto file_result = build_document_japanese_font_preamble(
        true, file_font, JapaneseSpacingMode::FontMetrics);
    if (!file_result.valid()) {
        return fail(
            "font-file preamble",
            "valid=false, error_message=" +
                japanese_font_test::to_utf8(file_result.error_message));
    }
    if (!contains(file_result.generated_setmainjfont, L"Path={") ||
        !contains(
            file_result.generated_setmainjfont,
            L"日本語 Font (A)[B].OTF") ||
        !contains(
            file_result.generated_setmainjfont,
            L"YokoFeatures={JFM=propw}") ||
        !excludes_forbidden_font_commands(file_result)) {
        return fail(
            "font-file preamble",
            "normalized path, FontMetrics options, or font isolation is incorrect");
    }
    if (japanese_font_display_value(file_font) != L"表示だけ") {
        return fail(
            "font-file display value",
            "the explicit display-only name was not returned");
    }

    for (const wchar_t* extension : {L".ttf", L".ttc"}) {
        const std::filesystem::path candidate = fixtures.path() /
            (std::wstring(L"日本語 font fixture") + extension);
        fixture_error.clear();
        if (!japanese_font_test::write_binary_fixture(
                candidate, "x", fixture_error)) {
            return fail("TTF/TTC fixture creation", fixture_error);
        }
        JapaneseFontConfig extension_font = file_font;
        extension_font.file_path = candidate;
        const auto extension_result = build_document_japanese_font_preamble(
            true, extension_font, JapaneseSpacingMode::Auto);
        if (!extension_result.valid()) {
            std::string detail = extension == std::wstring_view(L".ttf")
                ? "expected TTF validity=true; actual validity=false; error="
                : "expected TTC validity=true; actual validity=false; error=";
            detail += japanese_font_test::to_utf8(
                extension_result.error_message);
            return fail("supported font-file extension", detail);
        }
    }

    const std::filesystem::path unsupported_path =
        fixtures.path() / L"font.woff";
    fixture_error.clear();
    if (!japanese_font_test::write_binary_fixture(
            unsupported_path, "x", fixture_error)) {
        return fail("unsupported fixture creation", fixture_error);
    }
    JapaneseFontConfig unsupported = file_font;
    unsupported.file_path = unsupported_path;
    if (build_document_japanese_font_preamble(
            true, unsupported, JapaneseSpacingMode::Auto).valid()) {
        return fail(
            "unsupported font-file extension",
            "WOFF was accepted as a Japanese font file");
    }

    JapaneseFontConfig missing = file_font;
    missing.file_path = fixtures.path() / L"missing.otf";
    if (build_document_japanese_font_preamble(
            true, missing, JapaneseSpacingMode::Auto).valid()) {
        return fail("missing font file", "a nonexistent OTF was accepted");
    }

    JapaneseFontConfig unsafe_name = installed;
    unsafe_name.fontspec_family_name = L"Bad#Font";
    if (build_document_japanese_font_preamble(
            true, unsafe_name, JapaneseSpacingMode::Auto).valid()) {
        return fail(
            "unsafe installed-family name",
            "a TeX metacharacter was accepted in the family name");
    }

    JapaneseFontConfig unsafe_path = file_font;
    unsafe_path.file_path = fixtures.path() / L"Bad#Font.otf";
    if (build_document_japanese_font_preamble(
            true, unsafe_path, JapaneseSpacingMode::Auto).valid()) {
        return fail(
            "unsafe font-file path",
            "a TeX metacharacter was accepted in the file path");
    }

    return 0;
}
