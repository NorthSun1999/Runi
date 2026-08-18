#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "runi/memory.hpp"
#include "runi/core/result.hpp"
#include "runi/session.hpp"
#include "runi/task_state.hpp"

namespace runi {

inline constexpr std::string_view kCheckpointSchemaVersion = "phase1-v1";
inline constexpr std::string_view kCheckpointNoneStatus = "no-checkpoint";
inline constexpr std::string_view kCheckpointFullValidStatus = "full-valid";
inline constexpr std::string_view kCheckpointPartialStaleStatus = "partial-stale";
inline constexpr std::string_view kCheckpointWorkspaceMismatchStatus = "workspace-mismatch";
inline constexpr std::string_view kCheckpointSchemaMismatchStatus = "schema-mismatch";

class ICheckpointHost {
public:
    virtual ~ICheckpointHost() = default;
    [[nodiscard]] virtual SessionState& mutable_session() = 0;
    [[nodiscard]] virtual const SessionState& checkpoint_session() const = 0;
    [[nodiscard]] virtual LayeredMemory& checkpoint_memory() = 0;
    [[nodiscard]] virtual std::filesystem::path checkpoint_root() const = 0;
    [[nodiscard]] virtual JsonValue runtime_identity() const = 0;
    virtual Result<void> persist_session() = 0;
};

[[nodiscard]] JsonValue* current_checkpoint(SessionState& session);
[[nodiscard]] const JsonValue* current_checkpoint(const SessionState& session);
[[nodiscard]] JsonValue evaluate_resume_state(ICheckpointHost& host);
[[nodiscard]] std::string render_checkpoint_text(const ICheckpointHost& host, const JsonValue& resume_state);
[[nodiscard]] std::string infer_next_step(const TaskState& task_state);
[[nodiscard]] JsonValue create_checkpoint(
    ICheckpointHost& host,
    TaskState& task_state,
    std::string_view user_message,
    std::string_view trigger);

}  // namespace runi
