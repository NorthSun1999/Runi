#include "runi/task_state.hpp"

#include <utility>

#include "runi/core/time.hpp"

namespace runi {
namespace {

std::string string_field(const JsonValue& value, std::string_view key, std::string_view fallback = {}) {
    const auto* item = value.find(key);
    return item == nullptr ? std::string(fallback) : item->string_or(fallback);
}

std::size_t size_field(const JsonValue& value, std::string_view key) {
    const auto* item = value.find(key);
    const auto result = item == nullptr ? 0 : item->integer_or(0);
    return result < 0 ? 0U : static_cast<std::size_t>(result);
}

}  // namespace

TaskState TaskState::create(std::string task_id_value, std::string request, std::string run_id_value) {
    if (run_id_value.empty()) run_id_value = new_run_id();
    TaskState state;
    state.run_id = std::move(run_id_value);
    state.task_id = std::move(task_id_value);
    state.user_request = std::move(request);
    return state;
}

TaskState TaskState::from_json(const JsonValue& value) {
    TaskState state;
    state.run_id = string_field(value, "run_id");
    state.task_id = string_field(value, "task_id");
    state.user_request = string_field(value, "user_request");
    state.status = string_field(value, "status", kStatusRunning);
    state.tool_steps = size_field(value, "tool_steps");
    state.attempts = size_field(value, "attempts");
    state.last_tool = string_field(value, "last_tool");
    state.stop_reason = string_field(value, "stop_reason");
    state.final_answer = string_field(value, "final_answer");
    state.checkpoint_id = string_field(value, "checkpoint_id");
    state.resume_status = string_field(value, "resume_status");
    return state;
}

TaskState& TaskState::record_attempt() { ++attempts; return *this; }
TaskState& TaskState::record_tool(std::string_view name) { ++tool_steps; last_tool = name; return *this; }

TaskState& TaskState::stop(std::string_view reason, std::string_view terminal_status, std::string final) {
    status = terminal_status;
    stop_reason = reason;
    if (!final.empty()) final_answer = std::move(final);
    return *this;
}

TaskState& TaskState::stop_step_limit(std::string final) {
    return stop(kStopStepLimitReached, kStatusStopped, std::move(final));
}

TaskState& TaskState::stop_retry_limit(std::string final) {
    return stop(kStopRetryLimitReached, kStatusStopped, std::move(final));
}

TaskState& TaskState::stop_model_error(std::string final) {
    return stop(kStopModelError, kStatusFailed, std::move(final));
}

TaskState& TaskState::finish_success(std::string final) {
    status = kStatusCompleted;
    stop_reason = kStopFinalAnswerReturned;
    final_answer = std::move(final);
    return *this;
}

JsonValue TaskState::to_json() const {
    return JsonValue::Object{
        {"attempts", JsonValue(attempts)},
        {"checkpoint_id", JsonValue(checkpoint_id)},
        {"final_answer", JsonValue(final_answer)},
        {"last_tool", JsonValue(last_tool)},
        {"resume_status", JsonValue(resume_status)},
        {"run_id", JsonValue(run_id)},
        {"status", JsonValue(status)},
        {"stop_reason", JsonValue(stop_reason)},
        {"task_id", JsonValue(task_id)},
        {"tool_steps", JsonValue(tool_steps)},
        {"user_request", JsonValue(user_request)},
    };
}

}  // namespace runi
