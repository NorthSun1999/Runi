#pragma once

#include <cstdint>
#include <initializer_list>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace runi {

class JsonValue {
public:
    using Array = std::vector<JsonValue>;
    using Object = std::map<std::string, JsonValue, std::less<>>;
    using Storage = std::variant<std::nullptr_t, bool, std::int64_t, double, std::string, Array, Object>;

    JsonValue() noexcept;
    JsonValue(std::nullptr_t) noexcept;
    JsonValue(bool value) noexcept;
    JsonValue(int value) noexcept;
    JsonValue(std::size_t value) noexcept;
    JsonValue(std::int64_t value) noexcept;
    JsonValue(double value) noexcept;
    JsonValue(std::string value);
    JsonValue(const char* value);
    JsonValue(Array value);
    JsonValue(Object value);

    [[nodiscard]] bool is_null() const noexcept;
    [[nodiscard]] bool is_bool() const noexcept;
    [[nodiscard]] bool is_integer() const noexcept;
    [[nodiscard]] bool is_number() const noexcept;
    [[nodiscard]] bool is_string() const noexcept;
    [[nodiscard]] bool is_array() const noexcept;
    [[nodiscard]] bool is_object() const noexcept;

    [[nodiscard]] bool as_bool() const;
    [[nodiscard]] std::int64_t as_integer() const;
    [[nodiscard]] double as_number() const;
    [[nodiscard]] const std::string& as_string() const;
    [[nodiscard]] const Array& as_array() const;
    [[nodiscard]] Array& as_array();
    [[nodiscard]] const Object& as_object() const;
    [[nodiscard]] Object& as_object();

    [[nodiscard]] const JsonValue* find(std::string_view key) const noexcept;
    [[nodiscard]] JsonValue* find(std::string_view key) noexcept;
    [[nodiscard]] bool contains(std::string_view key) const noexcept;
    JsonValue& operator[](std::string key);
    const JsonValue& at(std::string_view key) const;

    [[nodiscard]] std::string string_or(std::string_view fallback = {}) const;
    [[nodiscard]] std::int64_t integer_or(std::int64_t fallback = 0) const noexcept;
    [[nodiscard]] bool bool_or(bool fallback = false) const noexcept;

    [[nodiscard]] const Storage& storage() const noexcept;

    friend bool operator==(const JsonValue&, const JsonValue&) = default;

private:
    Storage storage_;
};

}  // namespace runi
