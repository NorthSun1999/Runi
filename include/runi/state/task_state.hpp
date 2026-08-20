#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "runi/core/json_value.hpp"

namespace runi {

inline constexpr std::string_view kStatusRunning = "running";
inline constexpr std::string_view kStatusCompleted = "completed";
inline constexpr std::string_view kStatusStopped = "stopped";
inline constexpr std::string_view kStatusFailed = "failed";

inline constexpr std::string_view kStopFinalAnswerReturned = "final_answer_returned";
inline constexpr std::string_view kStopStepLimitReached = "step_limit_reached";
inline constexpr std::string_view kStopRetryLimitReached = "retry_limit_reached";
inline constexpr std::string_view kStopModelError = "model_error";
inline constexpr std::string_view kStopToolTimeout = "tool_timeout";
inline constexpr std::string_view kStopApprovalDenied = "approval_denied";
inline constexpr std::string_view kStopDelegateFailed = "delegate_failed";
inline constexpr std::string_view kStopPersistenceError = "persistence_error";
inline constexpr std::string_view kStopResumeLoadError = "resume_load_error";
inline constexpr std::string_view kStopCancelled = "cancelled";

struct TaskState {
    std::string run_id;
    std::string task_id;
    std::string user_request;
    std::string status{std::string(kStatusRunning)};
    std::size_t tool_steps{0};
    std::size_t attempts{0};
    std::string last_tool;
    std::string stop_reason;
    std::string final_answer;
    std::string checkpoint_id;
    std::string resume_status;

    [[nodiscard]] static TaskState create(
        std::string task_id,
        std::string user_request,
        std::string run_id = {});
    [[nodiscard]] static TaskState from_json(const JsonValue& value);

    TaskState& record_attempt();
    TaskState& record_tool(std::string_view name);
    TaskState& stop(
        std::string_view reason,
        std::string_view terminal_status = kStatusStopped,
        std::string final = {});
    TaskState& stop_step_limit(std::string final = {});
    TaskState& stop_retry_limit(std::string final = {});
    TaskState& stop_model_error(std::string final = {});
    TaskState& finish_success(std::string final);

    [[nodiscard]] JsonValue to_json() const;
};

}  // namespace runi
