#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <stop_token>
#include <string>
#include <vector>

#include "runi/core/result.hpp"
#include "runi/orchestration/execution.hpp"

namespace runi {

enum class AgentRole {
    Coordinator,
    Worker,
    Reviewer,
};

[[nodiscard]] std::string_view to_string(AgentRole role) noexcept;

struct AgentTask {
    std::string id;
    AgentRole role{AgentRole::Worker};
    std::set<std::string, std::less<>> required_capabilities;
    std::string input;
    JsonValue context_snapshot{JsonValue::Object{}};
    std::chrono::steady_clock::time_point deadline;
};

using AgentFunction = std::function<Result<std::string>(const AgentTask&, std::stop_token)>;

struct AgentDescriptor {
    std::string id;
    AgentRole role{AgentRole::Worker};
    std::set<std::string, std::less<>> capabilities;
    std::size_t max_concurrency{1};
    bool read_only{true};
    AgentFunction execute;
};

struct AgentRegistrySnapshot {
    std::size_t registered{0};
    std::size_t active{0};
    std::size_t capacity{0};
};

class AgentRegistry;

class AgentLease {
public:
    AgentLease() = default;
    ~AgentLease();
    AgentLease(AgentLease&& other) noexcept;
    AgentLease& operator=(AgentLease&& other) noexcept;
    AgentLease(const AgentLease&) = delete;
    AgentLease& operator=(const AgentLease&) = delete;

    [[nodiscard]] Result<std::string> execute(const AgentTask& task, std::stop_token stop_token = {}) const;
    [[nodiscard]] const std::string& agent_id() const noexcept;
    explicit operator bool() const noexcept;

private:
    struct SharedState;
    AgentLease(std::shared_ptr<SharedState> state, std::string agent_id, AgentFunction function);
    void release() noexcept;

    std::shared_ptr<SharedState> state_;
    std::string agent_id_;
    AgentFunction function_;
    friend class AgentRegistry;
};

class AgentRegistry {
public:
    AgentRegistry();

    [[nodiscard]] Result<void> add(AgentDescriptor descriptor);
    [[nodiscard]] Result<void> remove(std::string_view id);
    [[nodiscard]] Result<AgentLease> acquire(
        AgentRole role,
        const std::set<std::string, std::less<>>& required_capabilities,
        std::chrono::steady_clock::time_point deadline,
        std::stop_token stop_token = {});
    [[nodiscard]] AgentRegistrySnapshot snapshot() const;

private:
    std::shared_ptr<AgentLease::SharedState> state_;
};

struct AgentTaskOutcome {
    std::string task_id;
    bool success{false};
    std::string output;
    Error error;
};

struct MultiAgentOptions {
    bool fail_fast{false};
};

class MultiAgentRuntime {
public:
    MultiAgentRuntime(AgentRegistry& registry, BoundedExecutor& executor);
    [[nodiscard]] Result<std::vector<AgentTaskOutcome>> run_parallel(
        const std::vector<AgentTask>& tasks,
        const MultiAgentOptions& options = {},
        std::stop_token stop_token = {});

private:
    AgentRegistry& registry_;
    BoundedExecutor& executor_;
};

struct PatchProposal {
    std::string path;
    std::string expected_sha256;
    std::string new_content;
};

class WorkspaceCommitter {
public:
    explicit WorkspaceCommitter(std::filesystem::path workspace_root);
    [[nodiscard]] Result<std::vector<std::string>> commit(const std::vector<PatchProposal>& proposals);

private:
    std::filesystem::path workspace_root_;
    std::mutex mutex_;
};

}  // namespace runi
