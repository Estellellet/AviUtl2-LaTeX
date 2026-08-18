#include "DocumentTemplateBuilder.h"
#include "JapaneseFontConfig.h"
#include "ProcessRunner.h"
#include "SdkValueCopy.h"
#include "StepParser.h"

#include <windows.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

constexpr DWORD kProcessTimeoutMs = 60'000;
constexpr std::uintmax_t kMaximumProcessOutputBytes =
    8ULL * 1024ULL * 1024ULL;

struct GeneratedCase {
    std::string name;
    std::wstring document;
};

bool fail(std::string_view message) {
    std::cerr << message << '\n';
    return false;
}

std::size_t count_occurrences(
    std::wstring_view text,
    std::wstring_view value) {
    if (value.empty()) {
        return 0;
    }
    std::size_t count = 0;
    std::size_t position = 0;
    while ((position = text.find(value, position)) != std::wstring_view::npos) {
        ++count;
        position += value.size();
    }
    return count;
}

bool contains_forbidden_font_configuration(std::wstring_view document) {
    return document.find(L"\\setmainfont") != std::wstring_view::npos ||
        document.find(L"\\setmathfont") != std::wstring_view::npos ||
        document.find(L"unicode-math") != std::wstring_view::npos;
}

bool extract_step_groups(
    std::wstring_view document,
    std::vector<std::wstring>& colors,
    std::vector<std::wstring>& bodies) {
    constexpr std::wstring_view marker = L"\\begingroup\\color{";
    constexpr std::wstring_view body_separator = L"}\n";
    constexpr std::wstring_view end_marker = L"\\endgroup";
    std::size_t position = 0;
    while ((position = document.find(marker, position)) !=
           std::wstring_view::npos) {
        const std::size_t color_begin = position + marker.size();
        const std::size_t body_begin_marker =
            document.find(body_separator, color_begin);
        if (body_begin_marker == std::wstring_view::npos) {
            return false;
        }
        const std::size_t body_begin =
            body_begin_marker + body_separator.size();
        const std::size_t body_end = document.find(end_marker, body_begin);
        if (body_end == std::wstring_view::npos) {
            return false;
        }
        colors.emplace_back(
            document.substr(color_begin, body_begin_marker - color_begin));
        bodies.emplace_back(document.substr(body_begin, body_end - body_begin));
        position = body_end + end_marker.size();
    }
    return !bodies.empty();
}

bool wrapper_body_preserves_source(
    std::wstring_view wrapped_body,
    std::wstring_view source) {
    if (!source.empty() && source.back() == L'\n') {
        return wrapped_body == source;
    }
    return wrapped_body.size() == source.size() + 1 &&
        wrapped_body.starts_with(source) && wrapped_body.back() == L'\n';
}

bool verify_generated_document(
    std::string_view case_name,
    std::wstring_view source,
    const aviutl2_latex::DocumentTemplateOptions& options,
    std::wstring& generated) {
    const auto parsed = step_parser::parse_latex_steps(source, 32);
    if (parsed.limit_exceeded || parsed.steps.size() != 1 ||
        parsed.steps.front() != source) {
        std::cerr << case_name
                  << ": ordinary document lines were changed by StepParser\n";
        return false;
    }

    generated = aviutl2_latex::build_document_step_layer_source(
        parsed.steps, 0, options);
    std::vector<std::wstring> colors;
    std::vector<std::wstring> bodies;
    if (!extract_step_groups(generated, colors, bodies) ||
        colors.size() != 1 || colors.front() != L"black" ||
        bodies.size() != 1 ||
        !wrapper_body_preserves_source(bodies.front(), source)) {
        std::cerr << case_name
                  << ": document layer did not preserve one whole source block\n";
        return false;
    }
    if (count_occurrences(generated, L"\\begingroup\\color{") != 1 ||
        count_occurrences(generated, L"\\endgroup") != 1) {
        std::cerr << case_name << ": color wrapper was applied per line\n";
        return false;
    }
    if (generated.find(L"\\text{あ}") != std::wstring::npos ||
        generated.find(L"\\par") != std::wstring::npos) {
        std::cerr << case_name << ": user text was synthetically rewritten\n";
        return false;
    }
    if (contains_forbidden_font_configuration(generated)) {
        std::cerr << case_name
                  << ": generated document changed Latin or math fonts\n";
        return false;
    }
    return true;
}

std::string to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    const int size = WideCharToMultiByte(
        CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            static_cast<int>(value.size()), result.data(), size,
            nullptr, nullptr) != size) {
        return {};
    }
    return result;
}

bool write_utf8_file(
    const std::filesystem::path& path,
    std::wstring_view contents) {
    const std::string utf8 = to_utf8(contents);
    if (!contents.empty() && utf8.empty()) {
        return false;
    }
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(utf8.data(), static_cast<std::streamsize>(utf8.size()));
    return output.good();
}

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
}

bool compile_document(
    const std::filesystem::path& lualatex,
    const std::filesystem::path& root,
    const GeneratedCase& test_case) {
    const std::filesystem::path case_directory = root / test_case.name;
    std::error_code error;
    std::filesystem::create_directories(case_directory, error);
    if (error) {
        std::cerr << test_case.name << ": cannot create work directory: "
                  << error.message() << '\n';
        return false;
    }

    const std::filesystem::path tex_path = case_directory / L"main.tex";
    const std::filesystem::path pdf_path = case_directory / L"main.pdf";
    const std::filesystem::path output_path =
        case_directory / L"lualatex-process.log";
    std::filesystem::remove(pdf_path, error);
    error.clear();
    if (!write_utf8_file(tex_path, test_case.document)) {
        std::cerr << test_case.name << ": cannot write UTF-8 main.tex\n";
        return false;
    }

    const ProcessResult result = run_process(
        lualatex,
        {
            L"--interaction=nonstopmode",
            L"--halt-on-error",
            L"--file-line-error",
            L"--no-shell-escape",
            L"--output-directory=" + case_directory.wstring(),
            tex_path.wstring()
        },
        case_directory,
        output_path,
        kProcessTimeoutMs,
        std::stop_token{},
        kMaximumProcessOutputBytes);
    if (!result.started || result.timed_out || result.cancelled ||
        result.exit_code != 0 ||
        !std::filesystem::is_regular_file(pdf_path, error) || error) {
        std::cerr << test_case.name << ": LuaLaTeX failed"
                  << " (started=" << (result.started ? "yes" : "no")
                  << ", timed_out=" << (result.timed_out ? "yes" : "no")
                  << ", exit_code=" << result.exit_code << ")\n";
        const std::string output = read_file(output_path);
        const std::size_t begin = output.size() > 8192
            ? output.size() - 8192
            : 0;
        std::cerr << output.substr(begin) << '\n';
        return false;
    }
    return true;
}

bool verify_step_layers(
    const aviutl2_latex::DocumentTemplateOptions& options,
    std::vector<GeneratedCase>& compile_cases) {
    constexpr std::wstring_view source =
        L"$a$\r\nあ\r\n%<step>\r\n$b$\r\nい";
    const auto parsed = step_parser::parse_latex_steps(source, 32);
    if (parsed.limit_exceeded || parsed.steps.size() != 2 ||
        parsed.steps[0] != L"$a$\r\nあ" ||
        parsed.steps[1] != L"$b$\r\nい") {
        return fail("CRLF step parsing changed ordinary line endings");
    }

    for (std::size_t target = 0; target < parsed.steps.size(); ++target) {
        std::wstring document =
            aviutl2_latex::build_document_step_layer_source(
                parsed.steps, target, options);
        std::vector<std::wstring> colors;
        std::vector<std::wstring> bodies;
        if (!extract_step_groups(document, colors, bodies) ||
            bodies.size() != parsed.steps.size() || colors.size() != 2 ||
            colors[target] != L"black" ||
            colors[1 - target] != L"white") {
            return fail("step layer did not preserve both complete source blocks");
        }
        for (std::size_t index = 0; index < bodies.size(); ++index) {
            if (!wrapper_body_preserves_source(bodies[index], parsed.steps[index])) {
                return fail("step layer changed a source block line ending");
            }
        }
        compile_cases.push_back({
            "step-layer-" + std::to_string(target + 1),
            std::move(document)
        });
    }
    return true;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 1 && argc != 3) {
        std::wcerr << L"usage: DocumentMixedLineBreakSmoke "
                      L"[<lualatex.exe> <work-directory>]\n";
        return 2;
    }

    // This also guards the test fixture itself: the source file must be built
    // with /utf-8 and the Japanese body must reach main.tex as UTF-8, not in
    // the active Windows code page.
    if (to_utf8(L"$a$\nあ") != std::string("$a$\n\xE3\x81\x82")) {
        std::cerr << "Japanese fixture was not encoded as UTF-8\n";
        return 3;
    }

    bool source_too_long = false;
    bool source_allocation_failed = false;
    const auto source_snapshot = sdk_value::copy_refreshed_text_item(
        L"$a$\nあ", 1024, &source_too_long, &source_allocation_failed);
    if (!source_snapshot || *source_snapshot != L"$a$\nあ" ||
        *source_snapshot == L"$a$\\nあ" || source_too_long ||
        source_allocation_failed) {
        std::cerr << "raw FILTER_ITEM_TEXT snapshot changed multiline source\n";
        return 6;
    }

    aviutl2_latex::JapaneseFontConfig default_font;
    const auto japanese =
        aviutl2_latex::build_document_japanese_font_preamble(
            true,
            default_font,
            aviutl2_latex::JapaneseSpacingMode::Auto);
    if (!japanese.valid() ||
        japanese.preamble.find(L"\\usepackage[no-math]{fontspec}") ==
            std::wstring::npos ||
        japanese.preamble.find(L"\\usepackage{luatexja}") ==
            std::wstring::npos ||
        contains_forbidden_font_configuration(japanese.preamble)) {
        std::cerr << "Japanese preamble no longer isolates Latin/math fonts\n";
        return 4;
    }

    aviutl2_latex::JapaneseFontConfig installed_font;
    installed_font.source =
        aviutl2_latex::JapaneseFontSource::InstalledFamily;
    installed_font.fontspec_family_name = L"Yu Gothic";
    const auto installed_japanese =
        aviutl2_latex::build_document_japanese_font_preamble(
            true,
            installed_font,
            aviutl2_latex::JapaneseSpacingMode::Auto);
    if (!installed_japanese.valid() ||
        installed_japanese.preamble.find(L"\\setmainjfont") ==
            std::wstring::npos ||
        contains_forbidden_font_configuration(installed_japanese.preamble)) {
        std::cerr << "configured Japanese font leaked into Latin/math setup\n";
        return 5;
    }

    aviutl2_latex::DocumentTemplateOptions options;
    options.additional_preamble = japanese.preamble;
    const std::vector<std::pair<std::string, std::wstring>> fixtures{
        {"A-math-then-japanese", L"$a$\nあ"},
        {"B-japanese-then-math", L"あ\n$a$"},
        {"C-blank-line", L"$a$\n\nあ"},
        {"D-three-lines", L"あいうえお\n$a+b=c$\nかきくけこ"},
        {"E-display-math",
            L"これは日本語です。\n\n\\[\n"
            L"  \\lim_{x\\to0}\\frac{\\sin x}{x}=1\n"
            L"\\]\n\n次の段落です。"},
        {"F-inline-after", L"$a$ あ"},
        {"G-inline-before", L"あ $a$"},
        {"H-latin-between", L"$a$\nABC\nあ"},
        {"LF-explicit", L"$a$\nあ"},
        {"CRLF-explicit", L"$a$\r\nあ"},
        {"LF-final", L"$a$\nあ\n"},
        {"CRLF-final", L"$a$\r\nあ\r\n"}
    };

    std::vector<GeneratedCase> compile_cases;
    for (const auto& [name, source] : fixtures) {
        std::wstring generated;
        if (!verify_generated_document(name, source, options, generated)) {
            return 10;
        }
        if (name == "A-math-then-japanese" &&
            generated.find(L"$a$\\nあ") != std::wstring::npos) {
            fail("an alias-format \\n escape leaked into main.tex");
            return 11;
        }
        compile_cases.push_back({name, std::move(generated)});
    }

    const std::vector<std::pair<std::string, std::wstring>> math_modes{
        {"math-dollar", L"前\n$x+1$\n後"},
        {"math-parentheses", L"前\n\\(x+1\\)\n後"},
        {"math-brackets", L"前\n\\[x+1\\]\n後"},
        {"math-double-dollar", L"前\n$$x+1$$\n後"},
        {"math-equation", L"前\n\\begin{equation}x+1\\end{equation}\n後"},
        {"math-equation-star",
            L"前\n\\begin{equation*}x+1\\end{equation*}\n後"},
        {"math-align",
            L"前\n\\begin{align}x&=1\\\\y&=2\\end{align}\n後"},
        {"math-align-star",
            L"前\n\\begin{align*}x&=1\\\\y&=2\\end{align*}\n後"},
        {"math-aligned",
            L"前\n\\[\\begin{aligned}x&=1\\\\y&=2\\end{aligned}\\]\n後"}
    };
    for (const auto& [name, source] : math_modes) {
        std::wstring generated;
        if (!verify_generated_document(name, source, options, generated)) {
            return 12;
        }
        compile_cases.push_back({name, std::move(generated)});
    }

    const std::vector<std::pair<std::string, std::wstring>> alignments{
        {"left", L"\\RaggedRight"},
        {"justify", L"\\justifying"},
        {"center", L"\\Centering"},
        {"right", L"\\RaggedLeft"}
    };
    for (const auto& [name, command] : alignments) {
        auto minipage_options = options;
        minipage_options.minipage_enabled = true;
        minipage_options.formatted_minipage_width_cm = L"10.0";
        minipage_options.paragraph_alignment_command = command;
        std::wstring generated;
        if (!verify_generated_document(
                "minipage-" + name,
                L"$a$\nあ",
                minipage_options,
                generated) ||
            generated.find(L"\\usepackage{ragged2e}") ==
                std::wstring::npos ||
            generated.find(L"\\begin{minipage}{10.0cm}\n" + command +
                           L"\n\\begingroup") == std::wstring::npos ||
            count_occurrences(generated, L"\\begin{minipage}") != 1 ||
            count_occurrences(generated, L"\\end{minipage}") != 1) {
            std::cerr << "minipage-" << name
                      << ": wrapper or alignment command mismatch\n";
            return 13;
        }
        compile_cases.push_back({"minipage-" + name, std::move(generated)});
    }
    if (!verify_step_layers(options, compile_cases)) {
        return 14;
    }

    if (argc == 3) {
        const std::filesystem::path lualatex(argv[1]);
        if (!std::filesystem::is_regular_file(lualatex)) {
            std::wcerr << L"LuaLaTeX executable does not exist: "
                       << lualatex.wstring() << L'\n';
            return 20;
        }
        const std::filesystem::path root =
            std::filesystem::path(argv[2]) /
            (L"document-mixed-line-break-" +
                std::to_wstring(GetCurrentProcessId()));
        std::error_code error;
        std::filesystem::create_directories(root, error);
        if (error) {
            std::cerr << "cannot create integration test root: "
                      << error.message() << '\n';
            return 21;
        }
        for (const auto& test_case : compile_cases) {
            if (!compile_document(lualatex, root, test_case)) {
                return 22;
            }
        }
    }

    std::cout << "document mixed line breaks, step layers, minipage wrappers, "
                 "and Japanese font isolation passed\n";
    return 0;
}
