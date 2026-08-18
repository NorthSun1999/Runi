#include "runi/security.hpp"

#include <algorithm>
#include <cctype>

#include "runi/config.hpp"
#include "runi/core/text.hpp"

namespace runi {
namespace {

std::string upper_ascii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::toupper(character));
    });
    return result;
}

bool ends_with(std::string_view value, std::string_view suffix) {
    return value.size() >= suffix.size() && value.substr(value.size() - suffix.size()) == suffix;
}

}  // namespace

bool looks_sensitive_env_name(std::string_view name) {
    const auto upper = upper_ascii(name);
    for (const std::string marker : {"API_KEY", "TOKEN", "SECRET", "PASSWORD"}) {
        if (upper == marker || ends_with(upper, marker) || ends_with(upper, "_" + marker)) return true;
    }
    return false;
}

bool is_secret_env_name(std::string_view name, const std::set<std::string, std::less<>>& secret_names) {
    const auto upper = upper_ascii(name);
    return secret_names.contains(upper) || looks_sensitive_env_name(upper);
}

std::vector<std::pair<std::string, std::string>> configured_secret_env_items(
    const std::set<std::string, std::less<>>& secret_names) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [name, value] : environment_items()) {
        if (!value.empty() && secret_names.contains(upper_ascii(name))) result.emplace_back(name, value);
    }
    return result;
}

std::vector<std::pair<std::string, std::string>> detected_secret_env_items(
    const std::set<std::string, std::less<>>& secret_names) {
    std::vector<std::pair<std::string, std::string>> result;
    for (const auto& [name, value] : environment_items()) {
        if (!value.empty() && is_secret_env_name(name, secret_names)) result.emplace_back(name, value);
    }
    return result;
}

JsonValue secret_env_summary(const std::set<std::string, std::less<>>& secret_names, bool detected) {
    const auto items = detected ? detected_secret_env_items(secret_names) : configured_secret_env_items(secret_names);
    JsonValue::Array names;
    for (const auto& [name, value] : items) { static_cast<void>(value); names.emplace_back(name); }
    return JsonValue::Object{{"secret_env_count", JsonValue(items.size())}, {"secret_env_names", JsonValue(std::move(names))}};
}

std::string redact_text(std::string_view text, const std::set<std::string, std::less<>>& secret_names) {
    std::string result(text);
    auto items = detected_secret_env_items(secret_names);
    std::sort(items.begin(), items.end(), [](const auto& left, const auto& right) { return left.second.size() > right.second.size(); });
    for (const auto& [name, value] : items) {
        static_cast<void>(name);
        std::size_t position = 0;
        while (!value.empty() && (position = result.find(value, position)) != std::string::npos) {
            result.replace(position, value.size(), kRedactedValue);
            position += kRedactedValue.size();
        }
    }
    return result;
}

JsonValue redact_artifact(const JsonValue& value, const std::set<std::string, std::less<>>& secret_names, std::string_view key) {
    if (!key.empty() && is_secret_env_name(key, secret_names)) return JsonValue(std::string(kRedactedValue));
    if (value.is_string()) return JsonValue(redact_text(value.as_string(), secret_names));
    if (value.is_array()) {
        JsonValue::Array result;
        for (const auto& item : value.as_array()) result.push_back(redact_artifact(item, secret_names, key));
        return JsonValue(std::move(result));
    }
    if (value.is_object()) {
        JsonValue::Object result;
        for (const auto& [item_key, item] : value.as_object()) result.emplace(item_key, redact_artifact(item, secret_names, item_key));
        return JsonValue(std::move(result));
    }
    return value;
}

std::map<std::string, std::string, std::less<>> shell_environment(
    const std::vector<std::string>& allowlist,
    const std::filesystem::path& root) {
    const auto source = environment_items();
    std::map<std::string, std::string, std::less<>> filtered;
    for (const auto& name : allowlist) {
        if (const auto iterator = source.find(name); iterator != source.end()) filtered.emplace(*iterator);
    }
    filtered["PWD"] = root.string();
    if (!filtered.contains("PATH")) {
        if (const auto iterator = source.find("PATH"); iterator != source.end()) filtered.emplace(*iterator);
    }
    return filtered;
}

}  // namespace runi
