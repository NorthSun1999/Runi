#include "runi/orchestration/multi_agent.hpp"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <map>
#include <optional>
#include <utility>

#include "runi/core/sha256.hpp"
#include "runi/tool/workspace.hpp"

namespace runi {
namespace {

bool contains_all(
    const std::set<std::string, std::less<>>& available,
    const std::set<std::string, std::less<>>& required) {
    return std::all_of(required.begin(), required.end(), [&](const auto& capability) {
        return available.contains(capability);
    });
}

Error cancelled_error() {
    return make_error(ErrorCategory::Timeout, "agent_cancelled", "Agent task was cancelled");
}

struct StagedPatch {
    std::string relative;
    std::filesystem::path target;
    std::filesystem::path temporary;
    std::filesystem::path backup;
    bool existed{false};
    bool target_replaced{false};
    bool backup_created{false};
};

std::atomic<std::uint64_t> patch_sequence{0};

void cleanup_staged(std::vector<StagedPatch>& staged) noexcept {
    std::error_code error;
    for (auto& patch : staged) {
        std::filesystem::remove(patch.temporary, error);
        error.clear();
        std::filesystem::remove(patch.backup, error);
        error.clear();
    }
}

void rollback_staged(std::vector<StagedPatch>& staged) noexcept {
    std::error_code error;
    for (auto iterator = staged.rbegin(); iterator != staged.rend(); ++iterator) {
        auto& patch = *iterator;
        if (patch.target_replaced) {
            std::filesystem::remove(patch.target, error);
            error.clear();
        }
        if (patch.backup_created) {
            std::filesystem::rename(patch.backup, patch.target, error);
            error.clear();
        }
        std::filesystem::remove(patch.temporary, error);
        error.clear();
    }
}

}  // namespace

struct AgentLease::SharedState {
    struct Entry {
        AgentDescriptor descriptor;
        std::size_t active_tasks{0};
    };
    mutable std::mutex mutex;
    std::condition_variable_any condition;
    std::map<std::string, Entry, std::less<>> entries;
};

std::string_view to_string(AgentRole role) noexcept {
    switch (role) {
        case AgentRole::Coordinator: return "coordinator";
        case AgentRole::Worker: return "worker";
        case AgentRole::Reviewer: return "reviewer";
    }
    return "worker";
}

AgentLease::AgentLease(std::shared_ptr<SharedState> state, std::string agent_id, AgentFunction function)
    : state_(std::move(state)), agent_id_(std::move(agent_id)), function_(std::move(function)) {}

AgentLease::~AgentLease() {
    release();
}

AgentLease::AgentLease(AgentLease&& other) noexcept
    : state_(std::move(other.state_)), agent_id_(std::move(other.agent_id_)), function_(std::move(other.function_)) {}

AgentLease& AgentLease::operator=(AgentLease&& other) noexcept {
    if (this == &other) return *this;
    release();
    state_ = std::move(other.state_);
    agent_id_ = std::move(other.agent_id_);
    function_ = std::move(other.function_);
    return *this;
}

void AgentLease::release() noexcept {
    if (!state_) return;
    {
        std::scoped_lock lock(state_->mutex);
        const auto found = state_->entries.find(agent_id_);
        if (found != state_->entries.end() && found->second.active_tasks != 0) --found->second.active_tasks;
    }
    state_->condition.notify_all();
    state_.reset();
    function_ = {};
}

Result<std::string> AgentLease::execute(const AgentTask& task, std::stop_token stop_token) const {
    if (!state_ || !function_) return Result<std::string>::failure(make_error(
        ErrorCategory::Internal, "invalid_agent_lease", "Agent lease is no longer active"));
    if (stop_token.stop_requested()) return Result<std::string>::failure(cancelled_error());
    return function_(task, stop_token);
}

const std::string& AgentLease::agent_id() const noexcept {
    return agent_id_;
}

AgentLease::operator bool() const noexcept {
    return state_ != nullptr;
}

AgentRegistry::AgentRegistry() : state_(std::make_shared<AgentLease::SharedState>()) {}

Result<void> AgentRegistry::add(AgentDescriptor descriptor) {
    if (descriptor.id.empty()) return Result<void>::failure(make_error(
        ErrorCategory::Validation, "invalid_agent_id", "Agent id must not be empty"));
    if (descriptor.max_concurrency == 0) return Result<void>::failure(make_error(
        ErrorCategory::Validation, "invalid_agent_capacity", "Agent max_concurrency must be positive"));
    if (!descriptor.execute) return Result<void>::failure(make_error(
        ErrorCategory::Validation, "invalid_agent_executor", "Agent executor must be registered"));
    std::scoped_lock lock(state_->mutex);
    if (state_->entries.contains(descriptor.id)) return Result<void>::failure(make_error(
        ErrorCategory::Validation, "duplicate_agent", "Agent id is already registered"));
    const auto id = descriptor.id;
    state_->entries.emplace(id, AgentLease::SharedState::Entry{std::move(descriptor), 0});
    state_->condition.notify_all();
    return Result<void>::success();
}

Result<void> AgentRegistry::remove(std::string_view id) {
    std::scoped_lock lock(state_->mutex);
    const auto found = state_->entries.find(id);
    if (found == state_->entries.end()) return Result<void>::failure(make_error(
        ErrorCategory::Validation, "agent_not_found", "Agent is not registered"));
    if (found->second.active_tasks != 0) return Result<void>::failure(make_error(
        ErrorCategory::Validation, "agent_busy", "Agent still has active leases"));
    state_->entries.erase(found);
    return Result<void>::success();
}

Result<AgentLease> AgentRegistry::acquire(
    AgentRole role,
    const std::set<std::string, std::less<>>& required_capabilities,
    std::chrono::steady_clock::time_point deadline,
    std::stop_token stop_token) {
    std::unique_lock lock(state_->mutex);
    std::stop_callback stop_callback(stop_token, [state = state_] { state->condition.notify_all(); });
    const auto matches = [&](const AgentLease::SharedState::Entry& entry) {
        return entry.descriptor.role == role && contains_all(entry.descriptor.capabilities, required_capabilities);
    };
    const auto any_matching = [&] {
        return std::any_of(state_->entries.begin(), state_->entries.end(), [&](const auto& item) { return matches(item.second); });
    };
    if (!any_matching()) return Result<AgentLease>::failure(make_error(
        ErrorCategory::Validation, "agent_not_found", "No registered agent satisfies the requested role and capabilities"));

    while (true) {
        auto selected = state_->entries.end();
        for (auto iterator = state_->entries.begin(); iterator != state_->entries.end(); ++iterator) {
            auto& entry = iterator->second;
            if (!matches(entry) || entry.active_tasks >= entry.descriptor.max_concurrency) continue;
            if (selected == state_->entries.end() || entry.active_tasks < selected->second.active_tasks) selected = iterator;
        }
        if (selected != state_->entries.end()) {
            ++selected->second.active_tasks;
            return Result<AgentLease>::success(AgentLease(
                state_, selected->first, selected->second.descriptor.execute));
        }
        if (stop_token.stop_requested()) return Result<AgentLease>::failure(cancelled_error());
        if (std::chrono::steady_clock::now() >= deadline) return Result<AgentLease>::failure(make_error(
            ErrorCategory::Timeout, "agent_capacity_timeout", "Timed out waiting for an available agent capacity", true));
        state_->condition.wait_until(lock, deadline);
    }
}

AgentRegistrySnapshot AgentRegistry::snapshot() const {
    std::scoped_lock lock(state_->mutex);
    AgentRegistrySnapshot result;
    result.registered = state_->entries.size();
    for (const auto& [id, entry] : state_->entries) {
        static_cast<void>(id);
        result.active += entry.active_tasks;
        result.capacity += entry.descriptor.max_concurrency;
    }
    return result;
}

MultiAgentRuntime::MultiAgentRuntime(AgentRegistry& registry, BoundedExecutor& executor)
    : registry_(registry), executor_(executor) {}

Result<std::vector<AgentTaskOutcome>> MultiAgentRuntime::run_parallel(
    const std::vector<AgentTask>& tasks,
    const MultiAgentOptions& options,
    std::stop_token stop_token) {
    auto shared_stop = std::make_shared<std::stop_source>();
    std::stop_callback external_stop(stop_token, [shared_stop] { shared_stop->request_stop(); });
    std::vector<std::optional<std::future<AgentTaskOutcome>>> futures(tasks.size());
    std::vector<AgentTaskOutcome> outcomes(tasks.size());
    for (std::size_t index = 0; index < tasks.size(); ++index) {
        const auto task = tasks[index];
        auto submitted = executor_.submit([this, task, shared_stop, options](std::stop_token executor_stop) mutable {
            if (executor_stop.stop_requested()) shared_stop->request_stop();
            AgentTaskOutcome outcome;
            outcome.task_id = task.id;
            if (shared_stop->stop_requested()) {
                outcome.error = cancelled_error();
                return outcome;
            }
            auto lease = registry_.acquire(task.role, task.required_capabilities, task.deadline, shared_stop->get_token());
            if (!lease) {
                outcome.error = lease.error();
                if (options.fail_fast) shared_stop->request_stop();
                return outcome;
            }
            auto result = lease.value().execute(task, shared_stop->get_token());
            if (!result) {
                outcome.error = result.error();
                if (options.fail_fast) shared_stop->request_stop();
                return outcome;
            }
            outcome.success = true;
            outcome.output = std::move(result.value());
            return outcome;
        });
        outcomes[index].task_id = task.id;
        if (!submitted) {
            outcomes[index].error = submitted.error();
            if (options.fail_fast) shared_stop->request_stop();
        } else {
            futures[index] = std::move(submitted.value());
        }
    }
    for (std::size_t index = 0; index < futures.size(); ++index) {
        if (!futures[index].has_value()) continue;
        try {
            outcomes[index] = futures[index]->get();
        } catch (const std::exception& error) {
            outcomes[index].task_id = tasks[index].id;
            outcomes[index].error = make_error(
                ErrorCategory::Internal, "agent_task_exception", error.what());
            if (options.fail_fast) shared_stop->request_stop();
        }
    }
    return Result<std::vector<AgentTaskOutcome>>::success(std::move(outcomes));
}

WorkspaceCommitter::WorkspaceCommitter(std::filesystem::path workspace_root)
    : workspace_root_(std::filesystem::absolute(std::move(workspace_root)).lexically_normal()) {}

Result<std::vector<std::string>> WorkspaceCommitter::commit(const std::vector<PatchProposal>& proposals) {
    std::scoped_lock lock(mutex_);
    if (proposals.empty()) return Result<std::vector<std::string>>::success({});
    WorkspaceGuard guard(workspace_root_);
    std::set<std::filesystem::path> targets;
    std::vector<StagedPatch> staged;
    staged.reserve(proposals.size());
    const auto sequence = patch_sequence.fetch_add(1, std::memory_order_relaxed) + 1;

    for (std::size_t index = 0; index < proposals.size(); ++index) {
        const auto& proposal = proposals[index];
        const auto resolved = guard.resolve(proposal.path);
        if (!resolved) {
            cleanup_staged(staged);
            return Result<std::vector<std::string>>::failure(resolved.error());
        }
        if (!targets.insert(resolved.value()).second) {
            cleanup_staged(staged);
            return Result<std::vector<std::string>>::failure(make_error(
                ErrorCategory::Validation, "duplicate_patch_target", "Patch batch contains the same target more than once"));
        }
        const bool exists = std::filesystem::exists(resolved.value());
        if (!exists || proposal.expected_sha256.empty()) {
            cleanup_staged(staged);
            return Result<std::vector<std::string>>::failure(make_error(
                ErrorCategory::ResumeMismatch, "workspace_conflict", "Patch target does not match its expected base version"));
        }
        const auto hash = sha256_file(resolved.value());
        if (!hash || hash.value() != proposal.expected_sha256) {
            cleanup_staged(staged);
            return Result<std::vector<std::string>>::failure(make_error(
                ErrorCategory::ResumeMismatch, "workspace_conflict", "Patch target changed after the proposal was created"));
        }
        const auto suffix = ".runi-" + std::to_string(sequence) + "-" + std::to_string(index);
        StagedPatch item;
        item.relative = proposal.path;
        item.target = resolved.value();
        item.temporary = resolved.value().parent_path() / (resolved.value().filename().string() + suffix + ".tmp");
        item.backup = resolved.value().parent_path() / (resolved.value().filename().string() + suffix + ".bak");
        item.existed = true;
        staged.push_back(std::move(item));
        const auto written = write_text_file(staged.back().temporary, proposal.new_content);
        if (!written) {
            cleanup_staged(staged);
            return Result<std::vector<std::string>>::failure(written.error());
        }
    }

    std::error_code error;
    for (auto& patch : staged) {
        std::filesystem::rename(patch.target, patch.backup, error);
        if (error) {
            rollback_staged(staged);
            return Result<std::vector<std::string>>::failure(make_error(
                ErrorCategory::Persistence, "workspace_commit_failed", "Could not stage original file: " + error.message()));
        }
        patch.backup_created = true;
        std::filesystem::rename(patch.temporary, patch.target, error);
        if (error) {
            rollback_staged(staged);
            return Result<std::vector<std::string>>::failure(make_error(
                ErrorCategory::Persistence, "workspace_commit_failed", "Could not install staged patch: " + error.message()));
        }
        patch.target_replaced = true;
    }
    std::vector<std::string> committed;
    for (auto& patch : staged) {
        std::filesystem::remove(patch.backup, error);
        error.clear();
        committed.push_back(std::move(patch.relative));
    }
    return Result<std::vector<std::string>>::success(std::move(committed));
}

}  // namespace runi
