#include "runi/core/json_codec.hpp"

#include <array>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace runi {
namespace {

class Parser {
public:
    explicit Parser(std::string_view text) : text_(text) {}

    JsonValue parse() {
        skip_space();
        auto value = parse_value();
        skip_space();
        if (position_ != text_.size()) {
            fail("unexpected trailing JSON content");
        }
        return value;
    }

private:
    [[noreturn]] void fail(std::string message) const {
        throw std::runtime_error(std::move(message) + " at byte " + std::to_string(position_));
    }

    void skip_space() {
        while (position_ < text_.size() &&
               (text_[position_] == ' ' || text_[position_] == '\t' ||
                text_[position_] == '\r' || text_[position_] == '\n')) {
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < text_.size() && text_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    JsonValue parse_value() {
        if (position_ >= text_.size()) {
            fail("unexpected end of JSON");
        }
        switch (text_[position_]) {
            case 'n': return parse_literal("null", JsonValue(nullptr));
            case 't': return parse_literal("true", JsonValue(true));
            case 'f': return parse_literal("false", JsonValue(false));
            case '"': return JsonValue(parse_string());
            case '[': return parse_array();
            case '{': return parse_object();
            default:
                if (text_[position_] == '-' || (text_[position_] >= '0' && text_[position_] <= '9')) {
                    return parse_number();
                }
                fail("unexpected JSON token");
        }
    }

    JsonValue parse_literal(std::string_view literal, JsonValue value) {
        if (text_.substr(position_, literal.size()) != literal) {
            fail("invalid JSON literal");
        }
        position_ += literal.size();
        return value;
    }

    static void append_utf8(std::string& output, std::uint32_t codepoint) {
        if (codepoint <= 0x7fU) {
            output.push_back(static_cast<char>(codepoint));
        } else if (codepoint <= 0x7ffU) {
            output.push_back(static_cast<char>(0xc0U | (codepoint >> 6U)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else if (codepoint <= 0xffffU) {
            output.push_back(static_cast<char>(0xe0U | (codepoint >> 12U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        } else {
            output.push_back(static_cast<char>(0xf0U | (codepoint >> 18U)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 12U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | ((codepoint >> 6U) & 0x3fU)));
            output.push_back(static_cast<char>(0x80U | (codepoint & 0x3fU)));
        }
    }

    std::uint32_t parse_hex4() {
        if (position_ + 4 > text_.size()) {
            fail("truncated JSON unicode escape");
        }
        std::uint32_t value = 0;
        for (int index = 0; index < 4; ++index) {
            const char character = text_[position_++];
            value <<= 4U;
            if (character >= '0' && character <= '9') value |= static_cast<std::uint32_t>(character - '0');
            else if (character >= 'a' && character <= 'f') value |= static_cast<std::uint32_t>(character - 'a' + 10);
            else if (character >= 'A' && character <= 'F') value |= static_cast<std::uint32_t>(character - 'A' + 10);
            else fail("invalid JSON unicode escape");
        }
        return value;
    }

    std::string parse_string() {
        if (!consume('"')) fail("expected JSON string");
        std::string output;
        while (position_ < text_.size()) {
            const char character = text_[position_++];
            if (character == '"') return output;
            if (static_cast<unsigned char>(character) < 0x20U) fail("control character in JSON string");
            if (character != '\\') {
                output.push_back(character);
                continue;
            }
            if (position_ >= text_.size()) fail("truncated JSON escape");
            const char escaped = text_[position_++];
            switch (escaped) {
                case '"': output.push_back('"'); break;
                case '\\': output.push_back('\\'); break;
                case '/': output.push_back('/'); break;
                case 'b': output.push_back('\b'); break;
                case 'f': output.push_back('\f'); break;
                case 'n': output.push_back('\n'); break;
                case 'r': output.push_back('\r'); break;
                case 't': output.push_back('\t'); break;
                case 'u': {
                    std::uint32_t codepoint = parse_hex4();
                    if (codepoint >= 0xd800U && codepoint <= 0xdbffU) {
                        if (position_ + 2 > text_.size() || text_[position_] != '\\' || text_[position_ + 1] != 'u') {
                            fail("missing low unicode surrogate");
                        }
                        position_ += 2;
                        const auto low = parse_hex4();
                        if (low < 0xdc00U || low > 0xdfffU) fail("invalid low unicode surrogate");
                        codepoint = 0x10000U + ((codepoint - 0xd800U) << 10U) + (low - 0xdc00U);
                    }
                    append_utf8(output, codepoint);
                    break;
                }
                default: fail("invalid JSON escape");
            }
        }
        fail("unterminated JSON string");
    }

    JsonValue parse_number() {
        const auto start = position_;
        consume('-');
        if (consume('0')) {
        } else {
            if (position_ >= text_.size() || text_[position_] < '1' || text_[position_] > '9') fail("invalid JSON number");
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        }
        bool integral = true;
        if (consume('.')) {
            integral = false;
            if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') fail("invalid JSON number");
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        }
        if (position_ < text_.size() && (text_[position_] == 'e' || text_[position_] == 'E')) {
            integral = false;
            ++position_;
            if (position_ < text_.size() && (text_[position_] == '+' || text_[position_] == '-')) ++position_;
            if (position_ >= text_.size() || text_[position_] < '0' || text_[position_] > '9') fail("invalid JSON exponent");
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9') ++position_;
        }
        const auto token = text_.substr(start, position_ - start);
        if (integral) {
            std::int64_t value = 0;
            const auto [end, error] = std::from_chars(token.data(), token.data() + token.size(), value);
            if (error == std::errc{} && end == token.data() + token.size()) return JsonValue(value);
        }
        std::string copy(token);
        char* end = nullptr;
        const double value = std::strtod(copy.c_str(), &end);
        if (end != copy.c_str() + copy.size() || !std::isfinite(value)) fail("invalid JSON number");
        return JsonValue(value);
    }

    JsonValue parse_array() {
        consume('[');
        skip_space();
        JsonValue::Array result;
        if (consume(']')) return JsonValue(std::move(result));
        while (true) {
            skip_space();
            result.push_back(parse_value());
            skip_space();
            if (consume(']')) return JsonValue(std::move(result));
            if (!consume(',')) fail("expected comma in JSON array");
        }
    }

    JsonValue parse_object() {
        consume('{');
        skip_space();
        JsonValue::Object result;
        if (consume('}')) return JsonValue(std::move(result));
        while (true) {
            skip_space();
            if (position_ >= text_.size() || text_[position_] != '"') fail("expected JSON object key");
            auto key = parse_string();
            skip_space();
            if (!consume(':')) fail("expected colon after JSON object key");
            skip_space();
            result.insert_or_assign(std::move(key), parse_value());
            skip_space();
            if (consume('}')) return JsonValue(std::move(result));
            if (!consume(',')) fail("expected comma in JSON object");
        }
    }

    std::string_view text_;
    std::size_t position_{0};
};

void append_indent(std::string& output, int indent, int depth) {
    output.append(static_cast<std::size_t>(indent * depth), ' ');
}

void append_escaped(std::string& output, std::string_view value, bool ensure_ascii) {
    static constexpr char hex[] = "0123456789abcdef";
    const auto append_u16 = [&](std::uint32_t codepoint) {
        output += "\\u";
        output.push_back(hex[(codepoint >> 12U) & 0x0fU]);
        output.push_back(hex[(codepoint >> 8U) & 0x0fU]);
        output.push_back(hex[(codepoint >> 4U) & 0x0fU]);
        output.push_back(hex[codepoint & 0x0fU]);
    };
    output.push_back('"');
    for (std::size_t index = 0; index < value.size();) {
        const auto character = static_cast<unsigned char>(value[index]);
        switch (character) {
            case '"': output += "\\\""; ++index; break;
            case '\\': output += "\\\\"; ++index; break;
            case '\b': output += "\\b"; ++index; break;
            case '\f': output += "\\f"; ++index; break;
            case '\n': output += "\\n"; ++index; break;
            case '\r': output += "\\r"; ++index; break;
            case '\t': output += "\\t"; ++index; break;
            default:
                if (character < 0x20U) { output += "\\u00"; output.push_back(hex[character >> 4U]); output.push_back(hex[character & 0x0fU]); ++index; }
                else if (!ensure_ascii || character < 0x80U) { output.push_back(static_cast<char>(character)); ++index; }
                else {
                    std::uint32_t codepoint = 0xfffdU;
                    std::size_t length = 1;
                    if ((character & 0xe0U) == 0xc0U && index + 1 < value.size()) {
                        codepoint = static_cast<std::uint32_t>(character & 0x1fU); length = 2;
                    } else if ((character & 0xf0U) == 0xe0U && index + 2 < value.size()) {
                        codepoint = static_cast<std::uint32_t>(character & 0x0fU); length = 3;
                    } else if ((character & 0xf8U) == 0xf0U && index + 3 < value.size()) {
                        codepoint = static_cast<std::uint32_t>(character & 0x07U); length = 4;
                    }
                    bool valid = length > 1;
                    for (std::size_t offset = 1; valid && offset < length; ++offset) {
                        const auto continuation = static_cast<unsigned char>(value[index + offset]);
                        if ((continuation & 0xc0U) != 0x80U) valid = false;
                        else codepoint = (codepoint << 6U) | static_cast<std::uint32_t>(continuation & 0x3fU);
                    }
                    if (!valid) { append_u16(0xfffdU); ++index; break; }
                    index += length;
                    if (codepoint <= 0xffffU) append_u16(codepoint);
                    else {
                        codepoint -= 0x10000U;
                        append_u16(0xd800U + (codepoint >> 10U));
                        append_u16(0xdc00U + (codepoint & 0x3ffU));
                    }
                }
        }
    }
    output.push_back('"');
}

void append_json(std::string& output, const JsonValue& value, int indent, int depth, bool ensure_ascii, bool spaced) {
    if (value.is_null()) { output += "null"; return; }
    if (value.is_bool()) { output += value.as_bool() ? "true" : "false"; return; }
    if (value.is_integer()) { output += std::to_string(value.as_integer()); return; }
    if (std::holds_alternative<double>(value.storage())) {
        std::array<char, 64> buffer{};
        const auto [end, error] = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value.as_number(), std::chars_format::general);
        if (error == std::errc{}) {
            const auto start = output.size();
            output.append(buffer.data(), end);
            const auto number = std::string_view(output).substr(start);
            if (number.find_first_of(".eE") == std::string_view::npos) output += ".0";
        }
        else output += "0.0";
        return;
    }
    if (value.is_string()) { append_escaped(output, value.as_string(), ensure_ascii); return; }
    const bool pretty = indent >= 0;
    if (value.is_array()) {
        output.push_back('[');
        const auto& array = value.as_array();
        for (std::size_t index = 0; index < array.size(); ++index) {
            if (index != 0) output.push_back(',');
            if (pretty) { output.push_back('\n'); append_indent(output, indent, depth + 1); }
            append_json(output, array[index], indent, depth + 1, ensure_ascii, spaced);
        }
        if (pretty && !array.empty()) { output.push_back('\n'); append_indent(output, indent, depth); }
        output.push_back(']');
        return;
    }
    output.push_back('{');
    const auto& object = value.as_object();
    std::size_t index = 0;
    for (const auto& [key, item] : object) {
        if (index++ != 0) output.push_back(',');
        if (pretty) { output.push_back('\n'); append_indent(output, indent, depth + 1); }
        append_escaped(output, key, ensure_ascii);
        output += (pretty || spaced) ? ": " : ":";
        append_json(output, item, indent, depth + 1, ensure_ascii, spaced);
    }
    if (pretty && !object.empty()) { output.push_back('\n'); append_indent(output, indent, depth); }
    output.push_back('}');
}

}  // namespace

Result<JsonValue> parse_json(std::string_view text) {
    try {
        return Result<JsonValue>::success(Parser(text).parse());
    } catch (const std::exception& error) {
        return Result<JsonValue>::failure(make_error(
            ErrorCategory::Parse, "invalid_json", error.what(), false));
    }
}

std::string dump_json(const JsonValue& value, int indent, bool ensure_ascii) {
    std::string output;
    append_json(output, value, indent, 0, ensure_ascii, false);
    return output;
}

std::string dump_compatible_json(const JsonValue& value, bool ensure_ascii) {
    std::string output;
    append_json(output, value, -1, 0, ensure_ascii, true);
    std::string spaced;
    spaced.reserve(output.size() + output.size() / 10);
    bool in_string = false;
    bool escaped = false;
    for (const char character : output) {
        spaced.push_back(character);
        if (in_string) {
            if (escaped) escaped = false;
            else if (character == '\\') escaped = true;
            else if (character == '"') in_string = false;
        } else if (character == '"') in_string = true;
        else if (character == ',') spaced.push_back(' ');
    }
    return spaced;
}

}  // namespace runi
