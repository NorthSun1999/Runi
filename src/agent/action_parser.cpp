#include "runi/agent/action_parser.hpp"

#include <optional>
#include <regex>
#include <string>

#include "runi/core/json_codec.hpp"
#include "runi/core/text.hpp"

namespace runi {
namespace {

bool action_precedes_final(std::string_view raw, std::string_view marker) {
    const auto action = raw.find(marker);
    if (action == std::string_view::npos) return false;
    const auto final = raw.find("<final>");
    return final == std::string_view::npos || action < final;
}

}  // namespace

ModelAction ModelActionParser::parse(std::string_view raw) const {
    if (action_precedes_final(raw, "<tool>")) {
        const auto body = extract(raw, "tool");
        const auto decoded = parse_json(body);
        if (!decoded || !decoded.value().is_object()) {
            const auto problem = !decoded
                ? "model returned malformed tool JSON"
                : "tool payload must be a JSON object";
            return RetryRequest{retry_notice(problem)};
        }
        const auto& object = decoded.value().as_object();
        const auto name_iterator = object.find("name");
        const auto name = name_iterator == object.end() ? std::string{} : trim(name_iterator->second.string_or());
        if (name.empty()) return RetryRequest{retry_notice("tool payload is missing a tool name")};

        JsonValue::Object args;
        const auto args_iterator = object.find("args");
        if (args_iterator != object.end() && !args_iterator->second.is_null()) {
            if (!args_iterator->second.is_object()) return RetryRequest{retry_notice()};
            args = args_iterator->second.as_object();
        }
        return ToolCall{name, std::move(args)};
    }

    if (action_precedes_final(raw, "<tool")) {
        const auto tool = parse_xml_tool(raw);
        if (tool.has_value()) return *tool;
        return RetryRequest{retry_notice()};
    }

    if (raw.find("<final>") != std::string_view::npos) {
        const auto final = trim(extract(raw, "final"));
        if (!final.empty()) return FinalAnswer{final};
        return RetryRequest{retry_notice("model returned an empty <final> answer")};
    }

    const auto plain = trim(raw);
    if (!plain.empty()) return FinalAnswer{plain};
    return RetryRequest{retry_notice("model returned an empty response")};
}

std::string ModelActionParser::retry_notice(std::string_view problem) {
    std::string prefix = "Runtime notice: ";
    prefix += problem.empty() ? "model returned malformed tool output" : std::string(problem);
    return prefix +
        ". Reply with a valid <tool> call or a non-empty <final> answer. "
        "For multi-line files, prefer <tool name=\"write_file\" path=\"file.py\"><content>...</content></tool>.";
}

std::optional<ToolCall> ModelActionParser::parse_xml_tool(std::string_view raw) {
    static const std::regex expression(R"(<tool([^>]*)>([\s\S]*?)</tool>)");
    std::smatch match;
    const std::string input(raw);
    if (!std::regex_search(input, match, expression)) return std::nullopt;
    auto attrs = parse_attrs(match[1].str());
    const auto name_iterator = attrs.find("name");
    if (name_iterator == attrs.end()) return std::nullopt;
    const auto name = trim(name_iterator->second);
    if (name.empty()) return std::nullopt;
    attrs.erase(name_iterator);

    const auto body = match[2].str();
    JsonValue::Object args;
    for (const auto& [key, value] : attrs) args.emplace(key, JsonValue(value));
    for (const std::string key : {"content", "old_text", "new_text", "command", "task", "pattern", "path"}) {
        if (body.find("<" + key + ">") != std::string::npos) {
            args.insert_or_assign(key, JsonValue(extract_raw(body, key)));
        }
    }
    const auto body_text = trim_newlines(body);
    if (name == "write_file" && !args.contains("content") && !body_text.empty()) {
        args.emplace("content", JsonValue(body_text));
    }
    if (name == "delegate" && !args.contains("task") && !body_text.empty()) {
        args.emplace("task", JsonValue(trim(body_text)));
    }
    return ToolCall{name, std::move(args)};
}

std::map<std::string, std::string, std::less<>> ModelActionParser::parse_attrs(std::string_view text) {
    static const std::regex expression(R"re(([A-Za-z_][A-Za-z0-9_]*)\s*=\s*(?:"([^"]*)"|'([^']*)'))re");
    const std::string input(text);
    std::map<std::string, std::string, std::less<>> attrs;
    for (auto iterator = std::sregex_iterator(input.begin(), input.end(), expression);
         iterator != std::sregex_iterator(); ++iterator) {
        const auto& match = *iterator;
        attrs[match[1].str()] = match[2].matched ? match[2].str() : match[3].str();
    }
    return attrs;
}

std::string ModelActionParser::extract(std::string_view text, std::string_view tag) {
    const std::string start_tag = "<" + std::string(tag) + ">";
    const std::string end_tag = "</" + std::string(tag) + ">";
    const auto start_position = text.find(start_tag);
    if (start_position == std::string_view::npos) return std::string(text);
    const auto content_start = start_position + start_tag.size();
    const auto end_position = text.find(end_tag, content_start);
    if (end_position == std::string_view::npos) return trim(text.substr(content_start));
    return trim(text.substr(content_start, end_position - content_start));
}

std::string ModelActionParser::extract_raw(std::string_view text, std::string_view tag) {
    const std::string start_tag = "<" + std::string(tag) + ">";
    const std::string end_tag = "</" + std::string(tag) + ">";
    const auto start_position = text.find(start_tag);
    if (start_position == std::string_view::npos) return std::string(text);
    const auto content_start = start_position + start_tag.size();
    const auto end_position = text.find(end_tag, content_start);
    if (end_position == std::string_view::npos) return std::string(text.substr(content_start));
    return std::string(text.substr(content_start, end_position - content_start));
}

}  // namespace runi
