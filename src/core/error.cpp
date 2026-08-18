#include "runi/core/error.hpp"

#include <utility>

namespace runi {

std::string_view to_string(ErrorCategory category) noexcept {
    switch (category) {
        case ErrorCategory::Configuration: return "configuration";
        case ErrorCategory::ModelTransport: return "model_transport";
        case ErrorCategory::ModelProtocol: return "model_protocol";
        case ErrorCategory::Parse: return "parse";
        case ErrorCategory::Validation: return "validation";
        case ErrorCategory::PolicyDenied: return "policy_denied";
        case ErrorCategory::PathViolation: return "path_violation";
        case ErrorCategory::ToolExecution: return "tool_execution";
        case ErrorCategory::Timeout: return "timeout";
        case ErrorCategory::Persistence: return "persistence";
        case ErrorCategory::ResumeMismatch: return "resume_mismatch";
        case ErrorCategory::Internal: return "internal";
    }
    return "internal";
}

Error make_error(
    ErrorCategory category,
    std::string code,
    std::string message,
    bool retryable,
    JsonValue safe_details) {
    return Error{
        category,
        std::move(code),
        std::move(message),
        retryable,
        std::move(safe_details),
    };
}

}  // namespace runi
