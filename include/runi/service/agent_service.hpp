#pragma once

#include <filesystem>
#include <functional>
#include <memory>

#include "runi/model/model_client.hpp"
#include "runi/agent/runtime.hpp"
#include "runi/service/service.hpp"
#include "runi/state/sqlite_session_store.hpp"

namespace runi {

using ModelClientFactory = std::function<Result<std::shared_ptr<IModelClient>>() >;

class RuniAgentServiceHandler {
public:
    RuniAgentServiceHandler(
        std::shared_ptr<SqliteStateStore> state_store,
        std::filesystem::path artifact_root,
        ModelClientFactory model_factory,
        RuntimeOptions runtime_options = {});

    [[nodiscard]] Result<std::string> operator()(
        const RunInvocation& invocation, std::stop_token stop_token) const;

private:
    std::shared_ptr<SqliteStateStore> state_store_;
    std::filesystem::path artifact_root_;
    ModelClientFactory model_factory_;
    RuntimeOptions runtime_options_;
};

}  // namespace runi
