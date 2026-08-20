#pragma once

#include <string>

#include "runi/tool/tools.hpp"
#include "runi/tool/workspace.hpp"

namespace runi {

struct PromptPrefix {
    std::string text;
    std::string hash;
    std::string workspace_fingerprint;
    std::string tool_signature;
    std::string built_at;
};

[[nodiscard]] PromptPrefix build_prompt_prefix(
    const WorkspaceContext& workspace,
    const ToolRegistry& tools,
    std::string built_at = {});

}  // namespace runi
