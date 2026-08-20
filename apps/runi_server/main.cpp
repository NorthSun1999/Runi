#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>

#include "runi/service/agent_service.hpp"
#include "runi/model/config.hpp"
#include "runi/model/providers.hpp"
#include "runi/service/service.hpp"

namespace {

using namespace runi;
using namespace std::chrono_literals;

constexpr std::string_view kDefaultOllamaModel = "qwen3.5:4b";
constexpr std::string_view kDefaultOllamaHost = "http://127.0.0.1:11434";
constexpr std::string_view kDefaultOpenAIModel = "gpt-5.4";
constexpr std::string_view kDefaultOpenAIBase = "https://www.right.codes/codex/v1";
constexpr std::string_view kDefaultAnthropicModel = "claude-sonnet-4-6";
constexpr std::string_view kDefaultAnthropicBase = "https://www.right.codes/claude/v1";
constexpr std::string_view kDefaultDeepSeekModel = "deepseek-v4-pro";
constexpr std::string_view kDefaultDeepSeekBase = "https://api.deepseek.com/anthropic";

struct Args {
    std::filesystem::path cwd{"."};
    std::filesystem::path database;
    std::uint16_t port{8765};
    std::size_t workers{4};
    std::size_t queue_capacity{32};
    std::size_t max_steps{6};
    std::string approval{"never"};
};

void print_help() {
    std::cout <<
        "usage: runi_server [options]\n\n"
        "Loopback HTTP service for the Runi C++20 Agent runtime.\n\n"
        "options:\n"
        "  --cwd PATH           Allowed workspace root (default: .)\n"
        "  --database PATH      SQLite database (default: <cwd>/.runi/runi.db)\n"
        "  --port N             Loopback TCP port, 0 chooses an ephemeral port (default: 8765)\n"
        "  --workers N          Concurrent Agent run workers (default: 4)\n"
        "  --queue-capacity N   Waiting Agent run capacity (default: 32)\n"
        "  --max-steps N        Maximum AgentLoop steps per run (default: 6)\n"
        "  --approval MODE      auto or never for service tools (default: never)\n"
        "  -h, --help           Show this help message\n";
}

Args parse_args(int argc, char* argv[]) {
    Args args;
    for (int index = 1; index < argc; ++index) {
        const std::string option = argv[index];
        const auto value = [&]() -> std::string {
            if (++index >= argc) throw std::invalid_argument(option + " requires a value");
            return argv[index];
        };
        if (option == "-h" || option == "--help") {
            print_help();
            std::exit(0);
        }
        if (option == "--cwd") args.cwd = value();
        else if (option == "--database") args.database = value();
        else if (option == "--port") {
            const auto raw = std::stoul(value());
            if (raw > 65535) throw std::invalid_argument("port must be between 0 and 65535");
            args.port = static_cast<std::uint16_t>(raw);
        } else if (option == "--workers") args.workers = std::stoull(value());
        else if (option == "--queue-capacity") args.queue_capacity = std::stoull(value());
        else if (option == "--max-steps") args.max_steps = std::stoull(value());
        else if (option == "--approval") args.approval = value();
        else throw std::invalid_argument("unknown option: " + option);
    }
    if (args.workers == 0 || args.queue_capacity == 0 || args.max_steps == 0) {
        throw std::invalid_argument("workers, queue-capacity, and max-steps must be positive");
    }
    if (args.approval != "auto" && args.approval != "never") {
        throw std::invalid_argument("service approval must be auto or never");
    }
    return args;
}

ModelClientFactory model_factory_from_environment() {
    const auto provider = provider_env("RUNI_PROVIDER", {}, "deepseek");
    if (provider == "ollama") {
        const auto model = provider_env("RUNI_OLLAMA_MODEL", {"OLLAMA_MODEL"}, kDefaultOllamaModel);
        const auto host = provider_env("RUNI_OLLAMA_HOST", {"OLLAMA_HOST"}, kDefaultOllamaHost);
        return [model, host]() -> Result<std::shared_ptr<IModelClient>> {
            return Result<std::shared_ptr<IModelClient>>::success(
                std::make_shared<OllamaModelClient>(model, host, 0.2, 0.9, 300s));
        };
    }
    if (provider == "openai") {
        const auto model = provider_env("RUNI_OPENAI_MODEL", {"OPENAI_MODEL"}, kDefaultOpenAIModel);
        const auto base = provider_env("RUNI_OPENAI_API_BASE", {"OPENAI_API_BASE"}, kDefaultOpenAIBase);
        const auto key = provider_env("RUNI_OPENAI_API_KEY", {"OPENAI_API_KEY"});
        return [model, base, key]() -> Result<std::shared_ptr<IModelClient>> {
            return Result<std::shared_ptr<IModelClient>>::success(
                std::make_shared<OpenAICompatibleModelClient>(model, base, key, 0.2, 300s));
        };
    }
    if (provider == "anthropic" || provider == "deepseek") {
        const bool deepseek = provider == "deepseek";
        const auto model = deepseek ? provider_env("RUNI_DEEPSEEK_MODEL", {"DEEPSEEK_MODEL"}, kDefaultDeepSeekModel) :
            provider_env("RUNI_ANTHROPIC_MODEL", {"ANTHROPIC_MODEL"}, kDefaultAnthropicModel);
        const auto base = deepseek ? provider_env("RUNI_DEEPSEEK_API_BASE", {"DEEPSEEK_API_BASE"}, kDefaultDeepSeekBase) :
            provider_env("RUNI_ANTHROPIC_API_BASE", {"ANTHROPIC_API_BASE"}, kDefaultAnthropicBase);
        const auto key = deepseek ? provider_env("RUNI_DEEPSEEK_API_KEY", {"DEEPSEEK_API_KEY"}) :
            provider_env("RUNI_ANTHROPIC_API_KEY", {"ANTHROPIC_API_KEY"});
        return [model, base, key]() -> Result<std::shared_ptr<IModelClient>> {
            return Result<std::shared_ptr<IModelClient>>::success(
                std::make_shared<AnthropicCompatibleModelClient>(model, base, key, 0.2, 300s));
        };
    }
    return [provider]() -> Result<std::shared_ptr<IModelClient>> {
        return Result<std::shared_ptr<IModelClient>>::failure(make_error(
            ErrorCategory::Configuration, "unknown_provider", "Unknown RUNI_PROVIDER: " + provider));
    };
}

}  // namespace

int main(int argc, char* argv[]) {
    try {
        const auto args = parse_args(argc, argv);
        const auto root = std::filesystem::weakly_canonical(args.cwd);
        const auto loaded = load_project_env(root);
        if (!loaded) throw std::runtime_error(loaded.error().message);
        const auto database_path = args.database.empty() ? root / ".runi" / "runi.db" : args.database;
        auto opened = SqliteStateStore::open(database_path);
        if (!opened) throw std::runtime_error(opened.error().message);
        auto state_store = std::shared_ptr<SqliteStateStore>(std::move(opened.value()));
        const auto recovered = state_store->recover_running_runs();
        if (!recovered) throw std::runtime_error(recovered.error().message);

        RuntimeOptions runtime_options;
        runtime_options.approval_policy = args.approval;
        runtime_options.max_steps = args.max_steps;
        auto agent_handler = std::make_shared<RuniAgentServiceHandler>(
            state_store, root / ".runi", model_factory_from_environment(), runtime_options);
        BoundedExecutor run_executor(args.workers, args.queue_capacity);
        RuntimeService service(*state_store, run_executor,
            [agent_handler](const RunInvocation& invocation, std::stop_token stop_token) {
                return (*agent_handler)(invocation, stop_token);
            }, RuntimeServiceOptions{root, 1024 * 1024});
        SocketHttpServer server(service, SocketServerOptions{args.port, 4, 32, 1024 * 1024});
        const auto started = server.start();
        if (!started) throw std::runtime_error(started.error().message);
        std::cout << "runi_server listening on http://127.0.0.1:" << server.port() << '\n'
                  << "recovered interrupted runs: " << recovered.value() << '\n'
                  << "press Enter to stop\n";
        std::string line;
        std::getline(std::cin, line);
        server.stop();
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
