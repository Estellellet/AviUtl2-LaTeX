#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace step_parser {

struct StepParseResult {
    std::vector<std::wstring> steps;
    bool limit_exceeded = false;
};

// A delimiter is a line whose trimmed contents are exactly "%<step>".
// Ordinary contents retain their original LF/CRLF sequences and final line
// terminator. Only the separator line and the line terminator immediately
// before it are removed. Empty segments are ignored. Parsing stops as soon as
// one non-empty segment beyond maximum_steps is observed, so the result never
// stores more than the configured number of steps.
StepParseResult parse_latex_steps(
    std::wstring_view source,
    std::size_t maximum_steps);

}  // namespace step_parser
