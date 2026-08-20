#include "runi/evaluation/evaluation.hpp"

#include <algorithm>
#include <clocale>
#include <fstream>
#include <limits>
#include <map>
#include <numeric>
#include <set>

#include "runi/core/json_codec.hpp"
#include "runi/core/sha256.hpp"
#include "runi/core/text.hpp"
#include "runi/core/time.hpp"
#include "runi/tool/process_runner.hpp"
#include "runi/state/run_store.hpp"
#include "runi/agent/runtime.hpp"
#include "runi/state/session_store.hpp"
#include "runi/tool/tools.hpp"

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

struct CountLevel {
    std::string id;
    std::size_t count{0};
};

struct RequestLevel {
    std::string id;
    std::string text;
};

struct ContextMatrixSpec {
    std::size_t repetitions{0};
    std::vector<CountLevel> history_levels;
    std::vector<CountLevel> note_levels;
    std::vector<RequestLevel> request_levels;
    std::string history_prefix;
    std::string history_payload_char;
    std::size_t history_payload_chars{0};
    std::string note_prefix;
    std::string note_payload_char;
    std::size_t note_payload_chars{0};
    std::string note_tag;
    std::size_t total_budget{0};
    std::map<std::string, std::size_t, std::less<>> section_budgets;
    std::map<std::string, std::size_t, std::less<>> section_floors;
    std::vector<std::string> reduction_order;
    JsonValue definition;
};

struct ContextRunRecord {
    JsonValue row;
    std::size_t raw_prompt_chars{0};
    std::size_t managed_prompt_chars{0};
    double compression_ratio{0.0};
    bool current_request_preserved{false};
    bool prompt_budget_satisfied{false};
    bool reduction_order_valid{false};
    bool section_floors_respected{false};
    bool dynamic_reduction_triggered{false};
};

class EphemeralSessionStore final : public ISessionStore {
public:
    explicit EphemeralSessionStore(std::filesystem::path root) : root_(std::move(root)) {}
    std::filesystem::path path(std::string_view session_id) const override { return root_ / (std::string(session_id) + ".json"); }
    Result<std::filesystem::path> save(const SessionState& session) override {
        session_ = session;
        return Result<std::filesystem::path>::success(path(session.id));
    }
    Result<SessionState> load(std::string_view session_id) const override {
        if (session_.has_value() && session_->id == session_id) return Result<SessionState>::success(*session_);
        return Result<SessionState>::failure(make_error(ErrorCategory::Persistence, "session_not_found", "ephemeral session not found"));
    }
    std::optional<std::string> latest() const override {
        return session_.has_value() ? std::optional<std::string>(session_->id) : std::nullopt;
    }

private:
    std::filesystem::path root_;
    std::optional<SessionState> session_;
};

class EphemeralRunStore final : public IRunStore {
public:
    explicit EphemeralRunStore(std::filesystem::path root) : root_(std::move(root)) {}
    std::filesystem::path run_dir(std::string_view run_id) const override { return root_ / std::string(run_id); }
    Result<std::filesystem::path> start_run(const TaskState& task_state) override {
        return Result<std::filesystem::path>::success(run_dir(task_state.run_id));
    }
    Result<std::filesystem::path> write_task_state(const TaskState& task_state) override {
        return Result<std::filesystem::path>::success(run_dir(task_state.run_id) / "task_state.json");
    }
    Result<std::filesystem::path> append_trace(const TaskState& task_state, const JsonValue&) override {
        return Result<std::filesystem::path>::success(run_dir(task_state.run_id) / "trace.jsonl");
    }
    Result<std::filesystem::path> write_report(const TaskState& task_state, const JsonValue&) override {
        return Result<std::filesystem::path>::success(run_dir(task_state.run_id) / "report.json");
    }

private:
    std::filesystem::path root_;
};

Result<ContextMatrixSpec> parse_context_matrix(const JsonValue& data) {
    const auto invalid = [](std::string message) {
        return Result<ContextMatrixSpec>::failure(make_error(
            ErrorCategory::Validation, "invalid_context_matrix", std::move(message)));
    };
    if (!data.is_object()) return invalid("context ablation matrix must be an object");
    if (data.find("schema_version") == nullptr || data.at("schema_version").integer_or() != kContextAblationSchemaVersion) {
        return invalid("unsupported context ablation schema_version");
    }
    if (field(data, "artifact_type") != "context-ablation-matrix") {
        return invalid("artifact_type must be context-ablation-matrix");
    }
    ContextMatrixSpec spec;
    spec.definition = data;
    spec.repetitions = static_cast<std::size_t>(std::max<std::int64_t>(0, data.at("repetitions").integer_or()));
    if (spec.repetitions == 0) return invalid("repetitions must be positive");

    const auto parse_count_levels = [&](std::string_view key, std::vector<CountLevel>& target) -> Result<void> {
        const auto* levels = data.find(key);
        if (levels == nullptr || !levels->is_array() || levels->as_array().empty()) {
            return Result<void>::failure(make_error(ErrorCategory::Validation, "invalid_context_matrix",
                std::string(key) + " must be a non-empty array"));
        }
        std::set<std::string, std::less<>> ids;
        for (const auto& level : levels->as_array()) {
            const auto id = field(level, "id");
            const auto count = level.find("count") == nullptr ? 0 : level.at("count").integer_or();
            if (!level.is_object() || id.empty() || count <= 0 || !ids.insert(id).second) {
                return Result<void>::failure(make_error(ErrorCategory::Validation, "invalid_context_matrix",
                    std::string(key) + " contains an invalid or duplicate level"));
            }
            target.push_back(CountLevel{id, static_cast<std::size_t>(count)});
        }
        return Result<void>::success();
    };
    const auto history = parse_count_levels("history_levels", spec.history_levels); if (!history) return invalid(history.error().message);
    const auto notes = parse_count_levels("note_levels", spec.note_levels); if (!notes) return invalid(notes.error().message);

    const auto* requests = data.find("request_levels");
    if (requests == nullptr || !requests->is_array() || requests->as_array().empty()) return invalid("request_levels must be a non-empty array");
    std::set<std::string, std::less<>> request_ids;
    for (const auto& request : requests->as_array()) {
        const auto id = field(request, "id");
        const auto text = field(request, "text");
        if (!request.is_object() || id.empty() || text.empty() || !request_ids.insert(id).second) {
            return invalid("request_levels contains an invalid or duplicate level");
        }
        spec.request_levels.push_back(RequestLevel{id, text});
    }

    const auto* generator = data.find("generator");
    if (generator == nullptr || !generator->is_object()) return invalid("generator must be an object");
    spec.history_prefix = field(*generator, "history_prefix");
    spec.history_payload_char = field(*generator, "history_payload_char");
    spec.history_payload_chars = static_cast<std::size_t>(std::max<std::int64_t>(0,
        generator->find("history_payload_chars") == nullptr ? 0 : generator->at("history_payload_chars").integer_or()));
    spec.note_prefix = field(*generator, "note_prefix");
    spec.note_payload_char = field(*generator, "note_payload_char");
    spec.note_payload_chars = static_cast<std::size_t>(std::max<std::int64_t>(0,
        generator->find("note_payload_chars") == nullptr ? 0 : generator->at("note_payload_chars").integer_or()));
    spec.note_tag = field(*generator, "note_tag");
    if (spec.history_prefix.empty() || spec.history_payload_char.size() != 1 || spec.history_payload_chars == 0 ||
        spec.note_prefix.empty() || spec.note_payload_char.size() != 1 || spec.note_payload_chars == 0 || spec.note_tag.empty()) {
        return invalid("generator prefixes, one-byte payload chars, positive payload sizes, and note_tag are required");
    }

    const auto* budget = data.find("budget");
    if (budget == nullptr || !budget->is_object()) return invalid("budget must be an object");
    spec.total_budget = static_cast<std::size_t>(std::max<std::int64_t>(0,
        budget->find("total_chars") == nullptr ? 0 : budget->at("total_chars").integer_or()));
    const auto* sections = budget->find("section_chars");
    if (spec.total_budget == 0 || sections == nullptr || !sections->is_object()) return invalid("budget total_chars and section_chars are required");
    for (const auto& name : {"prefix", "memory", "relevant_memory", "history"}) {
        const auto* value = sections->find(name);
        if (value == nullptr || value->integer_or() <= 0) return invalid("section_chars is missing a positive budget for " + std::string(name));
        const auto section_budget = static_cast<std::size_t>(value->integer_or());
        spec.section_budgets.emplace(name, section_budget);
        spec.section_floors.emplace(name, std::max<std::size_t>(20, section_budget / 4));
    }
    const auto* order = budget->find("reduction_order");
    if (order == nullptr || !order->is_array() || order->as_array().empty()) return invalid("reduction_order must be a non-empty array");
    std::set<std::string, std::less<>> reduction_names;
    for (const auto& item : order->as_array()) {
        const auto name = trim(item.string_or());
        if (!spec.section_budgets.contains(name) || !reduction_names.insert(name).second) {
            return invalid("reduction_order contains an unknown or duplicate section");
        }
        spec.reduction_order.push_back(name);
    }

    const auto config_count = spec.history_levels.size() * spec.note_levels.size() * spec.request_levels.size();
    if (config_count != 12) return invalid("the fixed context ablation matrix must contain exactly 12 configurations");
    return Result<ContextMatrixSpec>::success(std::move(spec));
}

JsonValue size_map(const std::map<std::string, std::size_t, std::less<>>& values) {
    JsonValue::Object result;
    for (const auto& [name, value] : values) result.emplace(name, JsonValue(value));
    return JsonValue(std::move(result));
}

std::string compiler_identity() {
#if defined(__clang__)
    return std::string("clang ") + __clang_version__;
#elif defined(__GNUC__)
    return std::string("gcc ") + __VERSION__;
#elif defined(_MSC_VER)
    return "msvc " + std::to_string(_MSC_VER);
#else
    return "unknown";
#endif
}

std::string platform_identity() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

bool reduction_order_is_valid(const JsonValue& metadata, const std::vector<std::string>& order) {
    const auto* reductions = metadata.find("budget_reductions");
    if (reductions == nullptr || !reductions->is_array()) return false;
    bool first = true;
    std::size_t previous = 0;
    for (const auto& reduction : reductions->as_array()) {
        const auto name = field(reduction, "section");
        const auto found = std::find(order.begin(), order.end(), name);
        if (found == order.end()) return false;
        const auto index = static_cast<std::size_t>(std::distance(order.begin(), found));
        if (!first && index < previous) return false;
        first = false;
        previous = index;
    }
    return true;
}

bool section_floors_are_respected(const JsonValue& metadata,
    const std::map<std::string, std::size_t, std::less<>>& floors) {
    const auto* budgets = metadata.find("section_budgets");
    if (budgets == nullptr || !budgets->is_object()) return false;
    for (const auto& [name, floor] : floors) {
        const auto* value = budgets->find(name);
        if (value == nullptr || value->integer_or() < static_cast<std::int64_t>(floor)) return false;
    }
    return true;
}

bool current_request_is_preserved(const ContextBuildResult& result, std::string_view request) {
    const auto* current = result.metadata.find("current_request");
    if (current == nullptr || !current->is_object()) return false;
    const auto expected = "Current user request:\n" + std::string(request);
    return field(*current, "text") == request &&
        current->at("raw_chars").integer_or() == static_cast<std::int64_t>(utf8_length(request)) &&
        current->at("rendered_chars").integer_or() == static_cast<std::int64_t>(utf8_length(request)) &&
        result.prompt.ends_with(expected);
}

SessionState context_matrix_session(const ContextMatrixSpec& spec, const std::filesystem::path& root,
    std::size_t history_count, std::size_t note_count) {
    auto session = SessionState::create(root.string());
    LayeredMemory memory(default_memory_state(), root);
    for (std::size_t index = 0; index < note_count; ++index) {
        const auto minute = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
        memory.append_note(spec.note_prefix + std::to_string(index) + "-" +
            std::string(spec.note_payload_chars, spec.note_payload_char.front()), {spec.note_tag}, {},
            "2026-04-08T10:" + minute + ":00+00:00");
    }
    session.memory = memory.to_json();
    for (std::size_t index = 0; index < history_count; ++index) {
        const auto minute = index < 10 ? "0" + std::to_string(index) : std::to_string(index);
        HistoryItem item;
        item.role = index % 2 == 0 ? "user" : "assistant";
        item.content = spec.history_prefix + std::to_string(index) + "-" +
            std::string(spec.history_payload_chars, spec.history_payload_char.front());
        item.created_at = "2026-04-08T11:" + minute + ":00+00:00";
        session.history.push_back(std::move(item));
    }
    return session;
}

Result<ContextBuildResult> build_context_variant(const ContextMatrixSpec& spec, const WorkspaceContext& workspace,
    const SessionState& session, const std::filesystem::path& root, std::string_view request, bool reduction_enabled,
    std::string_view store_suffix) {
    RuntimeOptions runtime_options;
    runtime_options.approval_policy = "auto";
    runtime_options.feature_flags["context_reduction"] = reduction_enabled;
    auto model = std::make_shared<FakeModelClient>(std::vector<std::string>{});
    auto sessions = std::make_shared<EphemeralSessionStore>(root / ".runi" / ("sessions-" + std::string(store_suffix)));
    auto runs = std::make_shared<EphemeralRunStore>(root / ".runi" / ("runs-" + std::string(store_suffix)));
    Runi agent(model, workspace, sessions, runs, session, runtime_options);
    agent.context_manager().total_budget = spec.total_budget;
    agent.context_manager().section_budgets = spec.section_budgets;
    agent.context_manager().section_floor_overrides.clear();
    agent.context_manager().reduction_order = spec.reduction_order;
    return agent.build_prompt(request);
}

double mean_size(const std::vector<ContextRunRecord>& rows, bool raw) {
    if (rows.empty()) return 0.0;
    const auto total = std::accumulate(rows.begin(), rows.end(), std::uint64_t{0}, [raw](std::uint64_t sum, const auto& row) {
        return sum + (raw ? row.raw_prompt_chars : row.managed_prompt_chars);
    });
    return static_cast<double>(total) / static_cast<double>(rows.size());
}

double mean_ratio(const std::vector<ContextRunRecord>& rows) {
    if (rows.empty()) return 0.0;
    const auto total = std::accumulate(rows.begin(), rows.end(), 0.0, [](double sum, const auto& row) {
        return sum + row.compression_ratio;
    });
    return total / static_cast<double>(rows.size());
}

double bool_rate(const std::vector<ContextRunRecord>& rows, bool ContextRunRecord::*member) {
    if (rows.empty()) return 0.0;
    const auto count = std::count_if(rows.begin(), rows.end(), [member](const auto& row) { return row.*member; });
    return static_cast<double>(count) / static_cast<double>(rows.size());
}

Result<void> write_json_artifact(const std::filesystem::path& path, const JsonValue& artifact) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return Result<void>::failure(make_error(ErrorCategory::Persistence, "artifact_directory_failed",
        "Could not create artifact directory: " + error.message()));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure(make_error(ErrorCategory::Persistence, "artifact_write_failed",
        "Could not write context ablation artifact: " + path.string()));
    output << dump_json(artifact, 2, true) << '\n';
    return output ? Result<void>::success() : Result<void>::failure(make_error(
        ErrorCategory::Persistence, "artifact_write_failed", "Could not finish writing context ablation artifact: " + path.string()));
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

Result<JsonValue> run_context_ablation(ContextAblationOptions options) {
    options.matrix_path = std::filesystem::absolute(options.matrix_path);
    options.artifact_path = std::filesystem::absolute(options.artifact_path);
    options.workspace_root = std::filesystem::absolute(options.workspace_root);
    const auto loaded = read_json(options.matrix_path); if (!loaded) return loaded;
    const auto parsed = parse_context_matrix(loaded.value());
    if (!parsed) return Result<JsonValue>::failure(parsed.error());
    auto spec = parsed.value();
    if (options.repetitions_override > 0) spec.repetitions = options.repetitions_override;

    std::error_code error;
    const auto workspace_root = std::filesystem::absolute(options.workspace_root, error).lexically_normal();
    const auto case_root = std::filesystem::absolute(workspace_root / "current", error).lexically_normal();
    const auto relative_case = std::filesystem::relative(case_root, workspace_root, error);
    if (error || relative_case.is_absolute() || relative_case.empty() || *relative_case.begin() == "..") {
        return Result<JsonValue>::failure(make_error(ErrorCategory::PathViolation, "context_workspace_escape",
            "context ablation workspace escapes workspace_root"));
    }
    std::filesystem::remove_all(case_root, error); error.clear();
    std::filesystem::create_directories(case_root, error);
    if (error) return Result<JsonValue>::failure(make_error(ErrorCategory::Persistence,
        "context_workspace_create_failed", "Could not create context workspace: " + error.message()));
    const auto initial_readme = write_text_file(case_root / "README.md", "demo\n");
    if (!initial_readme) return Result<JsonValue>::failure(initial_readme.error());
    const auto stable_workspace = WorkspaceContext::build(case_root, case_root);
    if (!stable_workspace) return Result<JsonValue>::failure(stable_workspace.error());

    JsonValue::Array config_json;
    JsonValue::Array row_json;
    std::vector<ContextRunRecord> all_rows;
    std::size_t configs_with_dynamic_reduction = 0;
    std::string max_config_id;
    double max_ratio = -std::numeric_limits<double>::infinity();
    double min_ratio = std::numeric_limits<double>::infinity();

    for (const auto& history : spec.history_levels) {
        for (const auto& notes : spec.note_levels) {
            for (const auto& request : spec.request_levels) {
                const auto config_id = history.id + "-" + notes.id + "-" + request.id;
                std::vector<ContextRunRecord> config_rows;
                bool config_reduced = false;
                for (std::size_t repetition = 1; repetition <= spec.repetitions; ++repetition) {
                    const auto session = context_matrix_session(spec, case_root, history.count, notes.count);
                    const auto raw = build_context_variant(spec, stable_workspace.value(), session, case_root,
                        request.text, false, "raw");
                    if (!raw) return Result<JsonValue>::failure(raw.error());
                    const auto managed = build_context_variant(spec, stable_workspace.value(), session, case_root,
                        request.text, true, "managed");
                    if (!managed) return Result<JsonValue>::failure(managed.error());

                    ContextRunRecord record;
                    record.raw_prompt_chars = utf8_length(raw.value().prompt);
                    record.managed_prompt_chars = utf8_length(managed.value().prompt);
                    record.compression_ratio = record.raw_prompt_chars == 0 ? 0.0 :
                        static_cast<double>(static_cast<std::int64_t>(record.raw_prompt_chars) -
                            static_cast<std::int64_t>(record.managed_prompt_chars)) /
                        static_cast<double>(record.raw_prompt_chars);
                    record.current_request_preserved = current_request_is_preserved(managed.value(), request.text);
                    record.prompt_budget_satisfied = !managed.value().metadata.at("prompt_over_budget").bool_or(true);
                    record.reduction_order_valid = reduction_order_is_valid(managed.value().metadata, spec.reduction_order);
                    record.section_floors_respected = section_floors_are_respected(managed.value().metadata, spec.section_floors);
                    record.dynamic_reduction_triggered = !managed.value().metadata.at("budget_reductions").as_array().empty();
                    config_reduced = config_reduced || record.dynamic_reduction_triggered;
                    record.row = JsonValue::Object{
                        {"budget_reduction_count", JsonValue(managed.value().metadata.at("budget_reductions").as_array().size())},
                        {"budget_reductions", managed.value().metadata.at("budget_reductions")},
                        {"compression_ratio", JsonValue(record.compression_ratio)},
                        {"config_id", JsonValue(config_id)},
                        {"current_request_preserved", JsonValue(record.current_request_preserved)},
                        {"dynamic_reduction_triggered", JsonValue(record.dynamic_reduction_triggered)},
                        {"final_section_budgets", managed.value().metadata.at("section_budgets")},
                        {"history_count", JsonValue(history.count)},
                        {"history_level", JsonValue(history.id)},
                        {"managed_prompt_chars", JsonValue(record.managed_prompt_chars)},
                        {"managed_sections", managed.value().metadata.at("sections")},
                        {"note_count", JsonValue(notes.count)},
                        {"note_level", JsonValue(notes.id)},
                        {"prompt_budget_chars", JsonValue(spec.total_budget)},
                        {"prompt_budget_satisfied", JsonValue(record.prompt_budget_satisfied)},
                        {"raw_prompt_chars", JsonValue(record.raw_prompt_chars)},
                        {"reduction_order_valid", JsonValue(record.reduction_order_valid)},
                        {"repetition", JsonValue(repetition)},
                        {"request_chars", JsonValue(utf8_length(request.text))},
                        {"request_level", JsonValue(request.id)},
                        {"section_floors_respected", JsonValue(record.section_floors_respected)}};
                    row_json.push_back(record.row);
                    config_rows.push_back(record);
                    all_rows.push_back(std::move(record));
                }
                if (config_reduced) ++configs_with_dynamic_reduction;
                const auto config_max = std::max_element(config_rows.begin(), config_rows.end(), [](const auto& left, const auto& right) {
                    return left.compression_ratio < right.compression_ratio;
                })->compression_ratio;
                const auto config_min = std::min_element(config_rows.begin(), config_rows.end(), [](const auto& left, const auto& right) {
                    return left.compression_ratio < right.compression_ratio;
                })->compression_ratio;
                if (config_max > max_ratio) { max_ratio = config_max; max_config_id = config_id; }
                min_ratio = std::min(min_ratio, config_min);
                config_json.emplace_back(JsonValue::Object{
                    {"avg_managed_prompt_chars", JsonValue(mean_size(config_rows, false))},
                    {"avg_prompt_compression_ratio", JsonValue(mean_ratio(config_rows))},
                    {"avg_raw_prompt_chars", JsonValue(mean_size(config_rows, true))},
                    {"current_request_preserved_rate", JsonValue(bool_rate(config_rows, &ContextRunRecord::current_request_preserved))},
                    {"dynamic_reduction_rate", JsonValue(bool_rate(config_rows, &ContextRunRecord::dynamic_reduction_triggered))},
                    {"history_count", JsonValue(history.count)},
                    {"history_level", JsonValue(history.id)},
                    {"id", JsonValue(config_id)},
                    {"note_count", JsonValue(notes.count)},
                    {"note_level", JsonValue(notes.id)},
                    {"prompt_budget_satisfied_rate", JsonValue(bool_rate(config_rows, &ContextRunRecord::prompt_budget_satisfied))},
                    {"repetitions", JsonValue(spec.repetitions)},
                    {"request_level", JsonValue(request.id)}});
            }
        }
    }

    const auto avg_raw = mean_size(all_rows, true);
    const auto avg_managed = mean_size(all_rows, false);
    const auto request_rate = bool_rate(all_rows, &ContextRunRecord::current_request_preserved);
    const auto budget_rate = bool_rate(all_rows, &ContextRunRecord::prompt_budget_satisfied);
    const auto order_rate = bool_rate(all_rows, &ContextRunRecord::reduction_order_valid);
    const auto floor_rate = bool_rate(all_rows, &ContextRunRecord::section_floors_respected);
    const bool contract_passed = request_rate == 1.0 && budget_rate == 1.0 && order_rate == 1.0 && floor_rate == 1.0;
    const auto repo_root = options.matrix_path.parent_path().parent_path();
    ProcessRunner runner;
    const auto git_sha = runner.run(ProcessRequest{"git rev-parse HEAD", repo_root, {}, std::chrono::seconds(5), true});
    const auto git_branch = runner.run(ProcessRequest{"git branch --show-current", repo_root, {}, std::chrono::seconds(5), true});
    const auto git_status = runner.run(ProcessRequest{"git status --porcelain", repo_root, {}, std::chrono::seconds(5), true});
    const auto matrix_digest = sha256_file(options.matrix_path);
    JsonValue artifact{JsonValue::Object{
        {"artifact_type", JsonValue("context-ablation-v1")},
        {"captured_at", JsonValue(now_utc())},
        {"configs", JsonValue(std::move(config_json))},
        {"matrix", JsonValue::Object{
            {"config_count", JsonValue(spec.history_levels.size() * spec.note_levels.size() * spec.request_levels.size())},
            {"definition", spec.definition},
            {"repetitions", JsonValue(spec.repetitions)},
            {"sha256", JsonValue(matrix_digest ? "sha256:" + matrix_digest.value() : std::string{})},
            {"source", JsonValue(relative_to(options.matrix_path, repo_root))}}},
        {"reproducibility", JsonValue::Object{
            {"compiler", JsonValue(compiler_identity())},
            {"deterministic_inputs", JsonValue(true)},
            {"model_calls", JsonValue(0)},
            {"platform", JsonValue(platform_identity())},
            {"section_budgets", size_map(spec.section_budgets)},
            {"section_floors", size_map(spec.section_floors)},
            {"total_budget_chars", JsonValue(spec.total_budget)},
            {"timezone", JsonValue(options.timezone_name)},
            {"workspace_strategy", JsonValue("single immutable workspace snapshot with isolated in-memory session and run stores")}}},
        {"rows", JsonValue(std::move(row_json))},
        {"runtime", JsonValue::Object{
            {"branch", JsonValue(git_branch ? trim(git_branch.value().stdout_text) : std::string{})},
            {"commit_sha", JsonValue(git_sha ? trim(git_sha.value().stdout_text) : std::string{})},
            {"working_tree_dirty", JsonValue(git_status && !trim(git_status.value().stdout_text).empty())}}},
        {"schema_version", JsonValue(kContextAblationSchemaVersion)},
        {"summary", JsonValue::Object{
            {"avg_managed_prompt_chars", JsonValue(avg_managed)},
            {"avg_prompt_compression_ratio", JsonValue(mean_ratio(all_rows))},
            {"avg_raw_prompt_chars", JsonValue(avg_raw)},
            {"config_count", JsonValue(spec.history_levels.size() * spec.note_levels.size() * spec.request_levels.size())},
            {"configs_with_dynamic_reduction", JsonValue(configs_with_dynamic_reduction)},
            {"contract_passed", JsonValue(contract_passed)},
            {"current_request_preserved_rate", JsonValue(request_rate)},
            {"max_compression_config_id", JsonValue(max_config_id)},
            {"max_prompt_compression_ratio", JsonValue(max_ratio)},
            {"min_prompt_compression_ratio", JsonValue(min_ratio)},
            {"prompt_budget_satisfied_rate", JsonValue(budget_rate)},
            {"ratio_of_mean_prompt_chars", JsonValue(avg_raw == 0.0 ? 0.0 : (avg_raw - avg_managed) / avg_raw)},
            {"reduction_order_valid_rate", JsonValue(order_rate)},
            {"comparison_count", JsonValue(all_rows.size())},
            {"prompt_build_count", JsonValue(all_rows.size() * 2)},
            {"section_floors_respected_rate", JsonValue(floor_rate)}}}}};
    const auto written = write_json_artifact(options.artifact_path, artifact);
    if (!written) return Result<JsonValue>::failure(written.error());
    return Result<JsonValue>::success(std::move(artifact));
}

}  // namespace runi
