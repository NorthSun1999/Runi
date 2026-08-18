#pragma once

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runi/core/result.hpp"
#include "runi/model_action.hpp"
#include "runi/process_runner.hpp"
#include "runi/workspace.hpp"

namespace runi {

struct ToolContext {
    std::filesystem::path root;
    const WorkspaceGuard& guard;
    std::function<std::map<std::string, std::string, std::less<>>()> shell_env_provider;
    std::size_t depth{0};
    std::size_t max_depth{1};
    std::function<Result<std::string>(const JsonValue::Object&)> spawn_delegate;

    [[nodiscard]] Result<std::filesystem::path> path(std::string_view raw_path) const;
    [[nodiscard]] std::map<std::string, std::string, std::less<>> shell_env() const;
};

struct ToolDescriptor {
    std::string name;
    std::vector<std::pair<std::string, std::string>> schema;
    bool risky{false};
    std::string description;
};

struct ToolDefinition {
    ToolDescriptor descriptor;
    std::function<Result<void>(const JsonValue::Object&)> validate;
    std::function<Result<std::string>(const JsonValue::Object&)> run;
};

using ToolRegistry = std::vector<ToolDefinition>;

[[nodiscard]] std::vector<std::string> legal_tool_names();
[[nodiscard]] std::string tool_example(std::string_view name);
[[nodiscard]] ToolRegistry build_tool_registry(const ToolContext& context);
[[nodiscard]] const ToolDefinition* find_tool(const ToolRegistry& tools, std::string_view name);
[[nodiscard]] std::string tool_signature(const ToolRegistry& tools);

}  // namespace runi
