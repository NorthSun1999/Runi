#pragma once

#include <optional>
#include <stdexcept>
#include <utility>
#include <variant>

#include "runi/core/error.hpp"

namespace runi {

template <typename T>
class Result {
public:
    [[nodiscard]] static Result success(T value) {
        return Result(std::in_place_index<0>, std::move(value));
    }

    [[nodiscard]] static Result failure(Error error) {
        return Result(std::in_place_index<1>, std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return storage_.index() == 0;
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    [[nodiscard]] const T& value() const {
        if (!has_value()) {
            throw std::logic_error("Result does not contain a value");
        }
        return std::get<0>(storage_);
    }

    [[nodiscard]] T& value() {
        if (!has_value()) {
            throw std::logic_error("Result does not contain a value");
        }
        return std::get<0>(storage_);
    }

    [[nodiscard]] const Error& error() const {
        if (has_value()) {
            throw std::logic_error("Result does not contain an error");
        }
        return std::get<1>(storage_);
    }

private:
    template <std::size_t Index, typename Value>
    explicit Result(std::in_place_index_t<Index> index, Value&& value)
        : storage_(index, std::forward<Value>(value)) {}

    std::variant<T, Error> storage_;
};

template <>
class Result<void> {
public:
    [[nodiscard]] static Result success() {
        return Result(std::nullopt);
    }

    [[nodiscard]] static Result failure(Error error) {
        return Result(std::move(error));
    }

    [[nodiscard]] bool has_value() const noexcept {
        return !error_.has_value();
    }

    explicit operator bool() const noexcept {
        return has_value();
    }

    void value() const {
        if (!has_value()) {
            throw std::logic_error("Result does not contain a value");
        }
    }

    [[nodiscard]] const Error& error() const {
        if (has_value()) {
            throw std::logic_error("Result does not contain an error");
        }
        return *error_;
    }

private:
    explicit Result(std::optional<Error> error) : error_(std::move(error)) {}
    explicit Result(Error error) : error_(std::move(error)) {}

    std::optional<Error> error_;
};

}  // namespace runi
