#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "runi/action_parser.hpp"
#include "runi/context_manager.hpp"
#include "runi/core/json_codec.hpp"
#include "runi/core/text.hpp"
#include "runi/evaluation.hpp"
#include "runi/http_client.hpp"
#include "runi/providers.hpp"
#include "runi/runtime.hpp"

namespace {

using namespace runi;

int failures = 0;

void check(bool condition, std::string_view description) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) throw std::runtime_error("test fixture write failed");
}

class RecordingHttpClient final : public IHttpClient {
public:
    mutable std::string url;
    mutable std::string body;
    mutable std::map<std::string, std::string, std::less<>> request_headers;
    HttpResponse response;

    Result<HttpResponse> post(const std::string& request_url,
        const std::map<std::string, std::string, std::less<>>& headers,
        std::string_view request_body, std::chrono::milliseconds) const override {
        url = request_url;
        body = request_body;
        request_headers = headers;
        return Result<HttpResponse>::success(response);
    }
};

class ContextContractHost final : public IContextHost {
public:
    std::string prefix_text{"stable prefix"};
    SessionState state;

    const std::string& prefix() const override { return prefix_text; }
    std::string memory_text() override { return "Memory:\n- task_summary: active"; }
    std::string render_checkpoint_text() const override { return {}; }
    std::vector<JsonValue> memory_candidates(std::string_view, std::size_t) override { return {}; }
    const SessionState& session() const override { return state; }
    bool feature_enabled(std::string_view name) const override { return name != "context_reduction"; }
    std::string reusable_file_summary(std::string_view) const override { return {}; }
};

void test_json_contract() {
    const auto parsed = parse_json(R"({"args":{"path":"README.md"},"name":"read_file","unicode":"中文😀"})");
    check(parsed.has_value(), "JSON object parses");
    check(parsed && parsed.value().at("unicode").as_string() == "中文😀", "JSON preserves UTF-8 strings");
    const auto encoded = dump_compatible_json(parsed.value());
    check(encoded.find("\\u4e2d\\u6587\\ud83d\\ude00") != std::string::npos, "ensure_ascii uses Unicode code points and surrogate pairs");
    const auto roundtrip = parse_json(encoded);
    check(roundtrip && roundtrip.value() == parsed.value(), "JSON ensure_ascii round-trips");
    check(dump_json(JsonValue(0.2)) == "0.2", "JSON numbers use shortest round-trip representation");
    check(dump_json(JsonValue(0.0)) == "0.0", "JSON preserves the floating-point type for whole-valued doubles");
    check(split_lines("a\n").size() == 1 && split_lines("a\n\n").size() == 2 && split_lines("").empty(),
        "line splitting matches text splitlines without a phantom trailing line");
}

void test_action_protocol() {
    const ModelActionParser parser;
    auto action = parser.parse(R"(<tool>{"name":"read_file","args":{"path":"README.md","start":1}}</tool>)");
    const auto* json_tool = std::get_if<ToolCall>(&action);
    check(json_tool != nullptr && json_tool->name == "read_file" && json_tool->args.at("start").integer_or() == 1,
        "JSON tool envelope preserves name and args");

    action = parser.parse("<tool name=\"write_file\" path=\"note.txt\"><content>line 1\nline 2</content></tool>");
    const auto* xml_tool = std::get_if<ToolCall>(&action);
    check(xml_tool != nullptr && xml_tool->name == "write_file" && xml_tool->args.at("content").as_string() == "line 1\nline 2",
        "XML tool envelope preserves multiline content");

    action = parser.parse("<final>Done.</final>");
    const auto* final = std::get_if<FinalAnswer>(&action);
    check(final != nullptr && final->text == "Done.", "final envelope parses");

    action = parser.parse("plain final");
    final = std::get_if<FinalAnswer>(&action);
    check(final != nullptr && final->text == "plain final", "plain non-empty model output remains a final answer");

    action = parser.parse("<tool>{bad json}</tool>");
    const auto* retry = std::get_if<RetryRequest>(&action);
    check(retry != nullptr && retry->notice.starts_with("Runtime notice: model returned malformed tool JSON."),
        "malformed tool JSON returns the exact runtime retry protocol");

    action = parser.parse("<tool>{\"name\":\"read_file\",\"args\":null}</tool>");
    json_tool = std::get_if<ToolCall>(&action);
    check(json_tool != nullptr && json_tool->args.empty(), "null tool args normalize to an empty object");

    action = parser.parse("<tool>{\"name\":\"read_file\",\"args\":{}}</tool><final>ignored</final>");
    check(std::holds_alternative<ToolCall>(action), "a tool before final takes precedence");

    check(std::holds_alternative<RetryRequest>(parser.parse("<tool>[]</tool>")),
        "non-object JSON tool payload requests a retry");
    check(std::holds_alternative<RetryRequest>(parser.parse("<tool>{\"args\":{}}</tool>")),
        "missing JSON tool name requests a retry");
    check(std::holds_alternative<RetryRequest>(parser.parse("<tool>{\"name\":\"read_file\",\"args\":[]}</tool>")),
        "non-object JSON tool args request a retry");
    check(std::holds_alternative<RetryRequest>(parser.parse("<final>  </final>")) &&
        std::holds_alternative<RetryRequest>(parser.parse("")), "empty final and empty response request retries");
}

void test_prompt_and_context_contract(const std::filesystem::path& workspace_root) {
    write_file(workspace_root / "AGENTS.md", "agents\n");
    write_file(workspace_root / "README.md", "readme\n");
    write_file(workspace_root / "pyproject.toml", "project\n");
    write_file(workspace_root / "package.json", "{}\n");
    const auto workspace = WorkspaceContext::build(workspace_root, workspace_root);
    check(workspace.has_value(), "prompt-contract workspace builds");
    if (workspace) {
        const auto text = workspace.value().text();
        const auto agents = text.find("- AGENTS.md");
        const auto readme = text.find("- README.md");
        const auto project = text.find("- pyproject.toml");
        const auto package = text.find("- package.json");
        check(agents < readme && readme < project && project < package,
            "workspace documents preserve the declared prompt order");
    }

    ContextContractHost host;
    ContextManager manager(host);
    const auto built = manager.build("inspect this");
    check(built && built.value().prompt ==
        "stable prefix\n\nMemory:\n- task_summary: active\n\nRelevant memory:\n- none\n\nTranscript:\n- empty\n\nCurrent user request:\ninspect this",
        "context sections and separators remain exact when reduction is disabled");
    if (built) {
        const auto& budgets = built.value().metadata.at("section_budgets");
        check(budgets.at("relevant_memory").integer_or() ==
                static_cast<std::int64_t>(utf8_length("Relevant memory:\n- none")) &&
              budgets.at("history").integer_or() ==
                static_cast<std::int64_t>(utf8_length("Transcript:\n- empty")),
            "non-reduced context metadata reports the rendered raw section budgets");
    }
}

void test_provider_protocols() {
    auto openai_http = std::make_shared<RecordingHttpClient>();
    openai_http->response = HttpResponse{200,
        R"({"output_text":"ok","usage":{"input_tokens":12,"output_tokens":3,"total_tokens":15,"input_tokens_details":{"cached_tokens":5}}})",
        {{"content-type", "application/json"}}};
    OpenAICompatibleModelClient openai("gpt-test", "https://api.openai.com/v1", "secret", 0.2,
        std::chrono::seconds(3), openai_http);
    const auto openai_result = openai.complete("prompt", 64, CompletionOptions{"cache-key", "in_memory"});
    check(openai_result && openai_result.value() == "ok", "OpenAI Responses text extracts");
    const auto openai_payload = parse_json(openai_http->body);
    check(openai_http->url == "https://api.openai.com/v1/responses", "OpenAI uses /v1/responses");
    check(openai_payload && openai_payload.value().at("input").is_array() && openai_payload.value().at("max_output_tokens").integer_or() == 64,
        "OpenAI request body keeps Responses input schema");
    check(openai_payload && openai_payload.value().at("prompt_cache_key").string_or() == "cache-key",
        "OpenAI cache fields are forwarded only on supported hosts");
    check(openai.last_completion_metadata().at("cached_tokens").integer_or() == 5 &&
        openai.last_completion_metadata().at("cache_hit").bool_or(), "OpenAI usage/cache metadata normalizes");

    auto sse_http = std::make_shared<RecordingHttpClient>();
    sse_http->response = HttpResponse{200,
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"hel\"}\n\n"
        "data: {\"type\":\"response.output_text.delta\",\"delta\":\"lo\"}\n\n"
        "data: [DONE]\n", {{"content-type", "text/event-stream"}}};
    OpenAICompatibleModelClient sse("gpt-test", "https://example.test", "", std::nullopt, std::chrono::seconds(3), sse_http);
    const auto sse_result = sse.complete("prompt", 8);
    check(sse_result && sse_result.value() == "hello", "OpenAI SSE deltas concatenate");

    auto anthropic_http = std::make_shared<RecordingHttpClient>();
    anthropic_http->response = HttpResponse{200, R"({"content":[{"type":"text","text":"answer"}]})", {}};
    AnthropicCompatibleModelClient anthropic("claude-test", "https://example.test/v1", "key", 0.2,
        std::chrono::seconds(3), anthropic_http);
    const auto anthropic_result = anthropic.complete("prompt", 32);
    const auto anthropic_payload = parse_json(anthropic_http->body);
    check(anthropic_result && anthropic_result.value() == "answer", "Anthropic text content extracts");
    check(anthropic_http->url == "https://example.test/v1/messages" && anthropic_payload &&
        anthropic_payload.value().at("messages").is_array() && anthropic_payload.value().at("max_tokens").integer_or() == 32,
        "Anthropic Messages request schema stays intact");
}

void test_evaluation_contract() {
    const auto benchmark = load_benchmark(std::filesystem::path(RUNI_SOURCE_DIR) / "benchmarks" / "coding_tasks.json",
        std::filesystem::path(RUNI_SOURCE_DIR));
    check(benchmark && benchmark.value().at("schema_version").integer_or() == 1 &&
        benchmark.value().at("tasks").as_array().size() == 12, "fixed Agent benchmark schema validates all 12 tasks");
    JsonValue::Array rows{
        JsonValue::Object{{"passed", JsonValue(true)}, {"status", JsonValue("pass")}, {"verifier_passed", JsonValue(true)}, {"within_budget", JsonValue(true)}},
        JsonValue::Object{{"failure_category", JsonValue("verifier_failed")}, {"passed", JsonValue(false)}, {"status", JsonValue("fail")},
            {"verifier_passed", JsonValue(false)}, {"within_budget", JsonValue(false)}}};
    const auto summary = summarize_rows(rows);
    check(summary.at("total_tasks").integer_or() == 2 && summary.at("passed").integer_or() == 1 &&
        summary.at("failure_category_counts").at("verifier_failed").integer_or() == 1, "benchmark summary preserves pass and failure-category counts");
}

void test_agent_and_tools(const std::filesystem::path& workspace_root) {
    write_file(workspace_root / "README.md", "demo\n");
    write_file(workspace_root / "hello.txt", "alpha\nbeta\n");
    const auto workspace = WorkspaceContext::build(workspace_root, workspace_root);
    check(workspace.has_value(), "workspace fixture builds");
    if (!workspace) return;
    auto sessions = std::make_shared<SessionStore>(workspace_root / ".runi" / "sessions");
    auto runs = std::make_shared<RunStore>(workspace_root / ".runi" / "runs");
    auto model = std::make_shared<FakeModelClient>(std::vector<std::string>{
        R"(<tool>{"name":"read_file","args":{"path":"hello.txt","start":1,"end":1}}</tool>)",
        "<final>Done.</final>"});
    RuntimeOptions options;
    options.approval_policy = "auto";
    Runi agent(model, workspace.value(), sessions, runs, std::nullopt, options);
    const auto answer = agent.ask("Inspect hello.txt");
    check(answer && answer.value() == "Done.", "agent loop returns final answer after a tool call");
    check(agent.current_task_state != nullptr && agent.current_task_state->status == "completed" &&
        agent.current_task_state->stop_reason == "final_answer_returned", "agent TaskState reaches the same success contract");
    check(agent.session().history.size() == 3 && agent.session().history[1].role == "tool" &&
        agent.session().history[1].content.find("   1: alpha") != std::string::npos, "tool result is recorded in history");
    check(model->prompts.size() == 2 && model->prompts.front().find("You are runi") != std::string::npos &&
        model->prompts.back().find("[tool:read_file]") != std::string::npos, "model prompts preserve prefix and tool transcript flow");
    check(agent.current_task_state != nullptr && std::filesystem::exists(runs->report_path(agent.current_task_state->run_id)) &&
        std::filesystem::exists(runs->trace_path(agent.current_task_state->run_id)), "run report and JSONL trace persist");
    const auto report = agent.current_task_state == nullptr ? Result<JsonValue>::failure(make_error(ErrorCategory::Internal, "missing", "missing"))
        : runs->load_report(agent.current_task_state->run_id);
    check(report && report.value().at("status").string_or() == "completed" && report.value().at("tool_steps").integer_or() == 1,
        "report schema records status and tool step count");

    const auto escaped = agent.run_tool("read_file", {{"path", JsonValue("../outside.txt")}});
    check(escaped.find("path escapes workspace") != std::string::npos, "workspace guard rejects parent traversal");
    const auto unknown = agent.run_tool("not_a_tool", {});
    check(unknown == "error: unknown tool 'not_a_tool'", "unknown tools return structured feedback instead of terminating the loop");

    const auto write_result = agent.run_tool("write_file", {{"content", JsonValue("中文")}, {"path", JsonValue("note.txt")}});
    check(write_result == "wrote note.txt (2 chars)" && std::filesystem::exists(workspace_root / "note.txt"),
        "write_file reports Unicode character count and writes inside the workspace");
    const auto patch_result = agent.run_tool("patch_file", {{"new_text", JsonValue("gamma")}, {"old_text", JsonValue("beta")}, {"path", JsonValue("hello.txt")}});
    check(patch_result == "patched hello.txt", "patch_file performs one exact replacement");
}

void test_step_limit(const std::filesystem::path& workspace_root) {
    write_file(workspace_root / "README.md", "demo\n");
    const auto workspace = WorkspaceContext::build(workspace_root, workspace_root);
    if (!workspace) { check(false, "step-limit workspace builds"); return; }
    auto sessions = std::make_shared<SessionStore>(workspace_root / ".runi" / "sessions");
    auto runs = std::make_shared<RunStore>(workspace_root / ".runi" / "runs");
    auto model = std::make_shared<FakeModelClient>(std::vector<std::string>{R"(<tool>{"name":"read_file","args":{"path":"README.md"}}</tool>)"});
    RuntimeOptions options; options.approval_policy = "auto"; options.max_steps = 1;
    Runi agent(model, workspace.value(), sessions, runs, std::nullopt, options);
    const auto answer = agent.ask("Read once");
    check(answer && answer.value() == "Stopped after reaching the step limit without a final answer.", "step limit message is exact");
    check(agent.current_task_state != nullptr && agent.current_task_state->stop_reason == "step_limit_reached", "step limit stop reason persists");
}

}  // namespace

int main() {
    const auto base = std::filesystem::current_path() / "contract-test-workspaces";
    std::error_code error;
    const auto normalized = std::filesystem::weakly_canonical(std::filesystem::current_path(), error);
    const auto target = std::filesystem::absolute(base, error).lexically_normal();
    if (error || target.string().find(normalized.string()) != 0) {
        std::cerr << "unsafe test workspace target\n";
        return 2;
    }
    std::filesystem::remove_all(target, error);
    std::filesystem::create_directories(target);
    test_json_contract();
    test_action_protocol();
    test_provider_protocols();
    test_evaluation_contract();
    test_prompt_and_context_contract(target / "prompt-context");
    test_agent_and_tools(target / "agent");
    test_step_limit(target / "step-limit");
    std::filesystem::remove_all(target, error);
    if (failures != 0) {
        std::cerr << failures << " test(s) failed\n";
        return 1;
    }
    std::cout << "all contract tests passed\n";
    return 0;
}
