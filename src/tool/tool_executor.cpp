#include "runi/tool/tool_executor.hpp"

#include <algorithm>
#include <regex>

#include "runi/core/text.hpp"

namespace runi {
namespace {

JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

std::string path_security_event(std::string_view message) {
    return message.find("path escapes workspace") != std::string_view::npos ? "path_escape" : "";
}

}  // namespace

ToolExecutor::ToolExecutor(const ToolRegistry& tools, IToolHost& host) : tools_(tools), host_(host) {}

JsonValue ToolExecutor::metadata(
    std::string tool_status,
    std::string tool_error_code,
    std::string security_event_type,
    std::string risk_level,
    bool is_read_only,
    std::vector<std::string> affected_paths,
    bool workspace_changed,
    std::string workspace_fingerprint,
    std::vector<std::string> diff_summary) {
    JsonValue::Object result{
        {"affected_paths", strings(affected_paths)}, {"diff_summary", strings(diff_summary)},
        {"read_only", JsonValue(is_read_only)}, {"risk_level", JsonValue(std::move(risk_level))},
        {"security_event_type", JsonValue(std::move(security_event_type))},
        {"tool_error_code", JsonValue(std::move(tool_error_code))}, {"tool_status", JsonValue(std::move(tool_status))},
        {"workspace_changed", JsonValue(workspace_changed)}};
    if (!workspace_fingerprint.empty()) result.emplace("workspace_fingerprint", JsonValue(std::move(workspace_fingerprint)));
    return JsonValue(std::move(result));
}

ToolExecutionResult ToolExecutor::execute(const ToolCall& call) {
    if (const auto& allowed = host_.allowed_tools(); allowed.has_value() &&
        std::find(allowed->begin(), allowed->end(), call.name) == allowed->end()) {
        return {"error: tool '" + call.name + "' is not allowed in this run",
            metadata("rejected", "tool_not_allowed", "", "high", false)};
    }
    const auto* tool = find_tool(tools_, call.name);
    if (tool == nullptr) return {"error: unknown tool '" + call.name + "'",
        metadata("rejected", "unknown_tool", "", "high", false)};

    const auto validation = tool->validate(call.args);
    if (!validation) {
        std::string message = "error: invalid arguments for " + call.name + ": " + validation.error().message;
        const auto example = tool_example(call.name);
        if (!example.empty()) message += "\nexample: " + example;
        return {message, metadata("rejected", "invalid_arguments", path_security_event(validation.error().message),
            tool->descriptor.risky ? "high" : "low", !tool->descriptor.risky)};
    }
    if (host_.repeated_tool_call(call.name, call.args)) return {
        "error: repeated identical tool call for " + call.name + "; choose a different tool or return a final answer",
        metadata("rejected", "repeated_identical_call", "", tool->descriptor.risky ? "high" : "low", !tool->descriptor.risky)};
    if (tool->descriptor.risky && !host_.approve(call.name, call.args)) return {
        "error: approval denied for " + call.name,
        metadata("rejected", "approval_denied", host_.read_only() ? "read_only_block" : "approval_denied", "high", false)};

    const auto before = tool->descriptor.risky ? host_.capture_workspace_snapshot() : WorkspaceSnapshot{};
    const auto execution = tool->run(call.args);
    const auto after = tool->descriptor.risky ? host_.capture_workspace_snapshot() : before;
    const auto [affected_paths, diff_summary] = host_.diff_workspace_snapshots(before, after);
    const bool workspace_changed = !affected_paths.empty();
    if (!execution) {
        auto result_metadata = metadata(workspace_changed ? "partial_success" : "error",
            workspace_changed ? "tool_partial_success" : "tool_failed", path_security_event(execution.error().message),
            tool->descriptor.risky ? "high" : "low", !tool->descriptor.risky, affected_paths, workspace_changed,
            host_.workspace_fingerprint(), diff_summary);
        host_.record_process_note_for_tool(call.name, result_metadata);
        return {"error: tool " + call.name + " failed: " + execution.error().message, std::move(result_metadata)};
    }

    auto content = clip(execution.value());
    std::string status = "ok";
    std::string error_code;
    if (call.name == "run_shell") {
        static const std::regex expression(R"(exit_code:\s*(-?\d+))");
        std::smatch match;
        if (std::regex_search(content, match, expression) && std::stoi(match[1].str()) != 0) {
            status = workspace_changed ? "partial_success" : "error";
            error_code = workspace_changed ? "tool_partial_success" : "tool_failed";
        }
    }
    host_.update_memory_after_tool(call.name, call.args, content);
    auto result_metadata = metadata(status, error_code, "", tool->descriptor.risky ? "high" : "low",
        !tool->descriptor.risky, affected_paths, workspace_changed, host_.workspace_fingerprint(), diff_summary);
    host_.record_process_note_for_tool(call.name, result_metadata);
    return {std::move(content), std::move(result_metadata)};
}

}  // namespace runi
