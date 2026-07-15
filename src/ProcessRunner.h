#pragma once

#include <windows.h>

#include <cstdint>
#include <filesystem>
#include <stop_token>
#include <string>
#include <vector>

struct ProcessResult {
    bool started = false;
    bool timed_out = false;
    bool cancelled = false;
    bool output_truncated = false;
    DWORD exit_code = static_cast<DWORD>(-1);
    std::uint64_t elapsed_ms = 0;
    std::wstring command_line;
    std::wstring error;
};

ProcessResult run_process(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& output_path,
    DWORD timeout_ms);

// Cancellable process execution used by the environment diagnostics worker.
// stdout/stderr are drained continuously. Bytes beyond maximum_output_bytes
// are discarded while the child is running; zero keeps complete output for
// existing callers.
ProcessResult run_process(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& output_path,
    DWORD timeout_ms,
    std::stop_token stop_token,
    std::uintmax_t maximum_output_bytes);
