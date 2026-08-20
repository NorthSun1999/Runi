#pragma once

#include <memory>

#include "runi/state/session_store.hpp"
#include "runi/state/state_store.hpp"

namespace runi {

class SqliteSessionStore final : public ISessionStore {
public:
    explicit SqliteSessionStore(std::shared_ptr<SqliteStateStore> state_store);

    [[nodiscard]] std::filesystem::path path(std::string_view session_id) const override;
    Result<std::filesystem::path> save(const SessionState& session) override;
    [[nodiscard]] Result<SessionState> load(std::string_view session_id) const override;
    [[nodiscard]] std::optional<std::string> latest() const override;

private:
    std::shared_ptr<SqliteStateStore> state_store_;
};

}  // namespace runi
