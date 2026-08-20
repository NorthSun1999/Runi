#include "runi/service/service.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <stdexcept>
#include <sstream>
#include <utility>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include "runi/core/json_codec.hpp"
#include "runi/core/text.hpp"

namespace runi {
namespace {

#ifdef _WIN32
using NativeSocket = SOCKET;
constexpr NativeSocket kInvalidSocket = INVALID_SOCKET;
class SocketRuntime {
public:
    SocketRuntime() {
        WSADATA data{};
        okay_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~SocketRuntime() { if (okay_) WSACleanup(); }
    bool okay() const noexcept { return okay_; }
private:
    bool okay_{false};
};
void close_socket(NativeSocket socket) noexcept { if (socket != kInvalidSocket) closesocket(socket); }
int last_socket_error() noexcept { return WSAGetLastError(); }
#else
using NativeSocket = int;
constexpr NativeSocket kInvalidSocket = -1;
class SocketRuntime {
public:
    bool okay() const noexcept { return true; }
};
void close_socket(NativeSocket socket) noexcept { if (socket != kInvalidSocket) ::close(socket); }
int last_socket_error() noexcept { return errno; }
#endif

SocketRuntime& socket_runtime() {
    static SocketRuntime runtime;
    return runtime;
}

NativeSocket from_handle(std::intptr_t handle) noexcept {
    return static_cast<NativeSocket>(handle);
}

std::intptr_t to_handle(NativeSocket socket) noexcept {
    return static_cast<std::intptr_t>(socket);
}

class SocketHandle {
public:
    explicit SocketHandle(NativeSocket socket = kInvalidSocket) : socket_(socket) {}
    ~SocketHandle() { close_socket(socket_); }
    SocketHandle(const SocketHandle&) = delete;
    SocketHandle& operator=(const SocketHandle&) = delete;
    SocketHandle(SocketHandle&& other) noexcept : socket_(std::exchange(other.socket_, kInvalidSocket)) {}
    SocketHandle& operator=(SocketHandle&& other) noexcept {
        if (this == &other) return *this;
        close_socket(socket_);
        socket_ = std::exchange(other.socket_, kInvalidSocket);
        return *this;
    }
    NativeSocket get() const noexcept { return socket_; }
    NativeSocket release() noexcept { return std::exchange(socket_, kInvalidSocket); }
private:
    NativeSocket socket_;
};

std::atomic<std::uint64_t> generated_ids{0};

std::string make_id(std::string_view prefix) {
    return std::string(prefix) + "-" + std::to_string(unix_time_millis()) + "-" +
        std::to_string(generated_ids.fetch_add(1, std::memory_order_relaxed) + 1);
}

ServiceResponse json_response(int status, JsonValue value) {
    return ServiceResponse{status, {{"content-type", "application/json"}}, dump_json(value)};
}

ServiceResponse error_response(int status, std::string code, std::string message) {
    return json_response(status, JsonValue::Object{
        {"error", JsonValue::Object{{"code", JsonValue(std::move(code))}, {"message", JsonValue(std::move(message))}}}});
}

JsonValue session_json(const RuntimeSessionRecord& session) {
    return JsonValue::Object{{"id", JsonValue(session.id)}, {"workspace_root", JsonValue(session.workspace_root)},
        {"state", session.state}, {"state_version", JsonValue(session.state_version)},
        {"created_at", JsonValue(session.created_at)}, {"updated_at", JsonValue(session.updated_at)}};
}

JsonValue run_json(const RuntimeRunRecord& run) {
    return JsonValue::Object{{"id", JsonValue(run.id)}, {"session_id", JsonValue(run.session_id)},
        {"parent_run_id", JsonValue(run.parent_run_id)}, {"request_id", JsonValue(run.request_id)},
        {"request", JsonValue(run.request)}, {"status", JsonValue(std::string(to_string(run.status)))},
        {"result", JsonValue(run.result)}, {"error", JsonValue(run.error)},
        {"created_at", JsonValue(run.created_at)}, {"updated_at", JsonValue(run.updated_at)}};
}

Result<JsonValue> parse_body(const ServiceRequest& request) {
    if (request.body.empty()) return Result<JsonValue>::success(JsonValue::Object{});
    const auto parsed = parse_json(request.body);
    if (!parsed || !parsed.value().is_object()) return Result<JsonValue>::failure(make_error(
        ErrorCategory::Validation, "invalid_json_body", "Request body must be a JSON object"));
    return parsed;
}

std::vector<std::string> path_segments(std::string_view target) {
    const auto query = target.find('?');
    if (query != std::string_view::npos) target = target.substr(0, query);
    std::vector<std::string> result;
    std::size_t start = 0;
    while (start < target.size()) {
        while (start < target.size() && target[start] == '/') ++start;
        if (start >= target.size()) break;
        const auto end = target.find('/', start);
        result.emplace_back(target.substr(start, end == std::string_view::npos ? target.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return result;
}

bool path_within(const std::filesystem::path& child, const std::filesystem::path& parent) {
    auto child_iterator = child.begin();
    for (auto parent_iterator = parent.begin(); parent_iterator != parent.end(); ++parent_iterator, ++child_iterator) {
        if (child_iterator == child.end() || *child_iterator != *parent_iterator) return false;
    }
    return true;
}

int status_for_error(const Error& error) {
    if (error.code == "executor_queue_full") return 503;
    if (error.code.find("conflict") != std::string::npos) return 409;
    if (error.code.find("not_found") != std::string::npos) return 404;
    if (error.category == ErrorCategory::Validation || error.category == ErrorCategory::PathViolation) return 400;
    return 500;
}

std::string lower_header(std::string_view value) {
    return lower_ascii(trim(value));
}

std::string header_value(const ServiceRequest& request, std::string_view name) {
    const auto direct = request.headers.find(name);
    if (direct != request.headers.end()) return direct->second;
    for (const auto& [key, value] : request.headers) if (lower_header(key) == name) return value;
    return {};
}

std::string status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 201: return "Created";
        case 202: return "Accepted";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 409: return "Conflict";
        case 413: return "Content Too Large";
        case 503: return "Service Unavailable";
        default: return "Internal Server Error";
    }
}

bool send_all(NativeSocket socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const auto remaining = data.size() - sent;
        const auto chunk = static_cast<int>(std::min<std::size_t>(remaining, 1U << 20U));
        const auto count = ::send(socket, data.data() + sent, chunk, 0);
        if (count <= 0) return false;
        sent += static_cast<std::size_t>(count);
    }
    return true;
}

std::string serialize_response(const ServiceResponse& response) {
    std::ostringstream output;
    output << "HTTP/1.1 " << response.status << ' ' << status_reason(response.status) << "\r\n";
    bool content_type = false;
    for (const auto& [name, value] : response.headers) {
        if (lower_header(name) == "content-type") content_type = true;
        output << name << ": " << value << "\r\n";
    }
    if (!content_type) output << "Content-Type: application/json\r\n";
    output << "Content-Length: " << response.body.size() << "\r\nConnection: close\r\n\r\n";
    output << response.body;
    return output.str();
}

Result<ServiceRequest> read_request(NativeSocket socket, std::size_t maximum) {
    std::string data;
    std::array<char, 4096> buffer{};
    std::size_t header_end = std::string::npos;
    while (header_end == std::string::npos) {
        const auto count = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0) return Result<ServiceRequest>::failure(make_error(
            ErrorCategory::ModelTransport, "http_read_failed", "Connection closed before HTTP headers completed"));
        data.append(buffer.data(), static_cast<std::size_t>(count));
        if (data.size() > maximum) return Result<ServiceRequest>::failure(make_error(
            ErrorCategory::Validation, "request_too_large", "HTTP request exceeds configured size limit"));
        header_end = data.find("\r\n\r\n");
    }
    std::istringstream headers(data.substr(0, header_end));
    ServiceRequest request;
    std::string version;
    if (!(headers >> request.method >> request.target >> version) || !version.starts_with("HTTP/1.")) {
        return Result<ServiceRequest>::failure(make_error(
            ErrorCategory::Validation, "invalid_http_request", "Invalid HTTP request line"));
    }
    std::string line;
    std::getline(headers, line);
    std::size_t content_length = 0;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto name = lower_header(std::string_view(line).substr(0, colon));
        const auto value = trim(std::string_view(line).substr(colon + 1));
        request.headers[name] = value;
        if (name == "content-length") {
            const auto* begin = value.data();
            const auto* end = begin + value.size();
            const auto parsed = std::from_chars(begin, end, content_length);
            if (parsed.ec != std::errc{} || parsed.ptr != end) return Result<ServiceRequest>::failure(make_error(
                ErrorCategory::Validation, "invalid_content_length", "Invalid HTTP Content-Length"));
        }
    }
    if (header_end + 4 + content_length > maximum) return Result<ServiceRequest>::failure(make_error(
        ErrorCategory::Validation, "request_too_large", "HTTP request exceeds configured size limit"));
    const auto body_start = header_end + 4;
    while (data.size() - body_start < content_length) {
        const auto count = ::recv(socket, buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count <= 0) return Result<ServiceRequest>::failure(make_error(
            ErrorCategory::ModelTransport, "http_read_failed", "Connection closed before HTTP body completed"));
        data.append(buffer.data(), static_cast<std::size_t>(count));
        if (data.size() > maximum) return Result<ServiceRequest>::failure(make_error(
            ErrorCategory::Validation, "request_too_large", "HTTP request exceeds configured size limit"));
    }
    request.body = data.substr(body_start, content_length);
    return Result<ServiceRequest>::success(std::move(request));
}

Result<ServiceResponse> parse_response(std::string_view data) {
    const auto header_end = data.find("\r\n\r\n");
    if (header_end == std::string_view::npos) return Result<ServiceResponse>::failure(make_error(
        ErrorCategory::ModelProtocol, "invalid_http_response", "HTTP response headers are incomplete"));
    std::istringstream headers{std::string(data.substr(0, header_end))};
    std::string version;
    ServiceResponse response;
    if (!(headers >> version >> response.status) || !version.starts_with("HTTP/1.")) {
        return Result<ServiceResponse>::failure(make_error(
            ErrorCategory::ModelProtocol, "invalid_http_response", "HTTP response status line is invalid"));
    }
    std::string line;
    std::getline(headers, line);
    std::size_t content_length = data.size() - header_end - 4;
    while (std::getline(headers, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        const auto name = lower_header(std::string_view(line).substr(0, colon));
        const auto value = trim(std::string_view(line).substr(colon + 1));
        response.headers[name] = value;
        if (name == "content-length") {
            const auto* begin = value.data();
            const auto* end = begin + value.size();
            const auto parsed = std::from_chars(begin, end, content_length);
            if (parsed.ec != std::errc{}) return Result<ServiceResponse>::failure(make_error(
                ErrorCategory::ModelProtocol, "invalid_http_response", "HTTP response Content-Length is invalid"));
        }
    }
    const auto body = data.substr(header_end + 4);
    if (body.size() < content_length) return Result<ServiceResponse>::failure(make_error(
        ErrorCategory::ModelProtocol, "invalid_http_response", "HTTP response body is incomplete"));
    response.body = std::string(body.substr(0, content_length));
    return Result<ServiceResponse>::success(std::move(response));
}

void set_socket_timeout(NativeSocket socket, std::chrono::milliseconds timeout) {
#ifdef _WIN32
    const auto value = static_cast<DWORD>(std::max<std::int64_t>(1, timeout.count()));
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&value), sizeof(value));
#else
    timeval value{};
    value.tv_sec = static_cast<time_t>(timeout.count() / 1000);
    value.tv_usec = static_cast<suseconds_t>((timeout.count() % 1000) * 1000);
    setsockopt(socket, SOL_SOCKET, SO_RCVTIMEO, &value, sizeof(value));
    setsockopt(socket, SOL_SOCKET, SO_SNDTIMEO, &value, sizeof(value));
#endif
}

}  // namespace

RuntimeService::RuntimeService(
    SqliteStateStore& state_store,
    BoundedExecutor& run_executor,
    RunHandler handler,
    RuntimeServiceOptions options)
    : state_store_(state_store), run_executor_(run_executor), handler_(std::move(handler)), options_(std::move(options)) {
    if (!handler_) throw std::invalid_argument("runtime service handler must be configured");
    options_.allowed_workspace_root = std::filesystem::absolute(options_.allowed_workspace_root).lexically_normal();
}

RuntimeService::~RuntimeService() {
    std::unique_lock lock(active_mutex_);
    stopping_ = true;
    for (const auto& [run_id, source] : active_runs_) {
        static_cast<void>(run_id);
        source->request_stop();
    }
    active_condition_.wait(lock, [this] { return active_runs_.empty(); });
}

ServiceResponse RuntimeService::handle(const ServiceRequest& request) {
    if (request.body.size() > options_.max_request_body) return error_response(
        413, "request_too_large", "Request body exceeds configured size limit");
    const auto segments = path_segments(request.target);
    if (request.method == "POST" && segments == std::vector<std::string>{"v1", "sessions"}) {
        return create_session(request);
    }
    if (request.method == "POST" && segments.size() == 4 && segments[0] == "v1" &&
        segments[1] == "sessions" && segments[3] == "runs") {
        return create_run(segments[2], request);
    }
    if (segments.size() >= 3 && segments[0] == "v1" && segments[1] == "runs") {
        if (request.method == "GET" && segments.size() == 3) return get_run(segments[2]);
        if (request.method == "GET" && segments.size() == 4 && segments[3] == "events") return get_events(segments[2], request);
        if (request.method == "POST" && segments.size() == 3) {
            const auto colon = segments[2].rfind(':');
            if (colon != std::string::npos) {
                const auto id = std::string_view(segments[2]).substr(0, colon);
                const auto operation = std::string_view(segments[2]).substr(colon + 1);
                if (operation == "cancel") return cancel_run(id);
                if (operation == "resume") return resume_run(id);
            }
        }
    }
    return error_response(404, "route_not_found", "Service route was not found");
}

ServiceResponse RuntimeService::create_session(const ServiceRequest& request) {
    const auto body = parse_body(request);
    if (!body) return error_response(400, body.error().code, body.error().message);
    const auto id = body.value().find("id") == nullptr ? make_id("session") : body.value().at("id").string_or();
    const auto workspace_text = body.value().find("workspace_root") == nullptr ? options_.allowed_workspace_root.string() :
        body.value().at("workspace_root").string_or();
    if (id.empty() || workspace_text.empty()) return error_response(400, "invalid_session", "Session id and workspace_root are required");
    std::error_code workspace_error;
    std::error_code allowed_error;
    const auto workspace = std::filesystem::weakly_canonical(std::filesystem::path(workspace_text), workspace_error);
    const auto allowed = std::filesystem::weakly_canonical(options_.allowed_workspace_root, allowed_error);
    if (workspace_error || allowed_error || !path_within(workspace, allowed)) return error_response(
        400, "workspace_not_allowed", "Session workspace is outside the configured service root");
    const auto created = state_store_.create_session(id, workspace.string(), JsonValue::Object{});
    if (!created) return error_response(status_for_error(created.error()), created.error().code, created.error().message);
    return json_response(201, session_json(created.value()));
}

ServiceResponse RuntimeService::create_run(std::string_view session_id, const ServiceRequest& request) {
    const auto session = state_store_.get_session(session_id);
    if (!session) return error_response(404, session.error().code, session.error().message);
    const auto body = parse_body(request);
    if (!body) return error_response(400, body.error().code, body.error().message);
    const auto id = body.value().find("id") == nullptr ? make_id("run") : body.value().at("id").string_or();
    const auto user_request = body.value().find("request") == nullptr ? std::string{} : body.value().at("request").string_or();
    const auto request_id = header_value(request, "idempotency-key");
    if (id.empty() || user_request.empty() || request_id.empty()) return error_response(
        400, "invalid_run", "Run id, request, and Idempotency-Key are required");
    const auto created = state_store_.create_run(id, std::string(session_id), request_id, user_request);
    if (!created) return error_response(status_for_error(created.error()), created.error().code, created.error().message);
    if (!created.value().created) return json_response(200, run_json(created.value().record));
    static_cast<void>(state_store_.append_event(id, "run.queued"));
    const auto submitted = submit_run(created.value().record);
    if (!submitted) return error_response(status_for_error(submitted.error()), submitted.error().code, submitted.error().message);
    return json_response(202, run_json(created.value().record));
}

ServiceResponse RuntimeService::get_run(std::string_view run_id) const {
    const auto run = state_store_.get_run(run_id);
    if (!run) return error_response(status_for_error(run.error()), run.error().code, run.error().message);
    return json_response(200, run_json(run.value()));
}

ServiceResponse RuntimeService::get_events(std::string_view run_id, const ServiceRequest& request) const {
    if (!state_store_.get_run(run_id)) return error_response(404, "run_not_found", "Runtime run was not found");
    std::int64_t after = 0;
    const auto value = header_value(request, "last-event-id");
    if (!value.empty()) {
        const auto* begin = value.data();
        const auto* end = begin + value.size();
        const auto parsed = std::from_chars(begin, end, after);
        if (parsed.ec != std::errc{} || parsed.ptr != end) return error_response(
            400, "invalid_event_id", "Last-Event-ID must be an integer");
    }
    const auto events = state_store_.events_after(run_id, after);
    if (!events) return error_response(500, events.error().code, events.error().message);
    std::string body;
    for (const auto& event : events.value()) {
        body += "id: " + std::to_string(event.sequence) + "\n";
        body += "event: " + event.type + "\n";
        body += "data: " + dump_json(event.payload) + "\n\n";
    }
    return ServiceResponse{200, {{"content-type", "text/event-stream"}, {"cache-control", "no-cache"}}, std::move(body)};
}

ServiceResponse RuntimeService::cancel_run(std::string_view run_id) {
    const auto run = state_store_.get_run(run_id);
    if (!run) return error_response(404, run.error().code, run.error().message);
    if (run.value().status != RunStatus::Queued && run.value().status != RunStatus::Running) return error_response(
        409, "run_state_conflict", "Only queued or running runs can be cancelled");
    {
        std::scoped_lock lock(active_mutex_);
        const auto active = active_runs_.find(run_id);
        if (active != active_runs_.end()) active->second->request_stop();
    }
    static_cast<void>(state_store_.append_event(run_id, "run.cancel_requested"));
    return json_response(202, run_json(run.value()));
}

ServiceResponse RuntimeService::resume_run(std::string_view run_id) {
    const auto run = state_store_.get_run(run_id);
    if (!run) return error_response(404, run.error().code, run.error().message);
    const auto queued = state_store_.transition_run(run_id, RunStatus::Interrupted, RunStatus::Queued);
    if (!queued) return error_response(409, queued.error().code, queued.error().message);
    static_cast<void>(state_store_.append_event(run_id, "run.resumed"));
    const auto submitted = submit_run(queued.value());
    if (!submitted) return error_response(status_for_error(submitted.error()), submitted.error().code, submitted.error().message);
    return json_response(202, run_json(queued.value()));
}

Result<void> RuntimeService::submit_run(const RuntimeRunRecord& run) {
    auto stop_source = std::make_shared<std::stop_source>();
    std::shared_ptr<std::mutex> session_mutex;
    {
        std::scoped_lock lock(active_mutex_);
        if (stopping_) return Result<void>::failure(make_error(
            ErrorCategory::Internal, "service_stopping", "Runtime service is stopping"));
        active_runs_[run.id] = stop_source;
        auto& slot = session_mutexes_[run.session_id];
        if (!slot) slot = std::make_shared<std::mutex>();
        session_mutex = slot;
    }
    auto submitted = run_executor_.submit([this, run, stop_source, session_mutex](std::stop_token executor_stop) {
        struct ActiveGuard {
            RuntimeService* service;
            std::string id;
            ~ActiveGuard() { service->remove_active(id); }
        } guard{this, run.id};
        if (executor_stop.stop_requested()) stop_source->request_stop();
        std::unique_lock session_lock(*session_mutex);
        auto current = state_store_.get_run(run.id);
        if (!current) return;
        if (stop_source->stop_requested()) {
            if (current.value().status == RunStatus::Queued) {
                static_cast<void>(state_store_.transition_run(run.id, RunStatus::Queued, RunStatus::Cancelled, {}, "cancelled"));
                static_cast<void>(state_store_.append_event(run.id, "run.cancelled"));
            }
            return;
        }
        auto running = state_store_.transition_run(run.id, RunStatus::Queued, RunStatus::Running);
        if (!running) return;
        static_cast<void>(state_store_.append_event(run.id, "run.started"));
        const auto session = state_store_.get_session(run.session_id);
        if (!session) {
            static_cast<void>(state_store_.transition_run(run.id, RunStatus::Running, RunStatus::Failed, {}, session.error().message));
            static_cast<void>(state_store_.append_event(run.id, "run.failed"));
            return;
        }
        Result<std::string> result = Result<std::string>::failure(make_error(
            ErrorCategory::Internal, "handler_failed", "Runtime handler did not complete"));
        try {
            result = handler_(RunInvocation{running.value(), session.value()}, stop_source->get_token());
        } catch (const std::exception& error) {
            result = Result<std::string>::failure(make_error(
                ErrorCategory::Internal, "handler_exception", error.what()));
        } catch (...) {
            result = Result<std::string>::failure(make_error(
                ErrorCategory::Internal, "handler_exception", "Unknown runtime handler exception"));
        }
        if (stop_source->stop_requested()) {
            static_cast<void>(state_store_.transition_run(run.id, RunStatus::Running, RunStatus::Cancelled, {}, "cancelled"));
            static_cast<void>(state_store_.append_event(run.id, "run.cancelled"));
        } else if (result) {
            static_cast<void>(state_store_.transition_run(run.id, RunStatus::Running, RunStatus::Succeeded, result.value()));
            static_cast<void>(state_store_.append_event(run.id, "run.succeeded",
                JsonValue::Object{{"result", JsonValue(result.value())}}));
        } else {
            static_cast<void>(state_store_.transition_run(run.id, RunStatus::Running, RunStatus::Failed, {}, result.error().message));
            static_cast<void>(state_store_.append_event(run.id, "run.failed",
                JsonValue::Object{{"code", JsonValue(result.error().code)}}));
        }
    });
    if (!submitted) {
        remove_active(run.id);
        static_cast<void>(state_store_.transition_run(run.id, RunStatus::Queued, RunStatus::Failed, {}, submitted.error().message));
        static_cast<void>(state_store_.append_event(run.id, "run.rejected",
            JsonValue::Object{{"code", JsonValue(submitted.error().code)}}));
        return Result<void>::failure(submitted.error());
    }
    return Result<void>::success();
}

void RuntimeService::remove_active(std::string_view run_id) {
    {
        std::scoped_lock lock(active_mutex_);
        const auto active = active_runs_.find(run_id);
        if (active != active_runs_.end()) active_runs_.erase(active);
    }
    active_condition_.notify_all();
}

SocketHttpServer::SocketHttpServer(RuntimeService& service, SocketServerOptions options)
    : service_(service), options_(options),
      connection_executor_(options.connection_workers, options.connection_queue_capacity) {}

SocketHttpServer::~SocketHttpServer() {
    stop();
}

Result<void> SocketHttpServer::start() {
    if (running_.exchange(true)) return Result<void>::failure(make_error(
        ErrorCategory::Validation, "server_already_running", "HTTP server is already running"));
    if (!socket_runtime().okay()) {
        running_ = false;
        return Result<void>::failure(make_error(ErrorCategory::ModelTransport,
            "socket_runtime_failed", "Could not initialize the socket runtime"));
    }
    SocketHandle listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (listener.get() == kInvalidSocket) {
        running_ = false;
        return Result<void>::failure(make_error(ErrorCategory::ModelTransport,
            "socket_create_failed", "Could not create HTTP listener socket: " + std::to_string(last_socket_error())));
    }
    int reuse = 1;
#ifdef _WIN32
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse), sizeof(reuse));
#else
    setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options_.port);
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        ::listen(listener.get(), SOMAXCONN) != 0) {
        running_ = false;
        return Result<void>::failure(make_error(ErrorCategory::ModelTransport,
            "socket_bind_failed", "Could not bind loopback HTTP listener: " + std::to_string(last_socket_error())));
    }
    sockaddr_in bound{};
#ifdef _WIN32
    int length = sizeof(bound);
#else
    socklen_t length = sizeof(bound);
#endif
    if (getsockname(listener.get(), reinterpret_cast<sockaddr*>(&bound), &length) != 0) {
        running_ = false;
        return Result<void>::failure(make_error(ErrorCategory::ModelTransport,
            "socket_name_failed", "Could not inspect bound HTTP listener"));
    }
    bound_port_ = ntohs(bound.sin_port);
    listener_ = to_handle(listener.release());
    accept_thread_ = std::jthread([this](std::stop_token stop_token) { accept_loop(stop_token); });
    return Result<void>::success();
}

void SocketHttpServer::accept_loop(std::stop_token stop_token) noexcept {
    while (!stop_token.stop_requested() && running_) {
        sockaddr_in peer{};
#ifdef _WIN32
        int length = sizeof(peer);
#else
        socklen_t length = sizeof(peer);
#endif
        const auto client = ::accept(from_handle(listener_.load()), reinterpret_cast<sockaddr*>(&peer), &length);
        if (client == kInvalidSocket) {
            if (!running_ || stop_token.stop_requested()) return;
            continue;
        }
        set_socket_timeout(client, std::chrono::seconds(5));
        auto submitted = connection_executor_.submit([this, client](std::stop_token connection_stop) {
            SocketHandle connection(client);
            if (connection_stop.stop_requested()) return;
            const auto request = read_request(connection.get(), options_.max_request_bytes);
            ServiceResponse response;
            if (!request) {
                const auto status = request.error().code == "request_too_large" ? 413 : 400;
                response = error_response(status, request.error().code, request.error().message);
            } else {
                response = service_.handle(request.value());
            }
            static_cast<void>(send_all(connection.get(), serialize_response(response)));
        });
        if (!submitted) {
            SocketHandle connection(client);
            static_cast<void>(send_all(connection.get(), serialize_response(error_response(
                503, "connection_queue_full", "HTTP connection queue is full"))));
        }
    }
}

void SocketHttpServer::stop() noexcept {
    if (!running_.exchange(false)) return;
    const auto listener = from_handle(listener_.exchange(-1));
    if (listener != kInvalidSocket) {
#ifdef _WIN32
        shutdown(listener, SD_BOTH);
#else
        shutdown(listener, SHUT_RDWR);
#endif
        close_socket(listener);
    }
    if (accept_thread_.joinable()) {
        accept_thread_.request_stop();
        accept_thread_.join();
    }
    connection_executor_.shutdown();
    bound_port_ = 0;
}

std::uint16_t SocketHttpServer::port() const noexcept {
    return bound_port_.load();
}

bool SocketHttpServer::running() const noexcept {
    return running_.load();
}

RuniClient::RuniClient(std::string host, std::uint16_t port, std::chrono::milliseconds timeout)
    : host_(std::move(host)), port_(port), timeout_(timeout) {}

Result<ServiceResponse> RuniClient::send(const ServiceRequest& request) const {
    if (!socket_runtime().okay()) return Result<ServiceResponse>::failure(make_error(
        ErrorCategory::ModelTransport, "socket_runtime_failed", "Could not initialize the socket runtime"));
    SocketHandle socket(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
    if (socket.get() == kInvalidSocket) return Result<ServiceResponse>::failure(make_error(
        ErrorCategory::ModelTransport, "socket_create_failed", "Could not create client socket"));
    set_socket_timeout(socket.get(), timeout_);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port_);
    if (inet_pton(AF_INET, host_.c_str(), &address.sin_addr) != 1) return Result<ServiceResponse>::failure(make_error(
        ErrorCategory::Validation, "invalid_client_host", "RuniClient currently requires a numeric IPv4 host"));
    if (::connect(socket.get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        return Result<ServiceResponse>::failure(make_error(ErrorCategory::ModelTransport,
            "client_connect_failed", "Could not connect to Runi server: " + std::to_string(last_socket_error()), true));
    }
    std::ostringstream output;
    output << request.method << ' ' << request.target << " HTTP/1.1\r\nHost: " << host_ << ':' << port_ << "\r\n";
    for (const auto& [name, value] : request.headers) output << name << ": " << value << "\r\n";
    output << "Content-Length: " << request.body.size() << "\r\nConnection: close\r\n\r\n" << request.body;
    if (!send_all(socket.get(), output.str())) return Result<ServiceResponse>::failure(make_error(
        ErrorCategory::ModelTransport, "client_write_failed", "Could not write HTTP request"));
    std::string response;
    std::array<char, 4096> buffer{};
    constexpr std::size_t maximum_response = 4 * 1024 * 1024;
    while (true) {
        const auto count = ::recv(socket.get(), buffer.data(), static_cast<int>(buffer.size()), 0);
        if (count == 0) break;
        if (count < 0) return Result<ServiceResponse>::failure(make_error(
            ErrorCategory::ModelTransport, "client_read_failed", "Could not read HTTP response", true));
        response.append(buffer.data(), static_cast<std::size_t>(count));
        if (response.size() > maximum_response) return Result<ServiceResponse>::failure(make_error(
            ErrorCategory::ModelProtocol, "response_too_large", "HTTP response exceeds client size limit"));
    }
    return parse_response(response);
}

}  // namespace runi
