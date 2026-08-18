#pragma once

#include <filesystem>
#include <string_view>

#include "runi/core/result.hpp"
#include "runi/task_state.hpp"

namespace runi {

class IRunStore {
public:
    virtual ~IRunStore() = default;
    [[nodiscard]] virtual std::filesystem::path run_dir(std::string_view run_id) const = 0;
    virtual Result<std::filesystem::path> start_run(const TaskState& task_state) = 0;
    virtual Result<std::filesystem::path> write_task_state(const TaskState& task_state) = 0;
    virtual Result<std::filesystem::path> append_trace(const TaskState& task_state, const JsonValue& event) = 0;
    virtual Result<std::filesystem::path> write_report(const TaskState& task_state, const JsonValue& report) = 0;
};

class RunStore final : public IRunStore {
public:
    explicit RunStore(std::filesystem::path root);

    [[nodiscard]] std::filesystem::path run_dir(std::string_view run_id) const override;
    [[nodiscard]] std::filesystem::path task_state_path(std::string_view run_id) const;
    [[nodiscard]] std::filesystem::path trace_path(std::string_view run_id) const;
    [[nodiscard]] std::filesystem::path report_path(std::string_view run_id) const;
    Result<std::filesystem::path> start_run(const TaskState& task_state) override;
    Result<std::filesystem::path> write_task_state(const TaskState& task_state) override;
    Result<std::filesystem::path> append_trace(const TaskState& task_state, const JsonValue& event) override;
    Result<std::filesystem::path> write_report(const TaskState& task_state, const JsonValue& report) override;
    [[nodiscard]] Result<JsonValue> load_task_state(std::string_view run_id) const;
    [[nodiscard]] Result<JsonValue> load_report(std::string_view run_id) const;

private:
    Result<void> write_json_atomic(const std::filesystem::path& path, const JsonValue& payload) const;
    std::filesystem::path root_;
};

}  // namespace runi
