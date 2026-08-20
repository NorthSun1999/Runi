#include "runi/state/sqlite_session_store.hpp"

#include <stdexcept>
#include <utility>

namespace runi {

SqliteSessionStore::SqliteSessionStore(std::shared_ptr<SqliteStateStore> state_store)
    : state_store_(std::move(state_store)) {
    if (!state_store_) throw std::invalid_argument("SQLite session store requires a state store");
}

std::filesystem::path SqliteSessionStore::path(std::string_view session_id) const {
    static_cast<void>(session_id);
    return state_store_->path();
}

Result<std::filesystem::path> SqliteSessionStore::save(const SessionState& session) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        const auto current = state_store_->get_session(session.id);
        if (!current) {
            if (current.error().code != "session_not_found") return Result<std::filesystem::path>::failure(current.error());
            const auto created = state_store_->create_session(session.id, session.workspace_root, session.to_json());
            if (!created) return Result<std::filesystem::path>::failure(created.error());
            return Result<std::filesystem::path>::success(path(session.id));
        }
        const auto updated = state_store_->compare_and_swap_session(
            session.id, current.value().state_version, session.to_json());
        if (updated) return Result<std::filesystem::path>::success(path(session.id));
        if (updated.error().code != "session_version_conflict") {
            return Result<std::filesystem::path>::failure(updated.error());
        }
    }
    return Result<std::filesystem::path>::failure(make_error(
        ErrorCategory::ResumeMismatch, "session_version_conflict", "Session changed repeatedly while saving"));
}

Result<SessionState> SqliteSessionStore::load(std::string_view session_id) const {
    const auto record = state_store_->get_session(session_id);
    if (!record) return Result<SessionState>::failure(record.error());
    auto session = SessionState::from_json(record.value().state);
    if (session.id.empty()) session.id = record.value().id;
    if (session.created_at.empty()) session.created_at = record.value().created_at;
    if (session.workspace_root.empty()) session.workspace_root = record.value().workspace_root;
    session.ensure_shape();
    return Result<SessionState>::success(std::move(session));
}

std::optional<std::string> SqliteSessionStore::latest() const {
    const auto result = state_store_->latest_session_id();
    return result ? result.value() : std::nullopt;
}

}  // namespace runi
