#include "runi/state/session.hpp"

#include <utility>

#include "runi/core/time.hpp"

namespace runi {

HistoryItem HistoryItem::from_json(const JsonValue& value) {
    HistoryItem item;
    if (const auto* field = value.find("role")) item.role = field->string_or();
    if (const auto* field = value.find("content")) item.content = field->string_or();
    if (const auto* field = value.find("created_at")) item.created_at = field->string_or();
    if (const auto* field = value.find("name")) item.name = field->string_or();
    if (const auto* field = value.find("args"); field != nullptr && field->is_object()) item.args = *field;
    return item;
}

JsonValue HistoryItem::to_json() const {
    JsonValue::Object result{
        {"content", JsonValue(content)},
        {"created_at", JsonValue(created_at)},
        {"role", JsonValue(role)},
    };
    if (role == "tool") {
        result.emplace("args", args);
        result.emplace("name", JsonValue(name));
    }
    return result;
}

SessionState SessionState::create(std::string root) {
    SessionState state;
    state.id = new_session_id();
    state.created_at = now_utc();
    state.workspace_root = std::move(root);
    state.ensure_shape();
    return state;
}

SessionState SessionState::from_json(const JsonValue& value) {
    SessionState state;
    if (const auto* field = value.find("id")) state.id = field->string_or();
    if (const auto* field = value.find("created_at")) state.created_at = field->string_or();
    if (const auto* field = value.find("workspace_root")) state.workspace_root = field->string_or();
    if (const auto* field = value.find("history"); field != nullptr && field->is_array()) {
        for (const auto& item : field->as_array()) if (item.is_object()) state.history.push_back(HistoryItem::from_json(item));
    }
    if (const auto* field = value.find("memory")) state.memory = *field;
    if (const auto* field = value.find("checkpoints")) state.checkpoints = *field;
    if (const auto* field = value.find("runtime_identity")) state.runtime_identity = *field;
    if (const auto* field = value.find("resume_state")) state.resume_state = *field;
    state.ensure_shape();
    return state;
}

void SessionState::ensure_shape() {
    if (!memory.is_object()) memory = JsonValue::Object{};
    if (!checkpoints.is_object()) checkpoints = JsonValue::Object{};
    if (!checkpoints.contains("current_id")) checkpoints["current_id"] = JsonValue("");
    if (!checkpoints.contains("items") || !checkpoints.at("items").is_object()) checkpoints["items"] = JsonValue::Object{};
    if (!runtime_identity.is_object()) runtime_identity = JsonValue::Object{};
    if (!resume_state.is_object()) resume_state = JsonValue::Object{};
}

JsonValue SessionState::to_json() const {
    JsonValue::Array history_json;
    history_json.reserve(history.size());
    for (const auto& item : history) history_json.push_back(item.to_json());
    return JsonValue::Object{
        {"checkpoints", checkpoints},
        {"created_at", JsonValue(created_at)},
        {"history", JsonValue(std::move(history_json))},
        {"id", JsonValue(id)},
        {"memory", memory},
        {"resume_state", resume_state},
        {"runtime_identity", runtime_identity},
        {"workspace_root", JsonValue(workspace_root)},
    };
}

}  // namespace runi
