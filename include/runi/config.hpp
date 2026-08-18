#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runi/core/result.hpp"

namespace runi {

[[nodiscard]] std::optional<std::filesystem::path> find_project_env(const std::filesystem::path& start);
Result<std::map<std::string, std::string, std::less<>>> load_project_env(
    const std::filesystem::path& start,
    bool override_existing = true);
[[nodiscard]] std::string provider_env(
    std::string_view name,
    const std::vector<std::string>& legacy_names = {},
    std::string_view fallback = {});
[[nodiscard]] std::map<std::string, std::string, std::less<>> environment_items();
bool set_environment_value(std::string_view name, std::string_view value, bool override_existing = true);

}  // namespace runi
