#include "EnvironmentDiagnostics.h"

#include <windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>
#include <vector>

#include "AppPaths.h"
#include "LatexRenderer.h"
#include "ProcessRunner.h"

namespace {

using Clock = std::chrono::steady_clock;

constexpr DWORD kVersionTimeoutMs = 10'000;
constexpr DWORD kLatexTimeoutMs = 60'000;
constexpr DWORD kMutoolTimeoutMs = 30'000;
constexpr auto kDiagnosticTimeout = std::chrono::seconds(180);
constexpr std::uintmax_t kMaximumDiagnosticOutputBytes = 4u * 1024u * 1024u;

struct CategoryResult {
    std::wstring name;
    std::vector<std::wstring> missing_packages;
    std::wstring failure;
    bool completed = false;

    bool success() const {
        return completed && missing_packages.empty() && failure.empty();
    }
};

struct LatexTestResult {
    bool success = false;
    bool interrupted = false;
    ProcessResult process;
    std::filesystem::path pdf;
    std::wstring output;
};

struct DiagnosticContext {
    std::stop_token stop_token;
    Clock::time_point started_at = Clock::now();
    Clock::time_point deadline = started_at + kDiagnosticTimeout;
    bool cancelled = false;
    bool timed_out = false;
    bool overall_timed_out = false;
    std::wstring timeout_stage;

    bool should_stop() {
        if (stop_token.stop_requested()) {
            cancelled = true;
            return true;
        }
        if (Clock::now() >= deadline) {
            timed_out = true;
            overall_timed_out = true;
            if (timeout_stage.empty()) {
                timeout_stage = L"diagnostic_total";
            }
            return true;
        }
        return false;
    }
};

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), static_cast<int>(value.size()), nullptr, 0, nullptr, nullptr);
    if (size <= 0) return {};
    std::string result(static_cast<std::size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
        static_cast<int>(value.size()), result.data(), size, nullptr, nullptr);
    return result;
}

std::wstring read_utf8_lossy(
    const std::filesystem::path& path,
    bool output_truncated = false) {
    std::ifstream input(path, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(input)), {});
    if (bytes.empty()) {
        return output_truncated ? L"(output truncated)" : std::wstring{};
    }
    const int size = MultiByteToWideChar(CP_UTF8, 0, bytes.data(),
        static_cast<int>(bytes.size()), nullptr, 0);
    if (size <= 0) return L"(出力を読み取れません)";
    std::wstring value(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()),
        value.data(), size);
    if (output_truncated) {
        value += L"\r\n[diagnostic output truncated]";
    }
    return value;
}

bool write_utf8(const std::filesystem::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

std::wstring join(const std::vector<std::wstring>& values) {
    std::wstring result;
    for (const auto& value : values) {
        if (!result.empty()) result += L", ";
        result += value;
    }
    return result;
}

std::wstring first_nonempty_line(const std::wstring& value) {
    std::wistringstream input(value);
    std::wstring line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (!line.empty()) return line;
    }
    return {};
}

std::wstring category_line(const CategoryResult& category) {
    std::wstring result = category.name + L": ";
    if (!category.missing_packages.empty()) {
        result += join(category.missing_packages) + L" 不足";
        if (!category.failure.empty()) {
            result += L" / " + category.failure;
        }
        return result;
    }
    if (!category.failure.empty()) {
        return result + category.failure;
    }
    return result + (category.completed ? L"正常" : L"未確認");
}

void mark_interrupted(CategoryResult& category, const DiagnosticContext& context) {
    if (category.completed || !category.failure.empty()) {
        return;
    }
    if (context.cancelled) {
        category.failure = L"中止";
    } else if (context.timed_out) {
        category.failure = L"診断タイムアウト";
    }
}

ProcessResult run_diagnostic_process(
    DiagnosticContext& context,
    const std::wstring& stage,
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& output_path,
    DWORD requested_timeout_ms) {
    ProcessResult result;
    std::error_code remove_error;
    std::filesystem::remove(output_path, remove_error);
    if (context.should_stop()) {
        result.cancelled = context.cancelled;
        result.timed_out = context.overall_timed_out;
        return result;
    }

    const auto remaining_duration = context.deadline - Clock::now();
    const auto remaining_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        remaining_duration).count();
    if (remaining_ms <= 0) {
        context.timed_out = true;
        context.overall_timed_out = true;
        context.timeout_stage = stage;
        result.timed_out = true;
        return result;
    }

    const std::int64_t bounded_remaining = (std::min)(
        static_cast<std::int64_t>(remaining_ms),
        static_cast<std::int64_t>((std::numeric_limits<DWORD>::max)()));
    const auto remaining_dword = static_cast<DWORD>(bounded_remaining);
    const bool limited_by_total = remaining_dword < requested_timeout_ms;
    const DWORD timeout_ms = (std::min)(requested_timeout_ms, remaining_dword);
    result = run_process(executable, arguments, working_directory, output_path,
        timeout_ms, context.stop_token, kMaximumDiagnosticOutputBytes);

    if (result.cancelled) {
        context.cancelled = true;
    }
    if (result.timed_out) {
        context.timed_out = true;
        context.timeout_stage = stage;
        if (limited_by_total || Clock::now() >= context.deadline) {
            context.overall_timed_out = true;
        }
    }
    return result;
}

void log_command(
    const wchar_t* category,
    const wchar_t* stage,
    const wchar_t* missing_package,
    const ProcessResult& process,
    const std::wstring& output,
    bool success) {
    append_latest_log(
        "diagnostic_category: " + to_utf8(category) + "\n" +
        "diagnostic_result: " + std::string(success ? "success\n" : "failed\n") +
        "missing_packages: " +
            (missing_package == nullptr ? std::string("none\n")
                                        : to_utf8(missing_package) + "\n") +
        "failed_command: " +
            (success ? std::string("none\n")
                     : to_utf8(process.command_line) + "\n") +
        "failed_stage: " +
            (success ? std::string("none\n") : to_utf8(stage) + "\n") +
        "exit_code: " +
            (process.started ? std::to_string(process.exit_code) + "\n"
                             : std::string("not-started\n")) +
        "process_exit_code: " +
            (process.started ? std::to_string(process.exit_code) + "\n"
                             : std::string("not-started\n")) +
        "process_cancelled: " + std::string(process.cancelled ? "yes\n" : "no\n") +
        "process_timed_out: " + std::string(process.timed_out ? "yes\n" : "no\n") +
        "process_timeout_stage: " +
            (process.timed_out ? to_utf8(stage) + "\n" : std::string("none\n")) +
        "process_output_truncated: " +
            std::string(process.output_truncated ? "yes\n" : "no\n") +
        "process_elapsed_ms: " + std::to_string(process.elapsed_ms) + "\n" +
        "process_error: " +
            (process.error.empty() ? std::string("none\n")
                                   : to_utf8(process.error) + "\n") +
        "stdout:\n" + to_utf8(output) + "\n" +
        "stderr: merged_into_stdout\n");
}

LatexTestResult run_latex_test(
    DiagnosticContext& context,
    const ResolvedTools& tools,
    const std::filesystem::path& root,
    const std::wstring& test_name,
    std::string_view source,
    const wchar_t* category,
    const wchar_t* package_name) {
    LatexTestResult test;
    if (context.should_stop()) {
        test.interrupted = true;
        return test;
    }

    const auto directory = root / test_name;
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    const auto tex = directory / L"main.tex";
    const auto output_path = directory / L"output.log";
    test.pdf = directory / L"main.pdf";
    if (code || !write_utf8(tex, source)) {
        test.process.error = L"診断用TeXを書き込めません";
        log_command(category, L"diagnostic_tex_write", package_name,
            test.process, test.process.error, false);
        return test;
    }

    test.process = run_diagnostic_process(context, package_name,
        tools.lualatex_path,
        {L"--interaction=nonstopmode", L"--halt-on-error", L"--file-line-error",
         L"--no-shell-escape", L"--output-directory=" + directory.wstring(), tex.wstring()},
        directory, output_path, kLatexTimeoutMs);
    test.output = read_utf8_lossy(output_path, test.process.output_truncated);
    test.interrupted = test.process.cancelled || context.overall_timed_out;
    test.success = !test.interrupted && test.process.started &&
        !test.process.timed_out && test.process.exit_code == 0 &&
        std::filesystem::is_regular_file(test.pdf, code);
    log_command(category, package_name, test.success ? nullptr : package_name,
        test.process, test.output, test.success);
    return test;
}

bool test_package(
    DiagnosticContext& context,
    const ResolvedTools& tools,
    const std::filesystem::path& root,
    CategoryResult& category,
    const wchar_t* test_name,
    const wchar_t* package_name,
    const char* source,
    std::filesystem::path* successful_pdf = nullptr) {
    const auto test = run_latex_test(context,
        tools, root, test_name, source, category.name.c_str(), package_name);
    if (test.interrupted) {
        mark_interrupted(category, context);
        return false;
    }
    if (!test.success) {
        if (test.process.timed_out) {
            category.failure =
                L"MiKTeXの確認待ち、または診断がタイムアウトしました。";
        } else {
            category.missing_packages.emplace_back(package_name);
        }
    } else if (successful_pdf != nullptr) {
        *successful_pdf = test.pdf;
    }
    return true;
}

ProcessResult run_version_command(
    DiagnosticContext& context,
    const std::wstring& stage,
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& root,
    const std::filesystem::path& output_path) {
    return run_diagnostic_process(context, stage, executable, arguments,
        root, output_path, kVersionTimeoutMs);
}

void log_version_command(
    const wchar_t* category,
    const wchar_t* stage,
    const ProcessResult& process,
    const std::filesystem::path& output_path,
    bool success) {
    log_command(category, stage, nullptr, process,
        read_utf8_lossy(output_path, process.output_truncated), success);
}

void complete_or_mark_interrupted(
    CategoryResult& category,
    DiagnosticContext& context,
    bool completed) {
    category.completed = completed;
    if (!completed) {
        mark_interrupted(category, context);
    }
}

DiagnosticReport finish_report(
    DiagnosticReport report,
    DiagnosticContext& context,
    const CategoryResult& basic,
    const CategoryResult& japanese,
    const CategoryResult& tikz,
    const CategoryResult& pdf_conversion) {
    context.should_stop();
    report.cancelled = context.cancelled;
    report.timed_out = context.timed_out;
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        Clock::now() - context.started_at).count();
    append_latest_log(
        "diagnostic_finished: yes\n"
        "diagnostic_cancelled: " + std::string(report.cancelled ? "yes\n" : "no\n") +
        "diagnostic_timed_out: " + std::string(report.timed_out ? "yes\n" : "no\n") +
        "diagnostic_elapsed_ms: " + std::to_string(elapsed) + "\n" +
        "process_timeout_stage: " +
            (context.timeout_stage.empty() ? std::string("none\n")
                                           : to_utf8(context.timeout_stage) + "\n") +
        "diagnostic_category_results:\n" +
            to_utf8(category_line(basic) + L"\r\n" +
                category_line(japanese) + L"\r\n" +
                category_line(tikz) + L"\r\n" +
                category_line(pdf_conversion)) + "\n");
    return report;
}

} // namespace

DiagnosticReport run_environment_diagnostics(const ToolSettings& settings) {
    return run_environment_diagnostics(settings, std::stop_token{});
}

DiagnosticReport run_environment_diagnostics(
    const ToolSettings& settings,
    std::stop_token stop_token) {
    DiagnosticContext context{stop_token};
    DiagnosticReport report;
    CategoryResult basic{L"基本"};
    CategoryResult japanese{L"日本語"};
    CategoryResult tikz{L"TikZ"};
    CategoryResult pdf_conversion{L"PDF変換"};

    append_latest_log(
        "diagnostic_started: yes\n"
        "diagnostic_finished: no\n"
        "diagnostic_cancelled: no\n"
        "diagnostic_timed_out: no\n"
        "diagnostic_worker_thread: " + std::to_string(GetCurrentThreadId()) + "\n"
        "diagnostic_ui_blocked: no\n");

    if (context.should_stop()) {
        mark_interrupted(basic, context);
        mark_interrupted(japanese, context);
        mark_interrupted(tikz, context);
        mark_interrupted(pdf_conversion, context);
        report.summary = L"診断を中止しました。";
        report.details = category_line(basic) + L"\r\n" +
            category_line(japanese) + L"\r\n" + category_line(tikz) + L"\r\n" +
            category_line(pdf_conversion);
        return finish_report(std::move(report), context, basic, japanese, tikz,
            pdf_conversion);
    }

    const ResolvedTools tools = resolve_external_tools(settings);
    AppPaths paths;
    std::wstring path_error;
    if (!resolve_app_paths(paths, path_error) ||
        !ensure_runtime_directories(paths, path_error)) {
        basic.failure = L"一時領域へ書き込めません";
        basic.completed = true;
        japanese.failure = L"未確認";
        japanese.completed = true;
        tikz.failure = L"未確認";
        tikz.completed = true;
        pdf_conversion.failure = L"未確認";
        pdf_conversion.completed = true;
        report.summary = L"外部ツールを使用できません。";
        report.details = category_line(basic) + L"\r\n" +
            category_line(japanese) + L"\r\n" + category_line(tikz) + L"\r\n" +
            category_line(pdf_conversion);
        append_latest_log(
            "diagnostic_category: basic\n"
            "diagnostic_result: failed\n"
            "missing_packages: none\n"
            "failed_command: none\n"
            "failed_stage: diagnostic_work_directory\n"
            "exit_code: not-started\n"
            "stdout:\n" + to_utf8(path_error) + "\n"
            "stderr: none\n");
        return finish_report(std::move(report), context, basic, japanese, tikz,
            pdf_conversion);
    }

    const auto root = paths.work_root / L"environment-diagnostics";
    std::error_code code;
    std::filesystem::create_directories(root, code);
    if (code) {
        basic.failure = L"一時領域へ書き込めません";
        basic.completed = true;
    }

    bool lualatex_available = false;
    if (!basic.failure.empty()) {
        // Directory creation failure was already classified above.
    } else if (tools.lualatex_path.empty()) {
        basic.failure = L"LuaLaTeX未設定";
        basic.completed = true;
    } else {
        const auto output = root / L"lualatex-version.log";
        const ProcessResult version = run_version_command(context,
            L"lualatex_version", tools.lualatex_path, {L"--version"}, root, output);
        lualatex_available = version.started && !version.cancelled &&
            !version.timed_out && version.exit_code == 0;
        if (lualatex_available) {
            report.lualatex_version = first_nonempty_line(
                read_utf8_lossy(output, version.output_truncated));
        }
        log_version_command(L"基本", L"lualatex_version", version, output,
            lualatex_available);
        if (version.cancelled || context.overall_timed_out) {
            mark_interrupted(basic, context);
        } else if (!lualatex_available) {
            basic.failure = version.timed_out
                ? L"LuaLaTeXバージョン取得タイムアウト"
                : L"LuaLaTeX起動失敗";
            basic.completed = true;
        }
    }

    std::filesystem::path base_pdf;
    if (lualatex_available && !context.should_stop()) {
        bool completed = true;
        completed = completed && test_package(context, tools, root, basic,
            L"base-standalone", L"standalone",
            "\\documentclass[12pt,border=2pt]{standalone}\n"
            "\\begin{document}OK\\end{document}\n", &base_pdf);
        completed = completed && test_package(context, tools, root, basic,
            L"base-preview", L"preview",
            "\\documentclass{article}\n\\usepackage[active,tightpage]{preview}\n"
            "\\begin{document}\\begin{preview}OK\\end{preview}\\end{document}\n");
        completed = completed && test_package(context, tools, root, basic,
            L"base-amsmath", L"amsmath",
            "\\documentclass{article}\n\\usepackage{amsmath}\n"
            "\\begin{document}\\(a=b\\)\\end{document}\n");
        completed = completed && test_package(context, tools, root, basic,
            L"base-amssymb", L"amssymb",
            "\\documentclass{article}\n\\usepackage{amssymb}\n"
            "\\begin{document}\\(\\mathbb{R}\\)\\end{document}\n");
        completed = completed && test_package(context, tools, root, basic,
            L"base-xcolor", L"xcolor",
            "\\documentclass{article}\n\\usepackage{xcolor}\n"
            "\\begin{document}{\\color{black}OK}\\end{document}\n");
        if (completed) {
            const auto combined = run_latex_test(context, tools, root,
                L"base-minimal",
                "\\documentclass[12pt,border=2pt,preview]{standalone}\n"
                "\\usepackage{amsmath,amssymb,xcolor}\n"
                "\\begin{document}\\(E=mc^2\\)\\end{document}\n",
                L"基本", L"minimal_document");
            completed = !combined.interrupted;
            if (combined.process.timed_out) {
                basic.failure =
                    L"MiKTeXの確認待ち、または診断がタイムアウトしました。";
            } else if (!combined.success && basic.missing_packages.empty()) {
                basic.failure = L"最小文書コンパイル失敗";
            } else if (combined.success) {
                base_pdf = combined.pdf;
            }
        }
        complete_or_mark_interrupted(basic, context, completed);
    }

    if (lualatex_available && !context.should_stop()) {
        bool completed = true;
        completed = completed && test_package(context, tools, root, japanese,
            L"japanese-fontspec", L"fontspec",
            "\\documentclass{article}\n\\usepackage{fontspec}\n"
            "\\begin{document}ABC\\end{document}\n");
        completed = completed && test_package(context, tools, root, japanese,
            L"japanese-luatexja", L"luatexja",
            "\\documentclass{article}\n\\usepackage{luatexja}\n"
            "\\begin{document}日本語\\end{document}\n");
        completed = completed && test_package(context, tools, root, japanese,
            L"japanese-luatexja-fontspec", L"luatexja-fontspec",
            "\\documentclass{article}\n\\usepackage{luatexja-fontspec}\n"
            "\\begin{document}日本語ABC\\end{document}\n");
        completed = completed && test_package(context, tools, root, japanese,
            L"japanese-ragged2e", L"ragged2e",
            "\\documentclass{article}\n\\usepackage{ragged2e}\n"
            "\\begin{document}\\RaggedRight ABC\\end{document}\n");
        if (completed) {
            const auto combined = run_latex_test(context, tools, root,
                L"japanese-minimal",
                "\\documentclass[12pt,border=2pt]{standalone}\n"
                "\\usepackage{fontspec,luatexja,luatexja-fontspec,ragged2e}\n"
                "\\begin{document}短い日本語文書です。\\end{document}\n",
                L"日本語", L"japanese_minimal_document");
            completed = !combined.interrupted;
            if (combined.process.timed_out) {
                japanese.failure =
                    L"MiKTeXの確認待ち、または診断がタイムアウトしました。";
            } else if (!combined.success && japanese.missing_packages.empty()) {
                japanese.failure = L"日本語文書コンパイル失敗";
            }
        }
        complete_or_mark_interrupted(japanese, context, completed);
    } else if (!lualatex_available) {
        japanese.failure = L"未確認（LuaLaTeX使用不可）";
        japanese.completed = true;
    }

    if (lualatex_available && !context.should_stop()) {
        bool completed = true;
        completed = completed && test_package(context, tools, root, tikz,
            L"tikz-package", L"tikz",
            "\\documentclass{article}\n\\usepackage{tikz}\n"
            "\\begin{document}\\begin{tikzpicture}\\draw(0,0)--(1,0);"
            "\\end{tikzpicture}\\end{document}\n");
        completed = completed && test_package(context, tools, root, tikz,
            L"pgf-package", L"pgf",
            "\\documentclass{article}\n\\usepackage{pgf}\n"
            "\\begin{document}\\begin{pgfpicture}\\pgfpathmoveto{\\pgfpointorigin}"
            "\\pgfpathlineto{\\pgfpoint{1cm}{0cm}}\\pgfusepath{stroke}"
            "\\end{pgfpicture}\\end{document}\n");
        if (completed) {
            const auto combined = run_latex_test(context, tools, root,
                L"tikz-minimal",
                "\\documentclass[tikz,border=2pt]{standalone}\n\\usepackage{tikz}\n"
                "\\begin{document}\\begin{tikzpicture}\\draw(0,0)--(1,0);"
                "\\end{tikzpicture}\\end{document}\n",
                L"TikZ", L"tikz_minimal_document");
            completed = !combined.interrupted;
            if (combined.process.timed_out) {
                tikz.failure =
                    L"MiKTeXの確認待ち、または診断がタイムアウトしました。";
            } else if (!combined.success && tikz.missing_packages.empty()) {
                tikz.failure = L"tikzpictureコンパイル失敗";
            }
        }
        complete_or_mark_interrupted(tikz, context, completed);
    } else if (!lualatex_available) {
        tikz.failure = L"未確認（LuaLaTeX使用不可）";
        tikz.completed = true;
    }

    bool mutool_available = false;
    if (context.should_stop()) {
        mark_interrupted(pdf_conversion, context);
    } else if (tools.mutool_path.empty()) {
        pdf_conversion.failure = L"MuPDF未設定";
        pdf_conversion.completed = true;
    } else {
        const auto output = root / L"mutool-version.log";
        const ProcessResult version = run_version_command(context,
            L"mutool_version", tools.mutool_path, {L"-v"}, root, output);
        mutool_available = version.started && !version.cancelled &&
            !version.timed_out && version.exit_code == 0;
        if (mutool_available) {
            report.mutool_version = first_nonempty_line(
                read_utf8_lossy(output, version.output_truncated));
        }
        log_version_command(L"PDF変換", L"mutool_version", version, output,
            mutool_available);
        if (version.cancelled || context.overall_timed_out) {
            mark_interrupted(pdf_conversion, context);
        } else if (!mutool_available) {
            pdf_conversion.failure = version.timed_out
                ? L"mutoolバージョン取得タイムアウト"
                : L"mutool起動失敗";
            pdf_conversion.completed = true;
        } else if (base_pdf.empty()) {
            pdf_conversion.failure = L"変換元PDFなし";
            pdf_conversion.completed = true;
        } else {
            const auto png = root / L"diagnostic.png";
            const auto output_path = root / L"mutool-render.log";
            const ProcessResult convert = run_diagnostic_process(context,
                L"pdf_to_png", tools.mutool_path,
                {L"draw", L"-q", L"-o", png.wstring(), L"-F", L"png",
                 L"-r", L"72", base_pdf.wstring(), L"1"},
                root, output_path, kMutoolTimeoutMs);
            const bool converted = convert.started && !convert.cancelled &&
                !convert.timed_out && convert.exit_code == 0 &&
                std::filesystem::is_regular_file(png, code);
            log_command(L"PDF変換", L"pdf_to_png", nullptr, convert,
                read_utf8_lossy(output_path, convert.output_truncated), converted);
            if (convert.cancelled || context.overall_timed_out) {
                mark_interrupted(pdf_conversion, context);
            } else {
                if (!converted) {
                    pdf_conversion.failure = convert.timed_out
                        ? L"PDF変換タイムアウト"
                        : L"PDFからPNGへの変換失敗";
                }
                pdf_conversion.completed = true;
            }
        }
    }

    mark_interrupted(basic, context);
    mark_interrupted(japanese, context);
    mark_interrupted(tikz, context);
    mark_interrupted(pdf_conversion, context);
    report.details = category_line(basic) + L"\r\n" +
        category_line(japanese) + L"\r\n" + category_line(tikz) + L"\r\n" +
        category_line(pdf_conversion);
    const bool tools_available = lualatex_available && mutool_available;
    report.success = basic.success() && japanese.success() && tikz.success() &&
        pdf_conversion.success();
    if (context.cancelled) {
        report.summary = L"診断を中止しました。";
    } else if (context.timed_out) {
        report.summary = L"MiKTeXの確認待ち、または診断がタイムアウトしました。";
    } else if (!tools_available) {
        report.summary = L"外部ツールを使用できません。";
    } else if (report.success) {
        report.summary = L"環境は正常です。";
    } else {
        report.summary = L"一部機能に必要なパッケージが不足しています。";
    }
    append_latest_log(
        "operation: environment_diagnostics\n"
        "diagnostic_summary: " + to_utf8(report.summary) + "\n"
        "diagnostic_success: " + std::string(report.success ? "yes\n" : "no\n") +
        "diagnostic_categories:\n" + to_utf8(report.details) + "\n");
    return finish_report(std::move(report), context, basic, japanese, tikz,
        pdf_conversion);
}
