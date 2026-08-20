#pragma once

// Convenience header for applications embedding the Runi runtime.
// Evaluation is intentionally separate because it is a developer-only surface.
#include "runi/agent/runtime.hpp"
#include "runi/model/config.hpp"
#include "runi/model/providers.hpp"
#include "runi/orchestration/execution.hpp"
#include "runi/orchestration/multi_agent.hpp"
#include "runi/service/agent_service.hpp"
#include "runi/service/service.hpp"
#include "runi/state/context_cache.hpp"
#include "runi/state/sqlite_session_store.hpp"
#include "runi/state/state_store.hpp"
