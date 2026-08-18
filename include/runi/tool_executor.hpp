#pragma once

#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "runi/model_action.hpp"
#include "runi/tools.hpp"

namespace runi {

using WorkspaceSnapshot = std::map<std::string, std::string, std::less<>>;

struct ToolExecutionResult {
    std::string content;
    JsonValue metadata{JsonValue::Object{}};
};

class IToolHost {
public:
    virtual ~IToolHost() = default;
    [[nodiscard]] virtual const std::optional<std::vector<std::string>>& allowed_tools() const = 0;
    [[nodiscard]] virtual bool repeated_tool_call(std::string_view name, const JsonValue::Object& args) const = 0;
    [[nodiscard]] virtual bool approve(std::string_view name, const JsonValue::Object& args) = 0;
    [[nodiscard]] virtual bool read_only() const noexcept = 0;
    [[nodiscard]] virtual WorkspaceSnapshot capture_workspace_snapshot() const = 0;
    [[nodiscard]] virtual std::pair<std::vector<std::string>, std::vector<std::string>> diff_workspace_snapshots(
        const WorkspaceSnapshot& before,
        const WorkspaceSnapshot& after) const = 0;
    [[nodiscard]] virtual std::string workspace_fingerprint() const = 0;
    virtual void update_memory_after_tool(std::string_view name, const JsonValue::Object& args, std::string_view result) = 0;
    virtual void record_process_note_for_tool(std::string_view name, const JsonValue& metadata) = 0;
};

class IToolExecutor {
public:
    virtual ~IToolExecutor() = default;
    [[nodiscard]] virtual ToolExecutionResult execute(const ToolCall& call) = 0;
};

class ToolExecutor final : public IToolExecutor {
public:
    ToolExecutor(const ToolRegistry& tools, IToolHost& host);
    [[nodiscard]] ToolExecutionResult execute(const ToolCall& call) override;

private:
    [[nodiscard]] static JsonValue metadata(
        std::string tool_status,
        std::string tool_error_code = {},
        std::string security_event_type = {},
        std::string risk_level = "low",
        bool read_only = true,
        std::vector<std::string> affected_paths = {},
        bool workspace_changed = false,
        std::string workspace_fingerprint = {},
        std::vector<std::string> diff_summary = {});

    const ToolRegistry& tools_;
    IToolHost& host_;
};

}  // namespace runi
