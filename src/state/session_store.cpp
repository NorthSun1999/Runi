#include "runi/state/session_store.hpp"

#include <fstream>
#include <system_error>

#include "runi/core/json_codec.hpp"

namespace runi {

SessionStore::SessionStore(std::filesystem::path root) : root_(std::move(root)) {
    std::filesystem::create_directories(root_);
}

std::filesystem::path SessionStore::path(std::string_view session_id) const {
    return root_ / (std::string(session_id) + ".json");
}

Result<std::filesystem::path> SessionStore::save(const SessionState& session) {
    const auto target = path(session.id);
    std::ofstream output(target, std::ios::binary | std::ios::trunc);
    if (!output) return Result<std::filesystem::path>::failure(make_error(
        ErrorCategory::Persistence, "session_save_failed", "Could not save session: " + target.string()));
    output << dump_json(session.to_json(), 2, true);
    if (!output) return Result<std::filesystem::path>::failure(make_error(
        ErrorCategory::Persistence, "session_save_failed", "Could not save session: " + target.string()));
    return Result<std::filesystem::path>::success(target);
}

Result<SessionState> SessionStore::load(std::string_view session_id) const {
    const auto target = path(session_id);
    std::ifstream input(target, std::ios::binary);
    if (!input) return Result<SessionState>::failure(make_error(
        ErrorCategory::Persistence, "session_load_failed", "Could not load session: " + target.string()));
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    const auto decoded = parse_json(text);
    if (!decoded || !decoded.value().is_object()) return Result<SessionState>::failure(make_error(
        ErrorCategory::Persistence, "session_parse_failed", "Session JSON is invalid: " + target.string()));
    return Result<SessionState>::success(SessionState::from_json(decoded.value()));
}

std::optional<std::string> SessionStore::latest() const {
    std::optional<std::filesystem::path> newest;
    std::filesystem::file_time_type newest_time{};
    std::error_code error;
    if (!std::filesystem::exists(root_, error)) return std::nullopt;
    for (const auto& entry : std::filesystem::directory_iterator(root_, error)) {
        if (error || !entry.is_regular_file() || entry.path().extension() != ".json") continue;
        const auto time = entry.last_write_time(error);
        if (error) { error.clear(); continue; }
        if (!newest.has_value() || time > newest_time) { newest = entry.path(); newest_time = time; }
    }
    return newest.has_value() ? std::optional<std::string>(newest->stem().string()) : std::nullopt;
}

const std::filesystem::path& SessionStore::root() const noexcept { return root_; }

}  // namespace runi
