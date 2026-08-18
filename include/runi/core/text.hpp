#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace runi {

inline constexpr std::size_t kMaxToolOutput = 4000;
inline constexpr std::size_t kMaxHistory = 12000;

[[nodiscard]] std::string trim(std::string_view text);
[[nodiscard]] std::string trim_newlines(std::string_view text);
[[nodiscard]] std::string clip(std::string_view text, std::size_t limit = kMaxToolOutput);
[[nodiscard]] std::string middle(std::string_view text, std::size_t limit);
[[nodiscard]] std::vector<std::string> split_lines(std::string_view text);
[[nodiscard]] std::string join(const std::vector<std::string>& items, std::string_view separator);
[[nodiscard]] std::string lower_ascii(std::string_view text);
[[nodiscard]] std::size_t utf8_length(std::string_view text) noexcept;
[[nodiscard]] std::string utf8_prefix(std::string_view text, std::size_t characters);
[[nodiscard]] std::string utf8_suffix(std::string_view text, std::size_t characters);

}  // namespace runi
