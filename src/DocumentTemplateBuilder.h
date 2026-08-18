#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace aviutl2_latex {

// Settings which affect only the document template wrapper.  The Japanese
// preamble is supplied by JapaneseFontConfig so this module never inspects or
// rewrites the user's document body.
struct DocumentTemplateOptions {
    std::wstring additional_preamble;
    bool minipage_enabled = false;
    std::wstring formatted_minipage_width_cm = L"0.0";
    std::wstring paragraph_alignment_command;
};

// Builds one fixed-layout step layer for the document template.  Every step
// remains in the same document and is wrapped once as a whole; ordinary line
// endings inside a step are copied verbatim.  target_step is zero-based.
std::wstring build_document_step_layer_source(
    const std::vector<std::wstring>& steps,
    std::size_t target_step,
    const DocumentTemplateOptions& options);

}  // namespace aviutl2_latex
