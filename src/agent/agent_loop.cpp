#include "runi/agent/agent_loop.hpp"

#include <chrono>
#include <variant>

#include "runi/context/checkpoint.hpp"
#include "runi/core/text.hpp"
#include "runi/core/time.hpp"
#include "runi/agent/runtime.hpp"

namespace runi {
namespace {

using Clock = std::chrono::steady_clock;

std::int64_t elapsed_ms(Clock::time_point start) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
}

bool nonempty_array(const JsonValue* value) { return value != nullptr && value->is_array() && !value->as_array().empty(); }

std::string checkpoint_id(const JsonValue& checkpoint) {
    const auto* value = checkpoint.find("checkpoint_id");
    return value == nullptr ? std::string{} : value->string_or();
}

HistoryItem message(std::string role, std::string content) {
    HistoryItem item;
    item.role = std::move(role);
    item.content = std::move(content);
    item.created_at = now_utc();
    return item;
}

JsonValue completion_payload(const JsonValue& metadata, std::string kind, std::int64_t duration) {
    return JsonValue::Object{{"completion_metadata", metadata}, {"duration_ms", JsonValue(duration)}, {"kind", JsonValue(std::move(kind))}};
}

}  // namespace

AgentLoop::AgentLoop(Runi& agent) : agent_(agent) {}

Result<std::string> AgentLoop::run(std::string_view user_message, std::stop_token stop_token) {
    if (stop_token.stop_requested()) return Result<std::string>::failure(make_error(
        ErrorCategory::Timeout, "run_cancelled", "Agent run was cancelled before it started"));
    const auto run_started = Clock::now();
    agent_.set_task_summary(user_message);
    if (const auto saved = agent_.record(message("user", std::string(user_message))); !saved) return Result<std::string>::failure(saved.error());

    auto& task_state = agent_.initialize_task(user_message);
    const auto started = agent_.run_store().start_run(task_state);
    if (!started) return Result<std::string>::failure(started.error());
    agent_.current_run_dir = started.value();
    agent_.emit_trace(task_state, "run_started", JsonValue::Object{
        {"task_id", JsonValue(task_state.task_id)}, {"user_request", JsonValue(clip(user_message, 300))}});

    const auto finish_cancelled = [&]() -> Result<std::string> {
        const std::string final = "Run cancelled.";
        task_state.stop(kStopCancelled, kStatusStopped, final);
        const auto checkpoint = agent_.create_task_checkpoint(task_state, user_message, kStopCancelled);
        static_cast<void>(agent_.run_store().write_task_state(task_state));
        agent_.emit_trace(task_state, "checkpoint_created", JsonValue::Object{
            {"checkpoint_id", JsonValue(checkpoint_id(checkpoint))}, {"trigger", JsonValue(std::string(kStopCancelled))}});
        agent_.emit_trace(task_state, "run_cancelled", JsonValue::Object{
            {"run_duration_ms", JsonValue(elapsed_ms(run_started))}});
        static_cast<void>(agent_.run_store().write_report(task_state, agent_.redact_value(agent_.build_report(task_state))));
        return Result<std::string>::failure(make_error(ErrorCategory::Timeout, "run_cancelled", final));
    };

    std::size_t tool_steps = 0;
    std::size_t attempts = 0;
    const auto max_attempts = std::max(agent_.options().max_steps * 3, agent_.options().max_steps + 4);
    while (tool_steps < agent_.options().max_steps && attempts < max_attempts) {
        if (stop_token.stop_requested()) return finish_cancelled();
        ++attempts;
        task_state.record_attempt();
        if (const auto written = agent_.run_store().write_task_state(task_state); !written) return Result<std::string>::failure(written.error());
        const auto prompt_started = Clock::now();
        auto built = agent_.build_prompt(user_message);
        if (!built) return Result<std::string>::failure(built.error());
        auto prompt_metadata = built.value().metadata;
        agent_.emit_trace(task_state, "prompt_built", JsonValue::Object{
            {"duration_ms", JsonValue(elapsed_ms(prompt_started))}, {"prompt_metadata", prompt_metadata}});

        const auto resume_status = prompt_metadata.find("resume_status") == nullptr ? std::string{} : prompt_metadata.find("resume_status")->string_or();
        if (resume_status == kCheckpointPartialStaleStatus) {
            const auto checkpoint = agent_.create_task_checkpoint(task_state, user_message, "freshness_mismatch");
            agent_.run_store().write_task_state(task_state);
            agent_.emit_trace(task_state, "checkpoint_created", JsonValue::Object{
                {"checkpoint_id", JsonValue(checkpoint_id(checkpoint))}, {"trigger", JsonValue("freshness_mismatch")}});
        } else if (resume_status == kCheckpointWorkspaceMismatchStatus) {
            agent_.emit_trace(task_state, "runtime_identity_mismatch", JsonValue::Object{
                {"fields", prompt_metadata.find("runtime_identity_mismatch_fields") == nullptr
                    ? JsonValue::Array{} : *prompt_metadata.find("runtime_identity_mismatch_fields")}});
            const auto checkpoint = agent_.create_task_checkpoint(task_state, user_message, "workspace_mismatch");
            agent_.run_store().write_task_state(task_state);
            agent_.emit_trace(task_state, "checkpoint_created", JsonValue::Object{
                {"checkpoint_id", JsonValue(checkpoint_id(checkpoint))}, {"trigger", JsonValue("workspace_mismatch")}});
        }
        if (nonempty_array(prompt_metadata.find("budget_reductions"))) {
            const auto checkpoint = agent_.create_task_checkpoint(task_state, user_message, "context_reduction");
            agent_.run_store().write_task_state(task_state);
            agent_.emit_trace(task_state, "checkpoint_created", JsonValue::Object{
                {"checkpoint_id", JsonValue(checkpoint_id(checkpoint))}, {"trigger", JsonValue("context_reduction")}});
        }
        agent_.emit_trace(task_state, "model_requested", JsonValue::Object{
            {"attempts", JsonValue(task_state.attempts)},
            {"prompt_cache_key", prompt_metadata.find("prompt_cache_key") == nullptr ? JsonValue(nullptr) : *prompt_metadata.find("prompt_cache_key")},
            {"tool_steps", JsonValue(task_state.tool_steps)}});

        CompletionOptions completion_options;
        if (agent_.model_client().supports_prompt_cache()) {
            if (const auto* key = prompt_metadata.find("prompt_cache_key"); key != nullptr && key->is_string()) completion_options.prompt_cache_key = key->as_string();
            completion_options.prompt_cache_retention = "in_memory";
        }
        const auto model_started = Clock::now();
        auto completed = agent_.model_client().complete(built.value().prompt, agent_.options().max_new_tokens, completion_options);
        if (!completed) return Result<std::string>::failure(completed.error());
        if (stop_token.stop_requested()) return finish_cancelled();
        const auto completion_metadata = agent_.model_client().last_completion_metadata();
        agent_.merge_completion_metadata(completion_metadata);
        const auto action = agent_.parse(completed.value());

        if (std::holds_alternative<ToolCall>(action)) {
            agent_.emit_trace(task_state, "model_parsed", completion_payload(completion_metadata, "tool", elapsed_ms(model_started)));
            ++tool_steps;
            const auto& call = std::get<ToolCall>(action);
            task_state.record_tool(call.name);
            const auto tool_started = Clock::now();
            const auto tool_result = agent_.execute_tool(call.name, call.args);
            if (stop_token.stop_requested()) return finish_cancelled();
            HistoryItem item = message("tool", tool_result.content);
            item.name = call.name;
            item.args = JsonValue(call.args);
            if (const auto saved = agent_.record(std::move(item)); !saved) return Result<std::string>::failure(saved.error());
            agent_.run_store().write_task_state(task_state);
            JsonValue::Object trace{{"args", JsonValue(call.args)}, {"duration_ms", JsonValue(elapsed_ms(tool_started))},
                {"name", JsonValue(call.name)}, {"result", JsonValue(clip(tool_result.content, 500))}};
            if (tool_result.metadata.is_object()) for (const auto& [key, value] : tool_result.metadata.as_object()) trace[key] = value;
            agent_.emit_trace(task_state, "tool_executed", JsonValue(std::move(trace)));
            const auto checkpoint = agent_.create_task_checkpoint(task_state, user_message, "tool_executed");
            agent_.run_store().write_task_state(task_state);
            agent_.emit_trace(task_state, "checkpoint_created", JsonValue::Object{
                {"checkpoint_id", JsonValue(checkpoint_id(checkpoint))}, {"trigger", JsonValue("tool_executed")}});
            continue;
        }

        if (std::holds_alternative<RetryRequest>(action)) {
            agent_.emit_trace(task_state, "model_parsed", completion_payload(completion_metadata, "retry", elapsed_ms(model_started)));
            if (const auto saved = agent_.record(message("assistant", std::get<RetryRequest>(action).notice)); !saved) {
                return Result<std::string>::failure(saved.error());
            }
            agent_.run_store().write_task_state(task_state);
            continue;
        }

        agent_.emit_trace(task_state, "model_parsed", completion_payload(completion_metadata, "final", elapsed_ms(model_started)));
        auto final = trim(std::get<FinalAnswer>(action).text.empty() ? completed.value() : std::get<FinalAnswer>(action).text);
        if (const auto saved = agent_.record(message("assistant", final)); !saved) return Result<std::string>::failure(saved.error());
        task_state.finish_success(final);
        agent_.promote_durable_memory(user_message, final);
        const auto checkpoint = agent_.create_task_checkpoint(task_state, user_message, "run_finished");
        agent_.run_store().write_task_state(task_state);
        agent_.emit_trace(task_state, "checkpoint_created", JsonValue::Object{
            {"checkpoint_id", JsonValue(checkpoint_id(checkpoint))}, {"trigger", JsonValue("run_finished")}});
        agent_.emit_trace(task_state, "run_finished", JsonValue::Object{
            {"final_answer", JsonValue(final)}, {"run_duration_ms", JsonValue(elapsed_ms(run_started))},
            {"status", JsonValue(task_state.status)}, {"stop_reason", JsonValue(task_state.stop_reason)}});
        const auto report = agent_.run_store().write_report(task_state, agent_.redact_value(agent_.build_report(task_state)));
        if (!report) return Result<std::string>::failure(report.error());
        return Result<std::string>::success(std::move(final));
    }

    std::string final;
    if (attempts >= max_attempts && tool_steps < agent_.options().max_steps) {
        final = "Stopped after too many malformed model responses without a valid tool call or final answer.";
        task_state.stop_retry_limit(final);
    } else {
        final = "Stopped after reaching the step limit without a final answer.";
        task_state.stop_step_limit(final);
    }
    if (const auto saved = agent_.record(message("assistant", final)); !saved) return Result<std::string>::failure(saved.error());
    agent_.promote_durable_memory(user_message, final);
    agent_.run_store().write_task_state(task_state);
    const auto trigger = task_state.stop_reason.empty() ? std::string("run_stopped") : task_state.stop_reason;
    const auto checkpoint = agent_.create_task_checkpoint(task_state, user_message, trigger);
    agent_.emit_trace(task_state, "checkpoint_created", JsonValue::Object{
        {"checkpoint_id", JsonValue(checkpoint_id(checkpoint))}, {"trigger", JsonValue(trigger)}});
    agent_.emit_trace(task_state, "run_finished", JsonValue::Object{
        {"final_answer", JsonValue(final)}, {"run_duration_ms", JsonValue(elapsed_ms(run_started))},
        {"status", JsonValue(task_state.status)}, {"stop_reason", JsonValue(task_state.stop_reason)}});
    const auto report = agent_.run_store().write_report(task_state, agent_.redact_value(agent_.build_report(task_state)));
    if (!report) return Result<std::string>::failure(report.error());
    return Result<std::string>::success(std::move(final));
}

}  // namespace runi
