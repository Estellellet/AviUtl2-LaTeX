#include "StepParser.h"

#include <algorithm>
#include <cwctype>

namespace step_parser {
namespace {

std::wstring_view trim_view(std::wstring_view value) {
    std::size_t first = 0;
    while (first < value.size() && std::iswspace(value[first]) != 0) {
        ++first;
    }

    std::size_t last = value.size();
    while (last > first && std::iswspace(value[last - 1]) != 0) {
        --last;
    }
    return value.substr(first, last - first);
}

}  // namespace

StepParseResult parse_latex_steps(
    std::wstring_view source,
    std::size_t maximum_steps) {
    StepParseResult result;
    result.steps.reserve((std::min)(maximum_steps, std::size_t{32}));

    const auto append_step = [&](std::wstring_view step) -> bool {
        if (!trim_view(step).empty()) {
            if (result.steps.size() >= maximum_steps) {
                result.limit_exceeded = true;
                return false;
            }
            result.steps.emplace_back(step);
        }
        return true;
    };

    std::size_t step_begin = 0;
    std::size_t line_begin = 0;
    while (line_begin <= source.size()) {
        const std::size_t newline = source.find(L'\n', line_begin);
        const std::size_t line_end = newline == std::wstring_view::npos
            ? source.size()
            : newline;
        std::wstring_view line = source.substr(line_begin, line_end - line_begin);
        if (!line.empty() && line.back() == L'\r') {
            line.remove_suffix(1);
        }

        if (trim_view(line) == L"%<step>") {
            // The line terminator immediately before a standalone delimiter
            // separates the delimiter from the preceding step. Remove only
            // that separator; every ordinary LF/CRLF inside a step, including
            // a final terminator, is retained exactly as entered.
            std::size_t step_end = line_begin;
            if (step_end > step_begin && source[step_end - 1] == L'\n') {
                --step_end;
                if (step_end > step_begin && source[step_end - 1] == L'\r') {
                    --step_end;
                }
            }
            if (!append_step(source.substr(step_begin, step_end - step_begin))) {
                return result;
            }
            step_begin = newline == std::wstring_view::npos
                ? source.size()
                : newline + 1;
        }

        if (newline == std::wstring_view::npos) {
            break;
        }
        line_begin = newline + 1;
    }

    append_step(source.substr(step_begin));
    return result;
}

}  // namespace step_parser
