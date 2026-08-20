#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

#include "runi/model/config.hpp"
#include "runi/core/text.hpp"
#include "runi/model/providers.hpp"
#include "runi/agent/runtime.hpp"

namespace {

using namespace runi;

constexpr std::string_view kDefaultOllamaModel = "qwen3.5:4b";
constexpr std::string_view kDefaultOllamaHost = "http://127.0.0.1:11434";
constexpr std::string_view kDefaultOpenAIModel = "gpt-5.4";
constexpr std::string_view kDefaultOpenAIBase = "https://www.right.codes/codex/v1";
constexpr std::string_view kDefaultAnthropicModel = "claude-sonnet-4-6";
constexpr std::string_view kDefaultAnthropicBase = "https://www.right.codes/claude/v1";
constexpr std::string_view kDefaultDeepSeekModel = "deepseek-v4-pro";
constexpr std::string_view kDefaultDeepSeekBase = "https://api.deepseek.com/anthropic";

struct Args {
    std::vector<std::string> prompt;
    std::filesystem::path cwd{"."};
    std::optional<std::string> provider, model, host, base_url, resume;
    int ollama_timeout{300}, openai_timeout{300};
    std::string approval{"ask"};
    std::vector<std::string> secret_env_names;
    std::size_t max_steps{6}, max_new_tokens{512};
    double temperature{0.2}, top_p{0.9};
};

[[noreturn]] void usage_error(std::string message) {
    throw std::invalid_argument(std::move(message) + "\nUse --help to list available options.");
}

void print_help() {
    std::cout <<
        "usage: runi [options] [prompt ...]\n\n"
        "Minimal coding agent for DeepSeek, OpenAI-compatible, Anthropic-compatible, or Ollama models.\n\n"
        "options:\n"
        "  --cwd PATH                 Workspace directory (default: .)\n"
        "  --provider NAME            ollama, openai, anthropic, or deepseek\n"
        "  --model NAME               Model name override\n"
        "  --host URL                 Ollama server URL\n"
        "  --base-url URL             Provider API base URL\n"
        "  --ollama-timeout SECONDS   Ollama request timeout (default: 300)\n"
        "  --openai-timeout SECONDS   Compatible-provider timeout (default: 300)\n"
        "  --resume ID                Session id to resume or latest\n"
        "  --approval MODE            ask, auto, or never (default: ask)\n"
        "  --secret-env-name NAME     Extra secret environment variable; repeatable\n"
        "  --max-steps N              Maximum tool/model iterations (default: 6)\n"
        "  --max-new-tokens N         Maximum model output tokens (default: 512)\n"
        "  --temperature VALUE        Sampling temperature (default: 0.2)\n"
        "  --top-p VALUE              Ollama top-p value (default: 0.9)\n"
        "  -h, --help                 Show this help message\n";
}

Args parse_args(int argc, char* argv[]) {
    Args args;
    bool positional_only = false;
    for (int index = 1; index < argc; ++index) {
        const std::string token = argv[index];
        if (positional_only) {
            args.prompt.push_back(token);
            continue;
        }
        if (token == "--") {
            positional_only = true;
            continue;
        }

        std::string option = token;
        std::optional<std::string> attached_value;
        if (token.starts_with("--")) {
            if (const auto equals = token.find('='); equals != std::string::npos) {
                option = token.substr(0, equals);
                attached_value = token.substr(equals + 1);
            }
        }
        const auto value = [&]() -> std::string {
            if (attached_value.has_value()) return std::move(*attached_value);
            if (++index >= argc) usage_error("argument " + option + ": expected one value");
            return argv[index];
        };

        if (option == "--help" || option == "-h") {
            if (attached_value.has_value()) usage_error("argument " + option + ": ignored explicit argument");
            print_help();
            std::exit(0);
        }
        if (option == "--cwd") args.cwd = value();
        else if (option == "--provider") args.provider = value();
        else if (option == "--model") args.model = value();
        else if (option == "--host") args.host = value();
        else if (option == "--base-url") args.base_url = value();
        else if (option == "--ollama-timeout") args.ollama_timeout = std::stoi(value());
        else if (option == "--openai-timeout") args.openai_timeout = std::stoi(value());
        else if (option == "--resume") args.resume = value();
        else if (option == "--approval") args.approval = value();
        else if (option == "--secret-env-name") args.secret_env_names.push_back(value());
        else if (option == "--max-steps") args.max_steps = std::stoull(value());
        else if (option == "--max-new-tokens") args.max_new_tokens = std::stoull(value());
        else if (option == "--temperature") args.temperature = std::stod(value());
        else if (option == "--top-p") args.top_p = std::stod(value());
        else if (option.starts_with('-')) usage_error("unrecognized argument: " + token);
        else args.prompt.push_back(token);
    }
    if (args.approval != "ask" && args.approval != "auto" && args.approval != "never") usage_error("unknown approval policy: " + args.approval);
    if (args.max_steps == 0 || args.max_new_tokens == 0 || args.ollama_timeout <= 0 || args.openai_timeout <= 0) usage_error("numeric limits must be positive");
    return args;
}

std::string effective_provider(const Args& args) {
    const auto provider = args.provider.value_or(provider_env("RUNI_PROVIDER", {}, "deepseek"));
    static const std::vector<std::string> choices{"ollama", "openai", "anthropic", "deepseek"};
    if (std::find(choices.begin(), choices.end(), provider) == choices.end()) {
        usage_error("unknown provider: " + provider + ". expected one of: ollama, openai, anthropic, deepseek");
    }
    return provider;
}

std::string effective_model(const Args& args, std::string_view provider) {
    if (args.model.has_value() && !args.model->empty()) return *args.model;
    if (provider == "openai") return provider_env("RUNI_OPENAI_MODEL", {"OPENAI_MODEL"}, kDefaultOpenAIModel);
    if (provider == "anthropic") return provider_env("RUNI_ANTHROPIC_MODEL", {"ANTHROPIC_MODEL"}, kDefaultAnthropicModel);
    if (provider == "deepseek") return provider_env("RUNI_DEEPSEEK_MODEL", {"DEEPSEEK_MODEL"}, kDefaultDeepSeekModel);
    return provider_env("RUNI_OLLAMA_MODEL", {"OLLAMA_MODEL"}, kDefaultOllamaModel);
}

std::set<std::string, std::less<>> configured_secret_names(const Args& args) {
    std::set<std::string, std::less<>> names{
        "RUNI_OPENAI_API_KEY", "OPENAI_API_KEY", "OPENAI_API_TOKEN", "RUNI_ANTHROPIC_API_KEY", "ANTHROPIC_API_KEY",
        "ANTHROPIC_AUTH_TOKEN", "RUNI_DEEPSEEK_API_KEY", "DEEPSEEK_API_KEY", "RUNI_RIGHT_CODES_API_KEY",
        "RIGHT_CODES_API_KEY", "GITHUB_PAT", "GH_PAT"};
    const auto add = [&](std::string name) {
        std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
        if (!name.empty()) names.insert(std::move(name));
    };
    for (const auto& name : args.secret_env_names) add(name);
    const auto extras = provider_env("RUNI_SECRET_ENV_NAMES");
    std::size_t start = 0;
    while (start <= extras.size()) {
        const auto end = extras.find(',', start);
        add(trim(std::string_view(extras).substr(start, end == std::string::npos ? std::string::npos : end - start)));
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return names;
}

std::shared_ptr<IModelClient> build_model(const Args& args, std::string_view provider, const std::string& model) {
    using namespace std::chrono;
    if (provider == "openai") {
        const auto base = args.base_url.value_or(provider_env("RUNI_OPENAI_API_BASE", {"OPENAI_API_BASE"}, kDefaultOpenAIBase));
        const auto key = provider_env("RUNI_OPENAI_API_KEY", {"OPENAI_API_KEY", "RUNI_RIGHT_CODES_API_KEY", "RIGHT_CODES_API_KEY", "RUNI_ANTHROPIC_API_KEY", "ANTHROPIC_API_KEY"});
        return std::make_shared<OpenAICompatibleModelClient>(model, base, key, args.temperature, seconds(args.openai_timeout));
    }
    if (provider == "anthropic") {
        const auto base = args.base_url.value_or(provider_env("RUNI_ANTHROPIC_API_BASE", {"ANTHROPIC_API_BASE"}, kDefaultAnthropicBase));
        const auto key = provider_env("RUNI_ANTHROPIC_API_KEY", {"ANTHROPIC_API_KEY", "RUNI_RIGHT_CODES_API_KEY", "RIGHT_CODES_API_KEY", "RUNI_OPENAI_API_KEY", "OPENAI_API_KEY"});
        return std::make_shared<AnthropicCompatibleModelClient>(model, base, key, args.temperature, seconds(args.openai_timeout));
    }
    if (provider == "deepseek") {
        const auto base = args.base_url.value_or(provider_env("RUNI_DEEPSEEK_API_BASE", {"DEEPSEEK_API_BASE"}, kDefaultDeepSeekBase));
        const auto key = provider_env("RUNI_DEEPSEEK_API_KEY", {"DEEPSEEK_API_KEY"});
        return std::make_shared<AnthropicCompatibleModelClient>(model, base, key, args.temperature, seconds(args.openai_timeout));
    }
    const auto host = args.host.value_or(provider_env(
        "RUNI_OLLAMA_HOST", {"OLLAMA_HOST"}, kDefaultOllamaHost));
    return std::make_shared<OllamaModelClient>(model, host, args.temperature, args.top_p, seconds(args.ollama_timeout));
}

std::string welcome(const Runi& agent, std::string_view model) {
    constexpr std::size_t width = 80, inner = width - 4, gap = 3, left_width = (inner - gap) / 2, right_width = inner - gap - left_width;
    const auto divider = [](char c) { return "+" + std::string(width - 2, c) + "+"; };
    const auto center = [](std::string_view value) { auto body = middle(value, inner); const auto pad = inner > utf8_length(body) ? inner - utf8_length(body) : 0; return "| " + std::string(pad / 2, ' ') + body + std::string(pad - pad / 2, ' ') + " |"; };
    const auto logo = [&](std::string_view value, std::size_t indent) {
        constexpr std::size_t logo_width = 52;
        auto body = std::string(indent, ' ') + std::string(value);
        body.append(logo_width - std::min(logo_width, utf8_length(body)), ' ');
        return center(body);
    };
    const auto row = [](std::string_view value) { auto body = middle(value, width - 4); return "| " + body + std::string(width - 4 - utf8_length(body), ' ') + " |"; };
    const auto cell = [](std::string_view label, std::string_view value, std::size_t size) { auto body = middle(std::string(label) + std::string(9 - std::min<std::size_t>(9, label.size()), ' ') + " " + std::string(value), size); return body + std::string(size - utf8_length(body), ' '); };
    const auto pair = [&](std::string_view l1, std::string_view v1, std::string_view l2, std::string_view v2) { return "| " + cell(l1, v1, left_width) + std::string(gap, ' ') + cell(l2, v2, right_width) + " |"; };
    const auto& workspace = agent.workspace();
    return join({divider('='), logo(" _____    _    _   _   _   _____", 6),
        logo("|  __ \\  | |  | | | \\ | | |_   _|", 5),
        logo("| |__) | | |  | | |  \\| |   | |", 4),
        logo("|  _  /  | |  | | | . ` |   | |", 3),
        logo("| | \\ \\  | |__| | | |\\  |  _| |_", 2),
        logo("|_|  \\_\\  \\____/  |_| \\_| |_____|", 1),
        logo("\\_\\   \\_\\  \\____\\   \\_\\ \\_\\  \\_____\\", 2),
        divider('-'), row(""),
        row("WORKSPACE  " + middle(workspace.cwd, inner - 11)), pair("MODEL", model, "BRANCH", workspace.branch),
        pair("APPROVAL", agent.options().approval_policy, "SESSION", agent.session().id), row(""), divider('=')}, "\n");
}

std::string result_or_error(Result<std::string> result, bool& okay) {
    if (result) return result.value();
    okay = false;
    return result.error().message;
}

}  // namespace

int main(int argc, char* argv[]) {
#ifdef _WIN32
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
#endif
    try {
        const auto args = parse_args(argc, argv);
        const auto workspace_result = WorkspaceContext::build(args.cwd);
        if (!workspace_result) throw std::runtime_error(workspace_result.error().message);
        auto workspace = workspace_result.value();
        const auto loaded = load_project_env(workspace.repo_root);
        if (!loaded) throw std::runtime_error(loaded.error().message);
        const auto provider = effective_provider(args);
        const auto model_name = effective_model(args, provider);
        auto model = build_model(args, provider, model_name);
        const auto state_root = std::filesystem::path(workspace.repo_root) / ".runi";
        auto sessions = std::make_shared<SessionStore>(state_root / "sessions");
        auto runs = std::make_shared<RunStore>(state_root / "runs");
        std::optional<SessionState> session;
        if (args.resume.has_value()) {
            auto id = *args.resume;
            if (id == "latest") id = sessions->latest().value_or("");
            if (!id.empty()) {
                const auto loaded_session = sessions->load(id);
                if (!loaded_session) throw std::runtime_error(loaded_session.error().message);
                session = loaded_session.value();
            }
        }
        RuntimeOptions options;
        options.approval_policy = args.approval;
        options.max_steps = args.max_steps;
        options.max_new_tokens = args.max_new_tokens;
        options.secret_env_names = configured_secret_names(args);
        Runi agent(model, std::move(workspace), sessions, runs, std::move(session), std::move(options));
        std::cout << welcome(agent, model_name) << '\n';

        if (!args.prompt.empty()) {
            const auto prompt = trim(join(args.prompt, " "));
            if (!prompt.empty()) {
                std::cout << '\n';
                bool okay = true;
                const auto output = result_or_error(agent.ask(prompt), okay);
                (okay ? std::cout : std::cerr) << output << '\n';
                if (!okay) return 1;
            }
            return 0;
        }

        constexpr std::string_view help =
            "Commands:\n/help    Show this help message.\n/memory  Show the agent's distilled working memory.\n"
            "/session Show the path to the saved session file.\n/reset   Clear the current session history and memory.\n/exit    Exit the agent.";
        while (true) {
            std::cout << "\nruni> " << std::flush;
            std::string input;
            if (!std::getline(std::cin, input)) { std::cout << '\n'; return 0; }
            input = trim(input);
            if (input.empty()) continue;
            if (input == "/exit" || input == "/quit") return 0;
            if (input == "/help") { std::cout << help << '\n'; continue; }
            if (input == "/memory") { std::cout << agent.memory_text() << '\n'; continue; }
            if (input == "/session") { std::cout << agent.session_path().string() << '\n'; continue; }
            if (input == "/reset") {
                const auto reset = agent.reset();
                if (!reset) std::cerr << reset.error().message << '\n'; else std::cout << "session reset\n";
                continue;
            }
            std::cout << '\n';
            bool okay = true;
            const auto output = result_or_error(agent.ask(input), okay);
            (okay ? std::cout : std::cerr) << output << '\n';
        }
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
