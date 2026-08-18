#include "runi/providers.hpp"

#include <algorithm>
#include <thread>

#include "runi/core/json_codec.hpp"
#include "runi/core/text.hpp"

namespace runi {
namespace {

constexpr std::string_view kUserAgent = "runi/0.1";

std::string json_error_text(const JsonValue& value) {
    if (value.is_string()) return value.as_string();
    return dump_compatible_json(value);
}

Result<JsonValue> response_json(const HttpResponse& response, std::string_view backend) {
    auto parsed = parse_json(response.body);
    if (parsed) return parsed;
    return Result<JsonValue>::failure(make_error(ErrorCategory::ModelProtocol, "non_json_response",
        std::string(backend) + " error: backend returned non-JSON content that could not be parsed"));
}

Result<HttpResponse> post_with_retries(
    const IHttpClient& http,
    const std::string& url,
    const std::map<std::string, std::string, std::less<>>& headers,
    const std::string& body,
    std::chrono::milliseconds timeout,
    std::string_view backend,
    std::string_view unreachable_message) {
    for (int attempt = 0; attempt < 3; ++attempt) {
        auto response = http.post(url, headers, body, timeout);
        if (!response) {
            if (attempt < 2) {
                std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
                continue;
            }
            return Result<HttpResponse>::failure(make_error(
                ErrorCategory::ModelTransport, "backend_unreachable", std::string(unreachable_message), true));
        }
        if (response.value().status >= 500 && attempt < 2) {
            std::this_thread::sleep_for(std::chrono::milliseconds(500 * (attempt + 1)));
            continue;
        }
        if (response.value().status >= 400) return Result<HttpResponse>::failure(make_error(
            ErrorCategory::ModelTransport, "http_error", std::string(backend) + " request failed with HTTP " +
                std::to_string(response.value().status) + ": " + response.value().body,
            response.value().status >= 500));
        return response;
    }
    return Result<HttpResponse>::failure(make_error(ErrorCategory::ModelTransport, "backend_unreachable",
        std::string(unreachable_message), true));
}

const JsonValue* first_present(const JsonValue& object, std::initializer_list<std::string_view> keys) {
    for (const auto key : keys) {
        if (const auto* value = object.find(key); value != nullptr && !value->is_null()) return value;
    }
    return nullptr;
}

}  // namespace

std::string normalize_versioned_base_url(std::string base_url) {
    while (!base_url.empty() && base_url.back() == '/') base_url.pop_back();
    if (!base_url.ends_with("/v1")) base_url += "/v1";
    return base_url;
}

std::string extract_openai_text(const JsonValue& data) {
    if (!data.is_object()) return {};
    if (const auto* value = data.find("output_text"); value != nullptr && value->is_string() && !value->as_string().empty()) {
        return value->as_string();
    }
    if (const auto* output = data.find("output"); output != nullptr && output->is_array()) {
        for (const auto& item : output->as_array()) {
            if (!item.is_object()) continue;
            const auto* contents = item.find("content");
            if (contents == nullptr || !contents->is_array()) continue;
            for (const auto& content : contents->as_array()) {
                if (!content.is_object()) continue;
                const auto* text = content.find("text");
                if (text != nullptr && text->is_string() && !text->as_string().empty()) return text->as_string();
            }
        }
    }
    const auto* choices = data.find("choices");
    if (choices != nullptr && choices->is_array() && !choices->as_array().empty()) {
        const auto& choice = choices->as_array().front();
        const auto* message = choice.is_object() ? choice.find("message") : nullptr;
        const auto* content = message != nullptr && message->is_object() ? message->find("content") : nullptr;
        if (content != nullptr && content->is_string()) return content->as_string();
        if (content != nullptr && content->is_array()) {
            for (const auto& item : content->as_array()) {
                const auto* text = item.is_object() ? item.find("text") : nullptr;
                if (text != nullptr && text->is_string() && !text->as_string().empty()) return text->as_string();
            }
        }
    }
    return {};
}

std::pair<std::string, JsonValue> extract_openai_response_from_sse(std::string_view body) {
    JsonValue last_response{JsonValue::Object{}};
    std::string deltas;
    for (auto line : split_lines(body)) {
        line = trim(line);
        if (!line.starts_with("data:")) continue;
        auto payload = trim(std::string_view(line).substr(5));
        if (payload.empty() || payload == "[DONE]") continue;
        const auto parsed = parse_json(payload);
        if (!parsed) continue;
        JsonValue event = parsed.value();
        if (!event.is_object()) continue;
        const auto event_type = event.find("type") == nullptr ? std::string{} : event.find("type")->string_or();
        if (const auto* response = event.find("response"); response != nullptr && response->is_object()) {
            last_response = *response;
            if (event_type == "response.completed") {
                const auto text = extract_openai_text(*response);
                if (!text.empty()) return {text, *response};
            }
        }
        if (event_type == "response.output_text.delta") {
            if (const auto* delta = event.find("delta"); delta != nullptr && delta->is_string()) deltas += delta->as_string();
        } else if (event_type == "response.output_text.done") {
            if (const auto* text = event.find("text"); text != nullptr && text->is_string() && !text->as_string().empty()) {
                return {text->as_string(), last_response};
            }
        } else {
            const auto text = extract_openai_text(event);
            if (!text.empty()) return {text, event};
        }
    }
    if (!deltas.empty()) return {deltas, last_response};
    if (last_response.is_object()) return {extract_openai_text(last_response), last_response};
    return {{}, JsonValue::Object{}};
}

JsonValue extract_usage_cache_details(const JsonValue& data) {
    const auto* usage = data.is_object() ? data.find("usage") : nullptr;
    const JsonValue empty{JsonValue::Object{}};
    if (usage == nullptr || !usage->is_object()) usage = &empty;
    const auto* input = first_present(*usage, {"input_tokens", "prompt_tokens"});
    const auto* output = first_present(*usage, {"output_tokens", "completion_tokens"});
    const auto* details = first_present(*usage, {"input_tokens_details", "prompt_tokens_details"});
    std::int64_t cached = 0;
    if (details != nullptr && details->is_object()) {
        if (const auto* value = details->find("cached_tokens"); value != nullptr) cached = value->integer_or();
    }
    return JsonValue::Object{
        {"cache_hit", JsonValue(cached > 0)}, {"cached_tokens", JsonValue(cached)},
        {"input_tokens", input == nullptr ? JsonValue(nullptr) : *input},
        {"output_tokens", output == nullptr ? JsonValue(nullptr) : *output},
        {"total_tokens", usage->find("total_tokens") == nullptr ? JsonValue(nullptr) : *usage->find("total_tokens")}};
}

OllamaModelClient::OllamaModelClient(std::string model, std::string host, double temperature, double top_p,
    std::chrono::milliseconds timeout, std::shared_ptr<IHttpClient> http)
    : model_(std::move(model)), host_(std::move(host)), temperature_(temperature), top_p_(top_p), timeout_(timeout), http_(std::move(http)) {
    while (!host_.empty() && host_.back() == '/') host_.pop_back();
}

Result<std::string> OllamaModelClient::complete(std::string_view prompt, std::size_t max_new_tokens, const CompletionOptions&) {
    metadata_ = JsonValue::Object{};
    const JsonValue payload{JsonValue::Object{
        {"model", JsonValue(model_)}, {"options", JsonValue::Object{{"num_predict", JsonValue(max_new_tokens)},
            {"temperature", JsonValue(temperature_)}, {"top_p", JsonValue(top_p_)}}},
        {"prompt", JsonValue(std::string(prompt))}, {"raw", JsonValue(false)}, {"stream", JsonValue(false)}, {"think", JsonValue(false)}}};
    const auto response = http_->post(host_ + "/api/generate", {{"Content-Type", "application/json"}}, dump_json(payload), timeout_);
    if (!response || response.value().status >= 400) {
        if (response && response.value().status >= 400) return Result<std::string>::failure(make_error(
            ErrorCategory::ModelTransport, "http_error", "Ollama request failed with HTTP " +
                std::to_string(response.value().status) + ": " + response.value().body));
        return Result<std::string>::failure(make_error(ErrorCategory::ModelTransport, "ollama_unreachable",
            "Could not reach Ollama.\nMake sure `ollama serve` is running and the model is available.\nHost: " +
                host_ + "\nModel: " + model_, true));
    }
    const auto data = response_json(response.value(), "Ollama");
    if (!data) return Result<std::string>::failure(data.error());
    if (const auto* error = data.value().find("error"); error != nullptr && !error->is_null()) return Result<std::string>::failure(make_error(
        ErrorCategory::ModelProtocol, "ollama_error", "Ollama error: " + json_error_text(*error)));
    const auto* value = data.value().find("response");
    return Result<std::string>::success(value == nullptr ? std::string{} : value->string_or());
}
bool OllamaModelClient::supports_prompt_cache() const noexcept { return false; }
const JsonValue& OllamaModelClient::last_completion_metadata() const noexcept { return metadata_; }
std::string OllamaModelClient::model_name() const { return model_; }
std::string OllamaModelClient::client_name() const { return "OllamaModelClient"; }

OpenAICompatibleModelClient::OpenAICompatibleModelClient(std::string model, std::string base_url, std::string api_key,
    std::optional<double> temperature, std::chrono::milliseconds timeout, std::shared_ptr<IHttpClient> http)
    : model_(std::move(model)), base_url_(normalize_versioned_base_url(std::move(base_url))), api_key_(std::move(api_key)),
      temperature_(temperature), timeout_(timeout), http_(std::move(http)) {
    cache_supported_ = base_url_.find("openai.com") != std::string::npos || base_url_.find("right.codes") != std::string::npos;
}

Result<std::string> OpenAICompatibleModelClient::complete(std::string_view prompt, std::size_t max_new_tokens, const CompletionOptions& options) {
    metadata_ = JsonValue::Object{};
    JsonValue::Object payload{
        {"input", JsonValue::Array{JsonValue::Object{{"content", JsonValue::Array{JsonValue::Object{
            {"text", JsonValue(std::string(prompt))}, {"type", JsonValue("input_text")}}}}, {"role", JsonValue("user")}}}},
        {"max_output_tokens", JsonValue(max_new_tokens)}, {"model", JsonValue(model_)}, {"stream", JsonValue(false)}};
    if (temperature_.has_value()) payload["temperature"] = JsonValue(*temperature_);
    if (cache_supported_ && options.prompt_cache_key.has_value() && !options.prompt_cache_key->empty()) payload["prompt_cache_key"] = JsonValue(*options.prompt_cache_key);
    if (cache_supported_ && options.prompt_cache_retention.has_value() && !options.prompt_cache_retention->empty()) payload["prompt_cache_retention"] = JsonValue(*options.prompt_cache_retention);
    std::map<std::string, std::string, std::less<>> headers{{"Accept", "application/json"},
        {"Content-Type", "application/json"}, {"User-Agent", std::string(kUserAgent)}};
    if (!api_key_.empty()) headers["Authorization"] = "Bearer " + api_key_;
    const auto unreachable = "Could not reach the OpenAI-compatible backend.\nBase URL: " + base_url_ + "\nModel: " + model_;
    const auto response = post_with_retries(*http_, base_url_ + "/responses", headers, dump_json(JsonValue(std::move(payload))),
        timeout_, "OpenAI-compatible", unreachable);
    if (!response) return Result<std::string>::failure(response.error());
    const auto content_type = response.value().headers.contains("content-type") ? response.value().headers.at("content-type") : std::string{};
    JsonValue response_data{JsonValue::Object{}};
    std::string text;
    if (content_type.starts_with("text/event-stream") || trim(response.value().body).starts_with("data:")) {
        auto extracted = extract_openai_response_from_sse(response.value().body);
        text = std::move(extracted.first);
        response_data = std::move(extracted.second);
        if (!response_data.as_object().empty()) {
            metadata_ = extract_usage_cache_details(response_data);
            auto& object = metadata_.as_object();
            object["prompt_cache_supported"] = JsonValue(cache_supported_);
            object["prompt_cache_key"] = options.prompt_cache_key.has_value() ? JsonValue(*options.prompt_cache_key) : JsonValue(nullptr);
            object["prompt_cache_retention"] = options.prompt_cache_retention.has_value() ? JsonValue(*options.prompt_cache_retention) : JsonValue(nullptr);
        }
        if (text.empty()) return Result<std::string>::failure(make_error(ErrorCategory::ModelProtocol, "missing_text",
            "OpenAI-compatible error: could not extract text from event stream response"));
        return Result<std::string>::success(std::move(text));
    }
    const auto parsed = response_json(response.value(), "OpenAI-compatible");
    if (!parsed) return Result<std::string>::failure(parsed.error());
    if (const auto* error = parsed.value().find("error"); error != nullptr && !error->is_null()) return Result<std::string>::failure(make_error(
        ErrorCategory::ModelProtocol, "openai_error", "OpenAI-compatible error: " + json_error_text(*error)));
    metadata_ = extract_usage_cache_details(parsed.value());
    auto& object = metadata_.as_object();
    object["prompt_cache_supported"] = JsonValue(cache_supported_);
    object["prompt_cache_key"] = options.prompt_cache_key.has_value() ? JsonValue(*options.prompt_cache_key) : JsonValue(nullptr);
    object["prompt_cache_retention"] = options.prompt_cache_retention.has_value() ? JsonValue(*options.prompt_cache_retention) : JsonValue(nullptr);
    return Result<std::string>::success(extract_openai_text(parsed.value()));
}
bool OpenAICompatibleModelClient::supports_prompt_cache() const noexcept { return cache_supported_; }
const JsonValue& OpenAICompatibleModelClient::last_completion_metadata() const noexcept { return metadata_; }
std::string OpenAICompatibleModelClient::model_name() const { return model_; }
std::string OpenAICompatibleModelClient::client_name() const { return "OpenAICompatibleModelClient"; }

AnthropicCompatibleModelClient::AnthropicCompatibleModelClient(std::string model, std::string base_url, std::string api_key,
    std::optional<double> temperature, std::chrono::milliseconds timeout, std::shared_ptr<IHttpClient> http)
    : model_(std::move(model)), base_url_(normalize_versioned_base_url(std::move(base_url))), api_key_(std::move(api_key)),
      temperature_(temperature), timeout_(timeout), http_(std::move(http)) {}

Result<std::string> AnthropicCompatibleModelClient::complete(std::string_view prompt, std::size_t max_new_tokens, const CompletionOptions&) {
    metadata_ = JsonValue::Object{};
    JsonValue::Object payload{
        {"max_tokens", JsonValue(max_new_tokens)},
        {"messages", JsonValue::Array{JsonValue::Object{{"content", JsonValue::Array{JsonValue::Object{
            {"text", JsonValue(std::string(prompt))}, {"type", JsonValue("text")}}}}, {"role", JsonValue("user")}}}},
        {"model", JsonValue(model_)}, {"stream", JsonValue(false)}};
    if (temperature_.has_value()) payload["temperature"] = JsonValue(*temperature_);
    const std::map<std::string, std::string, std::less<>> headers{{"Content-Type", "application/json"},
        {"anthropic-version", "2023-06-01"}, {"x-api-key", api_key_}};
    const auto unreachable = "Could not reach the Anthropic-compatible backend.\nBase URL: " + base_url_ + "\nModel: " + model_;
    const auto response = post_with_retries(*http_, base_url_ + "/messages", headers, dump_json(JsonValue(std::move(payload))),
        timeout_, "Anthropic-compatible", unreachable);
    if (!response) return Result<std::string>::failure(response.error());
    const auto data = response_json(response.value(), "Anthropic-compatible");
    if (!data) return Result<std::string>::failure(data.error());
    if (const auto* error = data.value().find("error"); error != nullptr && !error->is_null()) return Result<std::string>::failure(make_error(
        ErrorCategory::ModelProtocol, "anthropic_error", "Anthropic-compatible error: " + json_error_text(*error)));
    const auto* content = data.value().find("content");
    if (content != nullptr && content->is_array()) {
        for (const auto& item : content->as_array()) {
            if (!item.is_object() || item.find("type") == nullptr || item.find("type")->string_or() != "text") continue;
            const auto* text = item.find("text");
            if (text != nullptr && text->is_string() && !text->as_string().empty()) return Result<std::string>::success(text->as_string());
        }
    }
    return Result<std::string>::failure(make_error(ErrorCategory::ModelProtocol, "missing_text",
        "Anthropic-compatible error: could not extract text from response"));
}
bool AnthropicCompatibleModelClient::supports_prompt_cache() const noexcept { return false; }
const JsonValue& AnthropicCompatibleModelClient::last_completion_metadata() const noexcept { return metadata_; }
std::string AnthropicCompatibleModelClient::model_name() const { return model_; }
std::string AnthropicCompatibleModelClient::client_name() const { return "AnthropicCompatibleModelClient"; }

}  // namespace runi
