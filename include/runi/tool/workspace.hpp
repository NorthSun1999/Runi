#pragma once

#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "runi/core/result.hpp"
#include "runi/tool/process_runner.hpp"

namespace runi {

inline const std::vector<std::string> kProjectDocNames{"AGENTS.md", "README.md", "pyproject.toml", "package.json"};
inline const std::set<std::string, std::less<>> kIgnoredPathNames{
    ".git", ".runi", "__pycache__", ".pytest_cache", ".ruff_cache", ".venv", "venv"};

struct WorkspaceContext {
    std::string cwd;
    std::string repo_root;
    std::string branch;
    std::string default_branch;
    std::string status;
    std::vector<std::string> recent_commits;
    std::map<std::string, std::string, std::less<>> project_docs;
    std::vector<std::string> project_doc_order;

    [[nodiscard]] static Result<WorkspaceContext> build(
        const std::filesystem::path& cwd,
        std::optional<std::filesystem::path> repo_root_override = std::nullopt,
        const IProcessRunner* process_runner = nullptr);
    [[nodiscard]] std::string text() const;
    [[nodiscard]] std::string fingerprint() const;
    [[nodiscard]] JsonValue to_json() const;
};

class WorkspaceGuard {
public:
    explicit WorkspaceGuard(std::filesystem::path root);
    [[nodiscard]] Result<std::filesystem::path> resolve(std::string_view raw_path) const;
    [[nodiscard]] const std::filesystem::path& root() const noexcept;

private:
    std::filesystem::path root_;
};

[[nodiscard]] Result<std::string> read_text_file(const std::filesystem::path& path, bool replace_errors = false);
[[nodiscard]] Result<void> write_text_file(const std::filesystem::path& path, std::string_view content);

}  // namespace runi
