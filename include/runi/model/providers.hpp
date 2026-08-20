#pragma once

#include <chrono>
#include <memory>
#include <optional>
#include <string>

#include "runi/model/http_client.hpp"
#include "runi/model/model_client.hpp"

namespace runi {

class OllamaModelClient final : public IModelClient {
public:
    OllamaModelClient(std::string model, std::string host, double temperature, double top_p,
        std::chrono::milliseconds timeout, std::shared_ptr<IHttpClient> http = std::make_shared<HttpClient>());
    [[nodiscard]] Result<std::string> complete(std::string_view prompt, std::size_t max_new_tokens,
        const CompletionOptions& options = {}) override;
    [[nodiscard]] bool supports_prompt_cache() const noexcept override;
    [[nodiscard]] const JsonValue& last_completion_metadata() const noexcept override;
    [[nodiscard]] std::string model_name() const override;
    [[nodiscard]] std::string client_name() const override;
private:
    std::string model_, host_;
    double temperature_, top_p_;
    std::chrono::milliseconds timeout_;
    std::shared_ptr<IHttpClient> http_;
    JsonValue metadata_{JsonValue::Object{}};
};

class OpenAICompatibleModelClient final : public IModelClient {
public:
    OpenAICompatibleModelClient(std::string model, std::string base_url, std::string api_key,
        std::optional<double> temperature, std::chrono::milliseconds timeout,
        std::shared_ptr<IHttpClient> http = std::make_shared<HttpClient>());
    [[nodiscard]] Result<std::string> complete(std::string_view prompt, std::size_t max_new_tokens,
        const CompletionOptions& options = {}) override;
    [[nodiscard]] bool supports_prompt_cache() const noexcept override;
    [[nodiscard]] const JsonValue& last_completion_metadata() const noexcept override;
    [[nodiscard]] std::string model_name() const override;
    [[nodiscard]] std::string client_name() const override;
private:
    std::string model_, base_url_, api_key_;
    std::optional<double> temperature_;
    std::chrono::milliseconds timeout_;
    std::shared_ptr<IHttpClient> http_;
    bool cache_supported_{false};
    JsonValue metadata_{JsonValue::Object{}};
};

class AnthropicCompatibleModelClient final : public IModelClient {
public:
    AnthropicCompatibleModelClient(std::string model, std::string base_url, std::string api_key,
        std::optional<double> temperature, std::chrono::milliseconds timeout,
        std::shared_ptr<IHttpClient> http = std::make_shared<HttpClient>());
    [[nodiscard]] Result<std::string> complete(std::string_view prompt, std::size_t max_new_tokens,
        const CompletionOptions& options = {}) override;
    [[nodiscard]] bool supports_prompt_cache() const noexcept override;
    [[nodiscard]] const JsonValue& last_completion_metadata() const noexcept override;
    [[nodiscard]] std::string model_name() const override;
    [[nodiscard]] std::string client_name() const override;
private:
    std::string model_, base_url_, api_key_;
    std::optional<double> temperature_;
    std::chrono::milliseconds timeout_;
    std::shared_ptr<IHttpClient> http_;
    JsonValue metadata_{JsonValue::Object{}};
};

[[nodiscard]] std::string normalize_versioned_base_url(std::string base_url);
[[nodiscard]] std::string extract_openai_text(const JsonValue& data);
[[nodiscard]] std::pair<std::string, JsonValue> extract_openai_response_from_sse(std::string_view body);
[[nodiscard]] JsonValue extract_usage_cache_details(const JsonValue& data);

}  // namespace runi
