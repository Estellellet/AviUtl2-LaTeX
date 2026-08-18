#include "SdkValueCopy.h"

#include <algorithm>
#include <array>
#include <iostream>
#include <optional>
#include <string>

int main() {
    std::array<char, 64> shared_buffer{};
    const auto simulated_sdk_call = [&](const char* value) -> const char* {
        shared_buffer.fill('\0');
        const std::string source(value);
        std::copy_n(
            source.begin(),
            (std::min)(source.size(), shared_buffer.size() - 1),
            shared_buffer.begin());
        return shared_buffer.data();
    };

    const std::optional<std::string> first =
        sdk_value::copy_string(simulated_sdk_call("Yu Gothic"));
    const std::optional<std::string> second =
        sdk_value::copy_string(simulated_sdk_call("font-file.otf"));
    if (!first || !second || *first != "Yu Gothic" ||
        *second != "font-file.otf") {
        std::cerr << "SDK values were not copied before buffer reuse\n";
        return 1;
    }
    if (sdk_value::copy_string(nullptr).has_value()) {
        std::cerr << "null SDK value was not preserved\n";
        return 2;
    }
    bool too_long = false;
    bool allocation_failed = false;
    if (sdk_value::copy_string_bounded(
            "12345678", 8, &too_long, &allocation_failed) != "12345678" ||
        too_long || allocation_failed) {
        std::cerr << "bounded SDK value rejected its exact limit\n";
        return 3;
    }
    if (sdk_value::copy_string_bounded(
            "123456789", 8, &too_long, &allocation_failed).has_value() ||
        !too_long || allocation_failed) {
        std::cerr << "oversized SDK value was not rejected\n";
        return 4;
    }
    if (sdk_value::copy_wstring_bounded(
            L"123456789", 8, &too_long, &allocation_failed).has_value() ||
        !too_long || allocation_failed) {
        std::cerr << "oversized wide SDK value was not rejected\n";
        return 5;
    }
    return 0;
}
