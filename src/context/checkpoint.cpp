#include "runi/context/checkpoint.hpp"

#include <algorithm>

#include "runi/core/text.hpp"
#include "runi/core/time.hpp"

namespace runi {
namespace {

JsonValue strings(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

std::vector<std::string> string_values(const JsonValue* value) {
    std::vector<std::string> result;
    if (value != nullptr && value->is_array()) for (const auto& item : value->as_array()) result.push_back(item.string_or());
    return result;
}

std::string field(const JsonValue& value, std::string_view name) {
    const auto* item = value.find(name);
    return item == nullptr ? std::string{} : item->string_or();
}

const std::vector<std::string> kIdentityKeys{
    "cwd", "model", "model_client", "approval_policy", "read_only", "max_steps", "max_new_tokens",
    "delegate_workers", "max_delegate_tasks", "max_depth",
    "feature_flags", "shell_env_allowlist", "workspace_fingerprint", "tool_signature"};

}  // namespace

JsonValue* current_checkpoint(SessionState& session) {
    session.ensure_shape();
    const auto id = session.checkpoints.at("current_id").string_or();
    if (id.empty()) return nullptr;
    return session.checkpoints["items"].find(id);
}

const JsonValue* current_checkpoint(const SessionState& session) {
    const auto* id = session.checkpoints.find("current_id");
    const auto* items = session.checkpoints.find("items");
    if (id == nullptr || items == nullptr || !items->is_object() || id->string_or().empty()) return nullptr;
    return items->find(id->string_or());
}

JsonValue evaluate_resume_state(ICheckpointHost& host) {
    auto& session = host.mutable_session();
    const auto previous_invalidations = session.resume_state.find("stale_summary_invalidations");
    const auto invalidated = host.checkpoint_memory().invalidate_stale_file_summaries();
    session.memory = host.checkpoint_memory().to_json();
    const auto* checkpoint = current_checkpoint(session);
    std::string status(kCheckpointNoneStatus);
    auto stale_paths = invalidated;
    std::vector<std::string> mismatch_fields;
    if (checkpoint != nullptr && checkpoint->is_object()) {
        if (field(*checkpoint, "schema_version") != kCheckpointSchemaVersion) status = kCheckpointSchemaMismatchStatus;
        else {
            if (const auto* key_files = checkpoint->find("key_files"); key_files != nullptr && key_files->is_array()) {
                for (const auto& item : key_files->as_array()) {
                    const auto path = field(item, "path");
                    if (path.empty()) continue;
                    const auto current = file_freshness(path, host.checkpoint_root());
                    const auto* expected = item.find("freshness");
                    if ((expected == nullptr || *expected != current) && std::find(stale_paths.begin(), stale_paths.end(), path) == stale_paths.end()) stale_paths.push_back(path);
                }
            }
            const auto* saved = checkpoint->find("runtime_identity");
            const auto current = host.runtime_identity();
            if (saved != nullptr && saved->is_object()) for (const auto& key : kIdentityKeys) {
                const auto* old_value = saved->find(key);
                const auto* new_value = current.find(key);
                if (old_value != nullptr && (new_value == nullptr || *old_value != *new_value)) mismatch_fields.push_back(key);
            }
            std::sort(mismatch_fields.begin(), mismatch_fields.end());
            if (!stale_paths.empty()) status = kCheckpointPartialStaleStatus;
            else if (!mismatch_fields.empty()) status = kCheckpointWorkspaceMismatchStatus;
            else status = kCheckpointFullValidStatus;
        }
    }
    const auto previous = previous_invalidations == nullptr ? 0 : previous_invalidations->integer_or(0);
    const auto invalidation_count = status == kCheckpointPartialStaleStatus
        ? std::max<std::int64_t>(static_cast<std::int64_t>(invalidated.size()), previous)
        : static_cast<std::int64_t>(invalidated.size());
    JsonValue result = JsonValue::Object{{"runtime_identity_mismatch_fields", strings(mismatch_fields)},
        {"stale_paths", strings(stale_paths)}, {"stale_summary_invalidations", JsonValue(invalidation_count)}, {"status", JsonValue(status)}};
    session.resume_state = result;
    session.runtime_identity = host.runtime_identity();
    return result;
}

std::string render_checkpoint_text(const ICheckpointHost& host, const JsonValue& resume_state) {
    const auto* checkpoint = current_checkpoint(host.checkpoint_session());
    if (checkpoint == nullptr) return {};
    std::vector<std::string> lines{"Task checkpoint:", "- Resume status: " + field(resume_state, "status"),
        "- Current goal: " + (field(*checkpoint, "current_goal").empty() ? "-" : field(*checkpoint, "current_goal")),
        "- Current blocker: " + (field(*checkpoint, "current_blocker").empty() ? "-" : field(*checkpoint, "current_blocker")),
        "- Next step: " + (field(*checkpoint, "next_step").empty() ? "-" : field(*checkpoint, "next_step"))};
    std::vector<std::string> files;
    if (const auto* items = checkpoint->find("key_files"); items != nullptr && items->is_array()) for (const auto& item : items->as_array()) {
        const auto path = field(item, "path"); if (!path.empty()) files.push_back(path);
    }
    lines.push_back("- Key files: " + (files.empty() ? "-" : join(files, ", ")));
    const auto completed = string_values(checkpoint->find("completed"));
    if (!completed.empty()) lines.push_back("- Completed: " + join(completed, " | "));
    const auto excluded = string_values(checkpoint->find("excluded"));
    if (!excluded.empty()) lines.push_back("- Excluded: " + join(excluded, " | "));
    const auto stale = string_values(resume_state.find("stale_paths"));
    if (!stale.empty()) lines.push_back("- Stale paths: " + join(stale, ", "));
    const auto summary = field(*checkpoint, "summary");
    if (!summary.empty()) lines.push_back("- Summary: " + summary);
    return join(lines, "\n");
}

std::string infer_next_step(const TaskState& task_state) {
    if (task_state.status == kStatusCompleted) return "No next step recorded.";
    if (task_state.stop_reason == kStopStepLimitReached) return "Resume from the latest checkpoint and continue the task.";
    if (!task_state.last_tool.empty()) return "Decide the next action after " + task_state.last_tool + ".";
    return "Continue the task from the latest checkpoint.";
}

JsonValue create_checkpoint(ICheckpointHost& host, TaskState& task_state, std::string_view user_message, std::string_view trigger) {
    auto& session = host.mutable_session();
    session.ensure_shape();
    const auto* current = current_checkpoint(session);
    const auto id = new_checkpoint_id();
    JsonValue::Array key_files;
    JsonValue::Object freshness;
    const auto& memory = host.checkpoint_memory().to_json();
    if (const auto* working = memory.find("working"); working != nullptr) {
        if (const auto* files = working->find("recent_files"); files != nullptr && files->is_array()) for (const auto& path_value : files->as_array()) {
            const auto path = path_value.string_or();
            const auto value = file_freshness(path, host.checkpoint_root());
            freshness.emplace(path, value);
            key_files.emplace_back(JsonValue::Object{{"freshness", value}, {"path", JsonValue(path)}});
        }
    }
    JsonValue::Array completed;
    if (!task_state.final_answer.empty()) completed.emplace_back(task_state.final_answer);
    JsonValue checkpoint = JsonValue::Object{
        {"checkpoint_id", JsonValue(id)}, {"completed", JsonValue(std::move(completed))}, {"created_at", JsonValue(now_utc())},
        {"current_blocker", JsonValue(task_state.stop_reason.empty() || task_state.stop_reason == kStopFinalAnswerReturned ? "" : task_state.stop_reason)},
        {"current_goal", JsonValue(std::string(user_message))}, {"excluded", JsonValue::Array{}}, {"freshness", JsonValue(std::move(freshness))},
        {"key_files", JsonValue(std::move(key_files))}, {"next_step", JsonValue(infer_next_step(task_state))},
        {"parent_checkpoint_id", JsonValue(current == nullptr ? "" : field(*current, "checkpoint_id"))},
        {"runtime_identity", host.runtime_identity()}, {"schema_version", JsonValue(std::string(kCheckpointSchemaVersion))},
        {"summary", JsonValue(std::string(trigger) + ": " + clip(user_message, 120))}};
    session.checkpoints["items"][id] = checkpoint;
    session.checkpoints["current_id"] = JsonValue(id);
    task_state.checkpoint_id = id;
    session.runtime_identity = host.runtime_identity();
    host.persist_session();
    return checkpoint;
}

}  // namespace runi
