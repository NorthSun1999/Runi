#include "runi/tool/workspace.hpp"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <sstream>

#include "runi/core/json_codec.hpp"
#include "runi/core/sha256.hpp"
#include "runi/core/text.hpp"

namespace runi {
namespace {

std::string git_value(
    const IProcessRunner& runner,
    const std::filesystem::path& cwd,
    std::string_view arguments,
    std::string fallback = {}) {
    const auto result = runner.run(ProcessRequest{
        "git " + std::string(arguments), cwd, {}, std::chrono::seconds(5), true});
    if (!result || result.value().exit_code != 0) return fallback;
    const auto value = trim(result.value().stdout_text);
    return value.empty() ? fallback : value;
}

std::string generic_relative(const std::filesystem::path& path, const std::filesystem::path& root) {
    std::error_code error;
    const auto relative = std::filesystem::relative(path, root, error);
    return error ? path.filename().string() : relative.generic_string();
}

Result<std::string> decode_utf8(std::string input, bool replace_errors) {
    std::string output;
    output.reserve(input.size());
    for (std::size_t index = 0; index < input.size();) {
        const auto lead = static_cast<unsigned char>(input[index]);
        if (lead < 0x80U) { output.push_back(input[index++]); continue; }
        std::size_t length = 0;
        std::uint32_t codepoint = 0;
        std::uint32_t minimum = 0;
        if ((lead & 0xe0U) == 0xc0U) { length = 2; codepoint = lead & 0x1fU; minimum = 0x80U; }
        else if ((lead & 0xf0U) == 0xe0U) { length = 3; codepoint = lead & 0x0fU; minimum = 0x800U; }
        else if ((lead & 0xf8U) == 0xf0U) { length = 4; codepoint = lead & 0x07U; minimum = 0x10000U; }
        bool valid = length != 0 && index + length <= input.size();
        for (std::size_t offset = 1; valid && offset < length; ++offset) {
            const auto value = static_cast<unsigned char>(input[index + offset]);
            if ((value & 0xc0U) != 0x80U) valid = false;
            else codepoint = (codepoint << 6U) | (value & 0x3fU);
        }
        valid = valid && codepoint >= minimum && codepoint <= 0x10ffffU && !(codepoint >= 0xd800U && codepoint <= 0xdfffU);
        if (valid) { output.append(input, index, length); index += length; continue; }
        if (!replace_errors) return Result<std::string>::failure(make_error(
            ErrorCategory::ToolExecution, "invalid_utf8", "file is not valid UTF-8"));
        output += "\xef\xbf\xbd";
        ++index;
    }
    return Result<std::string>::success(std::move(output));
}

}  // namespace

Result<WorkspaceContext> WorkspaceContext::build(
    const std::filesystem::path& requested_cwd,
    std::optional<std::filesystem::path> repo_root_override,
    const IProcessRunner* process_runner) {
    std::error_code error;
    auto absolute_cwd = std::filesystem::weakly_canonical(std::filesystem::absolute(requested_cwd, error), error);
    if (error || !std::filesystem::is_directory(absolute_cwd, error)) return Result<WorkspaceContext>::failure(make_error(
        ErrorCategory::Configuration, "invalid_workspace", "Workspace directory does not exist: " + requested_cwd.string()));
    ProcessRunner default_runner;
    const auto& runner = process_runner == nullptr ? static_cast<const IProcessRunner&>(default_runner) : *process_runner;
    auto root = repo_root_override.has_value()
        ? std::filesystem::weakly_canonical(std::filesystem::absolute(*repo_root_override, error), error)
        : std::filesystem::path(git_value(runner, absolute_cwd, "rev-parse --show-toplevel", absolute_cwd.string()));
    root = std::filesystem::weakly_canonical(root, error);
    if (error) root = absolute_cwd;

    WorkspaceContext result;
    result.cwd = absolute_cwd.string();
    result.repo_root = root.string();
    result.branch = git_value(runner, absolute_cwd, "branch --show-current", "-");
    if (result.branch.empty()) result.branch = "-";
    result.default_branch = git_value(runner, absolute_cwd, "symbolic-ref --short refs/remotes/origin/HEAD", "origin/main");
    if (result.default_branch.starts_with("origin/")) result.default_branch.erase(0, 7);
    result.status = clip(git_value(runner, absolute_cwd, "status --short", "clean"), 1500);
    if (result.status.empty()) result.status = "clean";
    for (const auto& line : split_lines(git_value(runner, absolute_cwd, "log --oneline -5"))) {
        if (!line.empty()) result.recent_commits.push_back(line);
    }

    for (const auto& base : {root, absolute_cwd}) {
        for (const auto& name : kProjectDocNames) {
            const auto path = base / name;
            if (!std::filesystem::exists(path, error)) continue;
            const auto key = generic_relative(path, root);
            if (result.project_docs.contains(key)) continue;
            const auto content = read_text_file(path, true);
            if (content) {
                result.project_docs.emplace(key, clip(content.value(), 1200));
                result.project_doc_order.push_back(key);
            }
        }
    }
    return Result<WorkspaceContext>::success(std::move(result));
}

std::string WorkspaceContext::text() const {
    std::vector<std::string> commit_lines;
    for (const auto& commit : recent_commits) commit_lines.push_back("- " + commit);
    std::vector<std::string> doc_lines;
    if (!project_doc_order.empty()) {
        for (const auto& path : project_doc_order) {
            const auto iterator = project_docs.find(path);
            if (iterator != project_docs.end()) doc_lines.push_back("- " + iterator->first + "\n" + iterator->second);
        }
    } else {
        for (const auto& [path, snippet] : project_docs) doc_lines.push_back("- " + path + "\n" + snippet);
    }
    return
        "Workspace:\n"
        "- cwd: " + cwd + "\n"
        "- repo_root: " + repo_root + "\n"
        "- branch: " + branch + "\n"
        "- default_branch: " + default_branch + "\n"
        "- status:\n" + status + "\n"
        "- recent_commits:\n" + (commit_lines.empty() ? "- none" : join(commit_lines, "\n")) + "\n"
        "- project_docs:\n" + (doc_lines.empty() ? "- none" : join(doc_lines, "\n"));
}

JsonValue WorkspaceContext::to_json() const {
    JsonValue::Array commits;
    for (const auto& item : recent_commits) commits.emplace_back(item);
    JsonValue::Object docs;
    for (const auto& [name, content] : project_docs) docs.emplace(name, JsonValue(content));
    return JsonValue::Object{
        {"branch", JsonValue(branch)}, {"cwd", JsonValue(cwd)}, {"default_branch", JsonValue(default_branch)},
        {"project_docs", JsonValue(std::move(docs))}, {"recent_commits", JsonValue(std::move(commits))},
        {"repo_root", JsonValue(repo_root)}, {"status", JsonValue(status)}};
}

std::string WorkspaceContext::fingerprint() const { return sha256(dump_compatible_json(to_json())); }

WorkspaceGuard::WorkspaceGuard(std::filesystem::path root) {
    std::error_code error;
    root_ = std::filesystem::weakly_canonical(std::filesystem::absolute(std::move(root), error), error);
}

Result<std::filesystem::path> WorkspaceGuard::resolve(std::string_view raw_path) const {
    std::error_code error;
    std::filesystem::path path{std::string(raw_path)};
    if (!path.is_absolute()) path = root_ / path;
    const auto resolved = std::filesystem::weakly_canonical(path, error);
    if (error) return Result<std::filesystem::path>::failure(make_error(
        ErrorCategory::PathViolation, "path_resolution_failed", "could not resolve path: " + std::string(raw_path)));
    const auto relative = std::filesystem::relative(resolved, root_, error);
    if (error || relative.is_absolute() || (!relative.empty() && *relative.begin() == "..")) return Result<std::filesystem::path>::failure(make_error(
        ErrorCategory::PathViolation, "path_escape", "path escapes workspace: " + std::string(raw_path)));
    return Result<std::filesystem::path>::success(resolved);
}

const std::filesystem::path& WorkspaceGuard::root() const noexcept { return root_; }

Result<std::string> read_text_file(const std::filesystem::path& path, bool replace_errors) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return Result<std::string>::failure(make_error(
        ErrorCategory::ToolExecution, "file_read_failed", "could not read file: " + path.string()));
    return decode_utf8(std::string((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()), replace_errors);
}

Result<void> write_text_file(const std::filesystem::path& path, std::string_view content) {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return Result<void>::failure(make_error(
        ErrorCategory::ToolExecution, "directory_create_failed", "could not create directory: " + path.parent_path().string()));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return Result<void>::failure(make_error(
        ErrorCategory::ToolExecution, "file_write_failed", "could not write file: " + path.string()));
    output.write(content.data(), static_cast<std::streamsize>(content.size()));
    return output ? Result<void>::success() : Result<void>::failure(make_error(
        ErrorCategory::ToolExecution, "file_write_failed", "could not write file: " + path.string()));
}

}  // namespace runi
