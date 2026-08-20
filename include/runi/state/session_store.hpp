#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "runi/core/result.hpp"
#include "runi/state/session.hpp"

namespace runi {

class ISessionStore {
public:
    virtual ~ISessionStore() = default;
    [[nodiscard]] virtual std::filesystem::path path(std::string_view session_id) const = 0;
    virtual Result<std::filesystem::path> save(const SessionState& session) = 0;
    [[nodiscard]] virtual Result<SessionState> load(std::string_view session_id) const = 0;
    [[nodiscard]] virtual std::optional<std::string> latest() const = 0;
};

class SessionStore final : public ISessionStore {
public:
    explicit SessionStore(std::filesystem::path root);
    [[nodiscard]] std::filesystem::path path(std::string_view session_id) const override;
    Result<std::filesystem::path> save(const SessionState& session) override;
    [[nodiscard]] Result<SessionState> load(std::string_view session_id) const override;
    [[nodiscard]] std::optional<std::string> latest() const override;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

private:
    std::filesystem::path root_;
};

}  // namespace runi
