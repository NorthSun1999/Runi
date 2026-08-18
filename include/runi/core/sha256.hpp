#pragma once

#include <filesystem>
#include <string>
#include <string_view>

#include "runi/core/result.hpp"

namespace runi {

[[nodiscard]] std::string sha256(std::string_view data);
[[nodiscard]] Result<std::string> sha256_file(const std::filesystem::path& path);

}  // namespace runi
