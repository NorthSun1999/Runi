#include <filesystem>
#include <iostream>
#include <string>

#include "runi/core/json_codec.hpp"
#include "runi/evaluation/evaluation.hpp"

int main(int argc, char* argv[]) {
    if (argc > 1 && std::string(argv[1]) == "context-ablation") {
        runi::ContextAblationOptions options;
        if (argc > 2) options.matrix_path = std::filesystem::path(argv[2]);
        if (argc > 3) options.artifact_path = std::filesystem::path(argv[3]);
        if (argc > 4) options.workspace_root = std::filesystem::path(argv[4]);
        if (argc > 5) {
            try {
                options.repetitions_override = static_cast<std::size_t>(std::stoull(argv[5]));
            } catch (const std::exception&) {
                std::cerr << "invalid context ablation repetitions\n";
                return 2;
            }
        }
        const auto result = runi::run_context_ablation(std::move(options));
        if (!result) {
            std::cerr << result.error().message << '\n';
            return 1;
        }
        const auto& summary = result.value().at("summary");
        std::cout << runi::dump_json(summary, 2, true) << '\n';
        return summary.at("contract_passed").bool_or() ? 0 : 3;
    }
    runi::BenchmarkOptions options;
    if (argc > 1) options.benchmark_path = std::filesystem::path(argv[1]);
    if (argc > 2) options.artifact_path = std::filesystem::path(argv[2]);
    if (argc > 3) options.workspace_root = std::filesystem::path(argv[3]);
    const auto result = runi::run_fixed_benchmark(std::move(options));
    if (!result) {
        std::cerr << result.error().message << '\n';
        return 1;
    }
    std::cout << runi::dump_json(result.value().at("summary"), 2, true) << '\n';
    return 0;
}
