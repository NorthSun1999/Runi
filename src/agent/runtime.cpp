#include "runi/agent/runtime.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <regex>
#include <stdexcept>

#include "runi/agent/agent_loop.hpp"
#include "runi/core/json_codec.hpp"
#include "runi/core/sha256.hpp"
#include "runi/core/text.hpp"
#include "runi/core/time.hpp"

namespace runi {
namespace {

JsonValue strings_json(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& value : values) result.emplace_back(value);
    return JsonValue(std::move(result));
}

std::vector<std::string> json_strings(const JsonValue* value) {
    std::vector<std::string> result;
    if (value == nullptr || !value->is_array()) return result;
    for (const auto& item : value->as_array()) if (item.is_string()) result.push_back(item.as_string());
    return result;
}

std::string object_string(const JsonValue& value, std::string_view key) {
    const auto* item = value.find(key);
    return item == nullptr ? std::string{} : item->string_or();
}

bool ignored_relative(const std::filesystem::path& relative) {
    for (const auto& part : relative) if (kIgnoredPathNames.contains(part.string())) return true;
    return false;
}

}  // namespace

Runi::Runi(
    std::shared_ptr<IModelClient> model_client,
    WorkspaceContext workspace,
    std::shared_ptr<ISessionStore> session_store,
    std::shared_ptr<IRunStore> run_store,
    std::optional<SessionState> session,
    RuntimeOptions options)
    : model_client_(std::move(model_client)), workspace_(std::move(workspace)), root_(workspace_.repo_root),
      guard_(root_), session_store_(std::move(session_store)), run_store_(std::move(run_store)),
      session_(session.has_value() ? std::move(*session) : SessionState::create(root_.string())), options_(std::move(options)),
      memory_(session_.memory, root_), context_manager_(*this) {
    session_.ensure_shape();
    memory_ = LayeredMemory(session_.memory, root_);
    std::set<std::string, std::less<>> normalized_secret_names;
    for (auto name : options_.secret_env_names) {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        normalized_secret_names.insert(std::move(name));
    }
    options_.secret_env_names = std::move(normalized_secret_names);
    tools_ = apply_tool_allowlist(build_tool_registry(tool_context()));
    tool_executor_ = std::make_unique<ToolExecutor>(tools_, *this);
    prefix_state_ = build_prompt_prefix(workspace_, tools_);
    sync_memory();
    resume_state_ = evaluate_resume_state(*this);
    persist_session();
}

ToolContext Runi::tool_context() {
    return ToolContext{
        root_, guard_, [this] { return shell_environment(options_.shell_env_allowlist, root_); },
        options_.depth, options_.max_depth, [this](const JsonValue::Object& args) { return spawn_delegate(args); }};
}

ToolRegistry Runi::apply_tool_allowlist(ToolRegistry tools) const {
    if (!options_.allowed_tools.has_value()) return tools;
    const auto legal = legal_tool_names();
    for (const auto& name : *options_.allowed_tools) {
        if (name.empty()) throw std::invalid_argument("allowed_tools must be a non-empty sequence of tool names");
        if (std::find(legal.begin(), legal.end(), name) == legal.end()) throw std::invalid_argument("unknown allowed tool: " + name);
    }
    tools.erase(std::remove_if(tools.begin(), tools.end(), [&](const auto& tool) {
        return std::find(options_.allowed_tools->begin(), options_.allowed_tools->end(), tool.descriptor.name) == options_.allowed_tools->end();
    }), tools.end());
    return tools;
}

void Runi::sync_memory() { session_.memory = memory_.to_json(); }

Result<std::string> Runi::ask(std::string_view user_message, std::stop_token stop_token) {
    return AgentLoop(*this).run(user_message, stop_token);
}
ModelAction Runi::parse(std::string_view raw) const { return parser_.parse(raw); }

ToolExecutionResult Runi::execute_tool(std::string_view name, const JsonValue::Object& args) {
    auto result = tool_executor_->execute(ToolCall{std::string(name), args});
    last_tool_result_metadata_ = result.metadata;
    return result;
}

std::string Runi::run_tool(std::string_view name, const JsonValue::Object& args) { return execute_tool(name, args).content; }

Result<void> Runi::reset() {
    session_.history.clear();
    session_.memory = default_memory_state();
    memory_ = LayeredMemory(session_.memory, root_);
    return persist_session();
}

JsonValue Runi::refresh_prefix(bool force) {
    const auto old_hash = prefix_state_.hash;
    const auto old_fingerprint = prefix_state_.workspace_fingerprint;
    const auto refreshed = WorkspaceContext::build(root_, root_);
    const bool workspace_changed = force || (refreshed && refreshed.value().fingerprint() != old_fingerprint);
    if (workspace_changed && refreshed) workspace_ = refreshed.value();
    const auto next = workspace_changed || force || old_hash.empty() ? build_prompt_prefix(workspace_, tools_) : prefix_state_;
    const bool prefix_changed = force || old_hash != next.hash;
    if (prefix_changed) prefix_state_ = next;
    last_prefix_refresh_ = JsonValue::Object{{"prefix_changed", JsonValue(prefix_changed)}, {"workspace_changed", JsonValue(workspace_changed)}};
    return last_prefix_refresh_;
}

Result<ContextBuildResult> Runi::build_prompt(std::string_view user_message) {
    const auto refresh = refresh_prefix();
    resume_state_ = evaluate_resume_state(*this);
    auto built = context_manager_.build(user_message);
    if (!built) return built;
    auto& metadata = built.value().metadata.as_object();
    metadata["prefix_chars"] = JsonValue(utf8_length(prefix_state_.text));
    metadata["workspace_chars"] = JsonValue(utf8_length(workspace_.text()));
    metadata["memory_chars"] = JsonValue(utf8_length(memory_text()));
    metadata["history_chars"] = JsonValue(utf8_length(history_text()));
    metadata["request_chars"] = JsonValue(utf8_length(user_message));
    metadata["tool_count"] = JsonValue(tools_.size());
    metadata["workspace_docs"] = JsonValue(workspace_.project_docs.size());
    metadata["recent_commits"] = JsonValue(workspace_.recent_commits.size());
    metadata["prefix_hash"] = JsonValue(prefix_state_.hash);
    metadata["prompt_cache_key"] = JsonValue(prefix_state_.hash);
    metadata["workspace_fingerprint"] = JsonValue(prefix_state_.workspace_fingerprint);
    metadata["tool_signature"] = JsonValue(prefix_state_.tool_signature);
    metadata["workspace_changed"] = JsonValue(refresh.at("workspace_changed").bool_or());
    metadata["prefix_changed"] = JsonValue(refresh.at("prefix_changed").bool_or());
    metadata["prompt_cache_supported"] = JsonValue(model_client_->supports_prompt_cache());
    metadata["resume_status"] = JsonValue(object_string(resume_state_, "status").empty() ? std::string(kCheckpointNoneStatus) : object_string(resume_state_, "status"));
    const auto* invalidations = resume_state_.find("stale_summary_invalidations");
    metadata["stale_summary_invalidations"] = JsonValue(invalidations == nullptr ? 0 : invalidations->integer_or());
    metadata["stale_paths"] = resume_state_.find("stale_paths") == nullptr ? JsonValue::Array{} : *resume_state_.find("stale_paths");
    metadata["runtime_identity_mismatch_fields"] = resume_state_.find("runtime_identity_mismatch_fields") == nullptr
        ? JsonValue::Array{} : *resume_state_.find("runtime_identity_mismatch_fields");
    const auto secret_summary = secret_env_summary(options_.secret_env_names, true);
    if (secret_summary.is_object()) for (const auto& [key, value] : secret_summary.as_object()) metadata[key] = value;
    last_prompt_metadata_ = built.value().metadata;
    return built;
}

void Runi::set_task_summary(std::string_view user_message) {
    memory_.set_task_summary(user_message);
    sync_memory();
}

TaskState& Runi::initialize_task(std::string_view user_message) {
    current_task_state_storage_ = TaskState::create(new_task_id(), std::string(user_message), new_run_id());
    current_task_state = &*current_task_state_storage_;
    const auto status = object_string(resume_state_, "status");
    current_task_state->resume_status = status.empty() ? std::string(kCheckpointNoneStatus) : status;
    return *current_task_state;
}

void Runi::merge_completion_metadata(const JsonValue& metadata) {
    last_completion_metadata_ = metadata;
    if (metadata.is_object() && last_prompt_metadata_.is_object()) {
        for (const auto& [key, value] : metadata.as_object()) last_prompt_metadata_[key] = value;
    }
}

JsonValue Runi::redact_value(const JsonValue& value) const { return redact_artifact(value, options_.secret_env_names); }

Result<void> Runi::record(HistoryItem item) {
    session_.history.push_back(std::move(item));
    return persist_session();
}

JsonValue Runi::emit_trace(TaskState& task_state, std::string_view event, JsonValue payload) {
    payload = redact_artifact(payload, options_.secret_env_names);
    if (!payload.is_object()) payload = JsonValue::Object{};
    payload["event"] = JsonValue(std::string(event));
    payload["created_at"] = JsonValue(now_utc());
    run_store_->append_trace(task_state, payload);
    return payload;
}

WorkspaceSnapshot Runi::capture_workspace_snapshot() const {
    WorkspaceSnapshot result;
    std::error_code error;
    for (std::filesystem::recursive_directory_iterator iterator(root_, std::filesystem::directory_options::skip_permission_denied, error), end;
         iterator != end; iterator.increment(error)) {
        if (error) { error.clear(); continue; }
        const auto relative = std::filesystem::relative(iterator->path(), root_, error);
        if (error || ignored_relative(relative)) { error.clear(); if (iterator->is_directory()) iterator.disable_recursion_pending(); continue; }
        if (!iterator->is_regular_file(error)) continue;
        const auto digest = sha256_file(iterator->path());
        if (digest) result[relative.generic_string()] = digest.value();
    }
    return result;
}

std::pair<std::vector<std::string>, std::vector<std::string>> Runi::diff_workspace_snapshots(
    const WorkspaceSnapshot& before, const WorkspaceSnapshot& after) const {
    std::set<std::string, std::less<>> all;
    for (const auto& [path, _] : before) all.insert(path);
    for (const auto& [path, _] : after) all.insert(path);
    std::vector<std::string> changed, summary;
    for (const auto& path : all) {
        const auto left = before.find(path), right = after.find(path);
        if (left != before.end() && right != after.end() && left->second == right->second) continue;
        changed.push_back(path);
        if (left == before.end()) summary.push_back("created:" + path);
        else if (right == after.end()) summary.push_back("deleted:" + path);
        else summary.push_back("modified:" + path);
    }
    return {changed, summary};
}

std::string Runi::history_text() const {
    if (session_.history.empty()) return "- empty";
    std::vector<std::string> lines;
    std::set<std::string, std::less<>> seen_reads;
    const auto recent_start = session_.history.size() > 6 ? session_.history.size() - 6 : 0;
    for (std::size_t index = 0; index < session_.history.size(); ++index) {
        const auto& item = session_.history[index];
        const bool recent = index >= recent_start;
        if (item.role == "tool" && item.name == "read_file" && !recent) {
            const auto path = item.args.find("path") == nullptr ? std::string{} : item.args.find("path")->string_or();
            if (seen_reads.contains(path)) continue;
            seen_reads.insert(path);
        }
        if (item.role == "tool") {
            lines.push_back("[tool:" + item.name + "] " + dump_compatible_json(item.args));
            lines.push_back(clip(item.content, recent ? 900 : 180));
        } else lines.push_back("[" + item.role + "] " + clip(item.content, recent ? 900 : 220));
    }
    return clip(join(lines, "\n"), 12000);
}

void Runi::update_memory_after_tool(std::string_view name, const JsonValue::Object& args, std::string_view result) {
    if (!feature_enabled("memory")) return;
    const auto iterator = args.find("path");
    if (iterator == args.end() || !iterator->second.is_string() || iterator->second.as_string().empty()) return;
    const auto path = memory_.canonical_path(iterator->second.as_string());
    if (name == "read_file" || name == "write_file" || name == "patch_file") memory_.remember_file(path);
    if (name == "read_file") {
        const auto summary = summarize_read_result(result);
        memory_.set_file_summary(path, summary).append_note(summary, {path}, path);
    } else if (name == "write_file" || name == "patch_file") memory_.invalidate_file_summary(path);
    sync_memory();
}

void Runi::record_process_note_for_tool(std::string_view name, const JsonValue& metadata) {
    const auto status = trim(object_string(metadata, "tool_status"));
    if (status != "partial_success" && status != "error" && status != "rejected") return;
    const auto paths = json_strings(metadata.find("affected_paths"));
    const auto path_text = paths.empty() ? std::string("workspace") : join(paths, ", ");
    std::string text;
    if (status == "partial_success") text = std::string(name) + " partial_success on " + path_text + "; inspect diff before retry";
    else if (status == "error") text = std::string(name) + " error on " + path_text + "; check the failure before retry";
    else text = std::string(name) + " rejected; choose a different action before retry";
    auto tags = std::vector<std::string>{"process", status}; tags.insert(tags.end(), paths.begin(), paths.end());
    memory_.append_note(text, tags, std::string(name), {}, "process");
    sync_memory();
}

std::string Runi::reject_durable_reason(std::string_view note_text) const {
    const auto text = trim(note_text), lowered = lower_ascii(text);
    if (text.empty()) return "empty";
    static const std::regex secret(R"((api[_ -]?key|token|secret|password)|sk-[A-Za-z0-9_-]{6,})", std::regex::icase);
    if (text.find(kRedactedValue) != std::string::npos || std::regex_search(text, secret)) return "secret_shaped";
    static const std::vector<std::string> prefixes{"current goal", "current blocker", "next step", "current phase", "key files", "freshness",
        "当前目标", "当前卡点", "下一步", "当前阶段", "关键文件", "已完成", "已排除"};
    for (const auto& prefix : prefixes) if (lowered.starts_with(prefix)) return "transient_task_state";
    static const std::regex noisy(R"(\b(stdout|stderr|traceback|exit_code)\b)", std::regex::icase);
    if (std::regex_search(text, noisy) || utf8_length(text) > 220) return "noisy_output";
    return {};
}

std::pair<std::vector<std::pair<std::string, std::string>>, std::vector<std::string>> Runi::extract_durable_promotions(
    std::string_view user_message, std::string_view final_answer) const {
    static const std::regex intent(R"(\b(capture|remember|save|store|persist|note)\b)", std::regex::icase);
    const auto user = std::string(user_message);
    if (!std::regex_search(user, intent) && user.find("记住") == std::string::npos && user.find("保存") == std::string::npos &&
        user.find("记录") == std::string::npos && user.find("沉淀") == std::string::npos && user.find("长期记忆") == std::string::npos &&
        user.find("持久记忆") == std::string::npos) return {};
    const std::vector<std::pair<std::string, std::regex>> patterns{
        {"project-conventions", std::regex(R"(^Project convention:\s*(.+)$)", std::regex::icase)},
        {"key-decisions", std::regex(R"(^Decision:\s*(.+)$)", std::regex::icase)},
        {"dependency-facts", std::regex(R"(^Dependency:\s*(.+)$)", std::regex::icase)},
        {"user-preferences", std::regex(R"(^Preference:\s*(.+)$)", std::regex::icase)},
        {"project-conventions", std::regex(R"(^项目约定：\s*(.+)$)")}, {"key-decisions", std::regex(R"(^决策：\s*(.+)$)")},
        {"dependency-facts", std::regex(R"(^依赖：\s*(.+)$)")}, {"user-preferences", std::regex(R"(^偏好：\s*(.+)$)")}};
    std::vector<std::pair<std::string, std::string>> promotions;
    std::vector<std::string> rejections;
    for (const auto& raw : split_lines(final_answer)) {
        const auto text = trim(raw);
        if (text.empty() || text.find(kRedactedValue) != std::string::npos) continue;
        for (const auto& [topic, pattern] : patterns) {
            std::smatch match;
            if (!std::regex_match(text, match, pattern)) continue;
            const auto note = trim(match[1].str());
            if (!note.empty()) {
                const auto reason = reject_durable_reason(note);
                if (reason.empty()) promotions.emplace_back(topic, note); else rejections.push_back(topic + ":" + reason);
            }
            break;
        }
    }
    return {promotions, rejections};
}

std::tuple<std::vector<std::string>, std::vector<std::string>, std::vector<std::string>> Runi::promote_durable_memory(
    std::string_view user_message, std::string_view final_answer) {
    auto [promotions, rejections] = extract_durable_promotions(user_message, final_answer);
    auto [promoted, superseded] = memory_.promote_durable(promotions);
    sync_memory();
    last_durable_promotions_ = promoted; last_durable_rejections_ = rejections; last_durable_superseded_ = superseded;
    return {promoted, rejections, superseded};
}

Result<std::string> Runi::spawn_delegate(const JsonValue::Object& args) {
    const auto task = args.contains("task") ? trim(args.at("task").string_or()) : std::string{};
    RuntimeOptions child_options = options_;
    child_options.approval_policy = "never";
    child_options.max_steps = args.contains("max_steps") ? static_cast<std::size_t>(std::max<std::int64_t>(1, args.at("max_steps").integer_or(3))) : 3;
    child_options.depth = options_.depth + 1;
    child_options.read_only = true;
    Runi child(model_client_, workspace_, session_store_, run_store_, std::nullopt, child_options);
    child.memory_.set_task_summary(task).append_note(clip(history_text(), 300));
    child.sync_memory();
    const auto answer = child.ask(task);
    if (!answer) return answer;
    return Result<std::string>::success("delegate_result:\n" + answer.value());
}

JsonValue Runi::build_report(const TaskState& task_state) const {
    return JsonValue::Object{
        {"attempts", JsonValue(task_state.attempts)}, {"checkpoint_id", JsonValue(task_state.checkpoint_id)},
        {"durable_promotions", strings_json(last_durable_promotions_)}, {"durable_rejections", strings_json(last_durable_rejections_)},
        {"durable_superseded", strings_json(last_durable_superseded_)}, {"final_answer", JsonValue(task_state.final_answer)},
        {"prompt_metadata", last_prompt_metadata_}, {"redacted_env", secret_env_summary(options_.secret_env_names, true)},
        {"resume_status", JsonValue(task_state.resume_status)}, {"run_id", JsonValue(task_state.run_id)},
        {"status", JsonValue(task_state.status)}, {"stop_reason", JsonValue(task_state.stop_reason)},
        {"task_id", JsonValue(task_state.task_id)}, {"task_state", task_state.to_json()}, {"tool_steps", JsonValue(task_state.tool_steps)}};
}

JsonValue Runi::create_task_checkpoint(TaskState& task_state, std::string_view user_message, std::string_view trigger) {
    return create_checkpoint(*this, task_state, user_message, trigger);
}

std::string Runi::new_task_id() const { return runi::new_task_id(); }
std::string Runi::new_run_id() const { return runi::new_run_id(); }
IModelClient& Runi::model_client() noexcept { return *model_client_; }
IRunStore& Runi::run_store() noexcept { return *run_store_; }
const RuntimeOptions& Runi::options() const noexcept { return options_; }
const JsonValue& Runi::resume_state() const noexcept { return resume_state_; }
const JsonValue& Runi::last_prompt_metadata() const noexcept { return last_prompt_metadata_; }
std::filesystem::path Runi::root() const { return root_; }
const WorkspaceContext& Runi::workspace() const noexcept { return workspace_; }
std::filesystem::path Runi::session_path() const { return session_store_->path(session_.id); }
LayeredMemory& Runi::memory() noexcept { return memory_; }
ContextManager& Runi::context_manager() noexcept { return context_manager_; }

const std::optional<std::vector<std::string>>& Runi::allowed_tools() const { return options_.allowed_tools; }
bool Runi::repeated_tool_call(std::string_view name, const JsonValue::Object& args) const {
    std::vector<const HistoryItem*> events;
    for (const auto& item : session_.history) if (item.role == "tool") events.push_back(&item);
    if (events.size() < 2) return false;
    for (std::size_t index = events.size() - 2; index < events.size(); ++index) {
        if (events[index]->name != name || !events[index]->args.is_object() || events[index]->args.as_object() != args) return false;
    }
    return true;
}

bool Runi::approve(std::string_view name, const JsonValue::Object& args) {
    if (options_.read_only) return false;
    if (options_.approval_policy == "auto") return true;
    if (options_.approval_policy == "never") return false;
    std::cout << "approve " << name << ' ' << dump_compatible_json(JsonValue(args)) << "? [y/N] " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) return false;
    answer = lower_ascii(trim(answer));
    return answer == "y" || answer == "yes";
}

bool Runi::read_only() const noexcept { return options_.read_only; }
std::string Runi::workspace_fingerprint() const { return workspace_.fingerprint(); }
const std::string& Runi::prefix() const { return prefix_state_.text; }
std::string Runi::memory_text() { return memory_.render_memory_text(); }
std::string Runi::render_checkpoint_text() const { return runi::render_checkpoint_text(*this, resume_state_); }
std::vector<JsonValue> Runi::memory_candidates(std::string_view query, std::size_t limit) { return memory_.retrieval_candidates(query, limit); }
const SessionState& Runi::session() const { return session_; }
bool Runi::feature_enabled(std::string_view name) const {
    const auto iterator = options_.feature_flags.find(name);
    return iterator != options_.feature_flags.end() && iterator->second;
}

std::string Runi::reusable_file_summary(std::string_view path) const {
    const auto* summaries = session_.memory.find("file_summaries");
    const auto* item = summaries != nullptr && summaries->is_object() ? summaries->find(path) : nullptr;
    if (item == nullptr || !item->is_object()) return {};
    if (const auto* freshness = item->find("freshness"); freshness != nullptr && *freshness == file_freshness(path, root_)) {
        return object_string(*item, "summary");
    }
    return {};
}

SessionState& Runi::mutable_session() { return session_; }
const SessionState& Runi::checkpoint_session() const { return session_; }
LayeredMemory& Runi::checkpoint_memory() { return memory_; }
std::filesystem::path Runi::checkpoint_root() const { return root_; }

JsonValue Runi::runtime_identity() const {
    JsonValue::Object flags;
    for (const auto& [key, value] : options_.feature_flags) flags[key] = JsonValue(value);
    return JsonValue::Object{
        {"approval_policy", JsonValue(options_.approval_policy)}, {"cwd", JsonValue(root_.string())},
        {"feature_flags", JsonValue(std::move(flags))}, {"max_new_tokens", JsonValue(options_.max_new_tokens)},
        {"max_steps", JsonValue(options_.max_steps)}, {"model", JsonValue(model_client_->model_name())},
        {"model_client", JsonValue(model_client_->client_name())}, {"read_only", JsonValue(options_.read_only)},
        {"session_id", JsonValue(session_.id)}, {"shell_env_allowlist", strings_json(options_.shell_env_allowlist)},
        {"tool_signature", JsonValue(tool_signature(tools_))}, {"workspace_fingerprint", JsonValue(prefix_state_.workspace_fingerprint.empty() ? workspace_.fingerprint() : prefix_state_.workspace_fingerprint)}};
}

Result<void> Runi::persist_session() {
    sync_memory();
    const auto saved = session_store_->save(session_);
    return saved ? Result<void>::success() : Result<void>::failure(saved.error());
}

}  // namespace runi
