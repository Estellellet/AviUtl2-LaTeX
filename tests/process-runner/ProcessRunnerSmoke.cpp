#include "ProcessRunner.h"

#include <windows.h>

#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stop_token>
#include <string_view>
#include <thread>

namespace {

std::filesystem::path system_executable(const wchar_t* name) {
    wchar_t directory[MAX_PATH]{};
    const UINT length = GetSystemDirectoryW(directory, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(directory) / name;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc == 2 && std::wstring_view(argv[1]) == L"--spam-output") {
        std::array<unsigned char, 64 * 1024> output{};
        output.fill('X');
        for (;;) {
            DWORD written = 0;
            if (!WriteFile(GetStdHandle(STD_OUTPUT_HANDLE), output.data(),
                    static_cast<DWORD>(output.size()), &written, nullptr) ||
                written != static_cast<DWORD>(output.size())) {
                return 0;
            }
        }
    }

    const auto ping = system_executable(L"ping.exe");
    const auto findstr = system_executable(L"findstr.exe");
    const auto root = std::filesystem::temp_directory_path() /
        (L"AviUtl2LaTeX-ProcessRunnerSmoke-" +
            std::to_wstring(GetCurrentProcessId()));
    std::error_code error;
    std::filesystem::remove_all(root, error);
    error.clear();
    std::filesystem::create_directories(root, error);
    if (error || !std::filesystem::is_regular_file(ping) ||
        !std::filesystem::is_regular_file(findstr)) {
        return 10;
    }

    const auto timed = run_process(ping,
        {L"-n", L"30", L"-w", L"1000", L"127.0.0.1"}, root,
        root / L"timeout.log", 100, std::stop_token{}, 1024 * 1024);
    if (!timed.started || !timed.timed_out || timed.cancelled) {
        std::wcerr << L"timeout test failed: " << timed.error << L"\n";
        return 20;
    }

    std::stop_source cancellation;
    std::jthread requester([&cancellation] {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        cancellation.request_stop();
    });
    const auto cancelled = run_process(ping,
        {L"-n", L"30", L"-w", L"1000", L"127.0.0.1"}, root,
        root / L"cancel.log", 10'000, cancellation.get_token(), 1024 * 1024);
    if (!cancelled.started || !cancelled.cancelled || cancelled.timed_out) {
        std::wcerr << L"cancellation test failed: " << cancelled.error << L"\n";
        return 30;
    }

    const auto truncated = run_process(ping,
        {L"-n", L"1", L"127.0.0.1"}, root,
        root / L"truncated.log", 10'000, std::stop_token{}, 16);
    if (!truncated.started || truncated.exit_code != 0 ||
        !truncated.output_truncated ||
        std::filesystem::file_size(root / L"truncated.log", error) != 16) {
        std::wcerr << L"output limit test failed: " << truncated.error << L"\n";
        return 40;
    }

    const auto large_input = root / L"large-input.txt";
    {
        std::ofstream output(large_input, std::ios::binary | std::ios::trunc);
        for (int line = 0; line < 32'768; ++line) {
            output << "0123456789abcdef0123456789abcdef\r\n";
        }
        if (!output.good()) {
            return 50;
        }
    }
    const auto continuously_drained = run_process(findstr,
        {L".", large_input.wstring()}, root, root / L"large-output.log",
        10'000, std::stop_token{}, 1024);
    if (!continuously_drained.started || continuously_drained.timed_out ||
        continuously_drained.exit_code != 0 ||
        !continuously_drained.output_truncated ||
        std::filesystem::file_size(root / L"large-output.log", error) != 1024) {
        std::wcerr << L"continuous output drain test failed: "
                   << continuously_drained.error << L"\n";
        return 60;
    }

    wchar_t executable_buffer[MAX_PATH]{};
    const DWORD executable_length = GetModuleFileNameW(
        nullptr, executable_buffer, static_cast<DWORD>(std::size(executable_buffer)));
    if (executable_length == 0 ||
        executable_length >= static_cast<DWORD>(std::size(executable_buffer))) {
        return 70;
    }
    const auto endless_output = run_process(
        std::filesystem::path(executable_buffer), {L"--spam-output"}, root,
        root / L"endless-output.log", 250, std::stop_token{}, 1024);
    if (!endless_output.started || !endless_output.timed_out ||
        !endless_output.output_truncated ||
        std::filesystem::file_size(root / L"endless-output.log", error) != 1024) {
        std::wcerr << L"endless output timeout test failed: "
                   << endless_output.error << L"\n";
        return 80;
    }

    std::wcout << L"timeout=ok cancellation=ok output_limit=ok drain=ok "
                  L"endless_output=ok\n";
    return 0;
}
