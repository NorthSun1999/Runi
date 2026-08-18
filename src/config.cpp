#include "runi/config.hpp"

#include <cstdlib>
#include <cwchar>
#include <fstream>
#include <regex>

#include "runi/core/text.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace runi {
namespace {

Result<std::optional<std::pair<std::string, std::string>>> parse_env_line(std::string_view raw) {
    auto line = trim(raw);
    if (line.empty() || line.starts_with('#')) {
        return Result<std::optional<std::pair<std::string, std::string>>>::success(std::nullopt);
    }
    if (line.starts_with("export ")) line = trim(std::string_view(line).substr(7));
    const auto delimiter = line.find('=');
    if (delimiter == std::string::npos) return Result<std::optional<std::pair<std::string, std::string>>>::failure(
        make_error(ErrorCategory::Configuration, "invalid_env_line", "invalid .env line: " + line));
    auto name = trim(std::string_view(line).substr(0, delimiter));
    auto value = trim(std::string_view(line).substr(delimiter + 1));
    static const std::regex name_pattern("^[A-Za-z_][A-Za-z0-9_]*$");
    if (!std::regex_match(name, name_pattern)) return Result<std::optional<std::pair<std::string, std::string>>>::failure(
        make_error(ErrorCategory::Configuration, "invalid_env_name", "invalid .env variable name: " + name));
    if (value.size() >= 2 && value.front() == value.back() && (value.front() == '\'' || value.front() == '"')) {
        value = value.substr(1, value.size() - 2);
    }
    return Result<std::optional<std::pair<std::string, std::string>>>::success(
        std::make_pair(std::move(name), std::move(value)));
}

}  // namespace

std::optional<std::filesystem::path> find_project_env(const std::filesystem::path& start) {
    std::error_code error;
    auto current = std::filesystem::absolute(start, error);
    if (error) return std::nullopt;
    current = std::filesystem::weakly_canonical(current, error);
    if (error) return std::nullopt;
    if (std::filesystem::is_regular_file(current, error)) current = current.parent_path();
    while (!current.empty()) {
        const auto candidate = current / ".env";
        if (std::filesystem::exists(candidate, error)) return candidate;
        const auto parent = current.parent_path();
        if (parent == current) break;
        current = parent;
    }
    return std::nullopt;
}

Result<std::map<std::string, std::string, std::less<>>> load_project_env(
    const std::filesystem::path& start,
    bool override_existing) {
    const auto path = find_project_env(start);
    std::map<std::string, std::string, std::less<>> loaded;
    if (!path.has_value()) return Result<decltype(loaded)>::success(std::move(loaded));
    std::ifstream input(*path, std::ios::binary);
    if (!input) return Result<decltype(loaded)>::failure(make_error(
        ErrorCategory::Configuration, "env_read_failed", "Could not read project .env: " + path->string()));
    std::string line;
    while (std::getline(input, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto parsed = parse_env_line(line);
        if (!parsed) return Result<decltype(loaded)>::failure(parsed.error());
        if (!parsed.value().has_value()) continue;
        auto [name, value] = *parsed.value();
        loaded[name] = value;
        set_environment_value(name, value, override_existing);
    }
    return Result<decltype(loaded)>::success(std::move(loaded));
}

std::string provider_env(std::string_view name, const std::vector<std::string>& legacy_names, std::string_view fallback) {
    std::vector<std::string> names{std::string(name)};
    names.insert(names.end(), legacy_names.begin(), legacy_names.end());
    for (const auto& item : names) {
        if (const auto* value = std::getenv(item.c_str()); value != nullptr && *value != '\0') return value;
    }
    return std::string(fallback);
}

std::map<std::string, std::string, std::less<>> environment_items() {
    std::map<std::string, std::string, std::less<>> result;
#ifdef _WIN32
    LPWCH block = GetEnvironmentStringsW();
    if (block == nullptr) return result;
    for (const wchar_t* cursor = block; *cursor != L'\0'; cursor += std::wcslen(cursor) + 1) {
        const std::wstring item(cursor);
        if (item.empty() || item.front() == L'=') continue;
        const auto delimiter = item.find(L'=');
        if (delimiter == std::wstring::npos) continue;
        const int name_size = WideCharToMultiByte(CP_UTF8, 0, item.data(), static_cast<int>(delimiter), nullptr, 0, nullptr, nullptr);
        const int value_size = WideCharToMultiByte(CP_UTF8, 0, item.data() + delimiter + 1,
            static_cast<int>(item.size() - delimiter - 1), nullptr, 0, nullptr, nullptr);
        std::string name(static_cast<std::size_t>(name_size), '\0');
        std::string value(static_cast<std::size_t>(value_size), '\0');
        WideCharToMultiByte(CP_UTF8, 0, item.data(), static_cast<int>(delimiter), name.data(), name_size, nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, item.data() + delimiter + 1, static_cast<int>(item.size() - delimiter - 1), value.data(), value_size, nullptr, nullptr);
        result.emplace(std::move(name), std::move(value));
    }
    FreeEnvironmentStringsW(block);
#else
    extern char** environ;
    for (char** cursor = environ; cursor != nullptr && *cursor != nullptr; ++cursor) {
        std::string item(*cursor);
        const auto delimiter = item.find('=');
        if (delimiter != std::string::npos) result.emplace(item.substr(0, delimiter), item.substr(delimiter + 1));
    }
#endif
    return result;
}

bool set_environment_value(std::string_view name, std::string_view value, bool override_existing) {
    if (!override_existing && std::getenv(std::string(name).c_str()) != nullptr) return true;
#ifdef _WIN32
    return _putenv_s(std::string(name).c_str(), std::string(value).c_str()) == 0;
#else
    return setenv(std::string(name).c_str(), std::string(value).c_str(), override_existing ? 1 : 0) == 0;
#endif
}

}  // namespace runi
