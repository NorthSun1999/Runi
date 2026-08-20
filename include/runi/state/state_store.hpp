#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runi/core/result.hpp"

struct sqlite3;

namespace runi {

enum class RunStatus {
    Queued,
    Running,
    Succeeded,
    Failed,
    Cancelling,
    Cancelled,
    Interrupted,
};

[[nodiscard]] std::string_view to_string(RunStatus status) noexcept;
[[nodiscard]] std::optional<RunStatus> run_status_from_string(std::string_view value) noexcept;
[[nodiscard]] std::int64_t unix_time_millis() noexcept;

struct RuntimeSessionRecord {
    std::string id;
    std::string workspace_root;
    JsonValue state{JsonValue::Object{}};
    std::int64_t state_version{0};
    std::string created_at;
    std::string updated_at;
};

struct RuntimeRunRecord {
    std::string id;
    std::string session_id;
    std::string parent_run_id;
    std::string request_id;
    std::string request;
    RunStatus status{RunStatus::Queued};
    std::string result;
    std::string error;
    std::int64_t deadline_ms{0};
    std::string created_at;
    std::string updated_at;
};

struct CreateRunResult {
    RuntimeRunRecord record;
    bool created{false};
};

struct RuntimeEvent {
    std::string run_id;
    std::int64_t sequence{0};
    std::string type;
    JsonValue payload{JsonValue::Object{}};
    std::string created_at;
};

struct MemoryRecord {
    std::string scope;
    std::string key;
    std::string kind;
    std::string text;
    std::vector<std::string> tags;
    std::string source_path;
    std::string source_sha256;
    std::int64_t expires_at_ms{0};
    std::int64_t version{0};
};

class SqliteStateStore {
public:
    [[nodiscard]] static Result<std::unique_ptr<SqliteStateStore>> open(const std::filesystem::path& path);
    ~SqliteStateStore();

    SqliteStateStore(const SqliteStateStore&) = delete;
    SqliteStateStore& operator=(const SqliteStateStore&) = delete;

    [[nodiscard]] Result<JsonValue> pragmas() const;
    [[nodiscard]] Result<RuntimeSessionRecord> create_session(
        std::string id, std::string workspace_root, JsonValue initial_state = JsonValue::Object{});
    [[nodiscard]] Result<RuntimeSessionRecord> get_session(std::string_view id) const;
    [[nodiscard]] Result<RuntimeSessionRecord> compare_and_swap_session(
        std::string_view id, std::int64_t expected_version, JsonValue state);
    [[nodiscard]] Result<std::optional<std::string>> latest_session_id() const;

    [[nodiscard]] Result<CreateRunResult> create_run(
        std::string id, std::string session_id, std::string request_id, std::string request,
        std::string parent_run_id = {}, std::int64_t deadline_ms = 0);
    [[nodiscard]] Result<RuntimeRunRecord> get_run(std::string_view id) const;
    [[nodiscard]] Result<RuntimeRunRecord> transition_run(
        std::string_view id, RunStatus expected, RunStatus desired,
        std::string result = {}, std::string error = {});
    [[nodiscard]] Result<std::size_t> recover_running_runs();

    [[nodiscard]] Result<RuntimeEvent> append_event(
        std::string_view run_id, std::string type, JsonValue payload = JsonValue::Object{});
    [[nodiscard]] Result<std::vector<RuntimeEvent>> events_after(
        std::string_view run_id, std::int64_t sequence) const;

    [[nodiscard]] Result<MemoryRecord> put_memory(MemoryRecord record);
    [[nodiscard]] Result<std::vector<MemoryRecord>> recall_memory(
        std::string_view scope, std::string_view query, std::size_t limit,
        std::int64_t now_ms = unix_time_millis()) const;
    [[nodiscard]] Result<std::size_t> invalidate_memory_source(
        std::string_view source_path, std::string_view current_sha256);

    [[nodiscard]] const std::filesystem::path& path() const noexcept;

private:
    SqliteStateStore(std::filesystem::path path, sqlite3* database);
    [[nodiscard]] Result<void> initialize();

    std::filesystem::path path_;
    sqlite3* database_{nullptr};
    mutable std::mutex mutex_;
};

}  // namespace runi
