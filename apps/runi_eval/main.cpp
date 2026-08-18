#include <filesystem>
#include <iostream>
#include <string>

#include "runi/core/json_codec.hpp"
#include "runi/evaluation.hpp"

int main(int argc, char* argv[]) {
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
