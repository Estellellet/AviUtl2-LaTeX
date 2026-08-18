#include "StepParser.h"

#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool expect_steps(
    const char* name,
    std::wstring_view source,
    std::size_t maximum_steps,
    std::initializer_list<std::wstring_view> expected,
    bool expected_limit_exceeded = false) {
    const auto result = step_parser::parse_latex_steps(source, maximum_steps);
    if (result.limit_exceeded != expected_limit_exceeded ||
        result.steps.size() != expected.size()) {
        std::cerr << name << ": unexpected status or step count\n";
        return false;
    }

    std::size_t index = 0;
    for (const std::wstring_view value : expected) {
        if (result.steps[index] != value) {
            std::cerr << name << ": step content mismatch at index "
                      << index << '\n';
            return false;
        }
        ++index;
    }
    return true;
}

}  // namespace

int main() {
    if (!expect_steps("empty", L"", 32, {}) ||
        !expect_steps("single", L"E=mc^2", 32, {L"E=mc^2"}) ||
        !expect_steps("lf", L"a\n%<step>\nb", 32, {L"a", L"b"}) ||
        !expect_steps("crlf", L"a\r\n%<step>\r\nb\r\n", 32,
            {L"a", L"b\r\n"}) ||
        !expect_steps("mixed-lf-exact", L"$a$\nあ", 32,
            {L"$a$\nあ"}) ||
        !expect_steps("mixed-crlf-exact", L"$a$\r\nあ", 32,
            {L"$a$\r\nあ"}) ||
        !expect_steps("final-lf-exact", L"$a$\nあ\n", 32,
            {L"$a$\nあ\n"}) ||
        !expect_steps("literal-backslash-n", L"$a$\\nあ", 32,
            {L"$a$\\nあ"}) ||
        !expect_steps("crlf-step-content",
            L"$a$\r\nあ\r\n%<step>\r\n$b$\r\nい", 32,
            {L"$a$\r\nあ", L"$b$\r\nい"}) ||
        !expect_steps("trimmed-delimiter",
            L"a\n \t %<step> \t \nb", 32, {L"a", L"b"}) ||
        !expect_steps("no-final-newline",
            L"a\n%<step>\nb", 32, {L"a", L"b"}) ||
        !expect_steps("empty-segments",
            L"%<step>\n\n%<step>\na\n%<step>\n \t\n%<step>", 32,
            {L"a"}) ||
        !expect_steps("ordinary-comment",
            L"a\n% ordinary comment\nb", 32,
            {L"a\n% ordinary comment\nb"}) ||
        !expect_steps("inline-text",
            L"\\text{%<step>}\nnode {%<step>};\n%<step> extra", 32,
            {L"\\text{%<step>}\nnode {%<step>};\n%<step> extra"}) ||
        !expect_steps("preserve-leading-lines",
            L"\n\na\n%<step>\n\nb", 32, {L"\n\na", L"\nb"}) ||
        !expect_steps("zero-limit-empty", L" \t\r\n", 0, {}) ||
        !expect_steps("zero-limit-nonempty", L"a", 0, {}, true)) {
        return 1;
    }

    std::wstring thirty_two;
    for (int index = 0; index < 32; ++index) {
        if (!thirty_two.empty()) {
            thirty_two += L"\n%<step>\n";
        }
        thirty_two += L"step-" + std::to_wstring(index + 1);
    }
    const auto accepted = step_parser::parse_latex_steps(thirty_two, 32);
    if (accepted.limit_exceeded || accepted.steps.size() != 32 ||
        accepted.steps.front() != L"step-1" ||
        accepted.steps.back() != L"step-32") {
        std::cerr << "32-step boundary was not accepted\n";
        return 2;
    }

    const auto rejected = step_parser::parse_latex_steps(
        thirty_two + L"\n%<step>\nstep-33", 32);
    if (!rejected.limit_exceeded || rejected.steps.size() != 32 ||
        rejected.steps.back() != L"step-32") {
        std::cerr << "33-step boundary was not rejected safely\n";
        return 3;
    }

    std::wstring many_steps = L"step-1";
    for (int index = 2; index <= 4096; ++index) {
        many_steps += L"\n%<step>\nstep-" + std::to_wstring(index);
    }
    const auto bounded = step_parser::parse_latex_steps(many_steps, 3);
    if (!bounded.limit_exceeded || bounded.steps.size() != 3 ||
        bounded.steps.back() != L"step-3") {
        std::cerr << "step accumulation was not bounded\n";
        return 4;
    }

    return 0;
}
