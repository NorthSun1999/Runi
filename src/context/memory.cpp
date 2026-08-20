#include "runi/context/memory.hpp"

#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <tuple>

#include "runi/core/sha256.hpp"
#include "runi/core/text.hpp"
#include "runi/core/time.hpp"
#include "runi/tool/workspace.hpp"

namespace runi {
namespace {

struct TopicMeta { std::string title; std::string summary; std::vector<std::string> tags; };

const std::map<std::string, TopicMeta, std::less<>> kTopicDefaults{
    {"project-conventions", {"Project Conventions", "Stable repository conventions.", {"convention"}}},
    {"key-decisions", {"Key Decisions", "Long-lived decisions and rationale anchors.", {"decision"}}},
    {"dependency-facts", {"Dependency Facts", "Stable dependency and environment facts.", {"dependency"}}},
    {"user-preferences", {"User Preferences", "Stable user preferences.", {"preference"}}},
};

std::string get_string(const JsonValue& value, std::string_view key, std::string_view fallback = {}) {
    const auto* field = value.find(key);
    return field == nullptr ? std::string(fallback) : field->string_or(fallback);
}

std::vector<std::string> string_array(const JsonValue* value) {
    std::vector<std::string> result;
    if (value == nullptr) return result;
    if (value->is_array()) {
        for (const auto& item : value->as_array()) if (item.is_string()) result.push_back(item.as_string());
    } else if (value->is_string() && !value->as_string().empty()) result.push_back(value->as_string());
    return result;
}

JsonValue to_array(const std::vector<std::string>& values) {
    JsonValue::Array result;
    for (const auto& item : values) result.emplace_back(item);
    return JsonValue(std::move(result));
}

void dedupe(std::vector<std::string>& values) {
    std::set<std::string, std::less<>> seen;
    values.erase(std::remove_if(values.begin(), values.end(), [&](const auto& value) {
        return !seen.insert(value).second;
    }), values.end());
}

std::set<std::string, std::less<>> tokens(std::string_view text) {
    static const std::regex expression("[A-Za-z0-9_]+");
    const auto lower = lower_ascii(text);
    std::set<std::string, std::less<>> result;
    for (auto it = std::sregex_iterator(lower.begin(), lower.end(), expression); it != std::sregex_iterator(); ++it) {
        result.insert(it->str());
    }
    return result;
}

int overlap(const std::set<std::string, std::less<>>& left, const std::set<std::string, std::less<>>& right) {
    int count = 0;
    for (const auto& item : left) if (right.contains(item)) ++count;
    return count;
}

JsonValue normalized_note(const JsonValue& value, std::size_t index) {
    if (value.is_string()) {
        return JsonValue::Object{{"created_at", JsonValue(now_utc())}, {"kind", JsonValue("episodic")},
            {"note_index", JsonValue(index)}, {"source", JsonValue("")}, {"tags", JsonValue::Array{}},
            {"text", JsonValue(clip(trim(value.as_string()), 500))}};
    }
    const auto text = value.is_object() ? clip(trim(get_string(value, "text")), 500) : clip(trim(""), 500);
    auto tags = value.is_object() ? string_array(value.find("tags")) : std::vector<std::string>{};
    dedupe(tags);
    const auto note_index_field = value.is_object() ? value.find("note_index") : nullptr;
    const auto note_index = note_index_field == nullptr ? index : static_cast<std::size_t>(std::max<std::int64_t>(0, note_index_field->integer_or(index)));
    return JsonValue::Object{
        {"created_at", JsonValue(value.is_object() ? get_string(value, "created_at", now_utc()) : now_utc())},
        {"kind", JsonValue(value.is_object() ? get_string(value, "kind", "episodic") : "episodic")},
        {"note_index", JsonValue(note_index)},
        {"source", JsonValue(value.is_object() ? get_string(value, "source") : "")},
        {"tags", to_array(tags)}, {"text", JsonValue(text)}};
}

}  // namespace

JsonValue default_memory_state() {
    return JsonValue::Object{
        {"episodic_notes", JsonValue::Array{}}, {"file_summaries", JsonValue::Object{}},
        {"files", JsonValue::Array{}}, {"next_note_index", JsonValue(0)}, {"notes", JsonValue::Array{}},
        {"task", JsonValue("")},
        {"working", JsonValue::Object{{"recent_files", JsonValue::Array{}}, {"task_summary", JsonValue("")}}}};
}

std::optional<std::filesystem::path> resolve_workspace_path(
    std::string_view raw_path,
    const std::optional<std::filesystem::path>& workspace_root) {
    std::filesystem::path path{std::string(raw_path)};
    if (!workspace_root.has_value()) return path;
    WorkspaceGuard guard(*workspace_root);
    const auto resolved = guard.resolve(raw_path);
    return resolved ? std::optional<std::filesystem::path>(resolved.value()) : std::nullopt;
}

std::string canonicalize_path(std::string_view raw_path, const std::optional<std::filesystem::path>& workspace_root) {
    const auto resolved = resolve_workspace_path(raw_path, workspace_root);
    if (!resolved.has_value() || !workspace_root.has_value()) return std::filesystem::path(std::string(raw_path)).generic_string();
    std::error_code error;
    return std::filesystem::relative(*resolved, *workspace_root, error).generic_string();
}

JsonValue file_freshness(std::string_view raw_path, const std::optional<std::filesystem::path>& workspace_root) {
    const auto resolved = resolve_workspace_path(raw_path, workspace_root);
    std::error_code error;
    if (!resolved.has_value() || !std::filesystem::is_regular_file(*resolved, error)) return JsonValue(nullptr);
    const auto result = sha256_file(*resolved);
    return result ? JsonValue(result.value()) : JsonValue(nullptr);
}

std::string summarize_read_result(std::string_view result, std::size_t limit) {
    std::vector<std::string> lines;
    for (const auto& line : split_lines(result)) {
        const auto value = trim(line);
        if (!value.empty()) lines.push_back(value);
    }
    if (lines.empty()) return "(empty)";
    if (lines.front().starts_with("# ")) lines.erase(lines.begin());
    if (lines.empty()) return "(empty)";
    if (lines.size() > 3) lines.resize(3);
    return clip(join(lines, " | "), limit);
}

DurableMemoryStore::DurableMemoryStore(std::filesystem::path root)
    : root_(std::move(root)), index_path_(root_ / "MEMORY.md"), topics_dir_(root_ / "topics") {}

std::vector<JsonValue> DurableMemoryStore::load_index() const {
    std::vector<JsonValue> topics;
    const auto content = read_text_file(index_path_, true);
    if (!content) return topics;
    JsonValue::Object* current = nullptr;
    static const std::regex topic_expression(R"(^- \[([^\]]+)\]\([^)]+\):\s*(.+)$)");
    for (const auto& raw : split_lines(content.value())) {
        const auto line = trim(raw);
        std::smatch match;
        if (std::regex_match(line, match, topic_expression)) {
            topics.emplace_back(JsonValue::Object{{"summary", JsonValue("")}, {"tags", JsonValue::Array{}},
                {"title", JsonValue(trim(match[2].str()))}, {"topic", JsonValue(trim(match[1].str()))}});
            current = &topics.back().as_object();
        } else if (current != nullptr && line.starts_with("- summary:")) {
            (*current)["summary"] = JsonValue(trim(std::string_view(line).substr(10)));
        } else if (current != nullptr && line.starts_with("- tags:")) {
            std::vector<std::string> values;
            std::stringstream stream(trim(std::string_view(line).substr(7)));
            std::string value;
            while (std::getline(stream, value, ',')) if (!(value = trim(value)).empty()) values.push_back(value);
            (*current)["tags"] = to_array(values);
        }
    }
    return topics;
}

std::vector<std::string> DurableMemoryStore::topic_slugs() const {
    std::vector<std::string> result;
    for (const auto& topic : load_index()) result.push_back(get_string(topic, "topic"));
    return result;
}

std::vector<JsonValue> DurableMemoryStore::load_topic_notes(std::string_view topic) const {
    std::vector<JsonValue> notes;
    const auto content = read_text_file(topics_dir_ / (std::string(topic) + ".md"), true);
    if (!content) return notes;
    bool capture = false;
    std::string updated_at;
    std::vector<std::string> tags;
    for (const auto& raw : split_lines(content.value())) {
        const auto line = trim(raw);
        if (line.starts_with("- tags:")) {
            tags.clear();
            std::stringstream stream(trim(std::string_view(line).substr(7)));
            std::string tag;
            while (std::getline(stream, tag, ',')) if (!(tag = trim(tag)).empty()) tags.push_back(tag);
        } else if (line.starts_with("- updated_at:")) updated_at = trim(std::string_view(line).substr(13));
        else if (line == "## Notes") capture = true;
        else if (capture && line.starts_with("- ")) notes.emplace_back(JsonValue::Object{
            {"created_at", JsonValue(updated_at.empty() ? now_utc() : updated_at)}, {"kind", JsonValue("durable")},
            {"source", JsonValue(std::string(topic))}, {"tags", to_array(tags)},
            {"text", JsonValue(trim(std::string_view(line).substr(2)))}});
    }
    return notes;
}

std::vector<JsonValue> DurableMemoryStore::retrieval_candidates(std::string_view query, std::size_t limit) const {
    struct Ranked { int tag; int keywords; std::string created_at; JsonValue note; };
    const auto query_tokens = tokens(query);
    std::vector<Ranked> ranked;
    for (const auto& topic : load_index()) {
        for (const auto& note : load_topic_notes(get_string(topic, "topic"))) {
            std::set<std::string, std::less<>> note_tokens = tokens(get_string(note, "text"));
            const auto title_tokens = tokens(get_string(topic, "title"));
            note_tokens.insert(title_tokens.begin(), title_tokens.end());
            const auto tags = string_array(note.find("tags"));
            std::set<std::string, std::less<>> tag_tokens;
            for (const auto& tag : tags) tag_tokens.insert(lower_ascii(tag));
            note_tokens.insert(tag_tokens.begin(), tag_tokens.end());
            const int tag_match = overlap(query_tokens, tag_tokens) > 0 ? 1 : 0;
            const int keywords = overlap(query_tokens, note_tokens);
            if (tag_match == 0 && keywords == 0) continue;
            ranked.push_back({tag_match, keywords, get_string(note, "created_at"), note});
        }
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return std::tie(left.tag, left.keywords, left.created_at) > std::tie(right.tag, right.keywords, right.created_at);
    });
    std::vector<JsonValue> result;
    for (std::size_t index = 0; index < std::min(limit, ranked.size()); ++index) result.push_back(ranked[index].note);
    return result;
}

std::optional<std::string> DurableMemoryStore::subject_key(std::string_view text) {
    static const std::vector<std::regex> expressions{
        std::regex(R"(^(.+?)\s+is\s+.+$)", std::regex::icase), std::regex(R"(^(.+?)\s+are\s+.+$)", std::regex::icase),
        std::regex(R"(^(.+?)\s+uses?\s+.+$)", std::regex::icase), std::regex(R"(^(.+?)\s+should\s+.+$)", std::regex::icase)};
    const std::string input(text);
    for (const auto& expression : expressions) {
        std::smatch match;
        if (std::regex_match(input, match, expression)) {
            const auto values = tokens(match[1].str());
            if (!values.empty()) return join(std::vector<std::string>(values.begin(), values.end()), " ");
        }
    }
    return std::nullopt;
}

void DurableMemoryStore::write_index(const std::vector<JsonValue>& topics) const {
    std::filesystem::create_directories(topics_dir_);
    std::vector<std::string> lines{"# Durable Memory Index", ""};
    for (const auto& topic : topics) {
        lines.push_back("- [" + get_string(topic, "topic") + "](topics/" + get_string(topic, "topic") + ".md): " + get_string(topic, "title"));
        lines.push_back("  - summary: " + get_string(topic, "summary"));
        lines.push_back("  - tags: " + join(string_array(topic.find("tags")), ", "));
    }
    const auto written = write_text_file(index_path_, join(lines, "\n") + "\n");
    if (!written) throw std::runtime_error(written.error().message);
}

void DurableMemoryStore::write_topic(std::string_view topic, const std::vector<std::string>& notes) const {
    const auto& meta = kTopicDefaults.at(std::string(topic));
    std::vector<std::string> lines{"# " + meta.title, "", "- topic: " + std::string(topic),
        "- summary: " + meta.summary, "- tags: " + join(meta.tags, ", "), "- updated_at: " + now_utc(), "", "## Notes"};
    for (const auto& note : notes) lines.push_back("- " + note);
    const auto written = write_text_file(topics_dir_ / (std::string(topic) + ".md"), join(lines, "\n") + "\n");
    if (!written) throw std::runtime_error(written.error().message);
}

std::pair<std::vector<std::string>, std::vector<std::string>> DurableMemoryStore::promote(
    const std::vector<std::pair<std::string, std::string>>& promotions) {
    if (promotions.empty()) return {};
    std::map<std::string, JsonValue, std::less<>> topic_map;
    for (const auto& topic : load_index()) topic_map[get_string(topic, "topic")] = topic;
    std::map<std::string, std::vector<std::string>, std::less<>> topic_notes;
    for (const auto& [slug, topic] : topic_map) {
        static_cast<void>(topic);
        for (const auto& note : load_topic_notes(slug)) topic_notes[slug].push_back(get_string(note, "text"));
    }
    std::vector<std::string> promoted;
    std::vector<std::string> superseded;
    for (const auto& [topic, note] : promotions) {
        const auto defaults = kTopicDefaults.find(topic);
        if (defaults == kTopicDefaults.end()) continue;
        if (!topic_map.contains(topic)) topic_map.emplace(topic, JsonValue::Object{
            {"summary", JsonValue(defaults->second.summary)}, {"tags", to_array(defaults->second.tags)},
            {"title", JsonValue(defaults->second.title)}, {"topic", JsonValue(topic)}});
        auto& existing = topic_notes[topic];
        if (std::find(existing.begin(), existing.end(), note) != existing.end()) continue;
        const auto new_subject = subject_key(note);
        bool replaced = false;
        if (new_subject.has_value()) {
            for (auto& old : existing) if (subject_key(old) == new_subject) {
                superseded.push_back(topic + ": " + old + " -> " + note);
                old = note; replaced = true; break;
            }
        }
        if (!replaced) existing.push_back(note);
        promoted.push_back(topic + ": " + note);
    }
    std::vector<JsonValue> topics;
    for (const auto& [slug, value] : topic_map) { static_cast<void>(slug); topics.push_back(value); }
    write_index(topics);
    for (const auto& [topic, notes] : topic_notes) write_topic(topic, notes);
    return {promoted, superseded};
}

LayeredMemory::LayeredMemory(JsonValue state, std::optional<std::filesystem::path> root)
    : state_(std::move(state)), workspace_root_(std::move(root)) {
    if (workspace_root_.has_value()) durable_store_.emplace(*workspace_root_ / ".runi" / "memory");
    normalize();
}

void LayeredMemory::normalize() {
    if (!state_.is_object()) state_ = default_memory_state();
    auto& object = state_.as_object();
    if (!object.contains("working") || !object["working"].is_object()) object["working"] = JsonValue::Object{};
    auto& working = object["working"].as_object();
    if (!working.contains("task_summary")) working["task_summary"] = JsonValue("");
    if (!working.contains("recent_files")) working["recent_files"] = JsonValue::Array{};
    auto summary = clip(trim(working["task_summary"].string_or()), 300);
    if (summary.empty() && object.contains("task")) summary = clip(trim(object["task"].string_or()), 300);
    auto files = string_array(&working["recent_files"]);
    if (files.empty() && object.contains("files")) files = string_array(&object["files"]);
    for (auto& path : files) path = canonicalize_path(path, workspace_root_);
    dedupe(files);
    if (files.size() > kWorkingFileLimit) files.erase(files.begin(), files.end() - static_cast<std::ptrdiff_t>(kWorkingFileLimit));
    working["task_summary"] = JsonValue(summary);
    working["recent_files"] = to_array(files);

    JsonValue::Array notes;
    const auto* current_notes = state_.find("episodic_notes");
    if (current_notes != nullptr && current_notes->is_array() && !current_notes->as_array().empty()) {
        std::size_t index = 0;
        for (const auto& note : current_notes->as_array()) notes.push_back(normalized_note(note, index++));
    } else if (const auto* legacy = state_.find("notes")) {
        std::size_t index = 0;
        for (const auto& note : legacy->is_array() ? legacy->as_array() : JsonValue::Array{}) notes.push_back(normalized_note(note, index++));
    }
    if (notes.size() > kEpisodicNoteLimit) notes.erase(notes.begin(), notes.end() - static_cast<std::ptrdiff_t>(kEpisodicNoteLimit));
    object["episodic_notes"] = JsonValue(notes);

    JsonValue::Object summaries;
    if (const auto* existing = state_.find("file_summaries"); existing != nullptr && existing->is_object()) {
        for (const auto& [raw_path, value] : existing->as_object()) {
            const auto path = canonicalize_path(raw_path, workspace_root_);
            const auto text = value.is_object() ? clip(trim(get_string(value, "summary")), 500) : clip(trim(value.string_or()), 500);
            if (path.empty() || text.empty()) continue;
            const auto* freshness = value.is_object() ? value.find("freshness") : nullptr;
            summaries.emplace(path, JsonValue::Object{{"created_at", JsonValue(value.is_object() ? get_string(value, "created_at", now_utc()) : now_utc())},
                {"freshness", freshness == nullptr ? JsonValue(nullptr) : *freshness}, {"summary", JsonValue(text)}});
        }
    }
    object["file_summaries"] = JsonValue(std::move(summaries));
    std::size_t next_index = static_cast<std::size_t>(std::max<std::int64_t>(0, object.contains("next_note_index") ? object["next_note_index"].integer_or(0) : 0));
    for (const auto& note : notes) next_index = std::max(next_index, static_cast<std::size_t>(note.at("note_index").integer_or(0) + 1));
    object["next_note_index"] = JsonValue(next_index);
    object["task"] = JsonValue(summary);
    object["files"] = to_array(files);
    std::vector<std::string> legacy_notes;
    for (const auto& note : notes) legacy_notes.push_back(get_string(note, "text"));
    object["notes"] = to_array(legacy_notes);
    object["durable_topics"] = to_array(durable_store_.has_value() ? durable_store_->topic_slugs() : std::vector<std::string>{});
}

const JsonValue& LayeredMemory::to_json() { normalize(); return state_; }
std::string LayeredMemory::canonical_path(std::string_view path) const { return canonicalize_path(path, workspace_root_); }

LayeredMemory& LayeredMemory::set_task_summary(std::string_view summary) {
    normalize();
    const auto value = clip(trim(summary), 300);
    state_["working"]["task_summary"] = JsonValue(value);
    state_["task"] = JsonValue(value);
    return *this;
}

LayeredMemory& LayeredMemory::remember_file(std::string_view raw_path) {
    normalize();
    const auto path = canonical_path(raw_path);
    if (path.empty()) return *this;
    auto files = string_array(state_["working"].find("recent_files"));
    files.erase(std::remove(files.begin(), files.end(), path), files.end());
    files.push_back(path);
    if (files.size() > kWorkingFileLimit) files.erase(files.begin());
    state_["working"]["recent_files"] = to_array(files);
    state_["files"] = to_array(files);
    return *this;
}

LayeredMemory& LayeredMemory::append_note(std::string_view raw, std::vector<std::string> tags, std::string source, std::string created, std::string kind) {
    normalize();
    const auto text = clip(trim(raw), 500);
    if (text.empty()) return *this;
    dedupe(tags);
    auto notes = state_["episodic_notes"].as_array();
    notes.erase(std::remove_if(notes.begin(), notes.end(), [&](const auto& note) { return get_string(note, "text") == text; }), notes.end());
    const auto index = state_["next_note_index"].integer_or(0);
    notes.emplace_back(JsonValue::Object{{"created_at", JsonValue(created.empty() ? now_utc() : created)}, {"kind", JsonValue(kind.empty() ? "episodic" : kind)},
        {"note_index", JsonValue(index)}, {"source", JsonValue(std::move(source))}, {"tags", to_array(tags)}, {"text", JsonValue(text)}});
    if (notes.size() > kEpisodicNoteLimit) notes.erase(notes.begin());
    state_["episodic_notes"] = JsonValue(notes);
    state_["next_note_index"] = JsonValue(index + 1);
    std::vector<std::string> legacy;
    for (const auto& note : notes) legacy.push_back(get_string(note, "text"));
    state_["notes"] = to_array(legacy);
    return *this;
}

LayeredMemory& LayeredMemory::set_file_summary(std::string_view raw_path, std::string_view raw_summary) {
    normalize();
    const auto path = canonical_path(raw_path);
    const auto summary = clip(trim(raw_summary), 500);
    if (!path.empty() && !summary.empty()) state_["file_summaries"][path] = JsonValue::Object{
        {"created_at", JsonValue(now_utc())}, {"freshness", file_freshness(path, workspace_root_)}, {"summary", JsonValue(summary)}};
    return *this;
}

LayeredMemory& LayeredMemory::invalidate_file_summary(std::string_view raw_path) {
    normalize();
    state_["file_summaries"].as_object().erase(canonical_path(raw_path));
    return *this;
}

std::vector<std::string> LayeredMemory::invalidate_stale_file_summaries() {
    normalize();
    std::vector<std::string> invalidated;
    auto& summaries = state_["file_summaries"].as_object();
    for (auto iterator = summaries.begin(); iterator != summaries.end();) {
        const auto current = file_freshness(iterator->first, workspace_root_);
        const auto* saved = iterator->second.find("freshness");
        if (saved != nullptr && *saved == current) ++iterator;
        else { invalidated.push_back(iterator->first); iterator = summaries.erase(iterator); }
    }
    return invalidated;
}

std::vector<JsonValue> LayeredMemory::retrieval_candidates(std::string_view query, std::size_t limit) {
    normalize();
    struct Ranked { int tag; int keywords; std::string created; std::int64_t index; JsonValue note; };
    const auto query_tokens = tokens(query);
    std::vector<Ranked> ranked;
    for (const auto& note : state_["episodic_notes"].as_array()) {
        std::set<std::string, std::less<>> note_tokens = tokens(get_string(note, "text") + " " + get_string(note, "source"));
        std::set<std::string, std::less<>> tags;
        for (const auto& tag : string_array(note.find("tags"))) tags.insert(lower_ascii(tag));
        note_tokens.insert(tags.begin(), tags.end());
        const int tag_match = overlap(query_tokens, tags) > 0 ? 1 : 0;
        const int keywords = overlap(query_tokens, note_tokens);
        if (tag_match == 0 && keywords == 0) continue;
        ranked.push_back({tag_match, keywords, get_string(note, "created_at"), note.at("note_index").integer_or(0), note});
    }
    if (durable_store_.has_value()) for (const auto& note : durable_store_->retrieval_candidates(query, limit)) {
        std::set<std::string, std::less<>> note_tokens = tokens(get_string(note, "text") + " " + get_string(note, "source"));
        std::set<std::string, std::less<>> tags;
        for (const auto& tag : string_array(note.find("tags"))) tags.insert(lower_ascii(tag));
        note_tokens.insert(tags.begin(), tags.end());
        ranked.push_back({overlap(query_tokens, tags) > 0 ? 1 : 0, overlap(query_tokens, note_tokens), get_string(note, "created_at"), -1, note});
    }
    std::sort(ranked.begin(), ranked.end(), [](const auto& left, const auto& right) {
        return std::tie(left.tag,left.keywords,left.created,left.index) > std::tie(right.tag,right.keywords,right.created,right.index);
    });
    std::vector<JsonValue> result;
    for (std::size_t index = 0; index < std::min(limit, ranked.size()); ++index) result.push_back(ranked[index].note);
    return result;
}

std::string LayeredMemory::retrieval_view(std::string_view query, std::size_t limit) {
    std::vector<std::string> lines{"Relevant memory:"};
    const auto candidates = retrieval_candidates(query, limit);
    if (candidates.empty()) lines.push_back("- none");
    else for (const auto& note : candidates) lines.push_back("- " + get_string(note, "text"));
    return join(lines, "\n");
}

std::string LayeredMemory::render_memory_text() {
    normalize();
    const auto files = string_array(state_["working"].find("recent_files"));
    std::vector<std::string> lines{"Memory:", "- task: " + (get_string(state_["working"], "task_summary").empty() ? "-" : get_string(state_["working"], "task_summary")),
        "- recent_files: " + (files.empty() ? "-" : join(files, ", "))};
    std::vector<std::string> summaries;
    for (std::size_t index = 0; index < std::min(kFileSummaryLimit, files.size()); ++index) {
        const auto* summary = state_["file_summaries"].find(files[index]);
        if (summary != nullptr && summary->is_object() && summary->at("freshness") == file_freshness(files[index], workspace_root_)) {
            const auto text = get_string(*summary, "summary");
            if (!text.empty()) summaries.push_back("  - " + files[index] + ": " + text);
        }
    }
    if (summaries.empty()) lines.push_back("- file_summaries: -");
    else { lines.push_back("- file_summaries:"); lines.insert(lines.end(), summaries.begin(), summaries.end()); }
    lines.push_back("- episodic_notes: " + std::to_string(state_["episodic_notes"].as_array().size()));
    const auto topics = string_array(state_.find("durable_topics"));
    lines.push_back("- durable_topics: " + (topics.empty() ? "-" : join(topics, ", ")));
    return join(lines, "\n");
}

bool LayeredMemory::is_effectively_empty() {
    normalize();
    return get_string(state_["working"], "task_summary").empty() &&
        string_array(state_["working"].find("recent_files")).empty() &&
        state_["episodic_notes"].as_array().empty() && state_["file_summaries"].as_object().empty();
}

std::pair<std::vector<std::string>, std::vector<std::string>> LayeredMemory::promote_durable(
    const std::vector<std::pair<std::string, std::string>>& promotions) {
    if (!durable_store_.has_value()) return {};
    const auto result = durable_store_->promote(promotions);
    normalize();
    return result;
}

}  // namespace runi
