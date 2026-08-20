#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "runi/state/context_cache.hpp"
#include "runi/service/agent_service.hpp"
#include "runi/core/json_codec.hpp"
#include "runi/core/sha256.hpp"
#include "runi/orchestration/execution.hpp"
#include "runi/orchestration/multi_agent.hpp"
#include "runi/runi.hpp"
#include "runi/service/service.hpp"
#include "runi/state/state_store.hpp"
#include "runi/state/sqlite_session_store.hpp"
#include "runi/tool/workspace.hpp"

namespace {

using namespace std::chrono_literals;
using namespace runi;

int failures = 0;

void check(bool condition, std::string_view description) {
    if (condition) return;
    ++failures;
    std::cerr << "FAIL: " << description << '\n';
}

void write_file(const std::filesystem::path& path, std::string_view content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    if (!output) throw std::runtime_error("runtime test fixture write failed");
}

template <typename Predicate>
bool eventually(Predicate predicate, std::chrono::milliseconds timeout = 2s) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return predicate();
}

void test_bounded_executor() {
    BoundedExecutor executor(2, 4);
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::promise<void> release;
    auto gate = release.get_future().share();
    std::vector<std::future<int>> futures;
    for (int index = 0; index < 4; ++index) {
        auto submitted = executor.submit([&, index](std::stop_token) {
            const auto now = active.fetch_add(1) + 1;
            auto observed = maximum.load();
            while (observed < now && !maximum.compare_exchange_weak(observed, now)) {}
            gate.wait();
            active.fetch_sub(1);
            return index;
        });
        check(submitted.has_value(), "bounded executor accepts work inside its capacity");
        if (submitted) futures.push_back(std::move(submitted.value()));
    }
    check(eventually([&] { return active.load() == 2; }), "bounded executor starts exactly the configured worker count");
    release.set_value();
    for (std::size_t index = 0; index < futures.size(); ++index) {
        check(futures[index].get() == static_cast<int>(index), "executor future preserves task result");
    }
    check(maximum.load() == 2, "executor never exceeds its worker concurrency");
    const auto snapshot = executor.snapshot();
    check(snapshot.accepted == 4 && snapshot.rejected == 0 && snapshot.active == 0,
        "executor exposes stable accepted, rejected, and active metrics");

    BoundedExecutor saturated(1, 1);
    std::promise<void> release_first;
    auto first_gate = release_first.get_future().share();
    std::atomic<bool> first_started{false};
    auto first = saturated.submit([&](std::stop_token) {
        first_started = true;
        first_gate.wait();
        return 1;
    });
    check(first.has_value() && eventually([&] { return first_started.load(); }), "saturation fixture occupies its worker");
    auto second = saturated.submit([](std::stop_token) { return 2; });
    auto third = saturated.submit([](std::stop_token) { return 3; });
    check(second.has_value(), "one task can wait in the bounded queue");
    check(!third && third.error().code == "executor_queue_full", "queue overflow is rejected instead of growing memory");
    release_first.set_value();
    if (first) check(std::move(first.value()).get() == 1, "running saturation task completes");
    if (second) check(std::move(second.value()).get() == 2, "queued saturation task completes");

    BoundedExecutor cancellable(1, 1);
    auto cancelled = cancellable.submit([](std::stop_token stop_token) {
        while (!stop_token.stop_requested()) std::this_thread::sleep_for(1ms);
        return stop_token.stop_requested();
    });
    check(cancelled.has_value(), "cancellable task is accepted");
    cancellable.request_stop();
    if (cancelled) check(std::move(cancelled.value()).get(), "executor stop token reaches a running task");
    auto after_stop = cancellable.submit([](std::stop_token) { return 0; });
    check(!after_stop && after_stop.error().code == "executor_stopped", "executor rejects work after stop");
}

AgentDescriptor worker_descriptor(std::string id, std::size_t capacity, std::atomic<int>* active = nullptr,
    std::atomic<int>* maximum = nullptr) {
    AgentDescriptor descriptor;
    descriptor.id = std::move(id);
    descriptor.role = AgentRole::Worker;
    descriptor.capabilities = {"inspect"};
    descriptor.max_concurrency = capacity;
    descriptor.read_only = true;
    descriptor.execute = [active, maximum](const AgentTask& task, std::stop_token stop_token) -> Result<std::string> {
        if (active != nullptr && maximum != nullptr) {
            const auto now = active->fetch_add(1) + 1;
            auto observed = maximum->load();
            while (observed < now && !maximum->compare_exchange_weak(observed, now)) {}
            std::this_thread::sleep_for(task.input == "slow" ? 30ms : 5ms);
            active->fetch_sub(1);
        }
        if (stop_token.stop_requested()) {
            return Result<std::string>::failure(make_error(ErrorCategory::Timeout, "agent_cancelled", "agent cancelled"));
        }
        if (task.input == "fail") {
            return Result<std::string>::failure(make_error(ErrorCategory::ToolExecution, "worker_failed", "worker failed"));
        }
        return Result<std::string>::success("done:" + task.input);
    };
    return descriptor;
}

AgentTask agent_task(std::string id, std::string input) {
    return AgentTask{std::move(id), AgentRole::Worker, {"inspect"}, std::move(input), JsonValue::Object{},
        std::chrono::steady_clock::now() + 2s};
}

void test_agent_registry_and_runtime(const std::filesystem::path& workspace) {
    AgentRegistry registry;
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    check(registry.add(worker_descriptor("worker-a", 2, &active, &maximum)).has_value(), "registry accepts a valid agent");
    check(!registry.add(worker_descriptor("worker-a", 1)), "registry rejects a duplicate agent id");
    auto invalid = worker_descriptor("invalid", 0);
    check(!registry.add(std::move(invalid)), "registry rejects zero concurrency capacity");

    auto first = registry.acquire(AgentRole::Worker, {"inspect"}, std::chrono::steady_clock::now() + 100ms);
    auto second = registry.acquire(AgentRole::Worker, {"inspect"}, std::chrono::steady_clock::now() + 100ms);
    check(first.has_value() && second.has_value(), "registry leases up to max_concurrency");
    auto unavailable = registry.acquire(AgentRole::Worker, {"inspect"}, std::chrono::steady_clock::now() + 20ms);
    check(!unavailable && unavailable.error().code == "agent_capacity_timeout", "registry applies bounded waiting at capacity");
    first = Result<AgentLease>::failure(make_error(ErrorCategory::Internal, "released", "released"));
    auto after_release = registry.acquire(AgentRole::Worker, {"inspect"}, std::chrono::steady_clock::now() + 100ms);
    check(after_release.has_value(), "move-only AgentLease releases capacity through RAII");
    second = Result<AgentLease>::failure(make_error(ErrorCategory::Internal, "released", "released"));
    after_release = Result<AgentLease>::failure(make_error(ErrorCategory::Internal, "released", "released"));

    BoundedExecutor executor(3, 8);
    MultiAgentRuntime runtime(registry, executor);
    const std::vector<AgentTask> tasks{agent_task("task-0", "slow"), agent_task("task-1", "fast"),
        agent_task("task-2", "fail"), agent_task("task-3", "fast-2")};
    MultiAgentOptions options;
    options.fail_fast = false;
    const auto outcomes = runtime.run_parallel(tasks, options);
    check(outcomes && outcomes.value().size() == tasks.size(), "collect_all returns one outcome for every child task");
    if (outcomes) {
        check(outcomes.value()[0].task_id == "task-0" && outcomes.value()[1].task_id == "task-1" &&
            outcomes.value()[2].error.code == "worker_failed" && outcomes.value()[3].output == "done:fast-2",
            "fan-in is stable by input order and preserves local failures");
    }
    check(maximum.load() <= 2, "AgentLease enforces per-agent concurrency below executor concurrency");

    MultiAgentOptions fail_fast;
    fail_fast.fail_fast = true;
    const auto stopped = runtime.run_parallel(
        {agent_task("fail-first", "fail"), agent_task("cancelled-child", "slow")}, fail_fast);
    check(stopped && !stopped.value()[0].success, "fail_fast preserves the triggering failure");

    write_file(workspace / "a.txt", "alpha\n");
    write_file(workspace / "b.txt", "beta\n");
    const auto a_hash = sha256_file(workspace / "a.txt");
    const auto b_hash = sha256_file(workspace / "b.txt");
    WorkspaceCommitter committer(workspace);
    const auto committed = committer.commit({
        PatchProposal{"a.txt", a_hash ? a_hash.value() : std::string{}, "alpha-2\n"},
        PatchProposal{"b.txt", b_hash ? b_hash.value() : std::string{}, "beta-2\n"}});
    check(committed && read_text_file(workspace / "a.txt").value() == "alpha-2\n" &&
        read_text_file(workspace / "b.txt").value() == "beta-2\n", "workspace committer applies a validated multi-file batch");
    const auto conflict = committer.commit({PatchProposal{"a.txt", "stale", "must-not-write\n"}});
    check(!conflict && conflict.error().code == "workspace_conflict" &&
        read_text_file(workspace / "a.txt").value() == "alpha-2\n", "workspace conflict leaves every target unchanged");
    const auto duplicate = committer.commit({
        PatchProposal{"a.txt", sha256_file(workspace / "a.txt").value(), "one\n"},
        PatchProposal{"a.txt", sha256_file(workspace / "a.txt").value(), "two\n"}});
    check(!duplicate && duplicate.error().code == "duplicate_patch_target", "workspace batch rejects duplicate target paths");
}

std::unique_ptr<SqliteStateStore> open_store(const std::filesystem::path& path) {
    auto opened = SqliteStateStore::open(path);
    check(opened.has_value(), "SQLite state store opens and migrates");
    return opened ? std::move(opened.value()) : nullptr;
}

void test_state_store_and_cache(const std::filesystem::path& root) {
    auto store = open_store(root / "state.db");
    if (!store) return;
    const auto pragmas = store->pragmas();
    check(pragmas && pragmas.value().at("journal_mode").string_or() == "wal" &&
        pragmas.value().at("foreign_keys").integer_or() == 1, "SQLite enables WAL and foreign keys");

    const auto session = store->create_session("session-1", root.string(), JsonValue::Object{{"history", JsonValue::Array{}}});
    check(session && session.value().state_version == 0, "state store creates version-zero session");
    const auto loaded = store->get_session("session-1");
    check(loaded && loaded.value().workspace_root == root.string(), "state store reads a created session");
    const auto updated = store->compare_and_swap_session("session-1", 0, JsonValue::Object{{"value", JsonValue(1)}});
    check(updated && updated.value().state_version == 1, "session CAS increments its version");
    const auto conflict = store->compare_and_swap_session("session-1", 0, JsonValue::Object{{"value", JsonValue(2)}});
    check(!conflict && conflict.error().code == "session_version_conflict", "stale session CAS is rejected");

    const auto first = store->create_run("run-1", "session-1", "request-1", "inspect repository");
    const auto duplicate = store->create_run("run-2", "session-1", "request-1", "different ignored request");
    check(first && first.value().created && duplicate && !duplicate.value().created &&
        duplicate.value().record.id == "run-1", "request idempotency returns the original run without duplication");
    check(store->transition_run("run-1", RunStatus::Queued, RunStatus::Running).has_value(),
        "run performs a valid queued-to-running transition");
    const auto invalid_transition = store->transition_run("run-1", RunStatus::Queued, RunStatus::Succeeded, "bad");
    check(!invalid_transition && invalid_transition.error().code == "run_state_conflict", "run transition checks the expected state");
    const auto first_event = store->append_event("run-1", "run.started", JsonValue::Object{{"worker", JsonValue(1)}});
    const auto second_event = store->append_event("run-1", "run.progress", JsonValue::Object{{"step", JsonValue(2)}});
    const auto replay = store->events_after("run-1", first_event ? first_event.value().sequence : 0);
    check(first_event && second_event && second_event.value().sequence == first_event.value().sequence + 1 &&
        replay && replay.value().size() == 1 && replay.value()[0].type == "run.progress",
        "run events are monotonic and replay after a sequence");
    const auto recovered = store->recover_running_runs();
    check(recovered && recovered.value() == 1 && store->get_run("run-1").value().status == RunStatus::Interrupted,
        "startup recovery marks running runs interrupted");

    const auto now = unix_time_millis();
    MemoryRecord memory;
    memory.scope = "workspace:one";
    memory.key = "file-summary";
    memory.kind = "summary";
    memory.text = "needle architecture fact";
    memory.tags = {"architecture", "runi"};
    memory.source_path = "a.txt";
    memory.source_sha256 = "sha-old";
    memory.expires_at_ms = now + 5000;
    check(store->put_memory(memory).has_value(), "scoped memory record persists");
    const auto recalled = store->recall_memory("workspace:one", "needle architecture", 5, now);
    check(recalled && recalled.value().size() == 1, "memory recall respects scope, TTL, and lexical relevance");
    check(store->invalidate_memory_source("a.txt", "sha-new").value() == 1 &&
        store->recall_memory("workspace:one", "needle", 5, now).value().empty(),
        "source hash drift invalidates stale memory");
    memory.key = "expired";
    memory.expires_at_ms = now - 1;
    check(store->put_memory(memory).has_value() &&
        store->recall_memory("workspace:one", "needle", 5, now).value().empty(), "expired memory is not recalled");

    ContextCache cache(2);
    std::atomic<int> computes{0};
    const auto first_cache = cache.get_or_compute("a", 1s, [&] {
        ++computes;
        return Result<std::string>::success("A");
    });
    const auto hit_cache = cache.get_or_compute("a", 1s, [&] {
        ++computes;
        return Result<std::string>::success("A2");
    });
    check(first_cache && hit_cache && hit_cache.value() == "A" && computes.load() == 1, "context cache returns a live LRU hit");
    static_cast<void>(cache.get_or_compute("b", 1s, [] { return Result<std::string>::success("B"); }));
    static_cast<void>(cache.get_or_compute("c", 1s, [] { return Result<std::string>::success("C"); }));
    static_cast<void>(cache.get_or_compute("a", 1s, [&] { ++computes; return Result<std::string>::success("A3"); }));
    check(computes.load() == 2, "context cache evicts the least recently used entry");
    static_cast<void>(cache.get_or_compute("ttl", 5ms, [] { return Result<std::string>::success("old"); }));
    std::this_thread::sleep_for(10ms);
    const auto refreshed = cache.get_or_compute("ttl", 1s, [] { return Result<std::string>::success("new"); });
    check(refreshed && refreshed.value() == "new", "context cache expires TTL entries");

    ContextCache singleflight(4);
    std::atomic<int> shared_computes{0};
    std::vector<std::future<Result<std::string>>> callers;
    for (int index = 0; index < 4; ++index) {
        callers.push_back(std::async(std::launch::async, [&] {
            return singleflight.get_or_compute("shared", 1s, [&] {
                ++shared_computes;
                std::this_thread::sleep_for(30ms);
                return Result<std::string>::success("shared-value");
            });
        }));
    }
    for (auto& caller : callers) check(caller.get().value() == "shared-value", "singleflight waiters share the computed value");
    check(shared_computes.load() == 1 && singleflight.snapshot().coalesced >= 3,
        "singleflight computes one value and counts coalesced callers");
}

void test_agent_service_adapter(const std::filesystem::path& root) {
    write_file(root / "README.md", "service fixture\n");
    auto opened = SqliteStateStore::open(root / ".runi" / "agent-service.db");
    check(opened.has_value(), "agent service SQLite store opens");
    if (!opened) return;
    auto store = std::shared_ptr<SqliteStateStore>(std::move(opened.value()));
    const auto session = store->create_session("agent-session", root.string(), JsonValue::Object{});
    check(session.has_value(), "agent service session is created");
    if (!session) return;
    std::atomic<int> models{0};
    ModelClientFactory factory = [&]() -> Result<std::shared_ptr<IModelClient>> {
        ++models;
        std::shared_ptr<IModelClient> model = std::make_shared<FakeModelClient>(
            std::vector<std::string>{"<final>served</final>"});
        return Result<std::shared_ptr<IModelClient>>::success(std::move(model));
    };
    RuntimeOptions runtime_options;
    runtime_options.approval_policy = "auto";
    RuniAgentServiceHandler handler(store, root / ".runi", factory, runtime_options);
    RuntimeRunRecord run;
    run.id = "agent-run";
    run.session_id = "agent-session";
    run.request = "answer through the service adapter";
    const auto answer = handler(RunInvocation{run, session.value()}, {});
    check(answer && answer.value() == "served", "service handler reaches the existing Runi AgentLoop");
    SqliteSessionStore sessions(store);
    const auto persisted = sessions.load("agent-session");
    check(persisted && persisted.value().history.size() == 2 &&
        persisted.value().history.back().content == "served", "AgentLoop session history persists through SQLite CAS");
    std::stop_source cancelled;
    cancelled.request_stop();
    const auto stopped = handler(RunInvocation{run, session.value()}, cancelled.get_token());
    check(!stopped && stopped.error().code == "run_cancelled" && models.load() == 1,
        "pre-cancelled service runs stop before constructing a model client");
}

ServiceRequest json_request(std::string method, std::string target, JsonValue body = JsonValue::Object{}) {
    ServiceRequest request;
    request.method = std::move(method);
    request.target = std::move(target);
    request.headers = {{"content-type", "application/json"}};
    request.body = dump_json(body);
    return request;
}

JsonValue response_json(const ServiceResponse& response) {
    const auto parsed = parse_json(response.body);
    return parsed ? parsed.value() : JsonValue::Object{};
}

void test_service_and_socket(const std::filesystem::path& root) {
    auto store = open_store(root / "service.db");
    if (!store) return;
    BoundedExecutor run_executor(2, 4);
    std::atomic<int> handler_calls{0};
    RuntimeService service(*store, run_executor,
        [&](const RunInvocation& invocation, std::stop_token stop_token) -> Result<std::string> {
            ++handler_calls;
            if (invocation.run.request == "block") {
                while (!stop_token.stop_requested()) std::this_thread::sleep_for(2ms);
                return Result<std::string>::failure(make_error(ErrorCategory::Timeout, "cancelled", "cancelled"));
            }
            return Result<std::string>::success("answer:" + invocation.run.request);
        }, RuntimeServiceOptions{root, 1024});

    auto create_session = json_request("POST", "/v1/sessions",
        JsonValue::Object{{"id", JsonValue("api-session")}, {"workspace_root", JsonValue(root.string())}});
    const auto session_response = service.handle(create_session);
    check(session_response.status == 201 && response_json(session_response).at("id").string_or() == "api-session",
        "service creates an allowed session");

    const auto resume_created = store->create_run(
        "resume-run", "api-session", "api-request-resume", "resume-me");
    check(resume_created && store->transition_run("resume-run", RunStatus::Queued, RunStatus::Running).has_value() &&
        store->recover_running_runs().value() == 1, "resume fixture persists an interrupted run");
    const auto resumed = service.handle(ServiceRequest{"POST", "/v1/runs/resume-run:resume", {}, {}});
    check(resumed.status == 202 && eventually([&] {
        return store->get_run("resume-run").value().status == RunStatus::Succeeded;
    }), "resume endpoint requeues and completes an interrupted run");
    const auto calls_before_idempotency = handler_calls.load();

    auto create_run = json_request("POST", "/v1/sessions/api-session/runs",
        JsonValue::Object{{"id", JsonValue("api-run")}, {"request", JsonValue("hello")}});
    create_run.headers["idempotency-key"] = "api-request-1";
    const auto run_response = service.handle(create_run);
    const auto duplicate_response = service.handle(create_run);
    check(run_response.status == 202 && duplicate_response.status == 200,
        "service differentiates accepted and idempotent replay responses");
    check(eventually([&] {
        const auto response = service.handle(ServiceRequest{"GET", "/v1/runs/api-run", {}, {}});
        return response.status == 200 && response_json(response).at("status").string_or() == "succeeded";
    }), "service executes an accepted run asynchronously");
    check(handler_calls.load() == calls_before_idempotency + 1, "idempotent replay does not execute the handler twice");

    auto blocking = json_request("POST", "/v1/sessions/api-session/runs",
        JsonValue::Object{{"id", JsonValue("cancel-run")}, {"request", JsonValue("block")}});
    blocking.headers["idempotency-key"] = "api-request-cancel";
    check(service.handle(blocking).status == 202, "service accepts a cancellable run");
    check(eventually([&] { return store->get_run("cancel-run").value().status == RunStatus::Running; }),
        "cancellation fixture reaches running state");
    const auto cancel = service.handle(ServiceRequest{"POST", "/v1/runs/cancel-run:cancel", {}, {}});
    check(cancel.status == 202 && eventually([&] {
        return store->get_run("cancel-run").value().status == RunStatus::Cancelled;
    }), "cancel endpoint propagates stop and persists cancelled state");

    const auto events = service.handle(ServiceRequest{"GET", "/v1/runs/api-run/events",
        {{"last-event-id", "1"}}, {}});
    check(events.status == 200 && events.headers.at("content-type") == "text/event-stream" &&
        events.body.find("event: run.succeeded") != std::string::npos,
        "SSE endpoint replays durable events after Last-Event-ID");

    auto oversized = json_request("POST", "/v1/sessions", JsonValue::Object{{"padding", JsonValue(std::string(2048, 'x'))}});
    check(service.handle(oversized).status == 413, "service rejects oversized request bodies");

    SocketHttpServer server(service, SocketServerOptions{0, 2, 8, 4096});
    const auto started = server.start();
    check(started.has_value() && server.port() != 0, "loopback HTTP server binds an ephemeral port");
    if (started) {
        RuniClient client("127.0.0.1", server.port(), 2s);
        const auto network_response = client.send(ServiceRequest{"GET", "/v1/runs/api-run", {}, {}});
        check(network_response && network_response.value().status == 200 &&
            response_json(network_response.value()).at("result").string_or() == "answer:hello",
            "C++ client completes a real loopback TCP/HTTP round trip");
        server.stop();
        check(!server.running(), "server stop joins listener and connection workers");
    }
}

void test_service_overload(const std::filesystem::path& root) {
    auto store = open_store(root / "overload.db");
    if (!store) return;
    check(store->create_session("overload-session", root.string()).has_value(), "overload session is created");
    BoundedExecutor executor(1, 1);
    RuntimeService service(*store, executor,
        [](const RunInvocation&, std::stop_token stop_token) -> Result<std::string> {
            while (!stop_token.stop_requested()) std::this_thread::sleep_for(2ms);
            return Result<std::string>::failure(make_error(ErrorCategory::Timeout, "cancelled", "cancelled"));
        }, RuntimeServiceOptions{root, 1024});
    const auto submit = [&](std::string id, std::string request_id) {
        auto request = json_request("POST", "/v1/sessions/overload-session/runs",
            JsonValue::Object{{"id", JsonValue(std::move(id))}, {"request", JsonValue("hold")}});
        request.headers["idempotency-key"] = std::move(request_id);
        return service.handle(request);
    };
    check(submit("overload-1", "overload-request-1").status == 202 && eventually([&] {
        return store->get_run("overload-1").value().status == RunStatus::Running;
    }), "overload fixture occupies its only run worker");
    check(submit("overload-2", "overload-request-2").status == 202, "overload fixture fills its waiting queue");
    const auto rejected = submit("overload-3", "overload-request-3");
    check(rejected.status == 503 && store->get_run("overload-3").value().status == RunStatus::Failed,
        "full run queue returns 503 and persists a terminal rejection");
    static_cast<void>(service.handle(ServiceRequest{"POST", "/v1/runs/overload-1:cancel", {}, {}}));
    static_cast<void>(service.handle(ServiceRequest{"POST", "/v1/runs/overload-2:cancel", {}, {}}));
    check(eventually([&] {
        return store->get_run("overload-1").value().status == RunStatus::Cancelled &&
            store->get_run("overload-2").value().status == RunStatus::Cancelled;
    }), "queued and running overload fixtures both converge after cancellation");
}

}  // namespace

int main() {
    const auto base = std::filesystem::current_path() / "runtime-test-workspaces";
    std::error_code error;
    const auto cwd = std::filesystem::weakly_canonical(std::filesystem::current_path(), error);
    const auto target = std::filesystem::absolute(base, error).lexically_normal();
    if (error || target.string().find(cwd.string()) != 0) {
        std::cerr << "unsafe runtime test workspace target\n";
        return 2;
    }
    std::filesystem::remove_all(target, error);
    std::filesystem::create_directories(target);
    test_bounded_executor();
    test_agent_registry_and_runtime(target / "workspace");
    test_state_store_and_cache(target / "state");
    test_agent_service_adapter(target / "agent-service");
    test_service_and_socket(target / "service");
    test_service_overload(target / "overload");
    std::filesystem::remove_all(target, error);
    if (failures != 0) {
        std::cerr << failures << " runtime test(s) failed\n";
        return 1;
    }
    std::cout << "all runtime service tests passed\n";
    return 0;
}
