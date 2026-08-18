#pragma once

#include <string>
#include <string_view>

#include "runi/core/json_value.hpp"

namespace runi {

enum class ErrorCategory {
    Configuration,
    ModelTransport,
    ModelProtocol,
    Parse,
    Validation,
    PolicyDenied,
    PathViolation,
    ToolExecution,
    Timeout,
    Persistence,
    ResumeMismatch,
    Internal,
};

struct Error {
    ErrorCategory category{ErrorCategory::Internal};
    std::string code;
    std::string message;
    bool retryable{false};
    JsonValue safe_details;
};

[[nodiscard]] std::string_view to_string(ErrorCategory category) noexcept;

[[nodiscard]] Error make_error(
    ErrorCategory category,
    std::string code,
    std::string message,
    bool retryable = false,
    JsonValue safe_details = {});

}  // namespace runi
