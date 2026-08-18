#include "runi/core/text.hpp"

#include <algorithm>
#include <cctype>

namespace runi {

std::string trim(std::string_view text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of(" \t\r\n");
    return std::string(text.substr(first, last - first + 1));
}

std::string trim_newlines(std::string_view text) {
    const auto first = text.find_first_not_of('\n');
    if (first == std::string_view::npos) return {};
    const auto last = text.find_last_not_of('\n');
    return std::string(text.substr(first, last - first + 1));
}

std::string clip(std::string_view text, std::size_t limit) {
    const auto length = utf8_length(text);
    if (length <= limit) return std::string(text);
    return utf8_prefix(text, limit) + "\n...[truncated " +
        std::to_string(length - limit) + " chars]";
}

std::string middle(std::string_view text, std::size_t limit) {
    std::string one_line(text);
    std::replace(one_line.begin(), one_line.end(), '\n', ' ');
    std::replace(one_line.begin(), one_line.end(), '\r', ' ');
    if (utf8_length(one_line) <= limit) return one_line;
    if (limit <= 3) return utf8_prefix(one_line, limit);
    const auto left = (limit - 3) / 2;
    const auto right = limit - 3 - left;
    return utf8_prefix(one_line, left) + "..." + utf8_suffix(one_line, right);
}

std::vector<std::string> split_lines(std::string_view text) {
    std::vector<std::string> lines;
    if (text.empty()) return lines;
    std::size_t start = 0;
    while (start < text.size()) {
        const auto end = text.find_first_of("\r\n", start);
        if (end == std::string_view::npos) {
            lines.emplace_back(text.substr(start));
            break;
        }
        lines.emplace_back(text.substr(start, end - start));
        start = end + 1;
        if (text[end] == '\r' && start < text.size() && text[start] == '\n') ++start;
    }
    return lines;
}

std::string join(const std::vector<std::string>& items, std::string_view separator) {
    std::string result;
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0) result.append(separator);
        result.append(items[index]);
    }
    return result;
}

std::string lower_ascii(std::string_view text) {
    std::string result(text);
    std::transform(result.begin(), result.end(), result.begin(), [](unsigned char character) {
        return static_cast<char>(std::tolower(character));
    });
    return result;
}

std::size_t utf8_length(std::string_view text) noexcept {
    std::size_t count = 0;
    for (const unsigned char character : text) if ((character & 0xc0U) != 0x80U) ++count;
    return count;
}

std::string utf8_prefix(std::string_view text, std::size_t characters) {
    std::size_t count = 0;
    std::size_t bytes = 0;
    while (bytes < text.size() && count < characters) {
        ++bytes;
        while (bytes < text.size() && (static_cast<unsigned char>(text[bytes]) & 0xc0U) == 0x80U) ++bytes;
        ++count;
    }
    return std::string(text.substr(0, bytes));
}

std::string utf8_suffix(std::string_view text, std::size_t characters) {
    std::size_t position = text.size();
    std::size_t count = 0;
    while (position > 0 && count < characters) {
        --position;
        while (position > 0 && (static_cast<unsigned char>(text[position]) & 0xc0U) == 0x80U) --position;
        ++count;
    }
    return std::string(text.substr(position));
}

}  // namespace runi
