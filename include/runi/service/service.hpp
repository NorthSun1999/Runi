#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <stop_token>
#include <string>
#include <string_view>

#include "runi/orchestration/execution.hpp"
#include "runi/state/state_store.hpp"

namespace runi {

struct ServiceRequest {
    std::string method;
    std::string target;
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
};

struct ServiceResponse {
    int status{500};
    std::map<std::string, std::string, std::less<>> headers;
    std::string body;
};

struct RunInvocation {
    RuntimeRunRecord run;
    RuntimeSessionRecord session;
};

using RunHandler = std::function<Result<std::string>(const RunInvocation&, std::stop_token)>;

struct RuntimeServiceOptions {
    std::filesystem::path allowed_workspace_root;
    std::size_t max_request_body{1024 * 1024};
};

class RuntimeService {
public:
    RuntimeService(
        SqliteStateStore& state_store,
        BoundedExecutor& run_executor,
        RunHandler handler,
        RuntimeServiceOptions options);
    ~RuntimeService();

    RuntimeService(const RuntimeService&) = delete;
    RuntimeService& operator=(const RuntimeService&) = delete;

    [[nodiscard]] ServiceResponse handle(const ServiceRequest& request);

private:
    [[nodiscard]] Result<void> submit_run(const RuntimeRunRecord& run);
    [[nodiscard]] ServiceResponse create_session(const ServiceRequest& request);
    [[nodiscard]] ServiceResponse create_run(std::string_view session_id, const ServiceRequest& request);
    [[nodiscard]] ServiceResponse get_run(std::string_view run_id) const;
    [[nodiscard]] ServiceResponse get_events(std::string_view run_id, const ServiceRequest& request) const;
    [[nodiscard]] ServiceResponse cancel_run(std::string_view run_id);
    [[nodiscard]] ServiceResponse resume_run(std::string_view run_id);
    void remove_active(std::string_view run_id);

    SqliteStateStore& state_store_;
    BoundedExecutor& run_executor_;
    RunHandler handler_;
    RuntimeServiceOptions options_;
    mutable std::mutex active_mutex_;
    std::condition_variable active_condition_;
    std::map<std::string, std::shared_ptr<std::stop_source>, std::less<>> active_runs_;
    std::map<std::string, std::shared_ptr<std::mutex>, std::less<>> session_mutexes_;
    bool stopping_{false};
};

struct SocketServerOptions {
    std::uint16_t port{0};
    std::size_t connection_workers{4};
    std::size_t connection_queue_capacity{32};
    std::size_t max_request_bytes{1024 * 1024};
};

class SocketHttpServer {
public:
    SocketHttpServer(RuntimeService& service, SocketServerOptions options = {});
    ~SocketHttpServer();

    SocketHttpServer(const SocketHttpServer&) = delete;
    SocketHttpServer& operator=(const SocketHttpServer&) = delete;

    [[nodiscard]] Result<void> start();
    void stop() noexcept;
    [[nodiscard]] std::uint16_t port() const noexcept;
    [[nodiscard]] bool running() const noexcept;

private:
    void accept_loop(std::stop_token stop_token) noexcept;

    RuntimeService& service_;
    SocketServerOptions options_;
    BoundedExecutor connection_executor_;
    std::jthread accept_thread_;
    std::atomic<std::intptr_t> listener_{-1};
    std::atomic<std::uint16_t> bound_port_{0};
    std::atomic<bool> running_{false};
};

class RuniClient {
public:
    RuniClient(std::string host, std::uint16_t port, std::chrono::milliseconds timeout);
    [[nodiscard]] Result<ServiceResponse> send(const ServiceRequest& request) const;

private:
    std::string host_;
    std::uint16_t port_;
    std::chrono::milliseconds timeout_;
};

}  // namespace runi
