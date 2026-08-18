#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "runi/model_action.hpp"

namespace runi {

class IActionParser {
public:
    virtual ~IActionParser() = default;
    [[nodiscard]] virtual ModelAction parse(std::string_view raw) const = 0;
};

class ModelActionParser final : public IActionParser {
public:
    [[nodiscard]] ModelAction parse(std::string_view raw) const override;

    [[nodiscard]] static std::string retry_notice(std::string_view problem = {});
    [[nodiscard]] static std::optional<ToolCall> parse_xml_tool(std::string_view raw);
    [[nodiscard]] static std::map<std::string, std::string, std::less<>> parse_attrs(std::string_view text);
    [[nodiscard]] static std::string extract(std::string_view text, std::string_view tag);
    [[nodiscard]] static std::string extract_raw(std::string_view text, std::string_view tag);
};

}  // namespace runi
