#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "runi/model/model_client.hpp"
#include "runi/tool/workspace.hpp"

namespace runi {

class Runi;

inline constexpr std::int64_t kBenchmarkSchemaVersion = 1;
inline constexpr std::int64_t kContextAblationSchemaVersion = 1;

[[nodiscard]] Result<JsonValue> validate_benchmark(const JsonValue& data, const std::filesystem::path& repo_root);
[[nodiscard]] Result<JsonValue> load_benchmark(const std::filesystem::path& path, std::optional<std::filesystem::path> repo_root = std::nullopt);
[[nodiscard]] JsonValue summarize_rows(const JsonValue::Array& rows);

using ModelClientFactory = std::function<std::shared_ptr<IModelClient>(const JsonValue&, const WorkspaceContext&)>;

struct BenchmarkOptions {
    std::filesystem::path benchmark_path{"benchmarks/coding_tasks.json"};
    std::filesystem::path artifact_path{"benchmarks/benchmark-v1.json"};
    std::filesystem::path workspace_root{"build/benchmark-workspaces"};
    std::string model_name{"FakeModelClient"};
    std::string model_version{"scripted-deterministic"};
    double temperature{0.0};
    double top_p{1.0};
    std::size_t max_new_tokens{64};
    std::string timezone_name{"Asia/Shanghai"};
    ModelClientFactory model_client_factory;
};

struct ContextAblationOptions {
    std::filesystem::path matrix_path{"benchmarks/context_ablation.json"};
    std::filesystem::path artifact_path{"benchmarks/results/context-ablation-v1.json"};
    std::filesystem::path workspace_root{"build/context-ablation-workspaces"};
    std::size_t repetitions_override{0};
    std::string timezone_name{"Asia/Shanghai"};
};

class BenchmarkEvaluator {
public:
    explicit BenchmarkEvaluator(BenchmarkOptions options = {});
    [[nodiscard]] Result<JsonValue> load() const;
    [[nodiscard]] Result<JsonValue> run();
    [[nodiscard]] Result<JsonValue> run_task(const JsonValue& task);

private:
    [[nodiscard]] Result<void> apply_task_setup(Runi& agent, const JsonValue& task, const std::filesystem::path& fixture_root) const;
    [[nodiscard]] static std::string failure_category(bool within_budget, bool verifier_passed,
        bool artifact_exists, bool non_failure_stop_reason);
    [[nodiscard]] Result<void> write_artifact(const JsonValue& artifact) const;

    BenchmarkOptions options_;
    std::filesystem::path repo_root_;
};

[[nodiscard]] Result<JsonValue> run_fixed_benchmark(BenchmarkOptions options = {});
[[nodiscard]] Result<JsonValue> run_harness_regression_v2(BenchmarkOptions options = {});
[[nodiscard]] Result<JsonValue> run_context_ablation(ContextAblationOptions options = {});

}  // namespace runi
