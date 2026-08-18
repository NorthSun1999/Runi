#include "runi/context_manager.hpp"

#include <algorithm>
#include <limits>
#include <set>

#include "runi/core/json_codec.hpp"
#include "runi/core/text.hpp"

namespace runi {
namespace {

const std::vector<std::string> kSectionOrder{"prefix", "memory", "relevant_memory", "history", "current_request"};

std::string tail_clip(std::string_view text, std::size_t limit) {
    const auto length = utf8_length(text);
    if (limit == 0) return {};
    if (length <= limit) return std::string(text);
    if (limit <= 3) return utf8_prefix(text, limit);
    return utf8_prefix(text, limit - 3) + "...";
}

std::string note_text(const JsonValue& note, std::string_view field, std::string_view fallback = {}) {
    const auto* value = note.find(field);
    return value == nullptr ? std::string(fallback) : value->string_or(fallback);
}

JsonValue string_array(const std::vector<std::string>& items) {
    JsonValue::Array values;
    for (const auto& item : items) values.emplace_back(item);
    return JsonValue(std::move(values));
}

std::size_t detail_count(const SectionRender& render, std::string_view key) {
    const auto* value = render.details.find(key);
    return value == nullptr ? 0 : static_cast<std::size_t>(std::max<std::int64_t>(0, value->integer_or(0)));
}

}  // namespace

ContextManager::ContextManager(IContextHost& host) : host_(host) {}

std::map<std::string, std::size_t, std::less<>> ContextManager::compute_section_floors() const {
    std::map<std::string, std::size_t, std::less<>> result;
    for (const auto& [name, budget] : section_budgets) result[name] = std::max<std::size_t>(20, budget / 4);
    for (const auto& [name, value] : section_floor_overrides) result[name] = value;
    return result;
}

Result<ContextBuildResult> ContextManager::build(std::string_view user_message) {
    const bool memory_enabled = host_.feature_enabled("memory");
    const bool relevant_enabled = host_.feature_enabled("relevant_memory");
    const bool reduction_enabled = host_.feature_enabled("context_reduction");
    std::map<std::string, std::string, std::less<>> texts{
        {"current_request", "Current user request:\n" + std::string(user_message)},
        {"history", ""},
        {"memory", memory_enabled ? host_.memory_text() : "Memory:\n- disabled"},
        {"prefix", host_.prefix()}};
    const auto checkpoint = trim(host_.render_checkpoint_text());
    if (!checkpoint.empty()) texts["prefix"] += "\n\n" + checkpoint;
    auto notes = memory_enabled && relevant_enabled ? host_.memory_candidates(user_message, kRelevantMemoryLimit) : std::vector<JsonValue>{};
    auto budgets = section_budgets;
    if (!reduction_enabled) {
        budgets["prefix"] = utf8_length(texts["prefix"]);
        budgets["memory"] = utf8_length(texts["memory"]);
        budgets["relevant_memory"] = std::numeric_limits<std::size_t>::max() / 4;
        budgets["history"] = std::numeric_limits<std::size_t>::max() / 4;
        auto full = render_sections(texts, budgets, notes);
        budgets["relevant_memory"] = utf8_length(full.at("relevant_memory").raw);
        budgets["history"] = utf8_length(full.at("history").raw);
    }
    auto rendered = render_sections(texts, budgets, notes);
    auto prompt = assemble(rendered);
    JsonValue::Array reductions;
    if (reduction_enabled) {
        const auto floors = compute_section_floors();
        while (utf8_length(prompt) > total_budget) {
            const auto overflow = utf8_length(prompt) - total_budget;
            bool reduced = false;
            for (const auto& section : reduction_order) {
                const auto floor = floors.contains(section) ? floors.at(section) : 0;
                const auto current = budgets.contains(section) ? budgets.at(section) : 0;
                if (current <= floor) continue;
                const auto next = std::max(floor, current > overflow ? current - overflow : 0U);
                if (next >= current) continue;
                reductions.emplace_back(JsonValue::Object{{"after_chars", JsonValue(next)}, {"before_chars", JsonValue(current)},
                    {"overflow_chars", JsonValue(overflow)}, {"section", JsonValue(section)}});
                budgets[section] = next;
                rendered = render_sections(texts, budgets, notes);
                prompt = assemble(rendered);
                reduced = true;
                break;
            }
            if (!reduced) break;
        }
    }
    return Result<ContextBuildResult>::success(ContextBuildResult{
        prompt, build_metadata(prompt, rendered, budgets, reductions, notes, user_message)});
}

SectionRender ContextManager::render_relevant_memory(const std::vector<JsonValue>& notes, std::size_t budget) const {
    const std::string header = "Relevant memory:";
    std::vector<std::string> texts;
    for (const auto& note : notes) if (!trim(note_text(note, "text")).empty()) texts.push_back(note_text(note, "text"));
    std::vector<std::string> raw_lines{header};
    if (texts.empty()) raw_lines.push_back("- none");
    else for (const auto& text : texts) raw_lines.push_back("- " + text);
    const auto raw = join(raw_lines, "\n");
    if (texts.empty()) return {raw, budget, raw, JsonValue::Object{{"note_budget", JsonValue(0)},
        {"rendered_count", JsonValue(0)}, {"rendered_notes", JsonValue::Array{}}, {"selected_count", JsonValue(0)}, {"selected_notes", JsonValue::Array{}}}};
    const auto overhead = utf8_length(header) + 3 * texts.size();
    std::size_t per_note = std::max<std::size_t>(1, budget > overhead ? (budget - overhead) / texts.size() : 1);
    std::vector<std::string> rendered_notes;
    std::string rendered;
    while (true) {
        rendered_notes.clear();
        for (const auto& text : texts) rendered_notes.push_back(tail_clip(text, per_note));
        std::vector<std::string> lines{header};
        for (const auto& text : rendered_notes) lines.push_back("- " + text);
        rendered = join(lines, "\n");
        if (utf8_length(rendered) <= budget || per_note <= 1) break;
        --per_note;
    }
    if (utf8_length(rendered) > budget && budget > 0) { rendered = tail_clip(raw, budget); rendered_notes = {rendered}; }
    return {raw, budget, rendered, JsonValue::Object{{"note_budget", JsonValue(per_note)},
        {"rendered_count", JsonValue(rendered_notes.size())}, {"rendered_notes", string_array(rendered_notes)},
        {"selected_count", JsonValue(texts.size())}, {"selected_notes", string_array(texts)}}};
}

std::string ContextManager::raw_history_text() const {
    if (host_.session().history.empty()) return "Transcript:\n- empty";
    std::vector<std::string> lines{"Transcript:"};
    for (const auto& item : host_.session().history) {
        if (item.role == "tool") {
            lines.push_back("[tool:" + item.name + "] " + dump_compatible_json(JsonValue(item.args)));
            lines.push_back(item.content);
        } else lines.push_back("[" + item.role + "] " + item.content);
    }
    return join(lines, "\n");
}

std::vector<std::string> ContextManager::render_history_item(const HistoryItem& item, std::size_t line_limit) const {
    if (item.role == "tool") return {"[tool:" + item.name + "] " + dump_compatible_json(JsonValue(item.args)), tail_clip(item.content, std::max<std::size_t>(20, line_limit))};
    return {"[" + item.role + "] " + tail_clip(item.content, line_limit)};
}

std::string ContextManager::summarize_old_tool_item(const HistoryItem& item) const {
    if (item.name == "run_shell") {
        const auto* value = item.args.find("command");
        const auto command = value == nullptr ? std::string("shell") : trim(value->string_or("shell"));
        std::vector<std::string> lines;
        for (const auto& raw : split_lines(item.content)) if (!trim(raw).empty()) lines.push_back(trim(raw));
        if (lines.size() > 3) lines.resize(3);
        return command + " -> " + (lines.empty() ? "(empty)" : join(lines, " | "));
    }
    return render_history_item(item, 60).front();
}

SectionRender ContextManager::render_history(std::size_t budget) const {
    const auto raw = raw_history_text();
    const auto& history = host_.session().history;
    if (history.empty()) return {raw, budget, "Transcript:\n- empty", JsonValue::Object{
        {"collapsed_duplicate_reads", JsonValue(0)}, {"older_entries_count", JsonValue(0)},
        {"rendered_entries", JsonValue::Array{}}, {"reused_file_summary_count", JsonValue(0)}, {"summarized_tool_count", JsonValue(0)}}};
    const std::size_t recent_window = 6;
    const std::size_t recent_start = history.size() > recent_window ? history.size() - recent_window : 0;
    struct Entry { bool recent; std::vector<std::string> lines; };
    std::vector<Entry> entries;
    std::set<std::string, std::less<>> seen_reads;
    std::size_t older = 0, collapsed = 0, reused = 0, summarized = 0;
    for (std::size_t index = 0; index < history.size(); ++index) {
        const auto& item = history[index];
        const bool recent = index >= recent_start;
        if (recent) { entries.push_back({true, render_history_item(item, 900)}); continue; }
        if (item.role == "tool" && item.name == "read_file") {
            const auto* value = item.args.find("path");
            const auto path = value == nullptr ? std::string{} : trim(value->string_or());
            if (seen_reads.contains(path)) { ++collapsed; continue; }
            seen_reads.insert(path);
            const auto summary = host_.reusable_file_summary(path);
            if (!summary.empty()) { entries.push_back(Entry{false, std::vector<std::string>{path + " -> " + summary}}); ++older; ++reused; continue; }
        }
        if (item.role == "tool") { entries.push_back({false, {summarize_old_tool_item(item)}}); ++older; ++summarized; continue; }
        entries.push_back({false, render_history_item(item, 60)});
    }
    std::vector<std::string> rendered_entries;
    for (auto iterator = entries.rbegin(); iterator != entries.rend(); ++iterator) {
        auto candidate_lines = iterator->lines;
        auto candidate = candidate_lines;
        candidate.insert(candidate.end(), rendered_entries.begin(), rendered_entries.end());
        if (utf8_length(join(std::vector<std::string>{"Transcript:", join(candidate, "\n")}, "\n")) <= budget) {
            rendered_entries = std::move(candidate); continue;
        }
        if (iterator->recent) {
            std::size_t used = utf8_length("Transcript:");
            for (const auto& line : rendered_entries) used += utf8_length(line) + 1;
            const auto available = std::max<std::size_t>(20, budget > used + 1 ? budget - used - 1 : 20);
            for (auto& line : candidate_lines) line = tail_clip(line, available);
        } else for (auto& line : candidate_lines) line = tail_clip(line, 20);
        candidate = candidate_lines;
        candidate.insert(candidate.end(), rendered_entries.begin(), rendered_entries.end());
        std::vector<std::string> with_header{"Transcript:"}; with_header.insert(with_header.end(), candidate.begin(), candidate.end());
        if (utf8_length(join(with_header, "\n")) <= budget) rendered_entries = std::move(candidate);
    }
    std::vector<std::string> lines{"Transcript:"}; lines.insert(lines.end(), rendered_entries.begin(), rendered_entries.end());
    auto rendered = join(lines, "\n");
    if (utf8_length(rendered) > budget && budget > 0) rendered = tail_clip(raw, budget);
    return {raw, budget, rendered, JsonValue::Object{{"collapsed_duplicate_reads", JsonValue(collapsed)},
        {"older_entries_count", JsonValue(older)}, {"recent_start", JsonValue(recent_start)}, {"recent_window", JsonValue(recent_window)},
        {"rendered_entries", string_array(rendered_entries)}, {"reused_file_summary_count", JsonValue(reused)},
        {"summarized_tool_count", JsonValue(summarized)}}};
}

std::map<std::string, SectionRender, std::less<>> ContextManager::render_sections(
    const std::map<std::string, std::string, std::less<>>& texts,
    const std::map<std::string, std::size_t, std::less<>>& budgets,
    const std::vector<JsonValue>& notes) const {
    std::map<std::string, SectionRender, std::less<>> result;
    for (const auto& section : kSectionOrder) {
        if (section == "current_request") result[section] = {texts.at(section), 0, texts.at(section), JsonValue::Object{}};
        else if (section == "relevant_memory") result[section] = render_relevant_memory(notes, budgets.at(section));
        else if (section == "history") result[section] = render_history(budgets.at(section));
        else result[section] = {texts.at(section), budgets.at(section), tail_clip(texts.at(section), budgets.at(section)), JsonValue::Object{}};
    }
    return result;
}

std::string ContextManager::assemble(const std::map<std::string, SectionRender, std::less<>>& rendered) const {
    std::vector<std::string> sections;
    for (const auto& name : kSectionOrder) sections.push_back(rendered.at(name).rendered);
    return trim(join(sections, "\n\n"));
}

JsonValue ContextManager::build_metadata(
    const std::string& prompt,
    const std::map<std::string, SectionRender, std::less<>>& rendered,
    const std::map<std::string, std::size_t, std::less<>>& budgets,
    const JsonValue::Array& reductions,
    const std::vector<JsonValue>& notes,
    std::string_view user_message) const {
    JsonValue::Object sections;
    JsonValue::Object budget_values;
    for (const auto& section : kSectionOrder) {
        const auto& value = rendered.at(section);
        sections.emplace(section, JsonValue::Object{{"budget_chars", section == "current_request" ? JsonValue(nullptr) : JsonValue(budgets.at(section))},
            {"raw_chars", JsonValue(utf8_length(value.raw))}, {"rendered_chars", JsonValue(utf8_length(value.rendered))}});
        budget_values.emplace(section, section == "current_request" ? JsonValue(nullptr) : JsonValue(budgets.at(section)));
    }
    std::vector<std::string> selected_texts, sources, kinds, rendered_notes;
    std::size_t durable = 0;
    for (const auto& note : notes) {
        selected_texts.push_back(note_text(note, "text"));
        sources.push_back(trim(note_text(note, "source")));
        auto kind = trim(note_text(note, "kind", "episodic"));
        if (kind.empty()) kind = "episodic";
        kinds.push_back(kind);
        if (kind == "durable") ++durable;
    }
    if (const auto* value = rendered.at("relevant_memory").details.find("rendered_notes"); value != nullptr && value->is_array()) {
        for (const auto& item : value->as_array()) rendered_notes.push_back(item.string_or());
    }
    JsonValue::Array order_json, reduction_order_json;
    for (const auto& item : kSectionOrder) order_json.emplace_back(item);
    for (const auto& item : reduction_order) reduction_order_json.emplace_back(item);
    return JsonValue::Object{
        {"budget_reductions", JsonValue(reductions)},
        {"current_request", JsonValue::Object{{"raw_chars", JsonValue(utf8_length(user_message))},
            {"rendered_chars", JsonValue(utf8_length(user_message))}, {"section_chars", JsonValue(utf8_length(rendered.at("current_request").rendered))},
            {"text", JsonValue(std::string(user_message))}}},
        {"history", JsonValue::Object{{"collapsed_duplicate_reads", JsonValue(detail_count(rendered.at("history"), "collapsed_duplicate_reads"))},
            {"older_entries_count", JsonValue(detail_count(rendered.at("history"), "older_entries_count"))},
            {"raw_chars", JsonValue(utf8_length(rendered.at("history").raw))}, {"rendered_chars", JsonValue(utf8_length(rendered.at("history").rendered))},
            {"reused_file_summary_count", JsonValue(detail_count(rendered.at("history"), "reused_file_summary_count"))},
            {"summarized_tool_count", JsonValue(detail_count(rendered.at("history"), "summarized_tool_count"))}}},
        {"prompt_budget_chars", JsonValue(total_budget)}, {"prompt_chars", JsonValue(utf8_length(prompt))},
        {"prompt_over_budget", JsonValue(utf8_length(prompt) > total_budget)}, {"reduction_order", JsonValue(std::move(reduction_order_json))},
        {"relevant_memory", JsonValue::Object{{"limit", JsonValue(kRelevantMemoryLimit)}, {"raw_chars", JsonValue(utf8_length(rendered.at("relevant_memory").raw))},
            {"rendered_chars", JsonValue(utf8_length(rendered.at("relevant_memory").rendered))}, {"rendered_count", JsonValue(rendered_notes.size())},
            {"rendered_notes", string_array(rendered_notes)}, {"selected_count", JsonValue(notes.size())},
            {"selected_durable_count", JsonValue(durable)}, {"selected_kinds", string_array(kinds)},
            {"selected_notes", string_array(selected_texts)}, {"selected_sources", string_array(sources)}}},
        {"section_budgets", JsonValue(std::move(budget_values))}, {"section_order", JsonValue(std::move(order_json))},
        {"sections", JsonValue(std::move(sections))}};
}

}  // namespace runi
