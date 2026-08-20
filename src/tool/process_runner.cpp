#include "runi/tool/process_runner.hpp"

#include <array>
#include <cstdio>
#include <future>
#include <sstream>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/wait.h>
#endif

namespace runi {
namespace {

#ifdef _WIN32

std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::string read_pipe(HANDLE handle) {
    std::string output;
    std::array<char, 4096> buffer{};
    DWORD count = 0;
    while (ReadFile(handle, buffer.data(), static_cast<DWORD>(buffer.size()), &count, nullptr) && count > 0) {
        output.append(buffer.data(), count);
    }
    CloseHandle(handle);
    return output;
}

std::vector<wchar_t> environment_block(const std::map<std::string, std::string, std::less<>>& environment) {
    std::vector<wchar_t> block;
    for (const auto& [name, value] : environment) {
        const auto item = widen(name + "=" + value);
        block.insert(block.end(), item.begin(), item.end());
        block.push_back(L'\0');
    }
    block.push_back(L'\0');
    return block;
}

#endif

}  // namespace

Result<ProcessResult> ProcessRunner::run(const ProcessRequest& request) const {
    if (request.command.empty()) return Result<ProcessResult>::failure(make_error(
        ErrorCategory::Validation, "empty_command", "command must not be empty"));
#ifdef _WIN32
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = TRUE;
    HANDLE stdout_read = nullptr;
    HANDLE stdout_write = nullptr;
    HANDLE stderr_read = nullptr;
    HANDLE stderr_write = nullptr;
    if (!CreatePipe(&stdout_read, &stdout_write, &attributes, 0) ||
        !CreatePipe(&stderr_read, &stderr_write, &attributes, 0)) {
        return Result<ProcessResult>::failure(make_error(
            ErrorCategory::ToolExecution, "pipe_create_failed", "Could not create process output pipes."));
    }
    SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0);
    SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;
    PROCESS_INFORMATION process{};

    auto command_line = widen("cmd.exe /D /S /C \"" + request.command + "\"");
    command_line.push_back(L'\0');
    const auto directory = widen(request.working_directory.string());
    auto environment = environment_block(request.environment);
    const bool use_custom_environment = !request.inherit_environment;
    const BOOL created = CreateProcessW(
        nullptr,
        command_line.data(),
        nullptr,
        nullptr,
        TRUE,
        CREATE_NO_WINDOW | (use_custom_environment ? CREATE_UNICODE_ENVIRONMENT : 0),
        use_custom_environment ? environment.data() : nullptr,
        directory.empty() ? nullptr : directory.c_str(),
        &startup,
        &process);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
    if (!created) {
        CloseHandle(stdout_read);
        CloseHandle(stderr_read);
        return Result<ProcessResult>::failure(make_error(
            ErrorCategory::ToolExecution, "process_start_failed", "Could not start shell command."));
    }

    auto stdout_future = std::async(std::launch::async, read_pipe, stdout_read);
    auto stderr_future = std::async(std::launch::async, read_pipe, stderr_read);
    const auto timeout_ms = static_cast<DWORD>(std::chrono::duration_cast<std::chrono::milliseconds>(request.timeout).count());
    const DWORD wait_result = WaitForSingleObject(process.hProcess, timeout_ms);
    const bool timed_out = wait_result == WAIT_TIMEOUT;
    if (timed_out) TerminateProcess(process.hProcess, 1);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD exit_code = 0;
    GetExitCodeProcess(process.hProcess, &exit_code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);

    ProcessResult result;
    result.exit_code = static_cast<int>(exit_code);
    result.stdout_text = stdout_future.get();
    result.stderr_text = stderr_future.get();
    result.timed_out = timed_out;
    if (timed_out) return Result<ProcessResult>::failure(make_error(
        ErrorCategory::Timeout, "process_timeout", "Command timed out after " + std::to_string(request.timeout.count()) + " seconds."));
    return Result<ProcessResult>::success(std::move(result));
#else
    const auto command = "cd \"" + request.working_directory.string() + "\" && " + request.command + " 2>&1";
    FILE* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) return Result<ProcessResult>::failure(make_error(
        ErrorCategory::ToolExecution, "process_start_failed", "Could not start shell command."));
    std::array<char, 4096> buffer{};
    std::string output;
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) output += buffer.data();
    const int status = pclose(pipe);
    ProcessResult result;
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : status;
    result.stdout_text = std::move(output);
    return Result<ProcessResult>::success(std::move(result));
#endif
}

}  // namespace runi
