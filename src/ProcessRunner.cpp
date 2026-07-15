#include "ProcessRunner.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

std::wstring quote_argument(const std::wstring& argument) {
    if (argument.empty()) {
        return L"\"\"";
    }
    if (argument.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return argument;
    }

    std::wstring result = L"\"";
    std::size_t backslashes = 0;
    for (const wchar_t character : argument) {
        if (character == L'\\') {
            ++backslashes;
        } else if (character == L'\"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(L'\"');
            backslashes = 0;
        } else {
            result.append(backslashes, L'\\');
            backslashes = 0;
            result.push_back(character);
        }
    }
    result.append(backslashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

std::wstring build_command_line(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments) {
    std::wstring command = quote_argument(executable.wstring());
    for (const auto& argument : arguments) {
        command.push_back(L' ');
        command += quote_argument(argument);
    }
    return command;
}

std::wstring windows_error(DWORD error) {
    wchar_t* buffer = nullptr;
    const DWORD size = FormatMessageW(
        FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
            FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, error, 0, reinterpret_cast<wchar_t*>(&buffer), 0, nullptr);
    std::wstring message = size != 0 && buffer != nullptr
        ? std::wstring(buffer, size)
        : L"Windows error " + std::to_wstring(error);
    if (buffer != nullptr) {
        LocalFree(buffer);
    }
    return message;
}

void append_error(ProcessResult& result, std::wstring_view message) {
    if (!result.error.empty()) {
        result.error += L" ";
    }
    result.error.append(message);
}

void drain_output_pipe(
    HANDLE pipe,
    HANDLE output_file,
    std::uintmax_t maximum_output_bytes,
    std::uintmax_t& output_bytes,
    bool& output_write_failed,
    ProcessResult& result) {
    std::array<unsigned char, 64 * 1024> buffer{};
    constexpr std::uintmax_t kMaximumDrainBytesPerPoll = 256 * 1024;
    std::uintmax_t drained_bytes = 0;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(pipe, nullptr, 0, nullptr, &available, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                append_error(result, L"PeekNamedPipe: " + windows_error(error));
            }
            return;
        }
        if (available == 0) {
            return;
        }

        const DWORD requested = (std::min)(
            available, static_cast<DWORD>(buffer.size()));
        DWORD received = 0;
        if (!ReadFile(pipe, buffer.data(), requested, &received, nullptr)) {
            const DWORD error = GetLastError();
            if (error != ERROR_BROKEN_PIPE) {
                append_error(result, L"ReadFile(process output): " +
                    windows_error(error));
            }
            return;
        }
        if (received == 0) {
            return;
        }
        drained_bytes += received;

        DWORD retained = received;
        if (maximum_output_bytes != 0) {
            const std::uintmax_t remaining = output_bytes < maximum_output_bytes
                ? maximum_output_bytes - output_bytes
                : 0;
            retained = static_cast<DWORD>((std::min)(
                remaining, static_cast<std::uintmax_t>(received)));
            if (retained != received) {
                result.output_truncated = true;
            }
        }

        if (retained != 0 && !output_write_failed) {
            DWORD written = 0;
            if (!WriteFile(
                    output_file, buffer.data(), retained, &written, nullptr) ||
                written != retained) {
                output_write_failed = true;
                append_error(result, L"WriteFile(process output): " +
                    windows_error(GetLastError()));
            } else {
                output_bytes += written;
            }
        }
        if (drained_bytes >= kMaximumDrainBytesPerPoll) {
            // Return to the process-monitor loop so cancellation and timeout
            // checks cannot be starved by a child which writes forever.
            return;
        }
    }
}

} // namespace

ProcessResult run_process(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& output_path,
    DWORD timeout_ms) {
    return run_process(executable, arguments, working_directory, output_path,
        timeout_ms, std::stop_token{}, 0);
}

ProcessResult run_process(
    const std::filesystem::path& executable,
    const std::vector<std::wstring>& arguments,
    const std::filesystem::path& working_directory,
    const std::filesystem::path& output_path,
    DWORD timeout_ms,
    std::stop_token stop_token,
    std::uintmax_t maximum_output_bytes) {
    const ULONGLONG started_at = GetTickCount64();
    ProcessResult result;
    result.command_line = build_command_line(executable, arguments);

    if (stop_token.stop_requested()) {
        result.cancelled = true;
        result.error = L"Process execution cancelled before launch.";
        return result;
    }

    SECURITY_ATTRIBUTES security{};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    HANDLE output_file = CreateFileW(
        output_path.c_str(), GENERIC_WRITE, FILE_SHARE_READ, nullptr,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (output_file == INVALID_HANDLE_VALUE) {
        result.error = L"CreateFileW(log): " + windows_error(GetLastError());
        return result;
    }

    HANDLE output_read = nullptr;
    HANDLE output_write = nullptr;
    if (!CreatePipe(&output_read, &output_write, &security, 0) ||
        !SetHandleInformation(output_read, HANDLE_FLAG_INHERIT, 0)) {
        result.error = L"CreatePipe(process output): " +
            windows_error(GetLastError());
        if (output_read != nullptr) {
            CloseHandle(output_read);
        }
        if (output_write != nullptr) {
            CloseHandle(output_write);
        }
        CloseHandle(output_file);
        return result;
    }

    HANDLE input = CreateFileW(
        L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &security,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (input == INVALID_HANDLE_VALUE) {
        result.error = L"CreateFileW(NUL): " + windows_error(GetLastError());
        CloseHandle(output_read);
        CloseHandle(output_write);
        CloseHandle(output_file);
        return result;
    }

    SIZE_T attribute_size = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_size);
    std::vector<std::byte> attribute_storage(attribute_size);
    auto* attribute_list = reinterpret_cast<PPROC_THREAD_ATTRIBUTE_LIST>(attribute_storage.data());
    if (!InitializeProcThreadAttributeList(attribute_list, 1, 0, &attribute_size)) {
        result.error = L"InitializeProcThreadAttributeList: " + windows_error(GetLastError());
        CloseHandle(input);
        CloseHandle(output_read);
        CloseHandle(output_write);
        CloseHandle(output_file);
        return result;
    }

    HANDLE inherited_handles[] = { output_write, input };
    if (!UpdateProcThreadAttribute(
            attribute_list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inherited_handles, sizeof(inherited_handles), nullptr, nullptr)) {
        result.error = L"UpdateProcThreadAttribute: " + windows_error(GetLastError());
        DeleteProcThreadAttributeList(attribute_list);
        CloseHandle(input);
        CloseHandle(output_read);
        CloseHandle(output_write);
        CloseHandle(output_file);
        return result;
    }

    HANDLE job = CreateJobObjectW(nullptr, nullptr);
    if (job == nullptr) {
        result.error = L"CreateJobObjectW: " + windows_error(GetLastError());
        DeleteProcThreadAttributeList(attribute_list);
        CloseHandle(input);
        CloseHandle(output_read);
        CloseHandle(output_write);
        CloseHandle(output_file);
        return result;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION job_information{};
    job_information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!SetInformationJobObject(
            job, JobObjectExtendedLimitInformation,
            &job_information, sizeof(job_information))) {
        result.error = L"SetInformationJobObject: " + windows_error(GetLastError());
        CloseHandle(job);
        DeleteProcThreadAttributeList(attribute_list);
        CloseHandle(input);
        CloseHandle(output_read);
        CloseHandle(output_write);
        CloseHandle(output_file);
        return result;
    }

    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = input;
    startup.StartupInfo.hStdOutput = output_write;
    startup.StartupInfo.hStdError = output_write;
    startup.lpAttributeList = attribute_list;

    PROCESS_INFORMATION process{};
    std::vector<wchar_t> mutable_command(result.command_line.begin(), result.command_line.end());
    mutable_command.push_back(L'\0');
    const DWORD creation_flags =
        CREATE_NO_WINDOW | CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
    const BOOL created = CreateProcessW(
        executable.c_str(), mutable_command.data(), nullptr, nullptr, TRUE,
        creation_flags, nullptr, working_directory.c_str(),
        &startup.StartupInfo, &process);

    DeleteProcThreadAttributeList(attribute_list);
    CloseHandle(input);
    CloseHandle(output_write);

    if (!created) {
        result.error = L"CreateProcessW: " + windows_error(GetLastError());
        CloseHandle(job);
        CloseHandle(output_read);
        CloseHandle(output_file);
        return result;
    }
    result.started = true;
    std::uintmax_t output_bytes = 0;
    bool output_write_failed = false;
    const auto drain_output = [&] {
        drain_output_pipe(
            output_read,
            output_file,
            maximum_output_bytes,
            output_bytes,
            output_write_failed,
            result);
    };

    if (!AssignProcessToJobObject(job, process.hProcess)) {
        result.error = L"AssignProcessToJobObject: " + windows_error(GetLastError());
        TerminateProcess(process.hProcess, 1);
        WaitForSingleObject(process.hProcess, 5000);
    } else if (ResumeThread(process.hThread) == static_cast<DWORD>(-1)) {
        result.error = L"ResumeThread: " + windows_error(GetLastError());
        TerminateJobObject(job, 1);
        WaitForSingleObject(process.hProcess, 5000);
    } else {
        const ULONGLONG deadline = GetTickCount64() + timeout_ms;
        for (;;) {
            // Drain stdout/stderr while the process is running.  Bytes above
            // the configured diagnostic limit are discarded immediately, so
            // neither a full pipe nor an unbounded log file can stall the
            // diagnostic worker.
            drain_output();
            if (stop_token.stop_requested()) {
                result.cancelled = true;
                result.error = L"Process execution cancelled.";
                TerminateJobObject(job, ERROR_CANCELLED);
                WaitForSingleObject(process.hProcess, 5000);
                break;
            }

            const ULONGLONG now = GetTickCount64();
            if (now >= deadline) {
                result.timed_out = true;
                TerminateJobObject(job, WAIT_TIMEOUT);
                WaitForSingleObject(process.hProcess, 5000);
                break;
            }

            const ULONGLONG remaining = deadline - now;
            const DWORD wait_ms = static_cast<DWORD>(
                std::min<ULONGLONG>(remaining, 50));
            const DWORD wait_result = WaitForSingleObject(process.hProcess, wait_ms);
            if (wait_result == WAIT_OBJECT_0) {
                drain_output();
                break;
            }
            if (wait_result == WAIT_FAILED) {
                result.error = L"WaitForSingleObject: " + windows_error(GetLastError());
                TerminateJobObject(job, 1);
                WaitForSingleObject(process.hProcess, 5000);
                break;
            }
        }
    }

    if (!GetExitCodeProcess(process.hProcess, &result.exit_code)) {
        result.error += L" GetExitCodeProcess: " + windows_error(GetLastError());
    }

    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    drain_output();
    // KILL_ON_JOB_CLOSE also removes any descendant which retained the output
    // handle after the main process ended. Drain once more after closing it.
    CloseHandle(job);
    drain_output();
    FlushFileBuffers(output_file);
    CloseHandle(output_read);
    CloseHandle(output_file);
    result.elapsed_ms = GetTickCount64() - started_at;
    return result;
}
