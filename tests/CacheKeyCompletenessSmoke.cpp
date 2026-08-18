#include "JapaneseFontConfig.h"
#include "JapaneseFontConfigTestSupport.h"

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace {

using aviutl2_latex::DocumentJapaneseFontPreamble;
using aviutl2_latex::JapaneseFontConfig;
using aviutl2_latex::JapaneseFontSource;
using aviutl2_latex::JapaneseSpacingMode;
using japanese_font_test::contains;

constexpr std::string_view kSuite = "CacheKeyCompletenessSmoke";

int fail(std::string_view check, std::string_view detail = {}) {
    return japanese_font_test::fail(kSuite, check, detail);
}

bool require_cache_tokens(
    std::string_view check,
    const DocumentJapaneseFontPreamble& result,
    const std::vector<std::wstring_view>& required_tokens) {
    for (const std::wstring_view token : required_tokens) {
        if (!contains(result.cache_material, token)) {
            std::cerr << '[' << kSuite << "] " << check
                      << " failed: required cache token is missing: "
                      << japanese_font_test::to_utf8(token) << '\n';
            return false;
        }
    }
    return true;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    using namespace aviutl2_latex;

    std::filesystem::path fixture_base;
    if (!japanese_font_test::read_explicit_fixture_base(
            argc, argv, kSuite, fixture_base)) {
        return 2;
    }

    JapaneseFontConfig installed;
    installed.source = JapaneseFontSource::InstalledFamily;
    installed.display_name = L"表示専用名";
    installed.fontspec_family_name = L"Yu Gothic";

    const auto automatic = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::Auto);
    if (!automatic.valid()) {
        return fail("installed Auto cache material", "preamble generation failed");
    }
    if (!require_cache_tokens(
            "installed Auto cache material",
            automatic,
            {
                L"japanese_enabled=1",
                L"japanese-font-source=installed-family",
                L"japanese-spacing-requested=auto",
                L"japanese-spacing-effective=font-metrics",
                L"japanese-jfm=propw",
                L"japanese-kerning=on",
                L"japanese-palt=on",
                L"japanese-kern-feature=on",
                L"fontspec-family-name=Yu Gothic"
            })) {
        return 1;
    }

    JapaneseFontConfig renamed_display = installed;
    renamed_display.display_name = L"別の表示名";
    const auto renamed_result = build_document_japanese_font_preamble(
        true, renamed_display, JapaneseSpacingMode::Auto);
    if (!renamed_result.valid() ||
        automatic.cache_material != renamed_result.cache_material ||
        contains(automatic.cache_material, installed.display_name) ||
        contains(renamed_result.cache_material, renamed_display.display_name)) {
        return fail(
            "display-only installed name exclusion",
            "changing a UI-only name changed or leaked into cache material");
    }

    const auto uniform = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::Uniform);
    if (!uniform.valid() ||
        !require_cache_tokens(
            "installed Uniform cache material",
            uniform,
            {
                L"japanese-spacing-requested=uniform",
                L"japanese-spacing-effective=uniform",
                L"japanese-jfm=ujis",
                L"japanese-kerning=off",
                L"japanese-palt=off",
                L"japanese-kern-feature=off"
            }) ||
        uniform.cache_material == automatic.cache_material) {
        return fail(
            "installed Uniform cache material",
            "Uniform semantics were incomplete or collided with Auto");
    }

    const auto metrics = build_document_japanese_font_preamble(
        true, installed, JapaneseSpacingMode::FontMetrics);
    if (!metrics.valid() ||
        !require_cache_tokens(
            "installed explicit FontMetrics cache material",
            metrics,
            {
                L"japanese-spacing-requested=font-metrics",
                L"japanese-spacing-effective=font-metrics",
                L"japanese-jfm=propw",
                L"japanese-kerning=on"
            }) ||
        metrics.cache_material == automatic.cache_material) {
        return fail(
            "installed explicit FontMetrics cache material",
            "requested mode was omitted or collided with Auto");
    }

    JapaneseFontConfig different_family = installed;
    different_family.fontspec_family_name = L"Meiryo";
    const auto different_family_result = build_document_japanese_font_preamble(
        true, different_family, JapaneseSpacingMode::Auto);
    if (!different_family_result.valid() ||
        !contains(
            different_family_result.cache_material,
            L"fontspec-family-name=Meiryo") ||
        different_family_result.cache_material == automatic.cache_material) {
        return fail(
            "installed family cache identity",
            "different fontspec family names produced the same cache material");
    }

    JapaneseFontConfig default_font;
    const auto default_result = build_document_japanese_font_preamble(
        true, default_font, JapaneseSpacingMode::Uniform);
    if (!default_result.valid() ||
        !require_cache_tokens(
            "default-font cache material",
            default_result,
            {
                L"japanese_enabled=1",
                L"japanese-font-source=default",
                L"japanese-spacing-requested=uniform",
                L"japanese-spacing-effective=default",
                L"japanese-jfm=default",
                L"japanese-kerning=default"
            }) ||
        default_result.cache_material == automatic.cache_material) {
        return fail(
            "default-font cache material",
            "Default font semantics were incomplete or collided with installed font");
    }

    const auto disabled = build_document_japanese_font_preamble(
        false, installed, JapaneseSpacingMode::Auto);
    if (!disabled.valid() || disabled.cache_material != L"japanese_enabled=0") {
        return fail(
            "Japanese-disabled cache material",
            "disabled output contains irrelevant font-dependent state");
    }

    japanese_font_test::ScopedFixtureDirectory fixtures(
        fixture_base, L"CacheKeyCompletenessSmoke");
    if (!fixtures.valid()) {
        return fail("fixture directory", fixtures.error_message());
    }

    const std::filesystem::path font_path =
        fixtures.path() / L"日本語 Font (A)[B].OTF";
    const std::string fixture = "font-identity";
    std::string fixture_error;
    if (!japanese_font_test::write_binary_fixture(
            font_path, fixture, fixture_error)) {
        return fail("initial font fixture", fixture_error);
    }

    JapaneseFontConfig file_font;
    file_font.source = JapaneseFontSource::FontFile;
    file_font.display_name = L"表示だけ";
    file_font.file_path = font_path;
    const auto initial_file_result = build_document_japanese_font_preamble(
        true, file_font, JapaneseSpacingMode::FontMetrics);
    if (!initial_file_result.valid()) {
        return fail(
            "font-file cache material",
            "expected validity=true; actual validity=false; error=" +
                japanese_font_test::to_utf8(
                    initial_file_result.error_message));
    }
    const std::wstring initial_size_token =
        L"font-file-size=" + std::to_wstring(fixture.size());
    if (initial_file_result.normalized_font_filename !=
            L"日本語 Font (A)[B].OTF" ||
        initial_file_result.normalized_font_directory.empty() ||
        initial_file_result.normalized_font_directory.back() != L'/' ||
        contains(initial_file_result.normalized_font_directory, L"\\")) {
        return fail(
            "normalized font-file path",
            "expected a native wide path normalized to forward-slash TeX form");
    }
    const std::wstring normalized_identity_token =
        L"normalized-font-file=" +
        initial_file_result.normalized_font_directory +
        initial_file_result.normalized_font_filename;
    if (!require_cache_tokens(
            "font-file cache material",
            initial_file_result,
            {
                L"japanese-font-source=font-file",
                L"japanese-spacing-requested=font-metrics",
                L"japanese-spacing-effective=font-metrics",
                normalized_identity_token,
                initial_size_token,
                L"font-file-last-write-time="
            }) ||
        contains(initial_file_result.cache_material, file_font.display_name)) {
        return fail(
            "font-file cache material",
            "path/size/time semantics are incomplete or a display-only name leaked");
    }

    JapaneseFontConfig renamed_file_display = file_font;
    renamed_file_display.display_name = L"別のファイル表示名";
    const auto renamed_file_result = build_document_japanese_font_preamble(
        true, renamed_file_display, JapaneseSpacingMode::FontMetrics);
    if (!renamed_file_result.valid() ||
        renamed_file_result.cache_material != initial_file_result.cache_material ||
        contains(
            renamed_file_result.cache_material,
            renamed_file_display.display_name)) {
        return fail(
            "display-only file name exclusion",
            "changing a file UI label changed or leaked into cache material");
    }

    const std::string changed_fixture = fixture + "-changed-and-longer";
    fixture_error.clear();
    if (!japanese_font_test::write_binary_fixture(
            font_path, changed_fixture, fixture_error)) {
        return fail("changed font fixture", fixture_error);
    }
    const auto changed_file_result = build_document_japanese_font_preamble(
        true, file_font, JapaneseSpacingMode::FontMetrics);
    const std::wstring changed_size_token =
        L"font-file-size=" + std::to_wstring(changed_fixture.size());
    if (!changed_file_result.valid() ||
        !contains(changed_file_result.cache_material, changed_size_token) ||
        changed_file_result.cache_material == initial_file_result.cache_material) {
        return fail(
            "font-file content identity",
            "a content/size change did not change cache material independently of mtime");
    }

    const std::filesystem::path other_font_path =
        fixtures.path() / L"別フォルダ名相当 Font.OTF";
    fixture_error.clear();
    if (!japanese_font_test::write_binary_fixture(
            other_font_path, changed_fixture, fixture_error)) {
        return fail("alternate font fixture", fixture_error);
    }
    JapaneseFontConfig other_file_font = file_font;
    other_file_font.file_path = other_font_path;
    const auto other_file_result = build_document_japanese_font_preamble(
        true, other_file_font, JapaneseSpacingMode::FontMetrics);
    if (!other_file_result.valid() ||
        other_file_result.cache_material == changed_file_result.cache_material) {
        return fail(
            "normalized font-file path identity",
            "different normalized paths with equal contents produced the same material");
    }

    return 0;
}
