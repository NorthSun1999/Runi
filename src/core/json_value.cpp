#include "runi/core/json_value.hpp"

#include <utility>

namespace runi {

JsonValue::JsonValue() noexcept : storage_(nullptr) {}
JsonValue::JsonValue(std::nullptr_t) noexcept : storage_(nullptr) {}
JsonValue::JsonValue(bool value) noexcept : storage_(value) {}
JsonValue::JsonValue(int value) noexcept : storage_(static_cast<std::int64_t>(value)) {}
JsonValue::JsonValue(std::size_t value) noexcept : storage_(static_cast<std::int64_t>(value)) {}
JsonValue::JsonValue(std::int64_t value) noexcept : storage_(value) {}
JsonValue::JsonValue(double value) noexcept : storage_(value) {}
JsonValue::JsonValue(std::string value) : storage_(std::move(value)) {}
JsonValue::JsonValue(const char* value) : storage_(std::string(value == nullptr ? "" : value)) {}
JsonValue::JsonValue(Array value) : storage_(std::move(value)) {}
JsonValue::JsonValue(Object value) : storage_(std::move(value)) {}

bool JsonValue::is_null() const noexcept { return std::holds_alternative<std::nullptr_t>(storage_); }
bool JsonValue::is_bool() const noexcept { return std::holds_alternative<bool>(storage_); }
bool JsonValue::is_integer() const noexcept { return std::holds_alternative<std::int64_t>(storage_); }
bool JsonValue::is_number() const noexcept { return is_integer() || std::holds_alternative<double>(storage_); }
bool JsonValue::is_string() const noexcept { return std::holds_alternative<std::string>(storage_); }
bool JsonValue::is_array() const noexcept { return std::holds_alternative<Array>(storage_); }
bool JsonValue::is_object() const noexcept { return std::holds_alternative<Object>(storage_); }

bool JsonValue::as_bool() const { return std::get<bool>(storage_); }
std::int64_t JsonValue::as_integer() const { return std::get<std::int64_t>(storage_); }

double JsonValue::as_number() const {
    if (is_integer()) {
        return static_cast<double>(as_integer());
    }
    return std::get<double>(storage_);
}

const std::string& JsonValue::as_string() const { return std::get<std::string>(storage_); }
const JsonValue::Array& JsonValue::as_array() const { return std::get<Array>(storage_); }
JsonValue::Array& JsonValue::as_array() { return std::get<Array>(storage_); }
const JsonValue::Object& JsonValue::as_object() const { return std::get<Object>(storage_); }
JsonValue::Object& JsonValue::as_object() { return std::get<Object>(storage_); }

const JsonValue* JsonValue::find(std::string_view key) const noexcept {
    if (!is_object()) {
        return nullptr;
    }
    const auto& object = std::get<Object>(storage_);
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

JsonValue* JsonValue::find(std::string_view key) noexcept {
    if (!is_object()) {
        return nullptr;
    }
    auto& object = std::get<Object>(storage_);
    const auto iterator = object.find(key);
    return iterator == object.end() ? nullptr : &iterator->second;
}

bool JsonValue::contains(std::string_view key) const noexcept {
    return find(key) != nullptr;
}

JsonValue& JsonValue::operator[](std::string key) {
    if (!is_object()) {
        storage_ = Object{};
    }
    return std::get<Object>(storage_)[std::move(key)];
}

const JsonValue& JsonValue::at(std::string_view key) const {
    return as_object().at(std::string(key));
}

std::string JsonValue::string_or(std::string_view fallback) const {
    return is_string() ? as_string() : std::string(fallback);
}

std::int64_t JsonValue::integer_or(std::int64_t fallback) const noexcept {
    return is_integer() ? std::get<std::int64_t>(storage_) : fallback;
}

bool JsonValue::bool_or(bool fallback) const noexcept {
    return is_bool() ? std::get<bool>(storage_) : fallback;
}

const JsonValue::Storage& JsonValue::storage() const noexcept { return storage_; }

}  // namespace runi
