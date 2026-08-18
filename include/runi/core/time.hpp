#pragma once

#include <string>

namespace runi {

[[nodiscard]] std::string now_utc();
[[nodiscard]] std::string local_timestamp();
[[nodiscard]] std::string random_hex(std::size_t length);
[[nodiscard]] std::string new_session_id();
[[nodiscard]] std::string new_task_id();
[[nodiscard]] std::string new_run_id();
[[nodiscard]] std::string new_checkpoint_id();

}  // namespace runi
