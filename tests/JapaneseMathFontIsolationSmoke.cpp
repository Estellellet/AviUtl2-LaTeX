#include "JapaneseFontConfig.h"
#include "ProcessRunner.h"

#include <windows.h>

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr DWORD kProcessTimeoutMs = 60'000;
constexpr std::uintmax_t kMaximumProcessOutputBytes = 8ULL * 1024ULL * 1024ULL;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return {};
    }
    return std::string(
        std::istreambuf_iterator<char>(input),
        std::istreambuf_iterator<char>());
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
    std::string_view contents) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    return output.good();
}

std::string output_tail(std::string value, std::size_t maximum_bytes = 4096) {
    if (value.size() > maximum_bytes) {
        value.erase(0, value.size() - maximum_bytes);
        value.insert(0, "...[output truncated]...\n");
    }
    return value;
}

void report_process_failure(
    std::string_view case_name,
    std::string_view stage,
    const ProcessResult& result,
    const std::filesystem::path& output_path) {
    std::cerr << case_name << ": " << stage << " failed"
              << " (started=" << (result.started ? "yes" : "no")
              << ", timed_out=" << (result.timed_out ? "yes" : "no")
              << ", cancelled=" << (result.cancelled ? "yes" : "no")
              << ", exit_code=" << result.exit_code << ")\n";
    if (!result.error.empty()) {
        std::wcerr << L"process error: " << result.error << L'\n';
    }
    std::cerr << "process output: " << output_path.string() << '\n'
              << output_tail(read_file(output_path)) << '\n';
}

std::string make_document(
    std::string_view additional_preamble,
    std::string_view body) {
    std::string document =
        "\\documentclass{article}\n"
        "\\usepackage{amsmath}\n"
        "\\pagestyle{empty}\n";
    document.append(additional_preamble);
    document += "\\begin{document}\n";
    document.append(body);
    document += "\n\\end{document}\n";
    return document;
}

bool compile_document(
    const std::filesystem::path& lualatex,
    const std::filesystem::path& root,
    std::string_view case_name,
    std::string_view document,
    std::filesystem::path& pdf_path) {
    const std::filesystem::path case_directory = root / case_name;
    std::error_code error;
    std::filesystem::create_directories(case_directory, error);
    if (error) {
        std::cerr << case_name << ": failed to create work directory: "
                  << error.message() << '\n';
        return false;
    }

    const std::filesystem::path tex_path = case_directory / L"main.tex";
    pdf_path = case_directory / L"main.pdf";
    const std::filesystem::path output_path =
        case_directory / L"lualatex-process.log";
    std::filesystem::remove(pdf_path, error);
    error.clear();
    if (!write_utf8_file(tex_path, document)) {
        std::cerr << case_name << ": failed to write UTF-8 main.tex\n";
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
        report_process_failure(case_name, "LuaLaTeX", result, output_path);
        return false;
    }
    return true;
}

std::string normalize_subset_font_name(std::string value) {
    if (value.size() > 7 && value[6] == '+' &&
        std::all_of(value.begin(), value.begin() + 6, [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z';
        })) {
        value.erase(0, 7);
    }
    return value;
}

std::set<std::string> parse_mutool_font_names(std::string_view output) {
    std::set<std::string> fonts;
    std::istringstream lines{std::string(output)};
    std::string line;
    bool in_font_section = false;
    while (std::getline(lines, line)) {
        if (line.find("Fonts (") != std::string::npos) {
            in_font_section = true;
            continue;
        }
        if (!in_font_section) {
            continue;
        }

        const std::size_t single_quote = line.find('\'');
        const std::size_t double_quote = line.find('"');
        const std::size_t first_quote = single_quote == std::string::npos
            ? double_quote
            : (double_quote == std::string::npos
                ? single_quote
                : (std::min)(single_quote, double_quote));
        const char quote = first_quote == std::string::npos
            ? '\0'
            : line[first_quote];
        const std::size_t second_quote = first_quote == std::string::npos
            ? std::string::npos
            : line.find(quote, first_quote + 1);
        if (first_quote != std::string::npos &&
            second_quote != std::string::npos && second_quote > first_quote + 1) {
            fonts.insert(normalize_subset_font_name(
                line.substr(first_quote + 1, second_quote - first_quote - 1)));
        }
    }
    return fonts;
}

bool inspect_fonts(
    const std::filesystem::path& mutool,
    std::string_view case_name,
    const std::filesystem::path& pdf_path,
    std::set<std::string>& fonts) {
    const std::filesystem::path output_path =
        pdf_path.parent_path() / L"mutool-fonts.log";
    const ProcessResult result = run_process(
        mutool,
        {L"info", L"-F", pdf_path.wstring()},
        pdf_path.parent_path(),
        output_path,
        kProcessTimeoutMs,
        std::stop_token{},
        kMaximumProcessOutputBytes);
    if (!result.started || result.timed_out || result.cancelled ||
        result.exit_code != 0) {
        report_process_failure(case_name, "mutool info -F", result, output_path);
        return false;
    }

    fonts = parse_mutool_font_names(read_file(output_path));
    if (fonts.empty()) {
        std::cerr << case_name
                  << ": mutool succeeded but no embedded font names were parsed\n"
                  << output_tail(read_file(output_path)) << '\n';
        return false;
    }
    return true;
}

std::string canonical_font_name(std::string_view value) {
    std::string result;
    for (const unsigned char byte : value) {
        if (std::isalnum(byte) != 0) {
            result.push_back(static_cast<char>(std::tolower(byte)));
        }
    }
    return result;
}

bool contains_yu_gothic(const std::set<std::string>& fonts) {
    return std::any_of(fonts.begin(), fonts.end(), [](const std::string& font) {
        return canonical_font_name(font).find("yugothic") != std::string::npos;
    });
}

bool contains_font_fragment(
    const std::set<std::string>& fonts,
    std::string_view fragment) {
    const std::string expected = canonical_font_name(fragment);
    return !expected.empty() && std::any_of(
        fonts.begin(), fonts.end(), [&](const std::string& font) {
            return canonical_font_name(font).find(expected) != std::string::npos;
        });
}

bool contains_all_fonts(
    const std::set<std::string>& actual,
    const std::set<std::string>& expected) {
    return std::all_of(expected.begin(), expected.end(),
        [&](const std::string& font) { return actual.contains(font); });
}

std::optional<std::string> build_japanese_preamble(
    const aviutl2_latex::JapaneseFontConfig& font) {
    const auto generated =
        aviutl2_latex::build_document_japanese_font_preamble(
            true,
            font,
            aviutl2_latex::JapaneseSpacingMode::Auto);
    if (!generated.valid()) {
        std::wcerr << L"Japanese font preamble failed: "
                   << generated.error_message << L'\n';
        return std::nullopt;
    }
    return to_utf8(generated.preamble);
}

void print_fonts(std::string_view label, const std::set<std::string>& fonts) {
    std::cerr << label << ':';
    for (const auto& font : fonts) {
        std::cerr << " [" << font << ']';
    }
    std::cerr << '\n';
}

bool compile_and_inspect(
    const std::filesystem::path& lualatex,
    const std::filesystem::path& mutool,
    const std::filesystem::path& root,
    std::string_view case_name,
    std::string_view document,
    std::set<std::string>& fonts,
    std::filesystem::path* compiled_pdf = nullptr) {
    std::filesystem::path pdf_path;
    if (!compile_document(lualatex, root, case_name, document, pdf_path) ||
        !inspect_fonts(mutool, case_name, pdf_path, fonts)) {
        return false;
    }
    if (compiled_pdf != nullptr) {
        *compiled_pdf = pdf_path;
    }
    return true;
}

// Extracted text is inspected below for glyph-level font attribution.
std::string font_for_stext_character(
    std::string_view output,
    std::string_view character) {
    const std::string marker = R"(<char c=")" +
        std::string(character) + R"(")";
    const std::size_t character_at = output.find(marker);
    const std::size_t font_at = character_at == std::string_view::npos
        ? std::string_view::npos
        : output.rfind(R"(<font name=")", character_at);
    if (font_at == std::string_view::npos) return {};
    const std::size_t begin = font_at + 12;
    const std::size_t end = output.find(char{34}, begin);
    return end == std::string_view::npos
        ? std::string{}
        : std::string(output.substr(begin, end - begin));
}

bool inspect_stext_character_font(
    const std::filesystem::path& mutool,
    std::string_view case_name,
    const std::filesystem::path& pdf_path,
    std::string_view character,
    std::string& font) {
    const std::filesystem::path output_path =
        pdf_path.parent_path() / L"mutool-stext.log";
    const ProcessResult result = run_process(
        mutool, {L"draw", L"-F", L"stext", pdf_path.wstring()},
        pdf_path.parent_path(), output_path, kProcessTimeoutMs,
        std::stop_token{}, kMaximumProcessOutputBytes);
    if (!result.started || result.timed_out || result.cancelled ||
        result.exit_code != 0) {
        report_process_failure(
            case_name, "mutool draw -F stext", result, output_path);
        return false;
    }
    font = font_for_stext_character(read_file(output_path), character);
    if (font.empty()) {
        std::cerr << case_name << ": character '" << character
                  << "' was not found in mutool structured text\n";
        return false;
    }
    return true;
}

bool inspect_stext_font_assignment(
    const std::filesystem::path& mutool,
    const std::filesystem::path& pdf_path) {
    const std::filesystem::path output_path =
        pdf_path.parent_path() / L"mutool-stext.log";
    const ProcessResult result = run_process(
        mutool, {L"draw", L"-F", L"stext", pdf_path.wstring()},
        pdf_path.parent_path(), output_path, kProcessTimeoutMs,
        std::stop_token{}, kMaximumProcessOutputBytes);
    if (!result.started || result.timed_out || result.cancelled ||
        result.exit_code != 0) {
        report_process_failure(
            "math-text-japanese", "mutool draw -F stext", result, output_path);
        return false;
    }
    const std::string output = read_file(output_path);
    const std::string japanese = font_for_stext_character(output, "&#x901f;");
    const std::string math = font_for_stext_character(output, "v");
    return canonical_font_name(japanese).find("yugothic") != std::string::npos &&
        canonical_font_name(math).find("yugothic") == std::string::npos &&
        !math.empty();
}

bool verify_configured_font(
    const std::filesystem::path& lualatex,
    const std::filesystem::path& mutool,
    const std::filesystem::path& root,
    std::string_view case_name,
    const aviutl2_latex::JapaneseFontConfig& font,
    std::string_view expected_font_fragment,
    const std::set<std::string>& baseline_math_fonts,
    std::string_view math_body) {
    const auto preamble = build_japanese_preamble(font);
    if (!preamble) return false;
    std::set<std::string> math_fonts;
    if (!compile_and_inspect(
            lualatex, mutool, root, std::string(case_name) + "-math",
            make_document(*preamble, math_body), math_fonts) ||
        math_fonts != baseline_math_fonts) {
        return false;
    }
    std::set<std::string> text_fonts;
    std::string text_and_math = "日本語本文です。\n";
    text_and_math.append(math_body);
    return compile_and_inspect(
            lualatex, mutool, root, std::string(case_name) + "-text",
            make_document(*preamble, text_and_math), text_fonts) &&
        contains_all_fonts(text_fonts, baseline_math_fonts) &&
        (expected_font_fragment.empty()
            ? text_fonts.size() > baseline_math_fonts.size()
            : contains_font_fragment(text_fonts, expected_font_fragment));
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 6) {
        std::wcerr << L"usage: JapaneseMathFontIsolationSmoke "
                      L"<lualatex.exe> <mutool.exe> <work-directory> "
                      L"<optional-family> <optional-font-file>\n";
        return 2;
    }

    const std::filesystem::path lualatex(argv[1]);
    const std::filesystem::path mutool(argv[2]);
    const std::filesystem::path requested_root(argv[3]);
    const std::wstring optional_family(argv[4]);
    const std::filesystem::path optional_font_file(argv[5]);
    if (!std::filesystem::is_regular_file(lualatex)) {
        std::wcerr << L"LuaLaTeX executable does not exist: "
                   << lualatex.wstring() << L'\n';
        return 3;
    }
    if (!std::filesystem::is_regular_file(mutool)) {
        std::wcerr << L"mutool executable does not exist: "
                   << mutool.wstring() << L'\n';
        return 4;
    }

    const std::filesystem::path root = requested_root /
        (L"japanese-math-font-isolation-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::create_directories(root, error);
    if (error) {
        std::cerr << "failed to create test root: " << error.message() << '\n';
        return 5;
    }

    constexpr std::string_view math_body = R"tex(\[
  \lim_{n\to\infty}
  \sum_{k=1}^{n}\frac{1}{n}
  =
  1,
  \qquad
  \sin x+\cos x,
  \qquad
  \operatorname{rank}A
\])tex";
    constexpr std::string_view japanese_packages =
        "\\usepackage[no-math]{fontspec}\n"
        "\\usepackage{luatexja}\n"
        "\\usepackage{luatexja-fontspec}\n";
    constexpr std::string_view regression_packages =
        "\\usepackage{fontspec}\n"
        "\\usepackage{luatexja}\n"
        "\\usepackage{luatexja-fontspec}\n";

    const std::string document_a = make_document("", math_body);
    const std::string document_b = make_document(
        std::string(regression_packages) +
            "\\setmainfont{Yu Gothic}\n"
            "\\setmainjfont{Yu Gothic}\n",
        math_body);
    const std::string document_c = make_document(
        std::string(japanese_packages) +
            "\\setmainjfont{Yu Gothic}\n",
        math_body);

    std::set<std::string> fonts_a;
    std::set<std::string> fonts_b;
    std::set<std::string> fonts_c;
    std::filesystem::path pdf_a;
    std::filesystem::path pdf_b;
    std::filesystem::path pdf_c;
    if (!compile_and_inspect(
            lualatex, mutool, root, "A-default", document_a, fonts_a, &pdf_a) ||
        !compile_and_inspect(
            lualatex, mutool, root, "B-main-and-japanese", document_b, fonts_b,
            &pdf_b) ||
        !compile_and_inspect(
            lualatex, mutool, root, "C-japanese-only", document_c, fonts_c,
            &pdf_c)) {
        return 10;
    }
    if (fonts_a != fonts_c) {
        std::cerr << "A and C embedded font sets differ; setmainjfont changed "
                     "the math-only document\n";
        print_fonts("A", fonts_a);
        print_fonts("C", fonts_c);
        return 11;
    }
    if (fonts_b == fonts_a || !contains_yu_gothic(fonts_b)) {
        std::cerr << "B did not demonstrate the setmainfont regression\n";
        print_fonts("A", fonts_a);
        print_fonts("B", fonts_b);
        return 12;
    }
    std::string lim_font_a;
    std::string lim_font_b;
    std::string lim_font_c;
    if (!inspect_stext_character_font(
            mutool, "A-default", pdf_a, "l", lim_font_a) ||
        !inspect_stext_character_font(
            mutool, "B-main-and-japanese", pdf_b, "l", lim_font_b) ||
        !inspect_stext_character_font(
            mutool, "C-japanese-only", pdf_c, "l", lim_font_c)) {
        return 13;
    }
    if (canonical_font_name(lim_font_a).find("yugothic") != std::string::npos ||
        canonical_font_name(lim_font_b).find("yugothic") == std::string::npos ||
        canonical_font_name(lim_font_c).find("yugothic") != std::string::npos) {
        std::cerr << "unexpected font assignment for the first l in \\lim: "
                  << "A=" << lim_font_a << ", B=" << lim_font_b
                  << ", C=" << lim_font_c << '\n';
        return 14;
    }

    std::vector<std::pair<std::wstring, std::string>> installed_fonts{
        {L"Yu Gothic", "yugothic"},
        {L"Yu Gothic UI", "yugothicui"},
        {L"Meiryo", "meiryo"}
    };
    if (!optional_family.empty()) {
        installed_fonts.emplace_back(optional_family, "corporate");
    }
    for (std::size_t index = 0; index < installed_fonts.size(); ++index) {
        aviutl2_latex::JapaneseFontConfig font;
        font.source = aviutl2_latex::JapaneseFontSource::InstalledFamily;
        font.fontspec_family_name = installed_fonts[index].first;
        if (!verify_configured_font(
                lualatex, mutool, root,
                "installed-" + std::to_string(index), font,
                installed_fonts[index].second, fonts_a, math_body)) {
            std::cerr << "installed Japanese font isolation failed at index "
                      << index << '\n';
            return 13;
        }
    }
    if (!optional_font_file.empty()) {
        aviutl2_latex::JapaneseFontConfig font;
        font.source = aviutl2_latex::JapaneseFontSource::FontFile;
        font.file_path = optional_font_file;
        if (!verify_configured_font(
                lualatex, mutool, root, "font-file", font, "",
                fonts_a, math_body)) {
            std::cerr << "Japanese font-file isolation failed\n";
            return 14;
        }
    }

    const std::vector<std::pair<std::string_view, std::string_view>>
        display_math_cases{
            {"inline-parentheses", R"tex(\(x^2+1\))tex"},
            {"inline-dollar", R"tex($x^2+1$)tex"},
            {"display-brackets", R"tex(\[x^2+1\])tex"},
            {"display-double-dollar", R"tex($$x^2+1$$)tex"},
            {"equation", R"tex(\begin{equation}x^2+1\end{equation})tex"},
            {"equation-star", R"tex(\begin{equation*}x^2+1\end{equation*})tex"},
            {"align", R"tex(\begin{align}x&=1\\y&=2\end{align})tex"},
            {"align-star", R"tex(\begin{align*}x&=1\\y&=2\end{align*})tex"},
            {"aligned", R"tex(\[\begin{aligned}x&=1\\y&=2\end{aligned}\])tex"},
            {"gather", R"tex(\begin{gather}x=1\\y=2\end{gather})tex"},
            {"gathered", R"tex(\[\begin{gathered}x=1\\y=2\end{gathered}\])tex"},
            {"split", R"tex(\begin{equation*}\begin{split}x&=1\\y&=2\end{split}\end{equation*})tex"},
            {"math-alphabets", R"tex(\[\mathrm{ABC},\ \mathbf{x},\ \mathit{f},\ \mathsf{X},\ \mathtt{code}\])tex"},
        };
    aviutl2_latex::JapaneseFontConfig display_font;
    display_font.source = aviutl2_latex::JapaneseFontSource::InstalledFamily;
    display_font.fontspec_family_name = L"Yu Gothic";
    const auto display_preamble = build_japanese_preamble(display_font);
    if (!display_preamble) return 15;
    for (const auto& [name, body] : display_math_cases) {
        std::set<std::string> default_fonts;
        std::set<std::string> configured_fonts;
        if (!compile_and_inspect(
                lualatex, mutool, root, std::string(name) + "-default",
                make_document("", body), default_fonts) ||
            !compile_and_inspect(
                lualatex, mutool, root, std::string(name) + "-japanese",
                make_document(*display_preamble, body), configured_fonts) ||
            configured_fonts != default_fonts) {
            std::cerr << name << ": Japanese preamble changed math fonts\n";
            return 20;
        }
    }

    constexpr std::string_view math_text_body = R"tex(\[
  v=\frac{dx}{dt}
  \qquad
    \text{速度}
\])tex";
    std::set<std::string> math_text_fonts;
    std::filesystem::path math_text_pdf;
    if (!compile_document(
            lualatex,
            root,
            "math-text-japanese",
            make_document(*display_preamble, math_text_body),
            math_text_pdf) ||
        !inspect_fonts(
            mutool, "math-text-japanese", math_text_pdf, math_text_fonts)) {
        return 30;
    }
    const bool shares_math_font = std::any_of(
        fonts_a.begin(), fonts_a.end(), [&](const std::string& font) {
            return math_text_fonts.contains(font);
        });
    if (!contains_yu_gothic(math_text_fonts) || !shares_math_font) {
        std::cerr << "math/text document did not contain both Yu Gothic and "
                     "a baseline math font\n";
        print_fonts("baseline math", fonts_a);
        print_fonts("math text", math_text_fonts);
        return 31;
    }
    if (!inspect_stext_font_assignment(mutool, math_text_pdf)) {
        std::cerr << "glyph-level font attribution did not separate Japanese "
                     "text from math glyphs\n";
        return 32;
    }

    std::cout << "A/C math fonts identical; B uses Yu Gothic; display math "
                 "variants compile; Japanese text and math fonts coexist\n";
    return 0;
}
