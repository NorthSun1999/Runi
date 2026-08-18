#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "runi/core/result.hpp"

namespace runi {

struct CompletionOptions {
    std::optional<std::string> prompt_cache_key;
    std::optional<std::string> prompt_cache_retention;
};

class IModelClient {
public:
    virtual ~IModelClient() = default;
    [[nodiscard]] virtual Result<std::string> complete(
        std::string_view prompt,
        std::size_t max_new_tokens,
        const CompletionOptions& options = {}) = 0;
    [[nodiscard]] virtual bool supports_prompt_cache() const noexcept = 0;
    [[nodiscard]] virtual const JsonValue& last_completion_metadata() const noexcept = 0;
    [[nodiscard]] virtual std::string model_name() const = 0;
    [[nodiscard]] virtual std::string client_name() const = 0;
};

class FakeModelClient final : public IModelClient {
public:
    explicit FakeModelClient(std::vector<std::string> outputs);
    [[nodiscard]] Result<std::string> complete(
        std::string_view prompt,
        std::size_t max_new_tokens,
        const CompletionOptions& options = {}) override;
    [[nodiscard]] bool supports_prompt_cache() const noexcept override;
    [[nodiscard]] const JsonValue& last_completion_metadata() const noexcept override;
    [[nodiscard]] std::string model_name() const override;
    [[nodiscard]] std::string client_name() const override;

    std::vector<std::string> prompts;

private:
    std::deque<std::string> outputs_;
    JsonValue metadata_{JsonValue::Object{}};
};

}  // namespace runi
