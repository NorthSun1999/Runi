#pragma once

#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "runi/core/json_value.hpp"

namespace runi {

inline constexpr std::string_view kRedactedValue = "<redacted>";

[[nodiscard]] bool looks_sensitive_env_name(std::string_view name);
[[nodiscard]] bool is_secret_env_name(std::string_view name, const std::set<std::string, std::less<>>& secret_names = {});
[[nodiscard]] std::vector<std::pair<std::string, std::string>> configured_secret_env_items(
    const std::set<std::string, std::less<>>& secret_names = {});
[[nodiscard]] std::vector<std::pair<std::string, std::string>> detected_secret_env_items(
    const std::set<std::string, std::less<>>& secret_names = {});
[[nodiscard]] JsonValue secret_env_summary(const std::set<std::string, std::less<>>& secret_names = {}, bool detected = false);
[[nodiscard]] std::string redact_text(std::string_view text, const std::set<std::string, std::less<>>& secret_names = {});
[[nodiscard]] JsonValue redact_artifact(
    const JsonValue& value,
    const std::set<std::string, std::less<>>& secret_names = {},
    std::string_view key = {});
[[nodiscard]] std::map<std::string, std::string, std::less<>> shell_environment(
    const std::vector<std::string>& allowlist,
    const std::filesystem::path& root);

}  // namespace runi
