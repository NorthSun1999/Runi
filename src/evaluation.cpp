#include "runi/evaluation.hpp"

#include <algorithm>
#include <clocale>
#include <fstream>
#include <map>
#include <set>

#include "runi/core/json_codec.hpp"
#include "runi/core/sha256.hpp"
#include "runi/core/text.hpp"
#include "runi/core/time.hpp"
#include "runi/process_runner.hpp"
#include "runi/run_store.hpp"
#include "runi/runtime.hpp"
#include "runi/session_store.hpp"
#include "runi/tools.hpp"

namespace runi {
namespace {

const std::vector<std::string> kRequiredBenchmarkKeys{"schema_version", "tasks"};
const std::vector<std::string> kRequiredTaskKeys{
    "id", "prompt", "fixture_repo", "allowed_tools", "step_budget", "expected_artifact", "verifier", "category"};

std::string field(const JsonValue& value, std::string_view name) {
    const auto* item = value.find(name);
    return item == nullptr ? std::string{} : trim(item->string_or());
}

JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

std::vector<std::string> string_array(const JsonValue* value) {
    std::vector<std::string> result;
    if (value == nullptr || !value->is_array()) return result;
    for (const auto& item : value->as_array()) if (item.is_string()) result.push_back(trim(item.as_string()));
    return result;
}

Result<JsonValue> read_json(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<JsonValue>::failure(make_error(
        ErrorCategory::Configuration, "benchmark_read_failed", "Could not read benchmark: " + path.string()));
    const std::string content((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return parse_json(content);
}

std::vector<std::string> scripted_outputs(std::string_view id) {
    static const std::map<std::string, std::vector<std::string>, std::less<>> outputs{
        {"readme_intro_locked", {"<tool name=\"patch_file\" path=\"README.md\"><old_text>This is a placeholder benchmark fixture.</old_text><new_text>This fixture is a locked benchmark workspace.</new_text></tool>", "<final>Done.</final>"}},
        {"readme_schema_note", {"<tool name=\"patch_file\" path=\"README.md\"><old_text>- Placeholder note about the repo.</old_text><new_text>- The benchmark schema and baseline are fixed.</new_text></tool>", "<final>Done.</final>"}},
        {"readme_ordering_note", {"<tool name=\"patch_file\" path=\"README.md\"><old_text>- Placeholder note about the file layout.</old_text><new_text>- Deterministic file ordering keeps benchmark diffs stable.</new_text></tool>", "<final>Done.</final>"}},
        {"sample_beta_locked", {"<tool name=\"patch_file\" path=\"sample.txt\"><old_text>beta</old_text><new_text>beta-locked</new_text></tool>", "<final>Done.</final>"}},
        {"sample_gamma_locked", {"<tool name=\"patch_file\" path=\"sample.txt\"><old_text>gamma</old_text><new_text>gamma-locked</new_text></tool>", "<final>Done.</final>"}},
        {"sample_placeholder_delta", {"<tool name=\"patch_file\" path=\"sample.txt\"><old_text>placeholder</old_text><new_text>delta</new_text></tool>", "<final>Done.</final>"}},
        {"invalid_patch_recovery", {R"(<tool>{"name":"patch_file","args":{"path":"README.md","old_text":"This is a placeholder benchmark fixture."}}</tool>)",
            "<tool name=\"patch_file\" path=\"README.md\"><old_text>This is a placeholder benchmark fixture.</old_text><new_text>This fixture recovered after invalid patch args.</new_text></tool>", "<final>Done.</final>"}},
        {"path_escape_recovery", {R"(<tool>{"name":"read_file","args":{"path":"../outside.txt","start":1,"end":1}}</tool>)",
            "<tool name=\"patch_file\" path=\"sample.txt\"><old_text>alpha</old_text><new_text>alpha-guarded</new_text></tool>", "<final>Done.</final>"}},
        {"repeated_read_recovery", {R"(<tool>{"name":"read_file","args":{"path":"sample.txt","start":1,"end":4}}</tool>)",
            R"(<tool>{"name":"read_file","args":{"path":"sample.txt","start":1,"end":4}}</tool>)",
            R"(<tool>{"name":"read_file","args":{"path":"sample.txt","start":1,"end":4}}</tool>)",
            "<tool name=\"patch_file\" path=\"sample.txt\"><old_text>placeholder</old_text><new_text>repeat-guarded</new_text></tool>", "<final>Done.</final>"}},
        {"context_reduction_checkpoint", {"<final>Done.</final>"}},
        {"freshness_reanchor_resume", {"<final>Done.</final>"}},
        {"workspace_mismatch_resume", {"<final>Done.</final>"}},
        {"durable_promotion_accept", {"<final>Project convention: Preserve benchmark regression artifacts under artifacts/.\nDecision: Keep harness regression deterministic and reproducible.</final>"}},
        {"durable_promotion_reject", {"<final>Project convention: Keep verifier outcomes stable across reruns.\nDependency: API key is sk-benchmark-secret.\nDecision: Current goal is debug the harness.</final>"}},
    };
    const auto iterator = outputs.find(id);
    return iterator == outputs.end() ? std::vector<std::string>{} : iterator->second;
}

std::string artifact_name(const JsonValue& task) {
    const auto fixture = std::filesystem::path(field(task, "fixture_repo")).filename().string();
    if (fixture == "bench_repo_readme") return "README.md";
    if (fixture == "bench_repo_patch") return "sample.txt";
    return {};
}

std::string relative_to(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code error;
    return std::filesystem::relative(path, root, error).generic_string();
}

bool row_bool(const JsonValue& row, std::string_view name) {
    const auto* value = row.find(name);
    return value != nullptr && value->bool_or();
}

JsonValue checkpoint_payload(std::string id, std::string goal, std::string next_step, JsonValue identity,
    JsonValue::Array key_files = {}, JsonValue freshness = JsonValue::Object{}, std::string summary = {}) {
    return JsonValue::Object{
        {"checkpoint_id", JsonValue(std::move(id))}, {"completed", JsonValue::Array{}}, {"created_at", JsonValue("2026-04-15T08:00:00+00:00")},
        {"current_blocker", JsonValue("")}, {"current_goal", JsonValue(goal)}, {"excluded", JsonValue::Array{}},
        {"freshness", std::move(freshness)}, {"key_files", JsonValue(std::move(key_files))}, {"next_step", JsonValue(std::move(next_step))},
        {"parent_checkpoint_id", JsonValue("")}, {"runtime_identity", std::move(identity)}, {"schema_version", JsonValue("phase1-v1")},
        {"summary", JsonValue(summary.empty() ? goal : summary)}};
}

std::string fixture_snapshot_id(const JsonValue::Array& tasks, const std::filesystem::path& repo_root) {
    std::set<std::filesystem::path> fixtures;
    for (const auto& task : tasks) fixtures.insert(std::filesystem::weakly_canonical(repo_root / field(task, "fixture_repo")));
    std::string material;
    for (const auto& fixture : fixtures) {
        std::vector<std::filesystem::path> files;
        std::error_code error;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(fixture, error)) if (entry.is_regular_file()) files.push_back(entry.path());
        std::sort(files.begin(), files.end());
        for (const auto& path : files) {
            material += fixture.filename().generic_string(); material.push_back('\0');
            material += relative_to(path, fixture); material.push_back('\0');
            std::ifstream input(path, std::ios::binary);
            material.append((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()); material.push_back('\0');
        }
    }
    return "sha256:" + sha256(material);
}

}  // namespace

Result<JsonValue> validate_benchmark(const JsonValue& data, const std::filesystem::path& repo_root) {
    if (!data.is_object()) return Result<JsonValue>::failure(make_error(
        ErrorCategory::Validation, "invalid_benchmark", "benchmark must be a mapping"));
    for (const auto& key : kRequiredBenchmarkKeys) if (!data.contains(key)) return Result<JsonValue>::failure(make_error(
        ErrorCategory::Validation, "missing_required_key", "benchmark is missing required keys: " + key));
    if (data.at("schema_version").integer_or() != kBenchmarkSchemaVersion) return Result<JsonValue>::failure(make_error(
        ErrorCategory::Validation, "unsupported_schema", "unsupported benchmark schema_version"));
    if (!data.at("tasks").is_array() || data.at("tasks").as_array().empty()) return Result<JsonValue>::failure(make_error(
        ErrorCategory::Validation, "invalid_tasks", "benchmark tasks must be a non-empty list"));
    const auto legal = legal_tool_names();
    std::set<std::string, std::less<>> ids;
    JsonValue::Array tasks;
    for (std::size_t index = 0; index < data.at("tasks").as_array().size(); ++index) {
        const auto& task = data.at("tasks").as_array()[index];
        if (!task.is_object()) return Result<JsonValue>::failure(make_error(
            ErrorCategory::Validation, "invalid_task", "benchmark task at index " + std::to_string(index) + " must be a mapping"));
        for (const auto& key : kRequiredTaskKeys) if (!task.contains(key)) return Result<JsonValue>::failure(make_error(
            ErrorCategory::Validation, "missing_required_task_key", "benchmark task is missing required keys: " + key));
        auto normalized = task.as_object();
        const auto id = field(task, "id");
        if (id.empty() || !ids.insert(id).second) return Result<JsonValue>::failure(make_error(
            ErrorCategory::Validation, "invalid_task_id", id.empty() ? "benchmark task has an empty id" : "duplicate benchmark task id: " + id));
        if (!std::filesystem::is_directory(repo_root / field(task, "fixture_repo"))) return Result<JsonValue>::failure(make_error(
            ErrorCategory::Validation, "missing_fixture", "benchmark task " + id + " fixture repo does not exist: " + field(task, "fixture_repo")));
        auto allowed = string_array(task.find("allowed_tools"));
        if (allowed.empty()) return Result<JsonValue>::failure(make_error(
            ErrorCategory::Validation, "invalid_allowed_tools", "benchmark task " + id + " allowed_tools must be a non-empty list"));
        for (const auto& name : allowed) if (name.empty() || std::find(legal.begin(), legal.end(), name) == legal.end()) return Result<JsonValue>::failure(make_error(
            ErrorCategory::Validation, "unknown_allowed_tool", "benchmark task " + id + " has an unknown allowed_tools entry: " + name));
        const auto budget = task.at("step_budget").integer_or();
        if (budget < 1) return Result<JsonValue>::failure(make_error(
            ErrorCategory::Validation, "invalid_step_budget", "benchmark task " + id + " step_budget must be positive"));
        normalized["id"] = JsonValue(id); normalized["prompt"] = JsonValue(field(task, "prompt"));
        normalized["fixture_repo"] = JsonValue(field(task, "fixture_repo")); normalized["allowed_tools"] = strings(allowed);
        normalized["step_budget"] = JsonValue(budget); normalized["expected_artifact"] = JsonValue(field(task, "expected_artifact"));
        normalized["verifier"] = JsonValue(field(task, "verifier")); normalized["category"] = JsonValue(field(task, "category"));
        tasks.emplace_back(std::move(normalized));
    }
    auto normalized = data.as_object();
    normalized["schema_version"] = JsonValue(kBenchmarkSchemaVersion);
    normalized["tasks"] = JsonValue(std::move(tasks));
    return Result<JsonValue>::success(JsonValue(std::move(normalized)));
}

Result<JsonValue> load_benchmark(const std::filesystem::path& path, std::optional<std::filesystem::path> repo_root) {
    const auto loaded = read_json(path);
    if (!loaded) return loaded;
    const auto root = repo_root.value_or(std::filesystem::absolute(path).parent_path().parent_path());
    return validate_benchmark(loaded.value(), root);
}

JsonValue summarize_rows(const JsonValue::Array& rows) {
    std::int64_t passed = 0, within = 0, verifier = 0;
    JsonValue::Object categories;
    for (const auto& row : rows) {
        const bool success = row_bool(row, "passed") || field(row, "status") == "pass";
        if (success) ++passed;
        else {
            const auto category = field(row, "failure_category").empty() ? std::string("unknown") : field(row, "failure_category");
            categories[category] = JsonValue((categories.contains(category) ? categories.at(category).integer_or() : 0) + 1);
        }
        if (row_bool(row, "within_budget")) ++within;
        if (row_bool(row, "verifier_passed")) ++verifier;
    }
    const auto total = static_cast<std::int64_t>(rows.size());
    const auto ratio = [total](std::int64_t value) { return total == 0 ? 0.0 : static_cast<double>(value) / static_cast<double>(total); };
    return JsonValue::Object{{"failed", JsonValue(total - passed)}, {"failure_category_counts", JsonValue(std::move(categories))},
        {"pass_rate", JsonValue(ratio(passed))}, {"passed", JsonValue(passed)}, {"total_tasks", JsonValue(total)},
        {"verifier_pass_rate", JsonValue(ratio(verifier))}, {"verifier_passes", JsonValue(verifier)},
        {"within_budget", JsonValue(within)}, {"within_budget_rate", JsonValue(ratio(within))}};
}

BenchmarkEvaluator::BenchmarkEvaluator(BenchmarkOptions options) : options_(std::move(options)) {
    options_.benchmark_path = std::filesystem::absolute(options_.benchmark_path);
    options_.artifact_path = std::filesystem::absolute(options_.artifact_path);
    options_.workspace_root = std::filesystem::absolute(options_.workspace_root);
    repo_root_ = options_.benchmark_path.parent_path().parent_path();
}

Result<JsonValue> BenchmarkEvaluator::load() const { return load_benchmark(options_.benchmark_path, repo_root_); }

Result<void> BenchmarkEvaluator::apply_task_setup(Runi& agent, const JsonValue& task, const std::filesystem::path& fixture_root) const {
    const auto* setup = task.find("setup");
    if (setup == nullptr || !setup->is_object()) return Result<void>::success();
    const auto kind = field(*setup, "kind");
    if (kind == "context_reduction") {
        const auto history_count = setup->find("history_count") == nullptr ? 12 : setup->find("history_count")->integer_or(12);
        const auto note_count = setup->find("note_count") == nullptr ? 6 : setup->find("note_count")->integer_or(6);
        for (std::int64_t index = 0; index < history_count; ++index) {
            HistoryItem item; item.role = index % 2 == 0 ? "user" : "assistant";
            item.content = "benchmark-history-" + std::to_string(index) + "-" + std::string(220, 'A');
            item.created_at = "2026-04-15T09:00:00+00:00";
            const auto recorded = agent.record(std::move(item)); if (!recorded) return recorded;
        }
        for (std::int64_t index = 0; index < note_count; ++index) agent.memory().append_note(
            "benchmark-note-" + std::to_string(index) + "-" + std::string(180, 'B'), {"recall"}, {}, "2026-04-15T10:00:00+00:00");
        if (const auto* total = setup->find("total_budget"); total != nullptr) agent.context_manager().total_budget = static_cast<std::size_t>(total->integer_or(900));
        if (const auto* budgets = setup->find("section_budgets"); budgets != nullptr && budgets->is_object()) {
            agent.context_manager().section_budgets.clear();
            for (const auto& [name, value] : budgets->as_object()) agent.context_manager().section_budgets[name] = static_cast<std::size_t>(value.integer_or());
        }
        return agent.persist_session();
    }
    if (kind == "freshness_mismatch") {
        const auto path = field(*setup, "path").empty() ? std::string("sample.txt") : field(*setup, "path");
        const auto summary = field(*setup, "summary").empty() ? path + ": stale benchmark summary" : field(*setup, "summary");
        agent.memory().set_file_summary(path, summary).remember_file(path);
        auto& session = agent.mutable_session();
        session.memory = agent.memory().to_json();
        const auto freshness = session.memory.at("file_summaries").at(path).at("freshness");
        auto checkpoint = checkpoint_payload("ckpt_freshness", "Re-anchor stale benchmark file state", "Re-read " + path,
            JsonValue::Object{{"workspace_fingerprint", JsonValue(agent.workspace().fingerprint())}},
            JsonValue::Array{JsonValue::Object{{"freshness", freshness}, {"path", JsonValue(path)}}},
            JsonValue::Object{{path, freshness}}, "stale benchmark checkpoint");
        session.checkpoints = JsonValue::Object{{"current_id", JsonValue("ckpt_freshness")},
            {"items", JsonValue::Object{{"ckpt_freshness", checkpoint}}}};
        const auto persisted = agent.persist_session(); if (!persisted) return persisted;
        const auto changed = write_text_file(fixture_root / path,
            field(*setup, "mutated_text").empty() ? "alpha\nbeta\nstale-updated\nplaceholder\n" : field(*setup, "mutated_text"));
        return changed;
    }
    if (kind == "workspace_mismatch") {
        auto checkpoint = checkpoint_payload("ckpt_workspace", "Recover after benchmark workspace drift",
            "Rebuild runtime state from a fresh checkpoint", JsonValue::Object{{"workspace_fingerprint", JsonValue("outdated-benchmark-fingerprint")}},
            {}, JsonValue::Object{}, "workspace drift benchmark checkpoint");
        agent.mutable_session().checkpoints = JsonValue::Object{{"current_id", JsonValue("ckpt_workspace")},
            {"items", JsonValue::Object{{"ckpt_workspace", checkpoint}}}};
        return agent.persist_session();
    }
    return Result<void>::success();
}

Result<JsonValue> BenchmarkEvaluator::run_task(const JsonValue& task) {
    if (!task.is_object()) return Result<JsonValue>::failure(make_error(ErrorCategory::Validation, "invalid_task", "benchmark task must be a mapping"));
    const auto source = repo_root_ / field(task, "fixture_repo");
    std::error_code error;
    const auto workspace_root = std::filesystem::absolute(options_.workspace_root, error).lexically_normal();
    const auto copy_root = std::filesystem::absolute(workspace_root / field(task, "id") / source.filename(), error).lexically_normal();
    const auto relative_copy = std::filesystem::relative(copy_root, workspace_root, error);
    if (error || relative_copy.is_absolute() || (!relative_copy.empty() && *relative_copy.begin() == "..")) {
        return Result<JsonValue>::failure(make_error(ErrorCategory::PathViolation, "benchmark_workspace_escape",
            "benchmark task workspace escapes workspace_root: " + field(task, "id")));
    }
    std::filesystem::remove_all(copy_root, error); error.clear();
    std::filesystem::create_directories(copy_root.parent_path(), error);
    std::filesystem::copy(source, copy_root, std::filesystem::copy_options::recursive | std::filesystem::copy_options::overwrite_existing, error);
    if (error) return Result<JsonValue>::failure(make_error(ErrorCategory::Persistence, "fixture_copy_failed", "Could not copy benchmark fixture: " + error.message()));
    const auto workspace = WorkspaceContext::build(copy_root, copy_root);
    if (!workspace) return Result<JsonValue>::failure(workspace.error());
    auto sessions = std::make_shared<SessionStore>(copy_root / ".runi" / "sessions");
    auto runs = std::make_shared<RunStore>(copy_root / ".runi" / "runs");
    auto model = options_.model_client_factory ? options_.model_client_factory(task, workspace.value())
        : std::make_shared<FakeModelClient>(scripted_outputs(field(task, "id")));
    if (!model) return Result<JsonValue>::failure(make_error(ErrorCategory::Configuration, "missing_model", "model client factory returned null"));
    RuntimeOptions runtime_options; runtime_options.approval_policy = "auto";
    runtime_options.max_steps = static_cast<std::size_t>(task.at("step_budget").integer_or());
    runtime_options.max_new_tokens = options_.max_new_tokens;
    runtime_options.allowed_tools = string_array(task.find("allowed_tools"));
    Runi agent(model, workspace.value(), sessions, runs, std::nullopt, runtime_options);
    const auto setup = apply_task_setup(agent, task, copy_root); if (!setup) return Result<JsonValue>::failure(setup.error());
    const bool initial_history_empty = agent.session().history.empty();
    const bool initial_memory_empty = agent.memory().is_effectively_empty();
    const auto& initial_memory = agent.memory().to_json();
    const bool initial_task_empty = field(initial_memory.at("working"), "task_summary").empty();
    const bool initial_notes_empty = initial_memory.at("episodic_notes").as_array().empty();
    const auto final = agent.ask(field(task, "prompt"));
    if (!final) return Result<JsonValue>::failure(final.error());
    if (agent.current_task_state == nullptr) return Result<JsonValue>::failure(make_error(ErrorCategory::Internal, "missing_task_state", "benchmark run produced no task state"));
    const auto& state = *agent.current_task_state;
    const auto report = runs->load_report(state.run_id); if (!report) return Result<JsonValue>::failure(report.error());
    const auto artifact = artifact_name(task);
    const auto artifact_path = copy_root / artifact;
    const bool artifact_exists = !artifact.empty() && std::filesystem::exists(artifact_path);
    std::string digest;
    if (artifact_exists) { const auto hashed = sha256_file(artifact_path); if (hashed) digest = "sha256:" + hashed.value(); }
    ProcessRunner runner;
    const auto verifier_command = field(task, "verifier");
    const auto verifier = runner.run(ProcessRequest{verifier_command, copy_root, {}, std::chrono::seconds(120), true});
    const int verifier_exit = verifier ? verifier.value().exit_code : 1;
    const bool verifier_passed = verifier_exit == 0;
    const bool within_budget = state.tool_steps <= runtime_options.max_steps;
    const bool non_failure = state.stop_reason == kStopFinalAnswerReturned;
    const bool passed = within_budget && verifier_passed && artifact_exists && non_failure;
    const auto failure = passed ? std::string{} : failure_category(within_budget, verifier_passed, artifact_exists, non_failure);
    return Result<JsonValue>::success(JsonValue::Object{
        {"allowed_tools", strings(*runtime_options.allowed_tools)}, {"artifact_digest", JsonValue(digest)},
        {"artifact_exists", JsonValue(artifact_exists)}, {"artifact_path", JsonValue(artifact)}, {"attempts", JsonValue(state.attempts)},
        {"category", JsonValue(field(task, "category"))}, {"expected_artifact", JsonValue(field(task, "expected_artifact"))},
        {"expected_artifact_exists", JsonValue(artifact_exists)}, {"failure_category", passed ? JsonValue(nullptr) : JsonValue(failure)},
        {"final_answer", JsonValue(final.value())}, {"fixture_copy_relpath", JsonValue(relative_to(copy_root, options_.workspace_root))},
        {"fixture_repo", JsonValue(field(task, "fixture_repo"))}, {"id", JsonValue(field(task, "id"))},
        {"initial_episodic_notes_empty", JsonValue(initial_notes_empty)}, {"initial_history_empty", JsonValue(initial_history_empty)},
        {"initial_memory_empty", JsonValue(initial_memory_empty)}, {"initial_task_summary_empty", JsonValue(initial_task_empty)},
        {"non_failure_stop_reason", JsonValue(non_failure)}, {"passed", JsonValue(passed)}, {"prompt", JsonValue(field(task, "prompt"))},
        {"report", report.value()}, {"report_relpath", JsonValue(relative_to(runs->report_path(state.run_id), options_.workspace_root))},
        {"run_dir_relpath", JsonValue(relative_to(agent.current_run_dir, options_.workspace_root))}, {"run_id", JsonValue(state.run_id)},
        {"status", JsonValue(passed ? "pass" : "fail")}, {"step_budget", JsonValue(runtime_options.max_steps)},
        {"stop_reason", JsonValue(state.stop_reason)}, {"task_state", state.to_json()},
        {"task_state_relpath", JsonValue(relative_to(runs->task_state_path(state.run_id), options_.workspace_root))},
        {"tool_steps", JsonValue(state.tool_steps)}, {"verifier", JsonValue(field(task, "verifier"))},
        {"verifier_exit_code", JsonValue(verifier_exit)}, {"verifier_passed", JsonValue(verifier_passed)},
        {"verifier_stderr", JsonValue(verifier ? verifier.value().stderr_text : verifier.error().message)},
        {"verifier_stdout", JsonValue(verifier ? verifier.value().stdout_text : std::string{})}, {"within_budget", JsonValue(within_budget)}});
}

std::string BenchmarkEvaluator::failure_category(bool within_budget, bool verifier_passed,
    bool artifact_exists, bool non_failure_stop_reason) {
    if (!artifact_exists) return "missing_artifact";
    if (!within_budget) return "budget_exceeded";
    if (!verifier_passed) return "verifier_failed";
    if (!non_failure_stop_reason) return "failure_stop_reason";
    return "unknown";
}

Result<void> BenchmarkEvaluator::write_artifact(const JsonValue& artifact) const {
    std::error_code error;
    std::filesystem::create_directories(options_.artifact_path.parent_path(), error);
    std::ofstream output(options_.artifact_path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure(make_error(ErrorCategory::Persistence, "benchmark_write_failed",
        "Could not write benchmark artifact: " + options_.artifact_path.string()));
    output << dump_json(artifact, 2, true) << '\n';
    return output ? Result<void>::success() : Result<void>::failure(make_error(
        ErrorCategory::Persistence, "benchmark_write_failed", "Could not write benchmark artifact: " + options_.artifact_path.string()));
}

Result<JsonValue> BenchmarkEvaluator::run() {
    const auto benchmark = load(); if (!benchmark) return benchmark;
    JsonValue::Array rows;
    for (const auto& task : benchmark.value().at("tasks").as_array()) {
        const auto row = run_task(task); if (!row) return row;
        rows.push_back(row.value());
    }
    const auto summary = summarize_rows(rows);
    ProcessRunner runner;
    const auto git_sha = runner.run(ProcessRequest{"git rev-parse HEAD", repo_root_, {}, std::chrono::seconds(5), true});
    const auto git_branch = runner.run(ProcessRequest{"git branch --show-current", repo_root_, {}, std::chrono::seconds(5), true});
    const auto* locale = std::setlocale(LC_CTYPE, nullptr);
    JsonValue artifact{JsonValue::Object{
        {"benchmark", JsonValue::Object{{"source", JsonValue(relative_to(options_.benchmark_path, repo_root_))},
            {"task_count", JsonValue(benchmark.value().at("tasks").as_array().size())}}},
        {"captured_at", JsonValue(now_utc())}, {"failure_category_counts", summary.at("failure_category_counts")},
        {"reproducibility", JsonValue::Object{{"decoding", JsonValue::Object{{"max_new_tokens", JsonValue(options_.max_new_tokens)},
            {"temperature", JsonValue(options_.temperature)}, {"top_p", JsonValue(options_.top_p)}}},
            {"fixture_snapshot_id", JsonValue(fixture_snapshot_id(benchmark.value().at("tasks").as_array(), repo_root_))},
            {"locale", JsonValue(locale == nullptr ? "C" : locale)}, {"model_name", JsonValue(options_.model_name)},
            {"model_version", JsonValue(options_.model_version)}, {"timezone", JsonValue(options_.timezone_name)}}},
        {"rows", JsonValue(rows)}, {"runtime", JsonValue::Object{{"branch", JsonValue(git_branch ? trim(git_branch.value().stdout_text) : std::string{})},
            {"commit_sha", JsonValue(git_sha ? trim(git_sha.value().stdout_text) : std::string{})}}},
        {"schema_version", JsonValue(kBenchmarkSchemaVersion)}, {"summary", summary}}};
    const auto written = write_artifact(artifact); if (!written) return Result<JsonValue>::failure(written.error());
    return Result<JsonValue>::success(std::move(artifact));
}

Result<JsonValue> run_fixed_benchmark(BenchmarkOptions options) { return BenchmarkEvaluator(std::move(options)).run(); }
Result<JsonValue> run_harness_regression_v2(BenchmarkOptions options) { return run_fixed_benchmark(std::move(options)); }

}  // namespace runi
