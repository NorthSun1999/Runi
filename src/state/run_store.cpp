#include "runi/state/run_store.hpp"

#include <fstream>
#include <iterator>
#include <system_error>

#include "runi/core/json_codec.hpp"
#include "runi/core/time.hpp"

namespace runi {
namespace {

Result<JsonValue> load_json_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<JsonValue>::failure(make_error(
        ErrorCategory::Persistence, "artifact_load_failed", "Could not read artifact: " + path.string()));
    const std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    return parse_json(text);
}

}  // namespace

RunStore::RunStore(std::filesystem::path root) : root_(std::move(root)) {
    std::filesystem::create_directories(root_);
}

std::filesystem::path RunStore::run_dir(std::string_view run_id) const { return root_ / std::string(run_id); }
std::filesystem::path RunStore::task_state_path(std::string_view run_id) const { return run_dir(run_id) / "task_state.json"; }
std::filesystem::path RunStore::trace_path(std::string_view run_id) const { return run_dir(run_id) / "trace.jsonl"; }
std::filesystem::path RunStore::report_path(std::string_view run_id) const { return run_dir(run_id) / "report.json"; }

Result<std::filesystem::path> RunStore::start_run(const TaskState& task_state) {
    std::error_code error;
    const auto directory = run_dir(task_state.run_id);
    std::filesystem::create_directories(directory, error);
    if (error) return Result<std::filesystem::path>::failure(make_error(
        ErrorCategory::Persistence, "run_start_failed", "Could not create run directory: " + directory.string()));
    const auto state = write_task_state(task_state);
    if (!state) return Result<std::filesystem::path>::failure(state.error());
    return Result<std::filesystem::path>::success(directory);
}

Result<std::filesystem::path> RunStore::write_task_state(const TaskState& task_state) {
    const auto target = task_state_path(task_state.run_id);
    const auto result = write_json_atomic(target, task_state.to_json());
    return result ? Result<std::filesystem::path>::success(target)
                  : Result<std::filesystem::path>::failure(result.error());
}

Result<std::filesystem::path> RunStore::append_trace(const TaskState& task_state, const JsonValue& event) {
    const auto target = trace_path(task_state.run_id);
    std::filesystem::create_directories(target.parent_path());
    std::ofstream output(target, std::ios::binary | std::ios::app);
    if (!output) return Result<std::filesystem::path>::failure(make_error(
        ErrorCategory::Persistence, "trace_append_failed", "Could not append trace: " + target.string()));
    output << dump_json(event, -1, true) << '\n';
    return Result<std::filesystem::path>::success(target);
}

Result<std::filesystem::path> RunStore::write_report(const TaskState& task_state, const JsonValue& report) {
    const auto target = report_path(task_state.run_id);
    const auto result = write_json_atomic(target, report);
    return result ? Result<std::filesystem::path>::success(target)
                  : Result<std::filesystem::path>::failure(result.error());
}

Result<JsonValue> RunStore::load_task_state(std::string_view run_id) const { return load_json_file(task_state_path(run_id)); }
Result<JsonValue> RunStore::load_report(std::string_view run_id) const { return load_json_file(report_path(run_id)); }

Result<void> RunStore::write_json_atomic(const std::filesystem::path& path, const JsonValue& payload) const {
    std::filesystem::create_directories(path.parent_path());
    const auto temporary = path.parent_path() / (path.filename().string() + "." + random_hex(8) + ".tmp");
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) return Result<void>::failure(make_error(
            ErrorCategory::Persistence, "atomic_write_failed", "Could not write temporary artifact: " + temporary.string()));
        output << dump_json(payload, 2, true) << '\n';
        if (!output) return Result<void>::failure(make_error(
            ErrorCategory::Persistence, "atomic_write_failed", "Could not write temporary artifact: " + temporary.string()));
    }
    std::error_code error;
#ifdef _WIN32
    std::filesystem::remove(path, error);
    error.clear();
#endif
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary);
        return Result<void>::failure(make_error(
            ErrorCategory::Persistence, "atomic_replace_failed", "Could not replace artifact: " + path.string()));
    }
    return Result<void>::success();
}

}  // namespace runi
