#pragma once

#include <string>
#include <string_view>

#include "runi/core/result.hpp"

namespace runi {

class Runi;

class AgentLoop {
public:
    explicit AgentLoop(Runi& agent);
    [[nodiscard]] Result<std::string> run(std::string_view user_message);
private:
    Runi& agent_;
};

}  // namespace runi
