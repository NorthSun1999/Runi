#include "runi/agent/prompt_prefix.hpp"

#include <vector>

#include "runi/core/sha256.hpp"
#include "runi/core/text.hpp"
#include "runi/core/time.hpp"

namespace runi {

PromptPrefix build_prompt_prefix(const WorkspaceContext& workspace, const ToolRegistry& tools, std::string built_at) {
    std::vector<std::string> tool_lines;
    for (const auto& tool : tools) {
        std::vector<std::string> fields;
        for (const auto& [name, type] : tool.descriptor.schema) fields.push_back(name + ": " + type);
        tool_lines.push_back("- " + tool.descriptor.name + "(" + join(fields, ", ") + ") [" +
            (tool.descriptor.risky ? "approval required" : "safe") + "] " + tool.descriptor.description);
    }
    const std::string examples =
        "<tool>{\"name\":\"list_files\",\"args\":{\"path\":\".\"}}</tool>\n"
        "<tool>{\"name\":\"read_file\",\"args\":{\"path\":\"README.md\",\"start\":1,\"end\":80}}</tool>\n"
        "<tool name=\"write_file\" path=\"binary_search.py\"><content>def binary_search(nums, target):\n"
        "    return -1\n</content></tool>\n"
        "<tool name=\"patch_file\" path=\"binary_search.py\"><old_text>return -1</old_text><new_text>return mid</new_text></tool>\n"
        "<tool>{\"name\":\"run_shell\",\"args\":{\"command\":\"uv run --with pytest python -m pytest -q\",\"timeout\":20}}</tool>\n"
        "<tool>{\"name\":\"delegate\",\"args\":{\"tasks\":[{\"id\":\"api\",\"task\":\"inspect the API\"},{\"id\":\"tests\",\"task\":\"inspect tests\"}],\"max_steps\":3}}</tool>\n"
        "<final>Done.</final>";
    const std::string text =
        "You are runi, a small local coding agent working inside a local repository.\n\n"
        "Rules:\n"
        "- Use tools instead of guessing about the workspace.\n"
        "- Return exactly one <tool>...</tool> or one <final>...</final>.\n"
        "- Tool calls must look like:\n"
        "  <tool>{\"name\":\"tool_name\",\"args\":{...}}</tool>\n"
        "- For write_file and patch_file with multi-line text, prefer XML style:\n"
        "  <tool name=\"write_file\" path=\"file.py\"><content>...</content></tool>\n"
        "- Final answers must look like:\n"
        "  <final>your answer</final>\n"
        "- Never invent tool results.\n"
        "- Keep answers concise and concrete.\n"
        "- If the user asks you to create or update a specific file and the path is clear, use write_file or patch_file instead of repeatedly listing files.\n"
        "- Before writing tests for existing code, read the implementation first.\n"
        "- When writing tests, match the current implementation unless the user explicitly asked you to change the code.\n"
        "- New files should be complete and runnable, including obvious imports.\n"
        "- Do not repeat the same tool call with the same arguments if it did not help. Choose a different tool or return a final answer.\n"
        "- Required tool arguments must not be empty. Do not call read_file, write_file, patch_file, run_shell, or delegate with args={}.\n\n"
        "Tools:\n" + join(tool_lines, "\n") + "\n\n"
        "Valid response examples:\n" + examples + "\n\n" + workspace.text();
    return PromptPrefix{text, sha256(text), workspace.fingerprint(), tool_signature(tools), built_at.empty() ? now_utc() : built_at};
}

}  // namespace runi
