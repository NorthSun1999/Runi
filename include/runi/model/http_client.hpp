#pragma once

#include <chrono>
#include <map>
#include <string>

#include "runi/core/result.hpp"

namespace runi {

struct HttpResponse {
    int status{0};
    std::string body;
    std::map<std::string, std::string, std::less<>> headers;
};

class IHttpClient {
public:
    virtual ~IHttpClient() = default;
    [[nodiscard]] virtual Result<HttpResponse> post(
        const std::string& url,
        const std::map<std::string, std::string, std::less<>>& headers,
        std::string_view body,
        std::chrono::milliseconds timeout) const = 0;
};

class HttpClient final : public IHttpClient {
public:
    [[nodiscard]] Result<HttpResponse> post(
        const std::string& url,
        const std::map<std::string, std::string, std::less<>>& headers,
        std::string_view body,
        std::chrono::milliseconds timeout) const override;
};

}  // namespace runi
