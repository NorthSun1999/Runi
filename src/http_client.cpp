#include "runi/http_client.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#else
#include <cstdio>
#endif

namespace runi {
namespace {

#ifdef _WIN32
std::wstring widen(std::string_view text) {
    if (text.empty()) return {};
    const int count = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (count <= 0) return {};
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count);
    return result;
}

std::string narrow(std::wstring_view text) {
    if (text.empty()) return {};
    const int count = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    if (count <= 0) return {};
    std::string result(static_cast<std::size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), result.data(), count, nullptr, nullptr);
    return result;
}

struct HandleCloser { void operator()(void* handle) const noexcept { if (handle != nullptr) WinHttpCloseHandle(handle); } };
using InternetHandle = std::unique_ptr<void, HandleCloser>;

std::string winhttp_message(DWORD code) {
    wchar_t* raw = nullptr;
    const DWORD flags = FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS;
    const auto count = FormatMessageW(flags, nullptr, code, 0, reinterpret_cast<wchar_t*>(&raw), 0, nullptr);
    std::wstring message = count == 0 || raw == nullptr ? L"WinHTTP error" : std::wstring(raw, count);
    if (raw != nullptr) LocalFree(raw);
    while (!message.empty() && (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' ')) message.pop_back();
    return narrow(message);
}
#endif

}  // namespace

Result<HttpResponse> HttpClient::post(
    const std::string& url,
    const std::map<std::string, std::string, std::less<>>& headers,
    std::string_view body,
    std::chrono::milliseconds timeout) const {
#ifdef _WIN32
    const auto wide_url = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwSchemeLength = static_cast<DWORD>(-1);
    parts.dwHostNameLength = static_cast<DWORD>(-1);
    parts.dwUrlPathLength = static_cast<DWORD>(-1);
    parts.dwExtraInfoLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide_url.c_str(), static_cast<DWORD>(wide_url.size()), 0, &parts)) {
        return Result<HttpResponse>::failure(make_error(ErrorCategory::Configuration, "invalid_url", "Invalid HTTP URL: " + url));
    }
    const std::wstring host(parts.lpszHostName, parts.dwHostNameLength);
    std::wstring target(parts.lpszUrlPath, parts.dwUrlPathLength);
    if (parts.dwExtraInfoLength > 0) target.append(parts.lpszExtraInfo, parts.dwExtraInfoLength);
    if (target.empty()) target = L"/";

    InternetHandle session(WinHttpOpen(L"runi/0.1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0));
    if (!session) return Result<HttpResponse>::failure(make_error(
        ErrorCategory::ModelTransport, "http_session_failed", winhttp_message(GetLastError()), true));
    const int timeout_ms = static_cast<int>(std::clamp<long long>(timeout.count(), 1, 2147483647LL));
    WinHttpSetTimeouts(session.get(), timeout_ms, timeout_ms, timeout_ms, timeout_ms);
    InternetHandle connection(WinHttpConnect(session.get(), host.c_str(), parts.nPort, 0));
    if (!connection) return Result<HttpResponse>::failure(make_error(
        ErrorCategory::ModelTransport, "http_connect_failed", winhttp_message(GetLastError()), true));
    const DWORD flags = parts.nScheme == INTERNET_SCHEME_HTTPS ? WINHTTP_FLAG_SECURE : 0;
    InternetHandle request(WinHttpOpenRequest(connection.get(), L"POST", target.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags));
    if (!request) return Result<HttpResponse>::failure(make_error(
        ErrorCategory::ModelTransport, "http_request_failed", winhttp_message(GetLastError()), true));

    std::wstring header_block;
    for (const auto& [name, value] : headers) header_block += widen(name + ": " + value + "\r\n");
    const BOOL sent = WinHttpSendRequest(request.get(), header_block.c_str(), static_cast<DWORD>(header_block.size()),
        body.empty() ? WINHTTP_NO_REQUEST_DATA : const_cast<char*>(body.data()), static_cast<DWORD>(body.size()),
        static_cast<DWORD>(body.size()), 0);
    if (!sent || !WinHttpReceiveResponse(request.get(), nullptr)) return Result<HttpResponse>::failure(make_error(
        ErrorCategory::ModelTransport, "http_transport_failed", winhttp_message(GetLastError()), true));

    HttpResponse response;
    DWORD status_size = sizeof(response.status);
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
        WINHTTP_HEADER_NAME_BY_INDEX, &response.status, &status_size, WINHTTP_NO_HEADER_INDEX);
    DWORD raw_size = 0;
    WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER, &raw_size, WINHTTP_NO_HEADER_INDEX);
    if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && raw_size > 0) {
        std::vector<wchar_t> raw(raw_size / sizeof(wchar_t));
        if (WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
                raw.data(), &raw_size, WINHTTP_NO_HEADER_INDEX)) {
            const auto all = narrow(std::wstring_view(raw.data()));
            std::size_t start = 0;
            while (start < all.size()) {
                const auto end = all.find("\r\n", start);
                const auto line = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
                const auto colon = line.find(':');
                if (colon != std::string::npos) {
                    auto name = line.substr(0, colon);
                    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
                    auto value = line.substr(colon + 1);
                    while (!value.empty() && value.front() == ' ') value.erase(value.begin());
                    response.headers[name] = value;
                }
                if (end == std::string::npos) break;
                start = end + 2;
            }
        }
    }
    while (true) {
        DWORD available = 0;
        if (!WinHttpQueryDataAvailable(request.get(), &available)) return Result<HttpResponse>::failure(make_error(
            ErrorCategory::ModelTransport, "http_read_failed", winhttp_message(GetLastError()), true));
        if (available == 0) break;
        std::string chunk(available, '\0');
        DWORD read = 0;
        if (!WinHttpReadData(request.get(), chunk.data(), available, &read)) return Result<HttpResponse>::failure(make_error(
            ErrorCategory::ModelTransport, "http_read_failed", winhttp_message(GetLastError()), true));
        response.body.append(chunk.data(), read);
    }
    return Result<HttpResponse>::success(std::move(response));
#else
    (void)url; (void)headers; (void)body; (void)timeout;
    return Result<HttpResponse>::failure(make_error(
        ErrorCategory::Configuration, "http_unavailable", "The bundled HTTP client currently requires Windows."));
#endif
}

}  // namespace runi
