#include "runi/service/agent_service.hpp"

#include <stdexcept>
#include <utility>

#include "runi/state/run_store.hpp"
#include "runi/tool/workspace.hpp"

namespace runi {

RuniAgentServiceHandler::RuniAgentServiceHandler(
    std::shared_ptr<SqliteStateStore> state_store,
    std::filesystem::path artifact_root,
    ModelClientFactory model_factory,
    RuntimeOptions runtime_options)
    : state_store_(std::move(state_store)), artifact_root_(std::move(artifact_root)),
      model_factory_(std::move(model_factory)), runtime_options_(std::move(runtime_options)) {
    if (!state_store_) throw std::invalid_argument("agent service handler requires a state store");
    if (!model_factory_) throw std::invalid_argument("agent service handler requires a model factory");
}

Result<std::string> RuniAgentServiceHandler::operator()(
    const RunInvocation& invocation, std::stop_token stop_token) const {
    if (stop_token.stop_requested()) return Result<std::string>::failure(make_error(
        ErrorCategory::Timeout, "run_cancelled", "Agent service run was cancelled before construction"));
    const auto workspace = WorkspaceContext::build(
        invocation.session.workspace_root, std::filesystem::path(invocation.session.workspace_root));
    if (!workspace) return Result<std::string>::failure(workspace.error());
    const auto model = model_factory_();
    if (!model) return Result<std::string>::failure(model.error());
    auto sessions = std::make_shared<SqliteSessionStore>(state_store_);
    auto runs = std::make_shared<RunStore>(artifact_root_ / "runs");
    auto session = sessions->load(invocation.session.id);
    if (!session) return Result<std::string>::failure(session.error());
    Runi agent(model.value(), workspace.value(), sessions, runs, session.value(), runtime_options_);
    return agent.ask(invocation.run.request, stop_token);
}

}  // namespace runi
