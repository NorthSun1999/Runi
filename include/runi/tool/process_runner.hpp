#pragma once

#include <chrono>
#include <filesystem>
#include <map>
#include <string>

#include "runi/core/result.hpp"

namespace runi {

struct ProcessRequest {
    std::string command;
    std::filesystem::path working_directory;
    std::map<std::string, std::string, std::less<>> environment;
    std::chrono::seconds timeout{20};
    bool inherit_environment{true};
};

struct ProcessResult {
    int exit_code{0};
    std::string stdout_text;
    std::string stderr_text;
    bool timed_out{false};
};

class IProcessRunner {
public:
    virtual ~IProcessRunner() = default;
    [[nodiscard]] virtual Result<ProcessResult> run(const ProcessRequest& request) const = 0;
};

class ProcessRunner final : public IProcessRunner {
public:
    [[nodiscard]] Result<ProcessResult> run(const ProcessRequest& request) const override;
};

}  // namespace runi
