#include "runi/model/model_client.hpp"

namespace runi {

FakeModelClient::FakeModelClient(std::vector<std::string> outputs)
    : outputs_(outputs.begin(), outputs.end()) {}

Result<std::string> FakeModelClient::complete(
    std::string_view prompt,
    std::size_t,
    const CompletionOptions&) {
    prompts.emplace_back(prompt);
    metadata_ = JsonValue::Object{};
    if (outputs_.empty()) return Result<std::string>::failure(make_error(
        ErrorCategory::ModelProtocol, "fake_model_exhausted", "fake model ran out of outputs"));
    auto result = std::move(outputs_.front());
    outputs_.pop_front();
    return Result<std::string>::success(std::move(result));
}

bool FakeModelClient::supports_prompt_cache() const noexcept { return false; }
const JsonValue& FakeModelClient::last_completion_metadata() const noexcept { return metadata_; }
std::string FakeModelClient::model_name() const { return {}; }
std::string FakeModelClient::client_name() const { return "FakeModelClient"; }

}  // namespace runi
