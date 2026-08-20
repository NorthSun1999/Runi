#include "runi/tool/tools.hpp"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <regex>
#include <set>
#include <sstream>

#include "runi/core/json_codec.hpp"
#include "runi/core/sha256.hpp"
#include "runi/core/text.hpp"

namespace runi {
namespace {

Result<std::string> string_arg(const JsonValue::Object& args, std::string_view name, bool required, std::string fallback = {}) {
    const auto iterator = args.find(name);
    if (iterator == args.end()) {
        if (required) return Result<std::string>::failure(make_error(
            ErrorCategory::Validation, "missing_argument", "'" + std::string(name) + "'"));
        return Result<std::string>::success(std::move(fallback));
    }
    if (!iterator->second.is_string()) return Result<std::string>::failure(make_error(
        ErrorCategory::Validation, "invalid_argument_type", std::string(name) + " must be a string"));
    return Result<std::string>::success(iterator->second.as_string());
}

Result<int> int_arg(const JsonValue::Object& args, std::string_view name, int fallback) {
    const auto iterator = args.find(name);
    if (iterator == args.end()) return Result<int>::success(fallback);
    if (iterator->second.is_integer()) return Result<int>::success(static_cast<int>(iterator->second.as_integer()));
    if (iterator->second.is_string()) {
        try { return Result<int>::success(std::stoi(iterator->second.as_string())); }
        catch (...) {}
    }
    return Result<int>::failure(make_error(ErrorCategory::Validation, "invalid_argument_type", std::string(name) + " must be an integer"));
}

std::string relative_text(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code error;
    return std::filesystem::relative(path, root, error).generic_string();
}

std::string quote_shell(std::string_view text) {
    std::string result = "\"";
    for (const char character : text) result += character == '"' ? "\\\"" : std::string(1, character);
    result.push_back('"');
    return result;
}

bool ignored(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    for (const auto& part : relative) if (kIgnoredPathNames.contains(part.string())) return true;
    return false;
}

ToolDefinition make_list_files(const ToolContext& context) {
    ToolDefinition tool;
    tool.descriptor = {"list_files", {{"path", "str='.'"}}, false, "List files in the workspace."};
    tool.validate = [context](const JsonValue::Object& args) -> Result<void> {
        const auto raw = string_arg(args, "path", false, ".");
        if (!raw) return Result<void>::failure(raw.error());
        const auto path = context.path(raw.value());
        if (!path) return Result<void>::failure(path.error());
        if (!std::filesystem::is_directory(path.value())) return Result<void>::failure(make_error(
            ErrorCategory::Validation, "not_directory", "path is not a directory"));
        return Result<void>::success();
    };
    tool.run = [context](const JsonValue::Object& args, std::stop_token) -> Result<std::string> {
        const auto raw = string_arg(args, "path", false, ".");
        if (!raw) return Result<std::string>::failure(raw.error());
        const auto path = context.path(raw.value());
        if (!path) return Result<std::string>::failure(path.error());
        std::vector<std::filesystem::directory_entry> entries;
        std::error_code error;
        for (const auto& entry : std::filesystem::directory_iterator(path.value(), error)) {
            if (!kIgnoredPathNames.contains(entry.path().filename().string())) entries.push_back(entry);
        }
        std::sort(entries.begin(), entries.end(), [](const auto& left, const auto& right) {
            if (left.is_regular_file() != right.is_regular_file()) return !left.is_regular_file();
            return lower_ascii(left.path().filename().string()) < lower_ascii(right.path().filename().string());
        });
        std::vector<std::string> lines;
        for (std::size_t index = 0; index < std::min<std::size_t>(200, entries.size()); ++index) {
            lines.push_back(std::string(entries[index].is_directory() ? "[D] " : "[F] ") + relative_text(entries[index].path(), context.root));
        }
        return Result<std::string>::success(lines.empty() ? "(empty)" : join(lines, "\n"));
    };
    return tool;
}

ToolDefinition make_read_file(const ToolContext& context) {
    ToolDefinition tool;
    tool.descriptor = {"read_file", {{"path", "str"}, {"start", "int=1"}, {"end", "int=200"}}, false, "Read a UTF-8 file by line range."};
    tool.validate = [context](const JsonValue::Object& args) -> Result<void> {
        const auto raw = string_arg(args, "path", true);
        if (!raw) return Result<void>::failure(raw.error());
        const auto path = context.path(raw.value());
        if (!path) return Result<void>::failure(path.error());
        if (!std::filesystem::is_regular_file(path.value())) return Result<void>::failure(make_error(
            ErrorCategory::Validation, "not_file", "path is not a file"));
        const auto start = int_arg(args, "start", 1); if (!start) return Result<void>::failure(start.error());
        const auto end = int_arg(args, "end", 200); if (!end) return Result<void>::failure(end.error());
        if (start.value() < 1 || end.value() < start.value()) return Result<void>::failure(make_error(
            ErrorCategory::Validation, "invalid_line_range", "invalid line range"));
        return Result<void>::success();
    };
    tool.run = [context](const JsonValue::Object& args, std::stop_token) -> Result<std::string> {
        const auto path = context.path(string_arg(args, "path", true).value());
        if (!path) return Result<std::string>::failure(path.error());
        const auto content = read_text_file(path.value(), true);
        if (!content) return Result<std::string>::failure(content.error());
        const int start = int_arg(args, "start", 1).value();
        const int end = int_arg(args, "end", 200).value();
        const auto lines = split_lines(content.value());
        std::ostringstream body;
        for (int number = start; number <= end && number <= static_cast<int>(lines.size()); ++number) {
            if (number != start) body << '\n';
            body << std::setw(4) << number << ": " << lines[static_cast<std::size_t>(number - 1)];
        }
        return Result<std::string>::success("# " + relative_text(path.value(), context.root) + "\n" + body.str());
    };
    return tool;
}

ToolDefinition make_search(const ToolContext& context) {
    ToolDefinition tool;
    tool.descriptor = {"search", {{"pattern", "str"}, {"path", "str='.'"}}, false, "Search the workspace with rg or a simple fallback."};
    tool.validate = [context](const JsonValue::Object& args) -> Result<void> {
        const auto pattern = string_arg(args, "pattern", false);
        if (!pattern || trim(pattern.value()).empty()) return Result<void>::failure(make_error(
            ErrorCategory::Validation, "empty_pattern", "pattern must not be empty"));
        const auto path = context.path(string_arg(args, "path", false, ".").value());
        return path ? Result<void>::success() : Result<void>::failure(path.error());
    };
    tool.run = [context](const JsonValue::Object& args, std::stop_token) -> Result<std::string> {
        const auto pattern = trim(string_arg(args, "pattern", true).value());
        const auto path = context.path(string_arg(args, "path", false, ".").value());
        if (!path) return Result<std::string>::failure(path.error());
        ProcessRunner runner;
        const auto rg = runner.run(ProcessRequest{
            "rg -n --smart-case --max-count 200 " + quote_shell(pattern) + " " + quote_shell(path.value().string()),
            context.root, {}, std::chrono::seconds(20), true});
        if (rg) {
            const auto stdout_text = trim(rg.value().stdout_text);
            const auto stderr_text = trim(rg.value().stderr_text);
            const auto diagnostics = lower_ascii(stdout_text + "\n" + stderr_text);
            const bool command_missing = diagnostics.find("not recognized") != std::string::npos ||
                diagnostics.find("not found") != std::string::npos || diagnostics.find("cannot find") != std::string::npos;
            if (!command_missing && (rg.value().exit_code == 0 || rg.value().exit_code == 1)) {
                if (!stdout_text.empty()) return Result<std::string>::success(stdout_text);
                if (!stderr_text.empty()) return Result<std::string>::success(stderr_text);
                return Result<std::string>::success("(no matches)");
            }
            if (!command_missing && !stderr_text.empty()) return Result<std::string>::success(stderr_text);
        }
        std::vector<std::filesystem::path> files;
        std::error_code error;
        if (std::filesystem::is_regular_file(path.value(), error)) files.push_back(path.value());
        else for (const auto& entry : std::filesystem::recursive_directory_iterator(path.value(), error)) {
            if (entry.is_regular_file() && !ignored(entry.path(), context.root)) files.push_back(entry.path());
        }
        std::vector<std::string> matches;
        const auto lowered_pattern = lower_ascii(pattern);
        for (const auto& file : files) {
            const auto content = read_text_file(file, true); if (!content) continue;
            const auto lines = split_lines(content.value());
            for (std::size_t index = 0; index < lines.size(); ++index) if (lower_ascii(lines[index]).find(lowered_pattern) != std::string::npos) {
                matches.push_back(relative_text(file, context.root) + ":" + std::to_string(index + 1) + ":" + lines[index]);
                if (matches.size() >= 200) return Result<std::string>::success(join(matches, "\n"));
            }
        }
        return Result<std::string>::success(matches.empty() ? "(no matches)" : join(matches, "\n"));
    };
    return tool;
}

ToolDefinition make_run_shell(const ToolContext& context) {
    ToolDefinition tool;
    tool.descriptor = {"run_shell", {{"command", "str"}, {"timeout", "int=20"}}, true, "Run a shell command in the repo root."};
    tool.validate = [](const JsonValue::Object& args) -> Result<void> {
        const auto command = string_arg(args, "command", false);
        if (!command || trim(command.value()).empty()) return Result<void>::failure(make_error(
            ErrorCategory::Validation, "empty_command", "command must not be empty"));
        const auto timeout = int_arg(args, "timeout", 20); if (!timeout) return Result<void>::failure(timeout.error());
        if (timeout.value() < 1 || timeout.value() > 120) return Result<void>::failure(make_error(
            ErrorCategory::Validation, "invalid_timeout", "timeout must be in [1, 120]"));
        return Result<void>::success();
    };
    tool.run = [context](const JsonValue::Object& args, std::stop_token) -> Result<std::string> {
        ProcessRunner runner;
        const auto result = runner.run(ProcessRequest{trim(string_arg(args, "command", true).value()), context.root,
            context.shell_env(), std::chrono::seconds(int_arg(args, "timeout", 20).value()), false});
        if (!result) return Result<std::string>::failure(result.error());
        return Result<std::string>::success(
            "exit_code: " + std::to_string(result.value().exit_code) + "\nstdout:\n" +
            (trim(result.value().stdout_text).empty() ? "(empty)" : trim(result.value().stdout_text)) + "\nstderr:\n" +
            (trim(result.value().stderr_text).empty() ? "(empty)" : trim(result.value().stderr_text)));
    };
    return tool;
}

ToolDefinition make_write_file(const ToolContext& context) {
    ToolDefinition tool;
    tool.descriptor = {"write_file", {{"path", "str"}, {"content", "str"}}, true, "Write a text file."};
    tool.validate = [context](const JsonValue::Object& args) -> Result<void> {
        const auto raw = string_arg(args, "path", true); if (!raw) return Result<void>::failure(raw.error());
        const auto path = context.path(raw.value()); if (!path) return Result<void>::failure(path.error());
        if (std::filesystem::is_directory(path.value())) return Result<void>::failure(make_error(
            ErrorCategory::Validation, "path_is_directory", "path is a directory"));
        if (!args.contains("content")) return Result<void>::failure(make_error(ErrorCategory::Validation, "missing_content", "missing content"));
        return Result<void>::success();
    };
    tool.run = [context](const JsonValue::Object& args, std::stop_token) -> Result<std::string> {
        const auto path = context.path(string_arg(args, "path", true).value()); if (!path) return Result<std::string>::failure(path.error());
        const auto content = string_arg(args, "content", true); if (!content) return Result<std::string>::failure(content.error());
        const auto written = write_text_file(path.value(), content.value()); if (!written) return Result<std::string>::failure(written.error());
        return Result<std::string>::success("wrote " + relative_text(path.value(), context.root) + " (" + std::to_string(utf8_length(content.value())) + " chars)");
    };
    return tool;
}

ToolDefinition make_patch_file(const ToolContext& context) {
    ToolDefinition tool;
    tool.descriptor = {"patch_file", {{"path", "str"}, {"old_text", "str"}, {"new_text", "str"}}, true, "Replace one exact text block in a file."};
    tool.validate = [context](const JsonValue::Object& args) -> Result<void> {
        const auto raw = string_arg(args, "path", true); if (!raw) return Result<void>::failure(raw.error());
        const auto path = context.path(raw.value()); if (!path) return Result<void>::failure(path.error());
        if (!std::filesystem::is_regular_file(path.value())) return Result<void>::failure(make_error(ErrorCategory::Validation, "not_file", "path is not a file"));
        const auto old_text = string_arg(args, "old_text", false); if (!old_text || old_text.value().empty()) return Result<void>::failure(make_error(
            ErrorCategory::Validation, "empty_old_text", "old_text must not be empty"));
        if (!args.contains("new_text")) return Result<void>::failure(make_error(ErrorCategory::Validation, "missing_new_text", "missing new_text"));
        const auto content = read_text_file(path.value()); if (!content) return Result<void>::failure(content.error());
        std::size_t count = 0, position = 0;
        while ((position = content.value().find(old_text.value(), position)) != std::string::npos) { ++count; position += old_text.value().size(); }
        if (count != 1) return Result<void>::failure(make_error(ErrorCategory::Validation, "non_unique_old_text",
            "old_text must occur exactly once, found " + std::to_string(count)));
        return Result<void>::success();
    };
    tool.run = [context](const JsonValue::Object& args, std::stop_token) -> Result<std::string> {
        const auto path = context.path(string_arg(args, "path", true).value()); if (!path) return Result<std::string>::failure(path.error());
        auto content = read_text_file(path.value()); if (!content) return Result<std::string>::failure(content.error());
        const auto old_text = string_arg(args, "old_text", true).value();
        content.value().replace(content.value().find(old_text), old_text.size(), string_arg(args, "new_text", true).value());
        const auto written = write_text_file(path.value(), content.value()); if (!written) return Result<std::string>::failure(written.error());
        return Result<std::string>::success("patched " + relative_text(path.value(), context.root));
    };
    return tool;
}

ToolDefinition make_delegate(const ToolContext& context) {
    ToolDefinition tool;
    tool.descriptor = {"delegate",
        {{"task", "str?"}, {"tasks", "array<str|{id?,task,max_steps?}>?"},
            {"max_steps", "int=3"}, {"fail_fast", "bool=false"}},
        false, "Run one child Agent or fan out several read-only child Agents in parallel."};
    tool.validate = [context](const JsonValue::Object& args) -> Result<void> {
        if (context.depth >= context.max_depth) return Result<void>::failure(make_error(ErrorCategory::Validation, "delegate_depth", "delegate depth exceeded"));
        const auto request = parse_delegate_request(args, context.max_delegate_tasks);
        return request ? Result<void>::success() : Result<void>::failure(request.error());
    };
    tool.run = [context](const JsonValue::Object& args, std::stop_token stop_token) -> Result<std::string> {
        if (context.depth >= context.max_depth) return Result<std::string>::failure(make_error(ErrorCategory::Validation, "delegate_depth", "delegate depth exceeded"));
        return context.spawn_delegate(args, stop_token);
    };
    return tool;
}

}  // namespace

Result<DelegateRequest> parse_delegate_request(
    const JsonValue::Object& args, std::size_t max_tasks) {
    const bool has_task = args.contains("task");
    const bool has_tasks = args.contains("tasks");
    if (has_task == has_tasks) return Result<DelegateRequest>::failure(make_error(
        ErrorCategory::Validation, "invalid_delegate_shape", "provide exactly one of task or tasks"));
    const auto max_steps = int_arg(args, "max_steps", 3);
    if (!max_steps) return Result<DelegateRequest>::failure(max_steps.error());
    if (max_steps.value() < 1) return Result<DelegateRequest>::failure(make_error(
        ErrorCategory::Validation, "invalid_delegate_steps", "max_steps must be positive"));
    if (const auto found = args.find("fail_fast"); found != args.end() && !found->second.is_bool()) {
        return Result<DelegateRequest>::failure(make_error(
            ErrorCategory::Validation, "invalid_argument_type", "fail_fast must be a boolean"));
    }

    DelegateRequest request;
    request.fail_fast = args.contains("fail_fast") && args.at("fail_fast").bool_or();
    request.legacy_single = has_task;
    if (has_task) {
        const auto task = string_arg(args, "task", true);
        if (!task || trim(task.value()).empty()) return Result<DelegateRequest>::failure(make_error(
            ErrorCategory::Validation, "empty_task", "task must not be empty"));
        request.tasks.push_back(DelegateTaskSpec{
            "child-1", trim(task.value()), static_cast<std::size_t>(max_steps.value())});
        return Result<DelegateRequest>::success(std::move(request));
    }

    const auto& tasks = args.at("tasks");
    if (!tasks.is_array()) return Result<DelegateRequest>::failure(make_error(
        ErrorCategory::Validation, "invalid_argument_type", "tasks must be an array"));
    if (tasks.as_array().empty()) return Result<DelegateRequest>::failure(make_error(
        ErrorCategory::Validation, "empty_delegate_tasks", "tasks must contain at least one child task"));
    if (tasks.as_array().size() > max_tasks) return Result<DelegateRequest>::failure(make_error(
        ErrorCategory::Validation, "delegate_task_limit", "tasks exceeds the configured child-task limit"));

    std::set<std::string, std::less<>> ids;
    for (std::size_t index = 0; index < tasks.as_array().size(); ++index) {
        const auto& item = tasks.as_array()[index];
        DelegateTaskSpec spec{
            "child-" + std::to_string(index + 1), {}, static_cast<std::size_t>(max_steps.value())};
        if (item.is_string()) {
            spec.task = trim(item.as_string());
        } else if (item.is_object()) {
            const auto* task = item.find("task");
            if (task == nullptr || !task->is_string()) return Result<DelegateRequest>::failure(make_error(
                ErrorCategory::Validation, "invalid_delegate_task", "each child task requires a string task field"));
            spec.task = trim(task->as_string());
            if (const auto* id = item.find("id"); id != nullptr) {
                if (!id->is_string() || trim(id->as_string()).empty()) return Result<DelegateRequest>::failure(make_error(
                    ErrorCategory::Validation, "invalid_delegate_id", "child task id must be a non-empty string"));
                spec.id = trim(id->as_string());
            }
            if (const auto* steps = item.find("max_steps"); steps != nullptr) {
                if (!steps->is_integer() || steps->integer_or() < 1) return Result<DelegateRequest>::failure(make_error(
                    ErrorCategory::Validation, "invalid_delegate_steps", "child max_steps must be a positive integer"));
                spec.max_steps = static_cast<std::size_t>(steps->integer_or());
            }
        } else {
            return Result<DelegateRequest>::failure(make_error(
                ErrorCategory::Validation, "invalid_delegate_task", "each child task must be a string or object"));
        }
        if (spec.task.empty()) return Result<DelegateRequest>::failure(make_error(
            ErrorCategory::Validation, "empty_task", "child task must not be empty"));
        if (!ids.insert(spec.id).second) return Result<DelegateRequest>::failure(make_error(
            ErrorCategory::Validation, "duplicate_delegate_id", "child task ids must be unique"));
        request.tasks.push_back(std::move(spec));
    }
    return Result<DelegateRequest>::success(std::move(request));
}

Result<std::filesystem::path> ToolContext::path(std::string_view raw_path) const { return guard.resolve(raw_path); }
std::map<std::string, std::string, std::less<>> ToolContext::shell_env() const { return shell_env_provider(); }

std::vector<std::string> legal_tool_names() { return {"list_files", "read_file", "search", "run_shell", "write_file", "patch_file", "delegate"}; }

std::string tool_example(std::string_view name) {
    static const std::map<std::string, std::string, std::less<>> examples{
        {"delegate", R"(<tool>{"name":"delegate","args":{"tasks":[{"id":"api","task":"inspect the API"},{"id":"tests","task":"inspect tests"}],"max_steps":3}}</tool>)"},
        {"list_files", R"(<tool>{"name":"list_files","args":{"path":"."}}</tool>)"},
        {"patch_file", R"(<tool name="patch_file" path="binary_search.py"><old_text>return -1</old_text><new_text>return mid</new_text></tool>)"},
        {"read_file", R"(<tool>{"name":"read_file","args":{"path":"README.md","start":1,"end":80}}</tool>)"},
        {"run_shell", R"(<tool>{"name":"run_shell","args":{"command":"uv run --with pytest python -m pytest -q","timeout":20}}</tool>)"},
        {"search", R"(<tool>{"name":"search","args":{"pattern":"binary_search","path":"."}}</tool>)"},
        {"write_file", "<tool name=\"write_file\" path=\"binary_search.py\"><content>def binary_search(nums, target):\n    return -1\n</content></tool>"}};
    const auto iterator = examples.find(name);
    return iterator == examples.end() ? std::string{} : iterator->second;
}

ToolRegistry build_tool_registry(const ToolContext& context) {
    ToolRegistry result;
    result.push_back(make_list_files(context));
    result.push_back(make_read_file(context));
    result.push_back(make_search(context));
    result.push_back(make_run_shell(context));
    result.push_back(make_write_file(context));
    result.push_back(make_patch_file(context));
    if (context.depth < context.max_depth) result.push_back(make_delegate(context));
    return result;
}

const ToolDefinition* find_tool(const ToolRegistry& tools, std::string_view name) {
    const auto iterator = std::find_if(tools.begin(), tools.end(), [&](const auto& tool) { return tool.descriptor.name == name; });
    return iterator == tools.end() ? nullptr : &*iterator;
}

std::string tool_signature(const ToolRegistry& tools) {
    JsonValue::Array payload;
    std::vector<const ToolDefinition*> sorted;
    for (const auto& tool : tools) sorted.push_back(&tool);
    std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) { return left->descriptor.name < right->descriptor.name; });
    for (const auto* tool : sorted) {
        JsonValue::Object schema;
        for (const auto& [name, type] : tool->descriptor.schema) schema.emplace(name, JsonValue(type));
        payload.emplace_back(JsonValue::Object{{"description", JsonValue(tool->descriptor.description)}, {"name", JsonValue(tool->descriptor.name)},
            {"risky", JsonValue(tool->descriptor.risky)}, {"schema", JsonValue(std::move(schema))}});
    }
    return sha256(dump_compatible_json(JsonValue(std::move(payload))));
}

}  // namespace runi
