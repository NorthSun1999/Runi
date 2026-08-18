#pragma once

#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "runi/core/result.hpp"
#include "runi/session.hpp"

namespace runi {

inline constexpr std::size_t kDefaultTotalBudget = 12000;
inline constexpr std::size_t kRelevantMemoryLimit = 3;

struct SectionRender {
    std::string raw;
    std::size_t budget{0};
    std::string rendered;
    JsonValue details{JsonValue::Object{}};
};

struct ContextBuildResult {
    std::string prompt;
    JsonValue metadata{JsonValue::Object{}};
};

class IContextHost {
public:
    virtual ~IContextHost() = default;
    [[nodiscard]] virtual const std::string& prefix() const = 0;
    [[nodiscard]] virtual std::string memory_text() = 0;
    [[nodiscard]] virtual std::string render_checkpoint_text() const = 0;
    [[nodiscard]] virtual std::vector<JsonValue> memory_candidates(std::string_view query, std::size_t limit) = 0;
    [[nodiscard]] virtual const SessionState& session() const = 0;
    [[nodiscard]] virtual bool feature_enabled(std::string_view name) const = 0;
    [[nodiscard]] virtual std::string reusable_file_summary(std::string_view path) const = 0;
};

class IContextBuilder {
public:
    virtual ~IContextBuilder() = default;
    [[nodiscard]] virtual Result<ContextBuildResult> build(std::string_view user_message) = 0;
};

class ContextManager final : public IContextBuilder {
public:
    explicit ContextManager(IContextHost& host);

    std::size_t total_budget{kDefaultTotalBudget};
    std::map<std::string, std::size_t, std::less<>> section_budgets{
        {"history", 5200}, {"memory", 1600}, {"prefix", 3600}, {"relevant_memory", 1200}};
    std::map<std::string, std::size_t, std::less<>> section_floor_overrides;
    std::vector<std::string> reduction_order{"relevant_memory", "history", "memory", "prefix"};

    [[nodiscard]] Result<ContextBuildResult> build(std::string_view user_message) override;

private:
    [[nodiscard]] std::map<std::string, std::size_t, std::less<>> compute_section_floors() const;
    [[nodiscard]] SectionRender render_relevant_memory(const std::vector<JsonValue>& notes, std::size_t budget) const;
    [[nodiscard]] SectionRender render_history(std::size_t budget) const;
    [[nodiscard]] std::string raw_history_text() const;
    [[nodiscard]] std::vector<std::string> render_history_item(const HistoryItem& item, std::size_t line_limit) const;
    [[nodiscard]] std::string summarize_old_tool_item(const HistoryItem& item) const;
    [[nodiscard]] std::map<std::string, SectionRender, std::less<>> render_sections(
        const std::map<std::string, std::string, std::less<>>& texts,
        const std::map<std::string, std::size_t, std::less<>>& budgets,
        const std::vector<JsonValue>& notes) const;
    [[nodiscard]] std::string assemble(const std::map<std::string, SectionRender, std::less<>>& rendered) const;
    [[nodiscard]] JsonValue build_metadata(
        const std::string& prompt,
        const std::map<std::string, SectionRender, std::less<>>& rendered,
        const std::map<std::string, std::size_t, std::less<>>& budgets,
        const JsonValue::Array& reductions,
        const std::vector<JsonValue>& notes,
        std::string_view user_message) const;

    IContextHost& host_;
};

}  // namespace runi
