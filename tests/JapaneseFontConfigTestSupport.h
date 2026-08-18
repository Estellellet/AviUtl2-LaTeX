#pragma once

#include <windows.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <system_error>

namespace japanese_font_test {

inline std::string to_utf8(std::wstring_view value) {
    if (value.empty()) {
        return {};
    }
    if (value.size() > static_cast<std::size_t>((std::numeric_limits<int>::max)())) {
        return "<value too large to report>";
    }
    const int input_size = static_cast<int>(value.size());
    const int size = WideCharToMultiByte(
        CP_UTF8,
        WC_ERR_INVALID_CHARS,
        value.data(),
        input_size,
        nullptr,
        0,
        nullptr,
        nullptr);
    if (size <= 0) {
        return "<invalid UTF-16>";
    }
    std::string result(static_cast<std::size_t>(size), '\0');
    if (WideCharToMultiByte(
            CP_UTF8,
            WC_ERR_INVALID_CHARS,
            value.data(),
            input_size,
            result.data(),
            size,
            nullptr,
            nullptr) != size) {
        return "<UTF-8 conversion failed>";
    }
    return result;
}

inline int fail(
    std::string_view suite,
    std::string_view check,
    std::string_view detail = {}) {
    std::cerr << '[' << suite << "] " << check << " failed";
    if (!detail.empty()) {
        std::cerr << ": " << detail;
    }
    std::cerr << '\n';
    return 1;
}

inline bool contains(std::wstring_view text, std::wstring_view value) {
    return text.find(value) != std::wstring_view::npos;
}

inline bool write_binary_fixture(
    const std::filesystem::path& path,
    std::string_view contents,
    std::string& error_message) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        error_message = "could not open the fixture file";
        return false;
    }
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    if (!output.good()) {
        error_message = "could not write the complete fixture file";
        return false;
    }
    output.close();
    if (!output) {
        error_message = "could not close the fixture file cleanly";
        return false;
    }
    return true;
}

class ScopedFixtureDirectory {
public:
    ScopedFixtureDirectory(
        const std::filesystem::path& fixture_base,
        std::wstring_view test_name) {
        std::error_code error;
        base_ = std::filesystem::absolute(fixture_base, error).lexically_normal();
        if (error || base_.empty()) {
            error_message_ = "could not resolve the explicit fixture base";
            return;
        }
        std::filesystem::create_directories(base_, error);
        if (error || !std::filesystem::is_directory(base_, error) || error) {
            error_message_ = "could not create the explicit fixture base";
            return;
        }

        static std::atomic<unsigned long long> next_counter{0};
        const unsigned long process_id = GetCurrentProcessId();
        for (unsigned int attempt = 0; attempt < 1024; ++attempt) {
            const unsigned long long counter =
                next_counter.fetch_add(1, std::memory_order_relaxed);
            std::wstring directory_name(test_name);
            directory_name += L"-日本語-";
            directory_name += std::to_wstring(process_id);
            directory_name.push_back(L'-');
            directory_name += std::to_wstring(counter);
            path_ = base_ / directory_name;

            error.clear();
            if (std::filesystem::create_directory(path_, error)) {
                owns_path_ = true;
                return;
            }
            if (error && error != std::errc::file_exists) {
                error_message_ =
                    "could not create a unique fixture directory: " +
                    error.message();
                path_.clear();
                return;
            }
        }
        error_message_ = "could not allocate a unique fixture directory";
        path_.clear();
    }

    ScopedFixtureDirectory(const ScopedFixtureDirectory&) = delete;
    ScopedFixtureDirectory& operator=(const ScopedFixtureDirectory&) = delete;

    ~ScopedFixtureDirectory() noexcept {
        if (!owns_path_) {
            return;
        }
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) {
            std::cerr << "[fixture-cleanup] could not remove test directory: "
                      << error.message() << '\n';
        }
    }

    bool valid() const noexcept {
        return owns_path_;
    }

    const std::filesystem::path& path() const noexcept {
        return path_;
    }

    const std::string& error_message() const noexcept {
        return error_message_;
    }

private:
    std::filesystem::path base_;
    std::filesystem::path path_;
    std::string error_message_;
    bool owns_path_ = false;
};

inline bool read_explicit_fixture_base(
    int argc,
    wchar_t** argv,
    std::string_view suite,
    std::filesystem::path& fixture_base) {
    if (argc != 2 || argv == nullptr || argv[1] == nullptr ||
        argv[1][0] == L'\0') {
        std::cerr << '[' << suite
                  << "] usage: <test-executable> <fixture-base-directory>\n";
        return false;
    }
    fixture_base = std::filesystem::path(argv[1]);
    return true;
}

} // namespace japanese_font_test
