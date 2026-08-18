#pragma once

#include <string>
#include <string_view>

#include "runi/core/result.hpp"

namespace runi {

[[nodiscard]] Result<JsonValue> parse_json(std::string_view text);
[[nodiscard]] std::string dump_json(const JsonValue& value, int indent = -1, bool ensure_ascii = false);
[[nodiscard]] std::string dump_compatible_json(const JsonValue& value, bool ensure_ascii = true);

}  // namespace runi
