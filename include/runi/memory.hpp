#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "runi/core/json_value.hpp"

namespace runi {

inline constexpr std::size_t kWorkingFileLimit = 8;
inline constexpr std::size_t kEpisodicNoteLimit = 12;
inline constexpr std::size_t kFileSummaryLimit = 6;

[[nodiscard]] JsonValue default_memory_state();
[[nodiscard]] std::optional<std::filesystem::path> resolve_workspace_path(
    std::string_view raw_path,
    const std::optional<std::filesystem::path>& workspace_root);
[[nodiscard]] std::string canonicalize_path(
    std::string_view raw_path,
    const std::optional<std::filesystem::path>& workspace_root);
[[nodiscard]] JsonValue file_freshness(
    std::string_view raw_path,
    const std::optional<std::filesystem::path>& workspace_root);
[[nodiscard]] std::string summarize_read_result(std::string_view result, std::size_t limit = 180);

class DurableMemoryStore {
public:
    explicit DurableMemoryStore(std::filesystem::path root);
    [[nodiscard]] std::vector<std::string> topic_slugs() const;
    [[nodiscard]] std::vector<JsonValue> load_index() const;
    [[nodiscard]] std::vector<JsonValue> load_topic_notes(std::string_view topic) const;
    [[nodiscard]] std::vector<JsonValue> retrieval_candidates(std::string_view query, std::size_t limit = 3) const;
    [[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> promote(
        const std::vector<std::pair<std::string, std::string>>& promotions);

private:
    [[nodiscard]] static std::optional<std::string> subject_key(std::string_view text);
    void write_index(const std::vector<JsonValue>& topics) const;
    void write_topic(std::string_view topic, const std::vector<std::string>& notes) const;

    std::filesystem::path root_;
    std::filesystem::path index_path_;
    std::filesystem::path topics_dir_;
};

class LayeredMemory {
public:
    explicit LayeredMemory(
        JsonValue state = default_memory_state(),
        std::optional<std::filesystem::path> workspace_root = std::nullopt);

    [[nodiscard]] const JsonValue& to_json();
    [[nodiscard]] std::string canonical_path(std::string_view path) const;
    LayeredMemory& set_task_summary(std::string_view summary);
    LayeredMemory& remember_file(std::string_view path);
    LayeredMemory& append_note(
        std::string_view text,
        std::vector<std::string> tags = {},
        std::string source = {},
        std::string created_at = {},
        std::string kind = "episodic");
    LayeredMemory& set_file_summary(std::string_view path, std::string_view summary);
    LayeredMemory& invalidate_file_summary(std::string_view path);
    [[nodiscard]] std::vector<std::string> invalidate_stale_file_summaries();
    [[nodiscard]] std::vector<JsonValue> retrieval_candidates(std::string_view query, std::size_t limit = 3);
    [[nodiscard]] std::string retrieval_view(std::string_view query, std::size_t limit = 3);
    [[nodiscard]] std::string render_memory_text();
    [[nodiscard]] bool is_effectively_empty();
    [[nodiscard]] std::pair<std::vector<std::string>, std::vector<std::string>> promote_durable(
        const std::vector<std::pair<std::string, std::string>>& promotions);

private:
    void normalize();
    JsonValue state_;
    std::optional<std::filesystem::path> workspace_root_;
    std::optional<DurableMemoryStore> durable_store_;
};

}  // namespace runi
