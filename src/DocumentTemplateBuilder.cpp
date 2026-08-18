#include "DocumentTemplateBuilder.h"

namespace aviutl2_latex {
namespace {

std::wstring build_step_layer_body(
    const std::vector<std::wstring>& steps,
    std::size_t target_step) {
    std::wstring body;
    for (std::size_t index = 0; index < steps.size(); ++index) {
        if (!body.empty()) {
            body.push_back(L'\n');
        }
        body += L"\\begingroup\\color{";
        body += index == target_step ? L"black" : L"white";
        body += L"}\n";
        body += steps[index];
        if (steps[index].empty() || steps[index].back() != L'\n') {
            body.push_back(L'\n');
        }
        body += L"\\endgroup";
    }
    return body;
}

}  // namespace

std::wstring build_document_step_layer_source(
    const std::vector<std::wstring>& steps,
    std::size_t target_step,
    const DocumentTemplateOptions& options) {
    std::wstring document =
        L"\\documentclass[12pt,border=2pt,preview]{standalone}\n"
        L"\\usepackage{amsmath}\n"
        L"\\usepackage{amssymb}\n"
        L"\\usepackage{xcolor}\n";
    document += options.additional_preamble;
    if (options.minipage_enabled) {
        document += L"\\usepackage{ragged2e}\n";
    }
    document +=
        L"\\newcommand{\\AviUtlSetDisplaySpacing}{%\n"
        L"  \\setlength{\\abovedisplayskip}{0.5\\baselineskip}%\n"
        L"  \\setlength{\\abovedisplayshortskip}{0.5\\baselineskip}%\n"
        L"  \\setlength{\\belowdisplayskip}{0.5\\baselineskip}%\n"
        L"  \\setlength{\\belowdisplayshortskip}{0.5\\baselineskip}%\n"
        L"}\n"
        L"\\AddToHook{env/minipage/begin}{%\n"
        L"  \\AviUtlSetDisplaySpacing\n"
        L"}\n"
        L"\\begin{document}\n"
        L"\\AviUtlSetDisplaySpacing\n";
    if (options.minipage_enabled) {
        document += L"\\noindent\n\\begin{minipage}{";
        document += options.formatted_minipage_width_cm;
        document += L"cm}\n";
        document += options.paragraph_alignment_command;
        document.push_back(L'\n');
    }

    document += build_step_layer_body(steps, target_step);
    if (options.minipage_enabled) {
        document += L"\n\\end{minipage}\n";
    } else {
        document.push_back(L'\n');
    }
    document += L"\\end{document}\n";
    return document;
}

}  // namespace aviutl2_latex
