#pragma once

#include <cstddef>
#include <new>
#include <optional>
#include <string>

namespace sdk_value {

// SDK string pointers may be invalidated by the next SDK call.  Always turn
// them into an owned value at the boundary where they are returned.
inline std::optional<std::string> copy_string(const char* value) {
    if (value == nullptr) {
        return std::nullopt;
    }
    return std::string(value);
}

inline std::optional<std::string> copy_string_bounded(
    const char* value,
    std::size_t maximum_bytes,
    bool* too_long = nullptr,
    bool* allocation_failed = nullptr) {
    if (too_long != nullptr) {
        *too_long = false;
    }
    if (allocation_failed != nullptr) {
        *allocation_failed = false;
    }
    if (value == nullptr) {
        return std::nullopt;
    }

    std::size_t length = 0;
    while (length <= maximum_bytes && value[length] != '\0') {
        ++length;
    }
    if (length > maximum_bytes) {
        if (too_long != nullptr) {
            *too_long = true;
        }
        return std::nullopt;
    }
    try {
        return std::string(value, length);
    } catch (const std::bad_alloc&) {
        if (allocation_failed != nullptr) {
            *allocation_failed = true;
        }
        return std::nullopt;
    }
}

inline std::optional<std::wstring> copy_wstring_bounded(
    const wchar_t* value,
    std::size_t maximum_characters,
    bool* too_long = nullptr,
    bool* allocation_failed = nullptr) {
    if (too_long != nullptr) *too_long = false;
    if (allocation_failed != nullptr) *allocation_failed = false;
    if (value == nullptr) return std::nullopt;
    std::size_t length = 0;
    while (length <= maximum_characters && value[length] != L'\0') ++length;
    if (length > maximum_characters) {
        if (too_long != nullptr) *too_long = true;
        return std::nullopt;
    }
    try {
        return std::wstring(value, length);
    } catch (const std::bad_alloc&) {
        if (allocation_failed != nullptr) *allocation_failed = true;
        return std::nullopt;
    }
}

// FILTER_ITEM_BUTTON refreshes FILTER_ITEM_TEXT::value with the focused
// object's raw multiline text before invoking its callback. Snapshot that
// value directly; get_object_item_value() exposes alias-file escaping and is
// intentionally not an input to this helper.
inline std::optional<std::wstring> copy_refreshed_text_item(
    const wchar_t* value,
    std::size_t maximum_characters,
    bool* too_long = nullptr,
    bool* allocation_failed = nullptr) {
    return copy_wstring_bounded(
        value, maximum_characters, too_long, allocation_failed);
}

} // namespace sdk_value
