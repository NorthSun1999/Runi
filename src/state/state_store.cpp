#include "runi/state/state_store.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <map>
#include <set>
#include <sqlite3.h>
#include <tuple>
#include <utility>

#include "runi/core/json_codec.hpp"
#include "runi/core/text.hpp"
#include "runi/core/time.hpp"

namespace runi {
namespace {

Error sqlite_error(sqlite3* database, std::string code, std::string message) {
    const auto* detail = database == nullptr ? "SQLite is not open" : sqlite3_errmsg(database);
    return make_error(ErrorCategory::Persistence, std::move(code), std::move(message) + ": " + detail);
}

Result<void> execute(sqlite3* database, const char* sql) {
    char* raw_error = nullptr;
    const auto status = sqlite3_exec(database, sql, nullptr, nullptr, &raw_error);
    if (status == SQLITE_OK) return Result<void>::success();
    std::string detail = raw_error == nullptr ? sqlite3_errmsg(database) : raw_error;
    sqlite3_free(raw_error);
    return Result<void>::failure(make_error(
        ErrorCategory::Persistence, "sqlite_execute_failed", "SQLite statement failed: " + detail));
}

class Statement {
public:
    Statement() = default;
    explicit Statement(sqlite3_stmt* statement) : statement_(statement) {}
    ~Statement() { if (statement_ != nullptr) sqlite3_finalize(statement_); }
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;
    Statement(Statement&& other) noexcept : statement_(std::exchange(other.statement_, nullptr)) {}
    Statement& operator=(Statement&& other) noexcept {
        if (this == &other) return *this;
        if (statement_ != nullptr) sqlite3_finalize(statement_);
        statement_ = std::exchange(other.statement_, nullptr);
        return *this;
    }
    sqlite3_stmt* get() const noexcept { return statement_; }
private:
    sqlite3_stmt* statement_{nullptr};
};

Result<Statement> prepare(sqlite3* database, const char* sql) {
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(database, sql, -1, &statement, nullptr) != SQLITE_OK) {
        return Result<Statement>::failure(sqlite_error(
            database, "sqlite_prepare_failed", "Could not prepare SQLite statement"));
    }
    return Result<Statement>::success(Statement(statement));
}

Result<void> bind_text(sqlite3* database, sqlite3_stmt* statement, int index, std::string_view value) {
    if (sqlite3_bind_text(statement, index, value.data(), static_cast<int>(value.size()), SQLITE_TRANSIENT) == SQLITE_OK) {
        return Result<void>::success();
    }
    return Result<void>::failure(sqlite_error(database, "sqlite_bind_failed", "Could not bind SQLite text value"));
}

std::string column_text(sqlite3_stmt* statement, int index) {
    const auto* text = sqlite3_column_text(statement, index);
    if (text == nullptr) return {};
    return std::string(reinterpret_cast<const char*>(text));
}

JsonValue parse_object_or_empty(std::string_view text) {
    const auto parsed = parse_json(text);
    return parsed ? parsed.value() : JsonValue::Object{};
}

class Transaction {
public:
    explicit Transaction(sqlite3* database) : database_(database) {}
    [[nodiscard]] Result<void> begin() {
        const auto begun = execute(database_, "BEGIN IMMEDIATE");
        active_ = begun.has_value();
        return begun;
    }
    [[nodiscard]] Result<void> commit() {
        const auto committed = execute(database_, "COMMIT");
        if (committed) active_ = false;
        return committed;
    }
    ~Transaction() {
        if (active_) static_cast<void>(execute(database_, "ROLLBACK"));
    }
private:
    sqlite3* database_;
    bool active_{false};
};

Result<RuntimeSessionRecord> read_session(sqlite3* database, std::string_view id) {
    auto statement = prepare(database,
        "SELECT id, workspace_root, state_json, state_version, created_at, updated_at FROM sessions WHERE id = ?");
    if (!statement) return Result<RuntimeSessionRecord>::failure(statement.error());
    const auto bound = bind_text(database, statement.value().get(), 1, id);
    if (!bound) return Result<RuntimeSessionRecord>::failure(bound.error());
    const auto status = sqlite3_step(statement.value().get());
    if (status == SQLITE_DONE) return Result<RuntimeSessionRecord>::failure(make_error(
        ErrorCategory::Persistence, "session_not_found", "Runtime session was not found"));
    if (status != SQLITE_ROW) return Result<RuntimeSessionRecord>::failure(sqlite_error(
        database, "sqlite_read_failed", "Could not read runtime session"));
    return Result<RuntimeSessionRecord>::success(RuntimeSessionRecord{
        column_text(statement.value().get(), 0), column_text(statement.value().get(), 1),
        parse_object_or_empty(column_text(statement.value().get(), 2)),
        sqlite3_column_int64(statement.value().get(), 3), column_text(statement.value().get(), 4),
        column_text(statement.value().get(), 5)});
}

Result<RuntimeRunRecord> read_run(sqlite3* database, std::string_view column, std::string_view value) {
    const auto sql = std::string(
        "SELECT id, session_id, parent_run_id, request_id, request, status, result, error, deadline_ms, created_at, updated_at "
        "FROM runs WHERE ") + std::string(column) + " = ?";
    auto statement = prepare(database, sql.c_str());
    if (!statement) return Result<RuntimeRunRecord>::failure(statement.error());
    const auto bound = bind_text(database, statement.value().get(), 1, value);
    if (!bound) return Result<RuntimeRunRecord>::failure(bound.error());
    const auto status = sqlite3_step(statement.value().get());
    if (status == SQLITE_DONE) return Result<RuntimeRunRecord>::failure(make_error(
        ErrorCategory::Persistence, "run_not_found", "Runtime run was not found"));
    if (status != SQLITE_ROW) return Result<RuntimeRunRecord>::failure(sqlite_error(
        database, "sqlite_read_failed", "Could not read runtime run"));
    const auto parsed_status = run_status_from_string(column_text(statement.value().get(), 5));
    if (!parsed_status.has_value()) return Result<RuntimeRunRecord>::failure(make_error(
        ErrorCategory::Persistence, "invalid_run_status", "Runtime run contains an unknown status"));
    return Result<RuntimeRunRecord>::success(RuntimeRunRecord{
        column_text(statement.value().get(), 0), column_text(statement.value().get(), 1),
        column_text(statement.value().get(), 2), column_text(statement.value().get(), 3),
        column_text(statement.value().get(), 4), *parsed_status, column_text(statement.value().get(), 6),
        column_text(statement.value().get(), 7), sqlite3_column_int64(statement.value().get(), 8),
        column_text(statement.value().get(), 9), column_text(statement.value().get(), 10)});
}

std::vector<std::string> query_tokens(std::string_view value) {
    std::vector<std::string> result;
    std::string token;
    for (const auto character : value) {
        const auto byte = static_cast<unsigned char>(character);
        if (std::isalnum(byte) != 0 || byte >= 0x80U || character == '_' || character == '-') {
            token.push_back(static_cast<char>(std::tolower(byte)));
        } else if (!token.empty()) {
            result.push_back(std::move(token));
            token.clear();
        }
    }
    if (!token.empty()) result.push_back(std::move(token));
    return result;
}

int relevance(const MemoryRecord& record, const std::vector<std::string>& tokens) {
    const auto text = lower_ascii(record.text);
    std::set<std::string, std::less<>> tags;
    for (const auto& tag : record.tags) tags.insert(lower_ascii(tag));
    int score = 0;
    for (const auto& token : tokens) {
        if (tags.contains(token)) score += 10;
        if (text.find(token) != std::string::npos) ++score;
    }
    return score;
}

JsonValue tags_json(const std::vector<std::string>& tags) {
    JsonValue::Array values;
    for (const auto& tag : tags) values.emplace_back(tag);
    return JsonValue(std::move(values));
}

std::vector<std::string> read_tags(std::string_view text) {
    const auto parsed = parse_json(text);
    std::vector<std::string> result;
    if (!parsed || !parsed.value().is_array()) return result;
    for (const auto& item : parsed.value().as_array()) if (item.is_string()) result.push_back(item.as_string());
    return result;
}

bool valid_transition(RunStatus expected, RunStatus desired) {
    switch (expected) {
        case RunStatus::Queued:
            return desired == RunStatus::Running || desired == RunStatus::Cancelled || desired == RunStatus::Failed;
        case RunStatus::Running:
            return desired == RunStatus::Succeeded || desired == RunStatus::Failed || desired == RunStatus::Cancelling ||
                desired == RunStatus::Cancelled || desired == RunStatus::Interrupted;
        case RunStatus::Cancelling:
            return desired == RunStatus::Cancelled || desired == RunStatus::Failed || desired == RunStatus::Interrupted;
        case RunStatus::Interrupted:
            return desired == RunStatus::Queued || desired == RunStatus::Cancelled;
        case RunStatus::Succeeded:
        case RunStatus::Failed:
        case RunStatus::Cancelled:
            return false;
    }
    return false;
}

}  // namespace

std::string_view to_string(RunStatus status) noexcept {
    switch (status) {
        case RunStatus::Queued: return "queued";
        case RunStatus::Running: return "running";
        case RunStatus::Succeeded: return "succeeded";
        case RunStatus::Failed: return "failed";
        case RunStatus::Cancelling: return "cancelling";
        case RunStatus::Cancelled: return "cancelled";
        case RunStatus::Interrupted: return "interrupted";
    }
    return "failed";
}

std::optional<RunStatus> run_status_from_string(std::string_view value) noexcept {
    if (value == "queued") return RunStatus::Queued;
    if (value == "running") return RunStatus::Running;
    if (value == "succeeded") return RunStatus::Succeeded;
    if (value == "failed") return RunStatus::Failed;
    if (value == "cancelling") return RunStatus::Cancelling;
    if (value == "cancelled") return RunStatus::Cancelled;
    if (value == "interrupted") return RunStatus::Interrupted;
    return std::nullopt;
}

std::int64_t unix_time_millis() noexcept {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

SqliteStateStore::SqliteStateStore(std::filesystem::path path, sqlite3* database)
    : path_(std::move(path)), database_(database) {}

SqliteStateStore::~SqliteStateStore() {
    if (database_ != nullptr) sqlite3_close(database_);
}

Result<std::unique_ptr<SqliteStateStore>> SqliteStateStore::open(const std::filesystem::path& path) {
    std::error_code error;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), error);
    if (error) return Result<std::unique_ptr<SqliteStateStore>>::failure(make_error(
        ErrorCategory::Persistence, "state_directory_failed", "Could not create state directory: " + error.message()));
    sqlite3* database = nullptr;
    const auto path_text = path.string();
    if (sqlite3_open_v2(path_text.c_str(), &database, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr) != SQLITE_OK) {
        const auto failure = sqlite_error(database, "sqlite_open_failed", "Could not open runtime state database");
        if (database != nullptr) sqlite3_close(database);
        return Result<std::unique_ptr<SqliteStateStore>>::failure(failure);
    }
    auto store = std::unique_ptr<SqliteStateStore>(new SqliteStateStore(path, database));
    const auto initialized = store->initialize();
    if (!initialized) return Result<std::unique_ptr<SqliteStateStore>>::failure(initialized.error());
    return Result<std::unique_ptr<SqliteStateStore>>::success(std::move(store));
}

Result<void> SqliteStateStore::initialize() {
    sqlite3_busy_timeout(database_, 5000);
    for (const auto* statement : {
        "PRAGMA journal_mode = WAL",
        "PRAGMA foreign_keys = ON",
        "PRAGMA synchronous = NORMAL",
        "CREATE TABLE IF NOT EXISTS sessions ("
            "id TEXT PRIMARY KEY, workspace_root TEXT NOT NULL, state_json TEXT NOT NULL, state_version INTEGER NOT NULL DEFAULT 0, "
            "created_at TEXT NOT NULL, updated_at TEXT NOT NULL)",
        "CREATE TABLE IF NOT EXISTS runs ("
            "id TEXT PRIMARY KEY, session_id TEXT NOT NULL REFERENCES sessions(id) ON DELETE CASCADE, parent_run_id TEXT NOT NULL DEFAULT '', "
            "request_id TEXT NOT NULL UNIQUE, request TEXT NOT NULL, status TEXT NOT NULL, result TEXT NOT NULL DEFAULT '', "
            "error TEXT NOT NULL DEFAULT '', deadline_ms INTEGER NOT NULL DEFAULT 0, created_at TEXT NOT NULL, updated_at TEXT NOT NULL)",
        "CREATE INDEX IF NOT EXISTS runs_session_status_idx ON runs(session_id, status)",
        "CREATE TABLE IF NOT EXISTS run_events ("
            "run_id TEXT NOT NULL REFERENCES runs(id) ON DELETE CASCADE, sequence INTEGER NOT NULL, event_type TEXT NOT NULL, "
            "payload_json TEXT NOT NULL, created_at TEXT NOT NULL, PRIMARY KEY(run_id, sequence))",
        "CREATE TABLE IF NOT EXISTS memory_records ("
            "scope TEXT NOT NULL, record_key TEXT NOT NULL, kind TEXT NOT NULL, text TEXT NOT NULL, tags_json TEXT NOT NULL, "
            "source_path TEXT NOT NULL DEFAULT '', source_sha256 TEXT NOT NULL DEFAULT '', expires_at_ms INTEGER NOT NULL DEFAULT 0, "
            "version INTEGER NOT NULL DEFAULT 1, PRIMARY KEY(scope, record_key))",
        "CREATE INDEX IF NOT EXISTS memory_scope_expiry_idx ON memory_records(scope, expires_at_ms)",
        "PRAGMA user_version = 1"}) {
        const auto result = execute(database_, statement);
        if (!result) return result;
    }
    return Result<void>::success();
}

Result<JsonValue> SqliteStateStore::pragmas() const {
    std::scoped_lock lock(mutex_);
    JsonValue::Object values;
    for (const auto& [name, sql] : std::vector<std::pair<std::string, const char*>>{
        {"journal_mode", "PRAGMA journal_mode"}, {"foreign_keys", "PRAGMA foreign_keys"},
        {"user_version", "PRAGMA user_version"}}) {
        auto statement = prepare(database_, sql);
        if (!statement) return Result<JsonValue>::failure(statement.error());
        if (sqlite3_step(statement.value().get()) != SQLITE_ROW) return Result<JsonValue>::failure(sqlite_error(
            database_, "sqlite_pragma_failed", "Could not read SQLite pragma"));
        if (sqlite3_column_type(statement.value().get(), 0) == SQLITE_INTEGER) {
            values[name] = JsonValue(sqlite3_column_int64(statement.value().get(), 0));
        } else {
            values[name] = JsonValue(lower_ascii(column_text(statement.value().get(), 0)));
        }
    }
    return Result<JsonValue>::success(JsonValue(std::move(values)));
}

Result<RuntimeSessionRecord> SqliteStateStore::create_session(
    std::string id, std::string workspace_root, JsonValue initial_state) {
    std::scoped_lock lock(mutex_);
    const auto timestamp = now_utc();
    auto statement = prepare(database_,
        "INSERT OR IGNORE INTO sessions(id, workspace_root, state_json, state_version, created_at, updated_at) VALUES(?, ?, ?, 0, ?, ?)");
    if (!statement) return Result<RuntimeSessionRecord>::failure(statement.error());
    const auto state = dump_json(initial_state);
    for (const auto& [index, value] : std::vector<std::pair<int, std::string_view>>{
        {1, id}, {2, workspace_root}, {3, state}, {4, timestamp}, {5, timestamp}}) {
        const auto bound = bind_text(database_, statement.value().get(), index, value);
        if (!bound) return Result<RuntimeSessionRecord>::failure(bound.error());
    }
    if (sqlite3_step(statement.value().get()) != SQLITE_DONE) return Result<RuntimeSessionRecord>::failure(sqlite_error(
        database_, "session_create_failed", "Could not create runtime session"));
    return read_session(database_, id);
}

Result<RuntimeSessionRecord> SqliteStateStore::get_session(std::string_view id) const {
    std::scoped_lock lock(mutex_);
    return read_session(database_, id);
}

Result<RuntimeSessionRecord> SqliteStateStore::compare_and_swap_session(
    std::string_view id, std::int64_t expected_version, JsonValue state) {
    std::scoped_lock lock(mutex_);
    auto statement = prepare(database_,
        "UPDATE sessions SET state_json = ?, state_version = state_version + 1, updated_at = ? WHERE id = ? AND state_version = ?");
    if (!statement) return Result<RuntimeSessionRecord>::failure(statement.error());
    const auto serialized = dump_json(state);
    const auto timestamp = now_utc();
    auto bound = bind_text(database_, statement.value().get(), 1, serialized); if (!bound) return Result<RuntimeSessionRecord>::failure(bound.error());
    bound = bind_text(database_, statement.value().get(), 2, timestamp); if (!bound) return Result<RuntimeSessionRecord>::failure(bound.error());
    bound = bind_text(database_, statement.value().get(), 3, id); if (!bound) return Result<RuntimeSessionRecord>::failure(bound.error());
    sqlite3_bind_int64(statement.value().get(), 4, expected_version);
    if (sqlite3_step(statement.value().get()) != SQLITE_DONE) return Result<RuntimeSessionRecord>::failure(sqlite_error(
        database_, "session_update_failed", "Could not update runtime session"));
    if (sqlite3_changes(database_) == 0) return Result<RuntimeSessionRecord>::failure(make_error(
        ErrorCategory::ResumeMismatch, "session_version_conflict", "Runtime session version changed before commit"));
    return read_session(database_, id);
}

Result<std::optional<std::string>> SqliteStateStore::latest_session_id() const {
    std::scoped_lock lock(mutex_);
    auto statement = prepare(database_, "SELECT id FROM sessions ORDER BY updated_at DESC, id DESC LIMIT 1");
    if (!statement) return Result<std::optional<std::string>>::failure(statement.error());
    const auto status = sqlite3_step(statement.value().get());
    if (status == SQLITE_DONE) return Result<std::optional<std::string>>::success(std::nullopt);
    if (status != SQLITE_ROW) return Result<std::optional<std::string>>::failure(sqlite_error(
        database_, "sqlite_read_failed", "Could not read latest runtime session"));
    return Result<std::optional<std::string>>::success(column_text(statement.value().get(), 0));
}

Result<CreateRunResult> SqliteStateStore::create_run(
    std::string id, std::string session_id, std::string request_id, std::string request,
    std::string parent_run_id, std::int64_t deadline_ms) {
    std::scoped_lock lock(mutex_);
    const auto existing = read_run(database_, "request_id", request_id);
    if (existing) return Result<CreateRunResult>::success(CreateRunResult{existing.value(), false});
    if (existing.error().code != "run_not_found") return Result<CreateRunResult>::failure(existing.error());
    Transaction transaction(database_);
    const auto begun = transaction.begin();
    if (!begun) return Result<CreateRunResult>::failure(begun.error());
    auto statement = prepare(database_,
        "INSERT INTO runs(id, session_id, parent_run_id, request_id, request, status, deadline_ms, created_at, updated_at) "
        "VALUES(?, ?, ?, ?, ?, 'queued', ?, ?, ?)");
    if (!statement) return Result<CreateRunResult>::failure(statement.error());
    const auto timestamp = now_utc();
    for (const auto& [index, value] : std::vector<std::pair<int, std::string_view>>{
        {1, id}, {2, session_id}, {3, parent_run_id}, {4, request_id}, {5, request}, {7, timestamp}, {8, timestamp}}) {
        const auto bound = bind_text(database_, statement.value().get(), index, value);
        if (!bound) return Result<CreateRunResult>::failure(bound.error());
    }
    sqlite3_bind_int64(statement.value().get(), 6, deadline_ms);
    if (sqlite3_step(statement.value().get()) != SQLITE_DONE) return Result<CreateRunResult>::failure(sqlite_error(
        database_, "run_create_failed", "Could not create runtime run"));
    const auto committed = transaction.commit();
    if (!committed) return Result<CreateRunResult>::failure(committed.error());
    const auto created = read_run(database_, "id", id);
    if (!created) return Result<CreateRunResult>::failure(created.error());
    return Result<CreateRunResult>::success(CreateRunResult{created.value(), true});
}

Result<RuntimeRunRecord> SqliteStateStore::get_run(std::string_view id) const {
    std::scoped_lock lock(mutex_);
    return read_run(database_, "id", id);
}

Result<RuntimeRunRecord> SqliteStateStore::transition_run(
    std::string_view id, RunStatus expected, RunStatus desired, std::string result, std::string error) {
    std::scoped_lock lock(mutex_);
    if (!valid_transition(expected, desired)) return Result<RuntimeRunRecord>::failure(make_error(
        ErrorCategory::ResumeMismatch, "run_state_conflict", "Requested runtime run transition is not allowed"));
    auto statement = prepare(database_,
        "UPDATE runs SET status = ?, result = ?, error = ?, updated_at = ? WHERE id = ? AND status = ?");
    if (!statement) return Result<RuntimeRunRecord>::failure(statement.error());
    const auto timestamp = now_utc();
    for (const auto& [index, value] : std::vector<std::pair<int, std::string_view>>{
        {1, to_string(desired)}, {2, result}, {3, error}, {4, timestamp}, {5, id}, {6, to_string(expected)}}) {
        const auto bound = bind_text(database_, statement.value().get(), index, value);
        if (!bound) return Result<RuntimeRunRecord>::failure(bound.error());
    }
    if (sqlite3_step(statement.value().get()) != SQLITE_DONE) return Result<RuntimeRunRecord>::failure(sqlite_error(
        database_, "run_update_failed", "Could not update runtime run"));
    if (sqlite3_changes(database_) == 0) return Result<RuntimeRunRecord>::failure(make_error(
        ErrorCategory::ResumeMismatch, "run_state_conflict", "Runtime run is no longer in the expected state"));
    return read_run(database_, "id", id);
}

Result<std::size_t> SqliteStateStore::recover_running_runs() {
    std::scoped_lock lock(mutex_);
    auto statement = prepare(database_, "UPDATE runs SET status = 'interrupted', updated_at = ? WHERE status IN ('running', 'cancelling')");
    if (!statement) return Result<std::size_t>::failure(statement.error());
    const auto timestamp = now_utc();
    const auto bound = bind_text(database_, statement.value().get(), 1, timestamp);
    if (!bound) return Result<std::size_t>::failure(bound.error());
    if (sqlite3_step(statement.value().get()) != SQLITE_DONE) return Result<std::size_t>::failure(sqlite_error(
        database_, "run_recovery_failed", "Could not recover interrupted runtime runs"));
    return Result<std::size_t>::success(static_cast<std::size_t>(sqlite3_changes(database_)));
}

Result<RuntimeEvent> SqliteStateStore::append_event(std::string_view run_id, std::string type, JsonValue payload) {
    std::scoped_lock lock(mutex_);
    Transaction transaction(database_);
    const auto begun = transaction.begin();
    if (!begun) return Result<RuntimeEvent>::failure(begun.error());
    auto sequence_query = prepare(database_, "SELECT COALESCE(MAX(sequence), 0) + 1 FROM run_events WHERE run_id = ?");
    if (!sequence_query) return Result<RuntimeEvent>::failure(sequence_query.error());
    auto bound = bind_text(database_, sequence_query.value().get(), 1, run_id);
    if (!bound) return Result<RuntimeEvent>::failure(bound.error());
    if (sqlite3_step(sequence_query.value().get()) != SQLITE_ROW) return Result<RuntimeEvent>::failure(sqlite_error(
        database_, "event_sequence_failed", "Could not allocate runtime event sequence"));
    const auto sequence = sqlite3_column_int64(sequence_query.value().get(), 0);
    auto insert = prepare(database_,
        "INSERT INTO run_events(run_id, sequence, event_type, payload_json, created_at) VALUES(?, ?, ?, ?, ?)");
    if (!insert) return Result<RuntimeEvent>::failure(insert.error());
    const auto serialized = dump_json(payload);
    const auto timestamp = now_utc();
    bound = bind_text(database_, insert.value().get(), 1, run_id); if (!bound) return Result<RuntimeEvent>::failure(bound.error());
    sqlite3_bind_int64(insert.value().get(), 2, sequence);
    bound = bind_text(database_, insert.value().get(), 3, type); if (!bound) return Result<RuntimeEvent>::failure(bound.error());
    bound = bind_text(database_, insert.value().get(), 4, serialized); if (!bound) return Result<RuntimeEvent>::failure(bound.error());
    bound = bind_text(database_, insert.value().get(), 5, timestamp); if (!bound) return Result<RuntimeEvent>::failure(bound.error());
    if (sqlite3_step(insert.value().get()) != SQLITE_DONE) return Result<RuntimeEvent>::failure(sqlite_error(
        database_, "event_append_failed", "Could not append runtime event"));
    const auto committed = transaction.commit();
    if (!committed) return Result<RuntimeEvent>::failure(committed.error());
    return Result<RuntimeEvent>::success(RuntimeEvent{
        std::string(run_id), sequence, std::move(type), std::move(payload), timestamp});
}

Result<std::vector<RuntimeEvent>> SqliteStateStore::events_after(
    std::string_view run_id, std::int64_t sequence) const {
    std::scoped_lock lock(mutex_);
    auto statement = prepare(database_,
        "SELECT run_id, sequence, event_type, payload_json, created_at FROM run_events "
        "WHERE run_id = ? AND sequence > ? ORDER BY sequence ASC");
    if (!statement) return Result<std::vector<RuntimeEvent>>::failure(statement.error());
    auto bound = bind_text(database_, statement.value().get(), 1, run_id);
    if (!bound) return Result<std::vector<RuntimeEvent>>::failure(bound.error());
    sqlite3_bind_int64(statement.value().get(), 2, sequence);
    std::vector<RuntimeEvent> events;
    while (true) {
        const auto status = sqlite3_step(statement.value().get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) return Result<std::vector<RuntimeEvent>>::failure(sqlite_error(
            database_, "event_read_failed", "Could not read runtime events"));
        events.push_back(RuntimeEvent{column_text(statement.value().get(), 0), sqlite3_column_int64(statement.value().get(), 1),
            column_text(statement.value().get(), 2), parse_object_or_empty(column_text(statement.value().get(), 3)),
            column_text(statement.value().get(), 4)});
    }
    return Result<std::vector<RuntimeEvent>>::success(std::move(events));
}

Result<MemoryRecord> SqliteStateStore::put_memory(MemoryRecord record) {
    std::scoped_lock lock(mutex_);
    auto statement = prepare(database_,
        "INSERT INTO memory_records(scope, record_key, kind, text, tags_json, source_path, source_sha256, expires_at_ms, version) "
        "VALUES(?, ?, ?, ?, ?, ?, ?, ?, 1) ON CONFLICT(scope, record_key) DO UPDATE SET "
        "kind=excluded.kind, text=excluded.text, tags_json=excluded.tags_json, source_path=excluded.source_path, "
        "source_sha256=excluded.source_sha256, expires_at_ms=excluded.expires_at_ms, version=memory_records.version+1");
    if (!statement) return Result<MemoryRecord>::failure(statement.error());
    const auto serialized_tags = dump_json(tags_json(record.tags));
    for (const auto& [index, value] : std::vector<std::pair<int, std::string_view>>{
        {1, record.scope}, {2, record.key}, {3, record.kind}, {4, record.text}, {5, serialized_tags},
        {6, record.source_path}, {7, record.source_sha256}}) {
        const auto bound = bind_text(database_, statement.value().get(), index, value);
        if (!bound) return Result<MemoryRecord>::failure(bound.error());
    }
    sqlite3_bind_int64(statement.value().get(), 8, record.expires_at_ms);
    if (sqlite3_step(statement.value().get()) != SQLITE_DONE) return Result<MemoryRecord>::failure(sqlite_error(
        database_, "memory_write_failed", "Could not write memory record"));
    auto version_query = prepare(database_, "SELECT version FROM memory_records WHERE scope = ? AND record_key = ?");
    if (!version_query) return Result<MemoryRecord>::failure(version_query.error());
    auto bound = bind_text(database_, version_query.value().get(), 1, record.scope); if (!bound) return Result<MemoryRecord>::failure(bound.error());
    bound = bind_text(database_, version_query.value().get(), 2, record.key); if (!bound) return Result<MemoryRecord>::failure(bound.error());
    if (sqlite3_step(version_query.value().get()) == SQLITE_ROW) record.version = sqlite3_column_int64(version_query.value().get(), 0);
    return Result<MemoryRecord>::success(std::move(record));
}

Result<std::vector<MemoryRecord>> SqliteStateStore::recall_memory(
    std::string_view scope, std::string_view query, std::size_t limit, std::int64_t now_ms) const {
    std::scoped_lock lock(mutex_);
    auto statement = prepare(database_,
        "SELECT scope, record_key, kind, text, tags_json, source_path, source_sha256, expires_at_ms, version "
        "FROM memory_records WHERE scope = ? AND (expires_at_ms = 0 OR expires_at_ms > ?)");
    if (!statement) return Result<std::vector<MemoryRecord>>::failure(statement.error());
    auto bound = bind_text(database_, statement.value().get(), 1, scope);
    if (!bound) return Result<std::vector<MemoryRecord>>::failure(bound.error());
    sqlite3_bind_int64(statement.value().get(), 2, now_ms);
    const auto tokens = query_tokens(query);
    std::vector<std::pair<int, MemoryRecord>> ranked;
    while (true) {
        const auto status = sqlite3_step(statement.value().get());
        if (status == SQLITE_DONE) break;
        if (status != SQLITE_ROW) return Result<std::vector<MemoryRecord>>::failure(sqlite_error(
            database_, "memory_read_failed", "Could not read memory records"));
        MemoryRecord record{column_text(statement.value().get(), 0), column_text(statement.value().get(), 1),
            column_text(statement.value().get(), 2), column_text(statement.value().get(), 3),
            read_tags(column_text(statement.value().get(), 4)), column_text(statement.value().get(), 5),
            column_text(statement.value().get(), 6), sqlite3_column_int64(statement.value().get(), 7),
            sqlite3_column_int64(statement.value().get(), 8)};
        const auto score = relevance(record, tokens);
        if (tokens.empty() || score > 0) ranked.emplace_back(score, std::move(record));
    }
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return std::tie(left.first, left.second.version) > std::tie(right.first, right.second.version);
    });
    std::vector<MemoryRecord> result;
    for (std::size_t index = 0; index < std::min(limit, ranked.size()); ++index) {
        result.push_back(std::move(ranked[index].second));
    }
    return Result<std::vector<MemoryRecord>>::success(std::move(result));
}

Result<std::size_t> SqliteStateStore::invalidate_memory_source(
    std::string_view source_path, std::string_view current_sha256) {
    std::scoped_lock lock(mutex_);
    auto statement = prepare(database_,
        "DELETE FROM memory_records WHERE source_path = ? AND source_sha256 <> ?");
    if (!statement) return Result<std::size_t>::failure(statement.error());
    auto bound = bind_text(database_, statement.value().get(), 1, source_path);
    if (!bound) return Result<std::size_t>::failure(bound.error());
    bound = bind_text(database_, statement.value().get(), 2, current_sha256);
    if (!bound) return Result<std::size_t>::failure(bound.error());
    if (sqlite3_step(statement.value().get()) != SQLITE_DONE) return Result<std::size_t>::failure(sqlite_error(
        database_, "memory_invalidate_failed", "Could not invalidate stale memory records"));
    return Result<std::size_t>::success(static_cast<std::size_t>(sqlite3_changes(database_)));
}

const std::filesystem::path& SqliteStateStore::path() const noexcept {
    return path_;
}

}  // namespace runi
