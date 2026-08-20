#pragma once

#include <string>
#include <variant>

#include "runi/core/json_value.hpp"

namespace runi {

struct ToolCall {
    std::string name;
    JsonValue::Object args;
};

struct RetryRequest {
    std::string notice;
};

struct FinalAnswer {
    std::string text;
};

using ModelAction = std::variant<ToolCall, RetryRequest, FinalAnswer>;

}  // namespace runi
