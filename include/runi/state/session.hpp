#pragma once

#include <string>
#include <vector>

#include "runi/core/json_value.hpp"

namespace runi {

struct HistoryItem {
    std::string role;
    std::string content;
    std::string created_at;
    std::string name;
    JsonValue args{JsonValue::Object{}};

    [[nodiscard]] static HistoryItem from_json(const JsonValue& value);
    [[nodiscard]] JsonValue to_json() const;
};

struct SessionState {
    std::string id;
    std::string created_at;
    std::string workspace_root;
    std::vector<HistoryItem> history;
    JsonValue memory{JsonValue::Object{}};
    JsonValue checkpoints{JsonValue::Object{}};
    JsonValue runtime_identity{JsonValue::Object{}};
    JsonValue resume_state{JsonValue::Object{}};

    [[nodiscard]] static SessionState create(std::string workspace_root);
    [[nodiscard]] static SessionState from_json(const JsonValue& value);
    void ensure_shape();
    [[nodiscard]] JsonValue to_json() const;
};

}  // namespace runi
