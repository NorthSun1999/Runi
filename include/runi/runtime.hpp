#pragma once

#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#include "runi/action_parser.hpp"
#include "runi/checkpoint.hpp"
#include "runi/context_manager.hpp"
#include "runi/model_client.hpp"
#include "runi/prompt_prefix.hpp"
#include "runi/run_store.hpp"
#include "runi/security.hpp"
#include "runi/session_store.hpp"
#include "runi/tool_executor.hpp"

namespace runi {

inline const std::vector<std::string> kDefaultShellEnvAllowlist{
    "HOME", "LANG", "LC_ALL", "LC_CTYPE", "LOGNAME", "PATH", "PWD", "SHELL", "TERM", "TMPDIR", "TMP", "TEMP", "USER"};

struct RuntimeOptions {
    std::string approval_policy{"ask"};
    std::size_t max_steps{6};
    std::size_t max_new_tokens{512};
    std::size_t depth{0};
    std::size_t max_depth{1};
    bool read_only{false};
    std::vector<std::string> shell_env_allowlist{kDefaultShellEnvAllowlist};
    std::set<std::string, std::less<>> secret_env_names;
    std::map<std::string, bool, std::less<>> feature_flags{
        {"context_reduction", true}, {"memory", true}, {"prompt_cache", true}, {"relevant_memory", true}};
    std::optional<std::vector<std::string>> allowed_tools;
};

class Runi final : public IToolHost, public IContextHost, public ICheckpointHost {
public:
    Runi(
        std::shared_ptr<IModelClient> model_client,
        WorkspaceContext workspace,
        std::shared_ptr<ISessionStore> session_store,
        std::shared_ptr<IRunStore> run_store,
        std::optional<SessionState> session = std::nullopt,
        RuntimeOptions options = {});

    [[nodiscard]] Result<std::string> ask(std::string_view user_message);
    [[nodiscard]] ModelAction parse(std::string_view raw) const;
    [[nodiscard]] ToolExecutionResult execute_tool(std::string_view name, const JsonValue::Object& args);
    [[nodiscard]] std::string run_tool(std::string_view name, const JsonValue::Object& args);
    [[nodiscard]] Result<void> reset();

    [[nodiscard]] Result<ContextBuildResult> build_prompt(std::string_view user_message);
    void set_task_summary(std::string_view user_message);
    [[nodiscard]] TaskState& initialize_task(std::string_view user_message);
    void merge_completion_metadata(const JsonValue& metadata);
    [[nodiscard]] JsonValue redact_value(const JsonValue& value) const;
    JsonValue emit_trace(TaskState& task_state, std::string_view event, JsonValue payload = JsonValue::Object{});
    [[nodiscard]] JsonValue build_report(const TaskState& task_state) const;
    [[nodiscard]] Result<void> record(HistoryItem item);
    [[nodiscard]] JsonValue create_task_checkpoint(TaskState& task_state, std::string_view user_message, std::string_view trigger);
    std::tuple<std::vector<std::string>, std::vector<std::string>, std::vector<std::string>>
        promote_durable_memory(std::string_view user_message, std::string_view final_answer);
    [[nodiscard]] std::pair<std::vector<std::pair<std::string, std::string>>, std::vector<std::string>>
        extract_durable_promotions(std::string_view user_message, std::string_view final_answer) const;
    [[nodiscard]] std::string reject_durable_reason(std::string_view note_text) const;
    [[nodiscard]] std::string history_text() const;
    [[nodiscard]] JsonValue refresh_prefix(bool force = false);
    [[nodiscard]] Result<std::string> spawn_delegate(const JsonValue::Object& args);

    [[nodiscard]] std::string new_task_id() const;
    [[nodiscard]] std::string new_run_id() const;

    [[nodiscard]] IModelClient& model_client() noexcept;
    [[nodiscard]] IRunStore& run_store() noexcept;
    [[nodiscard]] const RuntimeOptions& options() const noexcept;
    [[nodiscard]] const JsonValue& resume_state() const noexcept;
    [[nodiscard]] const JsonValue& last_prompt_metadata() const noexcept;
    [[nodiscard]] std::filesystem::path root() const;
    [[nodiscard]] const WorkspaceContext& workspace() const noexcept;
    [[nodiscard]] std::filesystem::path session_path() const;
    [[nodiscard]] LayeredMemory& memory() noexcept;
    [[nodiscard]] ContextManager& context_manager() noexcept;
    TaskState* current_task_state{nullptr};
    std::filesystem::path current_run_dir;

    // IToolHost
    [[nodiscard]] const std::optional<std::vector<std::string>>& allowed_tools() const override;
    [[nodiscard]] bool repeated_tool_call(std::string_view name, const JsonValue::Object& args) const override;
    [[nodiscard]] bool approve(std::string_view name, const JsonValue::Object& args) override;
    [[nodiscard]] bool read_only() const noexcept override;
    [[nodiscard]] WorkspaceSnapshot capture_workspace_snapshot() const override;
    [[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> diff_workspace_snapshots(
        const WorkspaceSnapshot& before, const WorkspaceSnapshot& after) const override;
    [[nodiscard]] std::string workspace_fingerprint() const override;
    void update_memory_after_tool(std::string_view name, const JsonValue::Object& args, std::string_view result) override;
    void record_process_note_for_tool(std::string_view name, const JsonValue& metadata) override;

    // IContextHost
    [[nodiscard]] const std::string& prefix() const override;
    [[nodiscard]] std::string memory_text() override;
    [[nodiscard]] std::string render_checkpoint_text() const override;
    [[nodiscard]] std::vector<JsonValue> memory_candidates(std::string_view query, std::size_t limit) override;
    [[nodiscard]] const SessionState& session() const override;
    [[nodiscard]] bool feature_enabled(std::string_view name) const override;
    [[nodiscard]] std::string reusable_file_summary(std::string_view path) const override;

    // ICheckpointHost
    [[nodiscard]] SessionState& mutable_session() override;
    [[nodiscard]] const SessionState& checkpoint_session() const override;
    [[nodiscard]] LayeredMemory& checkpoint_memory() override;
    [[nodiscard]] std::filesystem::path checkpoint_root() const override;
    [[nodiscard]] JsonValue runtime_identity() const override;
    Result<void> persist_session() override;

private:
    [[nodiscard]] ToolContext tool_context();
    [[nodiscard]] ToolRegistry apply_tool_allowlist(ToolRegistry tools) const;
    void sync_memory();

    std::shared_ptr<IModelClient> model_client_;
    WorkspaceContext workspace_;
    std::filesystem::path root_;
    WorkspaceGuard guard_;
    std::shared_ptr<ISessionStore> session_store_;
    std::shared_ptr<IRunStore> run_store_;
    SessionState session_;
    RuntimeOptions options_;
    LayeredMemory memory_;
    ToolRegistry tools_;
    std::unique_ptr<ToolExecutor> tool_executor_;
    ModelActionParser parser_;
    PromptPrefix prefix_state_;
    ContextManager context_manager_;
    JsonValue resume_state_{JsonValue::Object{}};
    JsonValue last_prompt_metadata_{JsonValue::Object{}};
    JsonValue last_completion_metadata_{JsonValue::Object{}};
    std::vector<std::string> last_durable_promotions_;
    std::vector<std::string> last_durable_rejections_;
    std::vector<std::string> last_durable_superseded_;
    JsonValue last_tool_result_metadata_{JsonValue::Object{}};
    JsonValue last_prefix_refresh_{JsonValue::Object{{"prefix_changed", JsonValue(false)}, {"workspace_changed", JsonValue(false)}}};
    std::optional<TaskState> current_task_state_storage_;
};

}  // namespace runi
