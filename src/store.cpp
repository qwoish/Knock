#include "knock/store.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <fstream>
#include <iomanip>
#include <limits>
#include <locale>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace knock {
namespace {
constexpr std::size_t max_state_bytes = 64 * 1024 * 1024;
constexpr std::size_t max_records = 10000;
constexpr std::size_t max_list_bytes = 800 * 1024;
constexpr std::size_t max_note_tags = 12;
constexpr std::size_t max_tag_bytes = 128;
constexpr std::size_t max_tag_codepoints = 32;
constexpr std::size_t max_note_messages = 2000;
constexpr std::size_t max_message_bytes = 64 * 1024;
constexpr std::size_t max_message_images = 8;
constexpr std::int64_t day_ms = 24 * 60 * 60 * 1000;

Json default_preferences() {
    return {{"saved_replies", Json::array({
                "I'm using the computer. Continue with headless tests.",
                "Finish everything else first, then ask again.",
                "Explain what you need to test before using the mouse."})},
            {"busy_until", 0}, {"trash_retention_days", 30}};
}

Json default_presence() {
    return {{"state", "unknown"}, {"message", ""}, {"updated_at", 0}};
}

Json empty_state() {
    return {{"schema", 1}, {"requests", Json::array()}, {"jobs", Json::array()},
            {"tasks", Json::array()}, {"notes", Json::array()}, {"trash", Json::array()},
            {"inbox", Json::array()}, {"agents", Json::array()},
            {"preferences", default_preferences()}, {"presence", default_presence()}, {"lease", nullptr}};
}

bool has_text(const std::string& value) {
    return std::any_of(value.begin(), value.end(), [](unsigned char c) {
        return !std::isspace(c);
    });
}

// Agents often pipe colored terminal output into a summary. Keep the words and
// line breaks, but drop escape sequences and other control characters so the
// card and clipboard receive plain text.
std::string plain_text(const std::string& value) {
    std::string result;
    result.reserve(value.size());
    for (std::size_t i = 0; i < value.size(); ++i) {
        const auto c = static_cast<unsigned char>(value[i]);
        if (c == 0x1B) {
            if (i + 1 >= value.size()) break;
            const auto kind = value[i + 1];
            if (kind == '[') {
                i += 2;
                while (i < value.size() && !(static_cast<unsigned char>(value[i]) >= 0x40 &&
                                             static_cast<unsigned char>(value[i]) <= 0x7E)) ++i;
            } else if (kind == ']' || kind == 'P' || kind == '_' || kind == '^') {
                i += 2;
                while (i < value.size() && value[i] != '\a' &&
                       !(value[i] == 0x1B && i + 1 < value.size() && value[i + 1] == '\\')) ++i;
                if (i < value.size() && value[i] == 0x1B) ++i;
            } else ++i;
            continue;
        }
        if (c == '\r') {
            result.push_back('\n');
            if (i + 1 < value.size() && value[i + 1] == '\n') ++i;
            continue;
        }
        if ((c < 0x20 && c != '\n' && c != '\t') || c == 0x7F) continue;
        // C1 controls are two UTF-8 bytes: 0xC2 0x80..0x9F.
        if (c == 0xC2 && i + 1 < value.size() && static_cast<unsigned char>(value[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(value[i + 1]) <= 0x9F) { ++i; continue; }
        result.push_back(value[i]);
    }
    return result;
}

std::string ascii_lower(std::string value) {
    for (auto& c : value) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    return value;
}

// A task belongs to a chat when its operator is the chat label, or its title
// starts with "LABEL:". Comparison ignores ASCII case only.
bool task_matches_chat(const Json& task, const std::string& chat) {
    const auto label = ascii_lower(chat);
    if (ascii_lower(task.value("operator", "")) == label) return true;
    const auto title = ascii_lower(task.value("title", ""));
    return title.size() > label.size() && title.starts_with(label) && title[label.size()] == ':';
}

std::string text_field(const Json& object, const char* field, std::size_t limit,
                       bool required = false, std::string fallback = {}) {
    const auto it = object.find(field);
    if (it == object.end()) {
        if (required) throw std::runtime_error(std::string("missing_field: ") + field);
        return fallback;
    }
    if (!it->is_string())
        throw std::runtime_error(std::string("invalid_string: ") + field);
    const auto value = it->get<std::string>();
    if (value.size() > limit)
        throw std::runtime_error(std::string("field_too_long: ") + field);
    if (value.find('\0') != std::string::npos)
        throw std::runtime_error(std::string("invalid_nul: ") + field);
    if (required && !has_text(value))
        throw std::runtime_error(std::string("empty_field: ") + field);
    // A Json constructed by C++ callers can contain invalid UTF-8 even though
    // the pipe JSON parser already rejects it.
    (void)Json(value).dump();
    return value;
}

// A title or status is shown on one line of the agents list.
std::string one_line(const Json& object, const char* field) {
    auto value = plain_text(text_field(object, field, 160, true));
    for (auto& c : value) if (c == '\n' || c == '\t') c = ' ';
    const auto first = value.find_first_not_of(' ');
    if (first == std::string::npos) throw std::runtime_error(std::string("empty_field: ") + field);
    return value.substr(first, value.find_last_not_of(' ') - first + 1);
}

std::string report_path(const Json& object) {
    const auto report = text_field(object, "report", 32768);
    if (std::any_of(report.begin(), report.end(), [](unsigned char c) { return c < 0x20 || c == 0x7F; }))
        throw std::runtime_error("invalid_report_path");
    return report;
}

Json note_tags(const Json& object) {
    const auto it = object.find("tags");
    if (it == object.end()) return Json::array();
    if (!it->is_array()) throw std::runtime_error("invalid_array: tags");
    if (it->size() > max_note_tags) throw std::runtime_error("too_many_tags: maximum 12");

    Json result = Json::array();
    std::set<std::string> seen;
    for (const auto& input : *it) {
        if (!input.is_string()) throw std::runtime_error("invalid_string: tag");
        auto tag = input.get<std::string>();
        const auto first = tag.find_first_not_of(' ');
        if (first == std::string::npos) throw std::runtime_error("empty_field: tag");
        tag = tag.substr(first, tag.find_last_not_of(' ') - first + 1);
        if (tag.size() > max_tag_bytes) throw std::runtime_error("field_too_long: tag");
        (void)Json(tag).dump(); // Validate UTF-8 before decoding scalar values below.

        std::size_t characters = 0;
        for (std::size_t offset = 0; offset < tag.size();) {
            const auto first_byte = static_cast<unsigned char>(tag[offset++]);
            const unsigned int trailing = first_byte < 0x80 ? 0 :
                (first_byte < 0xE0 ? 1 : (first_byte < 0xF0 ? 2 : 3));
            std::uint32_t codepoint = trailing ? first_byte & (0x3Fu >> trailing) : first_byte;
            for (unsigned int index = 0; index < trailing; ++index)
                codepoint = (codepoint << 6) | (static_cast<unsigned char>(tag[offset++]) & 0x3Fu);
            if (codepoint < 0x20 || (codepoint >= 0x7F && codepoint <= 0x9F) ||
                codepoint == 0x2028 || codepoint == 0x2029)
                throw std::runtime_error("invalid_control: tag");
            if (++characters > max_tag_codepoints) throw std::runtime_error("field_too_long: tag");
        }

        // Stable on every platform and locale. Preserve non-ASCII spelling;
        // full Unicode case folding would need a separate Unicode dependency.
        auto key = tag;
        for (auto& character : key)
            if (character >= 'A' && character <= 'Z') character += 'a' - 'A';
        if (seen.insert(std::move(key)).second) result.push_back(std::move(tag));
    }
    return result;
}

Json note_images(const Json& object) {
    const auto found = object.find("images");
    if (found == object.end()) return Json::array();
    if (!found->is_array()) throw std::runtime_error("invalid_array: images");
    if (found->size() > max_message_images) throw std::runtime_error("too_many_images: maximum 8");
    for (const auto& value : *found) {
        if (!value.is_string()) throw std::runtime_error("invalid_string: image");
        const auto name = value.get<std::string>();
        if (name.size() < 5 || name.size() > 132 || !name.ends_with(".png"))
            throw std::runtime_error("invalid_image_name");
        const auto stem = std::string_view(name).substr(0, name.size() - 4);
        if (!std::all_of(stem.begin(), stem.end(), [](unsigned char character) {
                return (character >= 'a' && character <= 'z') ||
                       (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '-' || character == '_';
            }))
            throw std::runtime_error("invalid_image_name");
    }
    return *found;
}

void require_message_content(const std::string& text, const Json& images) {
    if (!has_text(text) && images.empty()) throw std::runtime_error("empty_message");
}

std::string message_client_id(const Json& object, bool required = false) {
    const auto found = object.find("client_id");
    if (found == object.end()) {
        if (required) throw std::runtime_error("missing_field: client_id");
        return {};
    }
    if (!found->is_string()) throw std::runtime_error("invalid_client_id");
    const auto value = found->get<std::string>();
    if (value.size() != 32 || !std::all_of(value.begin(), value.end(), [](unsigned char ch) {
            return (ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f');
        })) throw std::runtime_error("invalid_client_id");
    return value;
}

Json message_view(const Json& message) {
    auto view = message;
    view.erase("append_payload");
    if (!view.contains("revision")) view["revision"] = 0;
    return view;
}

Json note_view(const Json& note, bool preview_body = false) {
    Json view = Json::object();
    for (const auto* field : {"id", "title", "body", "tags", "created_at", "updated_at", "deleted_at"})
        if (note.contains(field)) view[field] = note[field];
    view["message_count"] = note.contains("messages") ? note["messages"].size() : 0;
    if (preview_body) {
        const auto& body = note.at("body").get_ref<const std::string&>();
        if (body.size() > 512) {
            std::size_t length = 512;
            while (length && (static_cast<unsigned char>(body[length]) & 0xC0) == 0x80) --length;
            view["body"] = body.substr(0, length);
            view["body_truncated"] = true;
        }
    }
    return view;
}

std::int64_t integer_field(const Json& object, const char* field, std::int64_t low,
                           std::int64_t high, std::int64_t fallback,
                           bool required = false) {
    const auto it = object.find(field);
    if (it == object.end()) {
        if (required) throw std::runtime_error(std::string("missing_field: ") + field);
        return fallback;
    }
    if (!it->is_number_integer())
        throw std::runtime_error(std::string("invalid_integer: ") + field);
    if (it->is_number_unsigned() && it->get<std::uint64_t>() >
            static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw std::runtime_error(std::string("integer_out_of_range: ") + field);
    const auto value = it->get<std::int64_t>();
    if (value < low || value > high)
        throw std::runtime_error(std::string("integer_out_of_range: ") + field);
    return value;
}

std::string id_field(const Json& object) {
    const auto id = text_field(object, "id", 128, true);
    if (!std::all_of(id.begin(), id.end(), [](unsigned char c) {
            return std::isalnum(c) || c == '-' || c == '_' || c == '.';
        }))
        throw std::runtime_error("invalid_id");
    return id;
}

void validate_preferences(const Json& preferences) {
    if (!preferences.is_object()) throw std::runtime_error("invalid_preferences");
    integer_field(preferences, "busy_until", 0, INT64_MAX, 0);
    integer_field(preferences, "trash_retention_days", 1, 365, 30);
    if (preferences.contains("saved_replies")) {
        const auto& replies = preferences["saved_replies"];
        if (!replies.is_array()) throw std::runtime_error("invalid_array: saved_replies");
        if (replies.size() > 12) throw std::runtime_error("too_many_saved_replies: maximum 12");
        for (const auto& reply : replies)
            text_field(Json{{"reply", reply}}, "reply", 2000, true);
    }
}

bool is_one_of(const std::string& value, std::initializer_list<const char*> options) {
    return std::any_of(options.begin(), options.end(), [&](const char* option) {
        return value == option;
    });
}

void require_fields(const Json& record, std::initializer_list<const char*> fields) {
    for (const auto* field : fields) {
        if (!record.contains(field)) throw std::runtime_error(std::string("missing_field: ") + field);
    }
}

void validate_presence(const Json& presence) {
    if (!presence.is_object()) throw std::runtime_error("invalid_presence");
    require_fields(presence, {"state", "message", "updated_at"});
    if (!is_one_of(text_field(presence, "state", 16, true), {"unknown", "at_pc", "away", "busy"}))
        throw std::runtime_error("invalid_presence_state");
    text_field(presence, "message", 500);
    integer_field(presence, "updated_at", 0, INT64_MAX, 0, true);
}

Json& find_record(Json& records, const std::string& id) {
    for (auto& record : records) {
        if (record.at("id") == id) return record;
    }
    throw std::runtime_error("not_found: " + id);
}

Json record_summary(const Json& record) {
    Json result = Json::object();
    for (const auto* field : {"id", "chat", "title", "kind", "status", "operator", "project", "tags",
                             "created_at", "updated_at", "finished_at", "resolved_at", "expires_at",
                             "lease_seconds", "lease_until", "released_at", "release_reason",
                             "exit_code", "worker_pid", "archived", "deleted_at"}) {
        if (record.contains(field)) result[field] = record[field];
    }
    if (record.contains("messages")) result["message_count"] = record["messages"].size();
    return result;
}

Json list_records(const Json& state, const Json& command) {
    const auto collection = text_field(command, "collection", 16, true);
    if (!is_one_of(collection, {"requests", "jobs", "tasks", "notes", "trash", "inbox"}))
        throw std::runtime_error("invalid_collection");
    const auto offset = static_cast<std::uint64_t>(integer_field(command, "offset", 0, INT64_MAX, 0));
    const auto limit = static_cast<std::size_t>(integer_field(command, "limit", 1, 100, 50));
    if (command.contains("summaries") && !command["summaries"].is_boolean())
        throw std::runtime_error("invalid_boolean: summaries");
    const bool summaries = command.value("summaries", false);
    const auto& records = state[collection];
    const auto total = records.size();
    Json result{{"ok", true}, {"items", Json::array()}, {"total", total}, {"next_offset", nullptr}};
    if (offset >= total) return result;

    // Count encoded JSON bytes, including escaping, rather than source string
    // lengths. Reserve enough digits for next_offset before filling the page.
    std::size_t encoded_bytes = result.dump().size() + std::to_string(total).size();
    auto& items = result["items"];
    const auto start = static_cast<std::size_t>(offset);
    for (std::size_t index = start; index < total && items.size() < limit; ++index) {
        const auto& record = records[total - 1 - index];
        Json item = summaries ? record_summary(record) :
            (collection == "notes" || collection == "trash" ? note_view(record) : record);
        const auto item_bytes = item.dump().size() + (items.empty() ? 0 : 1);
        if (encoded_bytes + item_bytes > max_list_bytes) {
            if (items.empty()) throw std::runtime_error("list_item_exceeds_page_budget: use summaries=true");
            break;
        }
        encoded_bytes += item_bytes;
        items.push_back(std::move(item));
    }
    const auto next = start + items.size();
    if (next < total) result["next_offset"] = next;
    return result;
}

Json read_note(const Json& note, const Json& command) {
    const auto offset = static_cast<std::uint64_t>(integer_field(command, "offset", 0, INT64_MAX, 0));
    const auto limit = static_cast<std::size_t>(integer_field(command, "limit", 1, 100, 100));
    const auto& messages = note.at("messages");
    const auto total = messages.size();
    Json view = note_view(note);
    view["messages"] = Json::array();
    Json result{{"ok", true}, {"note", std::move(view)}, {"offset", offset},
                {"total_messages", total}, {"next_offset", nullptr}};
    if (offset >= total) return result;
    auto& page = result["note"]["messages"];
    std::size_t encoded_bytes = result.dump().size() + std::to_string(total).size();
    const auto start = static_cast<std::size_t>(offset);
    for (std::size_t index = start; index < total && page.size() < limit; ++index) {
        auto message = message_view(messages[index]);
        const auto message_bytes = message.dump().size() + (page.empty() ? 0 : 1);
        if (encoded_bytes + message_bytes > max_list_bytes) {
            if (page.empty()) throw std::runtime_error("note_message_exceeds_page_budget");
            break;
        }
        encoded_bytes += message_bytes;
        page.push_back(std::move(message));
    }
    const auto next = start + page.size();
    if (next < total) result["next_offset"] = next;
    return result;
}

struct TextMatch {
    std::size_t offset;
    std::size_t length;
};

// Search uses the same ordinal Windows comparison as the title picker. The
// portable test build uses the C.UTF-8 wide-character facet without changing
// the process locale; if unavailable, it preserves non-ASCII characters.
class NoteMatcher {
public:
    explicit NoteMatcher(const std::string& query) {
#ifdef _WIN32
        query_ = wide(query);
#else
        for (std::size_t offset = 0; offset < query.size();)
            query_.push_back(fold(decode(query, offset)));
        prefix_.resize(query_.size());
        for (std::size_t index = 1, matched = 0; index < query_.size(); ++index) {
            while (matched && query_[index] != query_[matched]) matched = prefix_[matched - 1];
            if (query_[index] == query_[matched]) ++matched;
            prefix_[index] = matched;
        }
#endif
    }

    std::optional<TextMatch> find(const std::string& text) const {
        if (text.empty()) return std::nullopt;
#ifdef _WIN32
        const auto value = wide(text);
        const auto found = FindStringOrdinal(FIND_FROMSTART, value.data(), static_cast<int>(value.size()),
            query_.data(), static_cast<int>(query_.size()), TRUE);
        if (found < 0) return std::nullopt;
        const auto offset = utf8_length(value.data(), found);
        const auto length = utf8_length(value.data() + found, static_cast<int>(query_.size()));
        return TextMatch{offset, length};
#else
        // KMP keeps repeated-prefix searches linear even in a large note. Only
        // the last query-length byte positions are needed to locate an excerpt.
        std::vector<std::size_t> starts(query_.size());
        std::size_t matched = 0, characters = 0;
        for (std::size_t offset = 0; offset < text.size(); ++characters) {
            starts[characters % starts.size()] = offset;
            const auto character = fold(decode(text, offset));
            while (matched && character != query_[matched]) matched = prefix_[matched - 1];
            if (character == query_[matched]) ++matched;
            if (matched == query_.size()) {
                const auto first = starts[(characters + 1 - matched) % starts.size()];
                return TextMatch{first, offset - first};
            }
        }
        return std::nullopt;
#endif
    }

private:
#ifdef _WIN32
    std::wstring query_;

    static std::wstring wide(const std::string& text) {
        if (text.empty()) return {};
        const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
            static_cast<int>(text.size()), nullptr, 0);
        if (!count) throw std::runtime_error("invalid_search_text");
        std::wstring result(static_cast<std::size_t>(count), L'\0');
        if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()),
                                result.data(), count)) throw std::runtime_error("invalid_search_text");
        return result;
    }

    static std::size_t utf8_length(const wchar_t* text, int count) {
        if (!count) return 0;
        const int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text, count, nullptr, 0, nullptr, nullptr);
        if (!length) throw std::runtime_error("invalid_search_match");
        return static_cast<std::size_t>(length);
    }
#else
    std::vector<wchar_t> query_;
    std::vector<std::size_t> prefix_;

    static wchar_t decode(const std::string& text, std::size_t& offset) {
        const auto lead = static_cast<unsigned char>(text[offset++]);
        const unsigned int trailing = lead < 0x80 ? 0 : (lead < 0xE0 ? 1 : (lead < 0xF0 ? 2 : 3));
        std::uint32_t value = trailing ? lead & (0x3Fu >> trailing) : lead;
        for (unsigned int index = 0; index < trailing; ++index)
            value = (value << 6) | (static_cast<unsigned char>(text[offset++]) & 0x3Fu);
        return static_cast<wchar_t>(value);
    }

    static wchar_t fold(wchar_t character) {
        static const std::locale locale = [] {
            try { return std::locale("C.UTF-8"); }
            catch (const std::runtime_error&) { return std::locale::classic(); }
        }();
        return std::use_facet<std::ctype<wchar_t>>(locale).tolower(character);
    }
#endif
};

std::string search_excerpt(const std::string& text, TextMatch match) {
    auto first = match.offset > 64 ? match.offset - 64 : 0;
    while (first && (static_cast<unsigned char>(text[first]) & 0xC0) == 0x80) --first;
    auto last = std::min(text.size(), std::min(first + 768,
        std::max(first + 256, match.offset + match.length + 64)));
    while (last < text.size() && last && (static_cast<unsigned char>(text[last]) & 0xC0) == 0x80) --last;
    auto excerpt = text.substr(first, last - first);
    for (auto& character : excerpt)
        if (character == '\r' || character == '\n' || character == '\t') character = ' ';
    return (first ? "…" : "") + excerpt + (last < text.size() ? "…" : "");
}

Json search_notes(const Json& state, const Json& command) {
    const NoteMatcher matcher(text_field(command, "query", 512, true));
    const auto offset = static_cast<std::uint64_t>(integer_field(command, "offset", 0, INT64_MAX, 0));
    const auto limit = static_cast<std::size_t>(integer_field(command, "limit", 1, 100, 50));
    std::vector<const Json*> notes;
    if (command.contains("id")) {
        const auto id = id_field(command);
        for (const auto& note : state["notes"])
            if (note["id"] == id) notes.push_back(&note);
        if (notes.empty()) throw std::runtime_error("not_found: " + id);
    } else {
        for (auto note = state["notes"].rbegin(); note != state["notes"].rend(); ++note) notes.push_back(&*note);
        std::stable_sort(notes.begin(), notes.end(), [](const Json* left, const Json* right) {
            return (*left)["updated_at"].get<std::int64_t>() > (*right)["updated_at"].get<std::int64_t>();
        });
    }
    Json result{{"ok", true}, {"items", Json::array()}, {"total", 0}, {"next_offset", nullptr}};
    auto& items = result["items"];
    std::size_t total = 0, bytes = result.dump().size() + 40;
    bool page_full = false;
    for (const auto* note : notes) {
        const auto& messages = (*note)["messages"];
        for (std::size_t index = 0; index < messages.size(); ++index) {
            const auto& message = messages[index];
            const auto& text = message["text"].get_ref<const std::string&>();
            const auto match = matcher.find(text);
            if (!match) continue;
            if (total++ < offset || items.size() >= limit || page_full) continue;
            Json item{{"note_id", (*note)["id"]}, {"title", (*note)["title"]},
                {"message_id", message["id"]}, {"message_index", index},
                {"excerpt", search_excerpt(text, *match)}, {"created_at", message["created_at"]}};
            const auto length = item.dump().size() + (items.empty() ? 0 : 1);
            if (bytes + length > max_list_bytes) { page_full = true; continue; }
            bytes += length;
            items.push_back(std::move(item));
        }
    }
    result["total"] = total;
    if (offset < total && items.size() < total - offset) result["next_offset"] = offset + items.size();
    return result;
}

Json note_search_locations(const Json& note, const Json& command) {
    const NoteMatcher matcher(text_field(command, "query", 512, true));
    Json items = Json::array();
    const auto& messages = note["messages"];
    for (std::size_t index = 0; index < messages.size(); ++index) {
        const auto& message = messages[index];
        if (matcher.find(message["text"].get_ref<const std::string&>()))
            items.push_back({{"message_id", message["id"]}, {"message_index", index}});
    }
    const auto count = items.size();
    Json result{{"ok", true}, {"items", std::move(items)}, {"total", count}};
    // At most 2000 IDs of at most 128 bytes fit comfortably. Keep the response
    // bound explicit if those storage limits are ever increased.
    if (result.dump().size() > max_list_bytes) throw std::runtime_error("search_locations_exceed_page_budget");
    return result;
}

void room_for_record(const Json& records) {
    if (records.size() >= max_records) throw std::runtime_error("history_limit_reached");
}

Json event(std::string type, const Json* record = nullptr, std::string message = {}) {
    Json result = {{"type", std::move(type)}, {"id", ""}, {"chat", ""},
                   {"title", ""}, {"message", std::move(message)}};
    if (record) {
        result["id"] = record->value("id", "");
        result["chat"] = record->value("chat", "");
        result["title"] = record->value("title", record->value("chat", ""));
        if (result["message"] == "") result["message"] = record->value("message", "");
    }
    return result;
}

void deliver(const ChangeCallback& callback, const std::vector<Json>& events) noexcept {
    if (!callback) return;
    for (const auto& notification : events) {
        // Persistence has committed. A UI callback failure must not turn a
        // successful operation into an apparent failure that gets retried.
        try { callback(notification); } catch (...) {}
    }
}

// The owner's rule: away means the mouse and keyboard are granted, unless the message
// withholds them. Reading free text can only err one way here: a message that merely
// looks like a refusal leaves the request with the person, as it always was.
bool away_grants_input(const Json& presence) {
    if (presence.value("state", "") != "away") return false;
    std::set<std::string> words;
    std::string word;
    const auto message = ascii_lower(presence.value("message", "")) + " ";
    for (std::size_t i = 0; i < message.size(); ++i) {
        const auto byte = static_cast<unsigned char>(message[i]);
        // An apostrophe, straight or curly, stays inside its word: don't, can't.
        if (message[i] == '\'') continue;
        if (message.compare(i, 3, "\xE2\x80\x99") == 0) { i += 2; continue; }
        if (byte < 0x80 && std::isalnum(byte)) { word.push_back(message[i]); continue; }
        if (!word.empty()) words.insert(word);
        word.clear();
    }
    const auto any = [&](std::initializer_list<const char*> list) {
        return std::any_of(list.begin(), list.end(), [&](const char* item) { return words.contains(item); });
    };
    if (any({"headless", "withhold", "withheld", "withholding"}) || (words.contains("hands") && words.contains("off")))
        return false;
    return !(any({"no", "not", "never", "without", "avoid", "dont", "cant", "cannot", "stop", "ask", "deny", "denied", "wait"}) &&
             any({"input", "inputs", "mouse", "keyboard", "desktop", "screen", "screens", "touch", "use"}));
}

void grant_lease(Json& state, Json& request, std::int64_t now) {
    const auto until = now + request["lease_seconds"].get<std::int64_t>() * 1000;
    request["lease_until"] = until;
    state["lease"] = {{"id", request["id"]}, {"chat", request["chat"]}, {"until", until}};
}

// Moves the line for the mouse and keyboard along when the lease is free. A request
// that has been waiting its turn goes to the person, or is granted while they are away.
// Without one of those events the person keeps whatever is already in front of them:
// a change of presence alone grants nothing.
std::optional<Json> advance_input_line(Json& state, std::int64_t now, bool lease_ended, bool arrival) {
    if (!state["lease"].is_null()) return std::nullopt;
    const auto head = std::find_if(state["requests"].begin(), state["requests"].end(), [](const Json& request) {
        return request["kind"] == "input" && request["status"] == "pending";
    });
    if (head == state["requests"].end()) return std::nullopt;
    if (!head->value("queued", false) && !lease_ended && !arrival) return std::nullopt;
    head->erase("queued");
    if (away_grants_input(state["presence"])) {
        grant_lease(state, *head, now);
        (*head)["status"] = "approved";
        (*head)["response"] = "Away";
        (*head)["granted_by"] = "away";
        (*head)["resolved_at"] = now;
        return event("updated", &*head);
    }
    if (arrival) return std::nullopt;
    return event("request", &*head);
}

Json quiet_place(const Json& state, const Json& window, std::int64_t now) {
    std::int64_t wait_ms = 0;
    std::size_t position = 1, ahead = 0;
    Json holder;
    for (const auto& other : state["quiet_windows"]) {
        if (other["id"] == window["id"]) break;
        if (other["status"] == "open") {
            wait_ms += std::max<std::int64_t>(0, other["until"].get<std::int64_t>() - now);
            holder = other;
            ++ahead;
        } else if (other["status"] == "waiting") {
            wait_ms += other["minutes"].get<std::int64_t>() * 60000;
            ++position;
            ++ahead;
        }
    }
    Json place{{"position", position}, {"ahead", ahead}, {"expected_wait_seconds", (wait_ms + 999) / 1000},
               {"expected_at", now + wait_ms},
               {"may_expire_first", now + wait_ms > window.value("expires_at", std::numeric_limits<std::int64_t>::max())}};
    if (!holder.is_null()) {
        place["holder"] = holder["chat"];
        place["holder_until"] = holder["until"];
    }
    return place;
}

void open_quiet_window(Json& window, std::int64_t now) {
    window["status"] = "open";
    window["opened_at"] = now;
    window["until"] = now + window["minutes"].get<std::int64_t>() * 60000;
    window.erase("expires_at");
}

void end_quiet_window(Json& window, std::int64_t now, const char* reason) {
    window["status"] = "ended";
    window["ended_at"] = now;
    window["end_reason"] = reason;
    window.erase("expires_at");
}

// One at a time: when nothing is open, the longest-waiting window opens and its clock
// starts now, not when it first asked.
void advance_quiet_line(Json& state, std::int64_t now) {
    auto& windows = state["quiet_windows"];
    const bool open = std::any_of(windows.begin(), windows.end(), [](const Json& window) { return window["status"] == "open"; });
    if (open) return;
    for (auto& window : windows) {
        if (window["status"] == "waiting") {
            open_quiet_window(window, now);
            return;
        }
    }
}

// Finished windows are only history. Keep a short tail of them, never an open one.
void trim_quiet_windows(Json& state) {
    constexpr std::size_t keep = 20;
    auto& windows = state["quiet_windows"];
    auto ended = std::count_if(windows.begin(), windows.end(), [](const Json& window) { return window["status"] == "ended"; });
    for (auto it = windows.begin(); ended > static_cast<std::ptrdiff_t>(keep) && it != windows.end();) {
        if ((*it)["status"] == "ended") { it = windows.erase(it); --ended; }
        else ++it;
    }
}

// True when a window's time is up, so expiry only saves when something changed.
bool quiet_window_due(const Json& window, std::int64_t now) {
    if (window["status"] == "open") return window["until"].get<std::int64_t>() <= now;
    return window["status"] == "waiting" && window["expires_at"].get<std::int64_t>() <= now;
}

bool expire_records(Json& state, std::int64_t now, bool* lease_ended = nullptr) {
    bool changed = false;
    for (auto& request : state["requests"]) {
        if (request["status"] == "pending" && request["expires_at"].get<std::int64_t>() <= now) {
            request["status"] = "expired";
            request["resolved_at"] = now;
            request["response"] = "";
            request.erase("queued");
            changed = true;
        }
    }
    if (!state["lease"].is_null() && state["lease"]["until"].get<std::int64_t>() <= now) {
        auto& request = find_record(state["requests"], state["lease"]["id"].get<std::string>());
        request["status"] = "released";
        request["released_at"] = now;
        request["release_reason"] = "expired";
        state["lease"] = nullptr;
        changed = true;
        if (lease_ended) *lease_ended = true;
    }
    if (state.contains("quiet_windows")) {
        bool quiet_changed = false;
        for (auto& window : state["quiet_windows"]) {
            if (!quiet_window_due(window, now)) continue;
            end_quiet_window(window, now, window["status"] == "open" ? "expired" : "timed_out");
            quiet_changed = true;
        }
        if (quiet_changed) {
            advance_quiet_line(state, now);
            changed = true;
        }
    }
    if (state.contains("trash")) {
        const auto cutoff = now - state["preferences"]["trash_retention_days"].get<std::int64_t>() * day_ms;
        auto& trash = state["trash"];
        const auto count = trash.size();
        trash.erase(std::remove_if(trash.begin(), trash.end(), [&](const Json& note) {
            return note["deleted_at"].get<std::int64_t>() <= cutoff;
        }), trash.end());
        changed = changed || trash.size() != count;
    }
    return changed;
}

void validate_state(const Json& state) {
    if (!state.is_object() || integer_field(state, "schema", 1, 1, 0, true) != 1)
        throw std::runtime_error("unsupported_state_schema");
    if (!state.contains("lease")) throw std::runtime_error("missing_lease");
    if (state.contains("preferences")) validate_preferences(state["preferences"]);
    if (state.contains("presence")) validate_presence(state["presence"]);
    std::set<std::string> ids;
    for (const auto* collection : {"requests", "jobs", "tasks", "notes", "trash", "inbox", "agents"}) {
        if ((std::string_view(collection) == "trash" || std::string_view(collection) == "inbox" ||
             std::string_view(collection) == "agents") && !state.contains(collection)) continue;
        if (!state.contains(collection) || !state[collection].is_array() ||
            state[collection].size() > max_records)
            throw std::runtime_error(std::string("invalid_collection: ") + collection);
        for (const auto& record : state[collection]) {
            if (!record.is_object()) throw std::runtime_error("invalid_record");
            if (!ids.insert(id_field(record)).second) throw std::runtime_error("duplicate_id");
            integer_field(record, "created_at", 0, INT64_MAX, 0, true);
            if (record.contains("archived") && !record["archived"].is_boolean())
                throw std::runtime_error("invalid_archived");
            const std::string category(collection);
            if (category == "requests") {
                require_fields(record, {"chat", "message", "kind", "status", "response", "lease_seconds", "lease_until", "expires_at"});
                text_field(record, "chat", 64, true);
                text_field(record, "message", 16000, true);
                if (!is_one_of(text_field(record, "kind", 16, true), {"input", "question"}))
                    throw std::runtime_error("invalid_request_kind");
                if (!is_one_of(text_field(record, "status", 16, true),
                    {"pending", "approved", "denied", "custom", "expired", "cancelled", "released"}))
                    throw std::runtime_error("invalid_request_status");
                text_field(record, "response", 16000, false);
                integer_field(record, "lease_seconds", 1, 3600, 0, true);
                integer_field(record, "lease_until", 0, INT64_MAX, 0, true);
                integer_field(record, "expires_at", record["created_at"].get<std::int64_t>(), INT64_MAX, 0, true);
                if (record["status"] == "custom") text_field(record, "response", 16000, true);
                if (record["status"] != "pending") integer_field(record, "resolved_at", 0, INT64_MAX, 0, true);
                if (record["status"] == "pending" &&
                    (record["response"] != "" || record["lease_until"] != 0 || record.value("archived", false)))
                    throw std::runtime_error("inconsistent_pending_request");
                if (record["kind"] == "question" && (record["lease_until"] != 0 || record["status"] == "released"))
                    throw std::runtime_error("inconsistent_question_lease");
            } else if (category == "agents") {
                require_fields(record, {"chat", "title", "status", "status_text", "report", "started_at", "updated_at"});
                text_field(record, "chat", 64, true);
                text_field(record, "title", 160, true);
                text_field(record, "status_text", 160);
                text_field(record, "report", 32768);
                if (!is_one_of(text_field(record, "status", 16, true), {"working", "done"}))
                    throw std::runtime_error("invalid_agent_status");
                integer_field(record, "started_at", 0, INT64_MAX, 0, true);
                integer_field(record, "updated_at", 0, INT64_MAX, 0, true);
            } else if (category == "inbox") {
                require_fields(record, {"chat", "status", "summary", "report", "finished_at", "updated_at"});
                text_field(record, "chat", 64, true);
                if (text_field(record, "status", 16, true) != "done") throw std::runtime_error("invalid_inbox_status");
                text_field(record, "summary", 16000, true);
                text_field(record, "report", 32768);
                integer_field(record, "finished_at", 0, INT64_MAX, 0, true);
                integer_field(record, "updated_at", 0, INT64_MAX, 0, true);
                if (record.contains("tasks_completed")) {
                    if (!record["tasks_completed"].is_array()) throw std::runtime_error("invalid_tasks_completed");
                    for (const auto& task : record["tasks_completed"])
                        if (!task.is_string()) throw std::runtime_error("invalid_tasks_completed");
                }
            } else if (category == "jobs") {
                require_fields(record, {"chat", "title", "command", "cwd", "log_path", "status", "exit_code", "output_tail", "resume_text"});
                text_field(record, "chat", 64, true);
                const auto status = text_field(record, "status", 16, true);
                if (!is_one_of(status, {"running", "completed", "failed", "notice"}))
                    throw std::runtime_error("invalid_job_status");
                text_field(record, "title", status == "notice" ? 16000 : 160, true);
                text_field(record, "command", 64000);
                text_field(record, "cwd", 32768);
                text_field(record, "log_path", 32768);
                text_field(record, "output_tail", 16000);
                text_field(record, "resume_text", 16000);
                text_field(record, "error", 16000);
                integer_field(record, "worker_pid", 0, UINT32_MAX, 0);
                if (status == "running") {
                    if (!record.contains("exit_code") || !record["exit_code"].is_null())
                        throw std::runtime_error("invalid_running_exit_code");
                } else {
                    integer_field(record, "exit_code", INT32_MIN, UINT32_MAX, 0, true);
                    integer_field(record, "finished_at", 0, INT64_MAX, 0, true);
                    if ((status == "completed" || status == "notice") && record["exit_code"] != 0)
                        throw std::runtime_error("inconsistent_success_exit_code");
                    if (status == "failed" && record["exit_code"] == 0)
                        throw std::runtime_error("inconsistent_failed_exit_code");
                }
            } else {
                require_fields(record, {"title", "body", "updated_at"});
                text_field(record, "title", 160, true);
                text_field(record, "body", 64000);
                integer_field(record, "updated_at", 0, INT64_MAX, 0, true);
                if (category == "tasks") {
                    require_fields(record, {"operator", "project", "status"});
                    text_field(record, "operator", 64);
                    text_field(record, "project", 160);
                    if (!is_one_of(text_field(record, "status", 16, true), {"todo", "in_progress", "done"}))
                        throw std::runtime_error("invalid_task_status");
                } else {
                    if (category == "trash") integer_field(record, "deleted_at", 0, INT64_MAX, 0, true);
                    else if (record.contains("deleted_at")) throw std::runtime_error("active_note_has_deleted_at");
                    (void)note_tags(record);
                    if (record.contains("messages")) {
                        const auto& messages = record["messages"];
                        if (!messages.is_array() || messages.size() > max_note_messages)
                            throw std::runtime_error("invalid_note_messages");
                        std::set<std::string> message_ids;
                        std::set<std::string> client_ids;
                        for (const auto& message : messages) {
                            if (!message.is_object()) throw std::runtime_error("invalid_note_message");
                            require_fields(message, {"id", "text", "images", "created_at"});
                            if (!message_ids.insert(id_field(message)).second)
                                throw std::runtime_error("duplicate_message_id");
                            integer_field(message, "created_at", 0, INT64_MAX, 0, true);
                            const auto revision = integer_field(message, "revision", 0, INT64_MAX, 0);
                            if (message.contains("edited_at")) {
                                integer_field(message, "edited_at", 0, INT64_MAX, 0, true);
                                if (!revision) throw std::runtime_error("invalid_message_revision");
                            } else if (revision) throw std::runtime_error("missing_message_edit_time");
                            const auto text = text_field(message, "text", max_message_bytes);
                            require_message_content(text, note_images(message));
                            const auto client_id = message_client_id(message);
                            if (!client_id.empty() && !client_ids.insert(client_id).second)
                                throw std::runtime_error("duplicate_message_client_id");
                            if (message.contains("append_payload")) {
                                const auto& payload = message["append_payload"];
                                if (client_id.empty() || !revision || !payload.is_object())
                                    throw std::runtime_error("invalid_append_payload");
                                require_fields(payload, {"text", "images"});
                                require_message_content(text_field(payload, "text", max_message_bytes), note_images(payload));
                            }
                        }
                    }
                }
            }
        }
    }
    if (state["notes"].size() + (state.contains("trash") ? state["trash"].size() : 0) > max_records)
        throw std::runtime_error("invalid_note_count");
    if (!state["lease"].is_null()) {
        const auto& lease = state["lease"];
        if (!lease.is_object()) throw std::runtime_error("invalid_lease");
        const auto id = id_field(lease);
        const auto chat = text_field(lease, "chat", 64, true);
        const auto until = integer_field(lease, "until", 1, INT64_MAX, 0, true);
        bool found = false;
        for (const auto& request : state["requests"]) {
            if (request["id"] == id) {
                found = request["kind"] == "input" && request["status"] == "approved" &&
                        request["chat"] == chat && request["lease_until"] == until;
                break;
            }
        }
        if (!found) throw std::runtime_error("inconsistent_lease");
    }
    if (state.contains("quiet_windows")) {
        const auto& windows = state["quiet_windows"];
        if (!windows.is_array() || windows.size() > max_records)
            throw std::runtime_error("invalid_collection: quiet_windows");
        std::size_t open = 0;
        for (const auto& window : windows) {
            if (!window.is_object()) throw std::runtime_error("invalid_record");
            if (!ids.insert(id_field(window)).second) throw std::runtime_error("duplicate_id");
            require_fields(window, {"chat", "reason", "minutes", "status", "created_at"});
            text_field(window, "chat", 64, true);
            text_field(window, "reason", 160, true);
            integer_field(window, "minutes", 1, 240, 0, true);
            integer_field(window, "created_at", 0, INT64_MAX, 0, true);
            const auto status = text_field(window, "status", 16, true);
            if (!is_one_of(status, {"open", "waiting", "ended"})) throw std::runtime_error("invalid_quiet_status");
            if (status == "open") {
                ++open;
                integer_field(window, "opened_at", 0, INT64_MAX, 0, true);
                integer_field(window, "until", 1, INT64_MAX, 0, true);
            } else if (status == "waiting") {
                integer_field(window, "expires_at", 0, INT64_MAX, 0, true);
            } else {
                integer_field(window, "ended_at", 0, INT64_MAX, 0, true);
                text_field(window, "end_reason", 16, true);
            }
        }
        if (open > 1) throw std::runtime_error("inconsistent_quiet_windows");
    }
}

void atomic_write(const std::filesystem::path& destination, const std::string& bytes) {
    const auto temporary = destination.parent_path() / (".state-" + make_id() + ".tmp");
#ifdef _WIN32
    HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        throw std::system_error(GetLastError(), std::system_category(), "create state temporary file");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            DWORD written = 0;
            if (!WriteFile(file, bytes.data() + offset, static_cast<DWORD>(bytes.size() - offset), &written, nullptr) || !written)
                throw std::system_error(GetLastError(), std::system_category(), "write state");
            offset += written;
        }
        if (!FlushFileBuffers(file))
            throw std::system_error(GetLastError(), std::system_category(), "flush state");
        if (!CloseHandle(file)) {
            file = INVALID_HANDLE_VALUE;
            throw std::system_error(GetLastError(), std::system_category(), "close state");
        }
        file = INVALID_HANDLE_VALUE;
        if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "replace state");
    } catch (...) {
        if (file != INVALID_HANDLE_VALUE) CloseHandle(file);
        DeleteFileW(temporary.c_str());
        throw;
    }
#else
    int file = ::open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (file < 0) throw std::system_error(errno, std::generic_category(), "create state temporary file");
    try {
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto written = ::write(file, bytes.data() + offset, bytes.size() - offset);
            if (written < 0 && errno == EINTR) continue;
            if (written <= 0) throw std::system_error(errno, std::generic_category(), "write state");
            offset += static_cast<std::size_t>(written);
        }
        if (::fsync(file) != 0) throw std::system_error(errno, std::generic_category(), "flush state");
        if (::close(file) != 0) {
            file = -1;
            throw std::system_error(errno, std::generic_category(), "close state");
        }
        file = -1;
        if (::rename(temporary.c_str(), destination.c_str()) != 0)
            throw std::system_error(errno, std::generic_category(), "replace state");
        // The rename is the commit point. Do not report rollback after it.
        const int directory = ::open(destination.parent_path().c_str(), O_RDONLY | O_DIRECTORY);
        if (directory >= 0) { (void)::fsync(directory); ::close(directory); }
    } catch (...) {
        if (file >= 0) ::close(file);
        ::unlink(temporary.c_str());
        throw;
    }
#endif
}
} // namespace

Json input_queue_place(const Json& state, const Json& request, std::int64_t now) {
    if (request.value("kind", "") != "input" || request.value("status", "") != "pending") return nullptr;
    std::int64_t wait_ms = 0;
    std::size_t older = 0;
    const auto& lease = state["lease"];
    if (!lease.is_null()) wait_ms += std::max<std::int64_t>(0, lease["until"].get<std::int64_t>() - now);
    for (const auto& other : state["requests"]) {
        if (other["id"] == request["id"]) break;
        if (other["kind"] == "input" && other["status"] == "pending") {
            ++older;
            wait_ms += other["lease_seconds"].get<std::int64_t>() * 1000;
        }
    }
    const auto ahead = older + (lease.is_null() ? 0 : 1);
    if (!ahead) return nullptr;
    Json place{{"position", older + 1}, {"ahead", ahead}, {"expected_wait_seconds", (wait_ms + 999) / 1000},
               {"expected_at", now + wait_ms},
               {"may_expire_first", now + wait_ms > request.value("expires_at", std::numeric_limits<std::int64_t>::max())}};
    if (!lease.is_null()) {
        place["holder"] = lease["chat"];
        place["holder_until"] = lease["until"];
    }
    return place;
}

bool input_request_queued(const Json& state, const Json& request) {
    if (request.value("kind", "") != "input" || request.value("status", "") != "pending") return false;
    return !state["lease"].is_null() || request.value("queued", false);
}

Json quiet_window_view(const Json& state, std::int64_t now) {
    Json view{{"open", false}, {"waiting", 0}};
    if (!state.contains("quiet_windows")) return view;
    std::size_t waiting = 0;
    for (const auto& window : state["quiet_windows"]) {
        if (window["status"] == "waiting") ++waiting;
        if (window["status"] != "open") continue;
        const auto until = window["until"].get<std::int64_t>();
        view["open"] = true;
        view["id"] = window["id"];
        view["chat"] = window["chat"];
        view["reason"] = window["reason"];
        view["opened_at"] = window["opened_at"];
        view["until"] = until;
        view["remaining_seconds"] = (std::max<std::int64_t>(0, until - now) + 999) / 1000;
    }
    view["waiting"] = waiting;
    return view;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string make_id() {
    static std::atomic<std::uint64_t> sequence{0};
    std::random_device source;
    const auto random = (static_cast<std::uint64_t>(source()) << 32) ^ source();
    std::ostringstream stream;
    stream << std::hex << now_ms() << '-' << random << '-' << sequence.fetch_add(1);
    return stream.str();
}

Store::Store(std::filesystem::path directory) : directory_(std::move(directory)), state_(empty_state()) {}
Store::~Store() { stop(); }

void Store::load() {
    std::lock_guard lock(mutex_);
    loaded_ = false;
    std::filesystem::create_directories(directory_);
    const auto path = directory_ / "state.json";
    Json loaded = empty_state();
    const bool exists = std::filesystem::exists(path);
    if (exists) {
        const auto size = std::filesystem::file_size(path);
        if (!size || size > max_state_bytes) throw std::runtime_error("state file is empty or too large; original preserved");
        std::ifstream input(path, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read state file; original preserved");
        std::string contents(static_cast<std::size_t>(size), '\0');
        input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
        if (!input || input.gcount() != static_cast<std::streamsize>(contents.size()))
            throw std::runtime_error("cannot read complete state file; original preserved");
        try {
            loaded = Json::parse(contents);
            validate_state(loaded);
        } catch (const std::exception& error) {
            throw std::runtime_error(std::string("invalid state file; original preserved: ") + error.what());
        }
    }
    bool dirty = !exists;
    if (!loaded.contains("trash")) { loaded["trash"] = Json::array(); dirty = true; }
    if (!loaded.contains("inbox")) { loaded["inbox"] = Json::array(); dirty = true; }
    // Added in memory only: opening a 0.7.0 state must not rewrite the file.
    if (!loaded.contains("agents")) loaded["agents"] = Json::array();
    if (!loaded.contains("quiet_windows")) loaded["quiet_windows"] = Json::array();
    if (!loaded.contains("preferences")) { loaded["preferences"] = default_preferences(); dirty = true; }
    if (!loaded.contains("presence")) { loaded["presence"] = default_presence(); dirty = true; }
    const auto defaults = default_preferences();
    for (const auto& [key, value] : defaults.items()) {
        if (!loaded["preferences"].contains(key)) { loaded["preferences"][key] = value; dirty = true; }
    }
    // Tags are additive in schema 1. Preserve old notes and their timestamps
    // while making the field available consistently to UI and CLI readers.
    for (const auto* collection : {"notes", "trash"}) for (auto& note : loaded[collection]) {
        auto tags = note_tags(note);
        if (!note.contains("tags") || note["tags"] != tags) {
            note["tags"] = std::move(tags);
            dirty = true;
        }
        if (!note.contains("messages")) {
            note["messages"] = Json::array();
            const auto body = note["body"].get<std::string>();
            if (has_text(body)) {
                note["messages"].push_back({{"id", make_id()}, {"text", body},
                    {"images", Json::array()}, {"created_at", note["updated_at"]}});
            }
            dirty = true;
        }
        for (auto& message : note["messages"]) {
            if (!message.contains("revision")) { message["revision"] = 0; dirty = true; }
        }
    }
    const auto now = now_ms();
    // An old Yes must not regain control after the tray process has restarted.
    for (auto& request : loaded["requests"]) {
        if (request["kind"] == "input" && request["status"] == "approved") {
            request["status"] = "released";
            request["released_at"] = now;
            request["release_reason"] = "restart";
            dirty = true;
        }
    }
    if (!loaded["lease"].is_null()) { loaded["lease"] = nullptr; dirty = true; }
    dirty = expire_records(loaded, now) || dirty;
    // A restart ends the lease, so the next in line can be answered. Nothing is granted
    // here: whoever was waiting lost their connection and may not be there any more.
    for (auto& request : loaded["requests"]) {
        if (request["kind"] != "input" || request["status"] != "pending") continue;
        if (request.value("queued", false)) { request.erase("queued"); dirty = true; }
        break;
    }
    auto previous = std::move(state_);
    state_ = std::move(loaded);
    try {
        if (dirty) save_locked();
        stopped_ = false;
        loaded_ = true;
    } catch (...) {
        state_ = std::move(previous);
        throw;
    }
}

void Store::save_locked() {
    const auto contents = state_.dump(2) + '\n';
    if (contents.size() > max_state_bytes) throw std::runtime_error("state_size_limit_reached");
    atomic_write(directory_ / "state.json", contents);
}

bool Store::expire_locked() {
    const auto now = now_ms();
    bool due = !state_["lease"].is_null() && state_["lease"]["until"].get<std::int64_t>() <= now;
    if (!due) {
        for (const auto& request : state_["requests"]) {
            if (request["status"] == "pending" && request["expires_at"].get<std::int64_t>() <= now) {
                due = true;
                break;
            }
        }
    }
    if (!due) {
        due = std::any_of(state_["quiet_windows"].begin(), state_["quiet_windows"].end(), [&](const Json& window) {
            return quiet_window_due(window, now);
        });
    }
    if (!due && !state_["trash"].empty()) {
        const auto cutoff = now - state_["preferences"]["trash_retention_days"].get<std::int64_t>() * day_ms;
        due = std::any_of(state_["trash"].begin(), state_["trash"].end(), [&](const Json& note) {
            return note["deleted_at"].get<std::int64_t>() <= cutoff;
        });
    }
    if (!due) return false;
    auto previous = state_;
    bool lease_ended = false;
    expire_records(state_, now, &lease_ended);
    auto moved = advance_input_line(state_, now, lease_ended, false);
    try { save_locked(); }
    catch (...) { state_ = std::move(previous); throw; }
    if (moved) line_events_.push_back(std::move(*moved));
    return true;
}

void Store::set_changed(ChangeCallback callback) {
    std::lock_guard lock(mutex_);
    callback_ = std::move(callback);
}

void Store::stop() {
    {
        std::lock_guard lock(mutex_);
        stopped_ = true;
    }
    changed_.notify_all();
}

void Store::tick() {
    ChangeCallback callback;
    bool expired = false;
    std::vector<Json> events;
    {
        std::lock_guard lock(mutex_);
        if (!loaded_ || stopped_) return;
        expired = expire_locked();
        callback = callback_;
        events.swap(line_events_);
    }
    if (expired) {
        changed_.notify_all();
        events.insert(events.begin(), event("updated"));
        deliver(callback, events);
    }
}

Json Store::snapshot() {
    auto result = handle({{"op", "snapshot"}});
    if (!result.value("ok", false)) throw std::runtime_error(result.value("error", "snapshot failed"));
    return std::move(result["state"]);
}

Json Store::handle(const Json& command, bool trusted_ui) {
    std::unique_lock lock(mutex_);
    Json result;
    std::vector<Json> events;
    ChangeCallback callback;
    try {
        if (!loaded_) throw std::runtime_error("store_not_loaded");
        if (stopped_) throw std::runtime_error("store_stopped");
        if (!command.is_object()) throw std::runtime_error("command_must_be_object");
        const auto op = text_field(command, "op", 64, true);
        if (op == "request.wait") {
            const auto id = id_field(command);
            const auto seconds = integer_field(command, "wait_seconds", 1, 86400, 86400);
            if (command.contains("stop_when_unavailable") && !command["stop_when_unavailable"].is_boolean())
                throw std::runtime_error("invalid_boolean: stop_when_unavailable");
            const bool stop_when_unavailable = command.value("stop_when_unavailable", false);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            for (;;) {
                if (stopped_) throw std::runtime_error("store_stopped");
                if (expire_locked()) {
                    const auto changed_callback = callback_;
                    std::vector<Json> expired{event("updated")};
                    for (auto& moved : line_events_) expired.push_back(std::move(moved));
                    line_events_.clear();
                    lock.unlock();
                    changed_.notify_all();
                    deliver(changed_callback, expired);
                    lock.lock();
                    continue;
                }
                const auto& request = find_record(state_["requests"], id);
                if (request["status"] != "pending" || std::chrono::steady_clock::now() >= deadline) {
                    result = {{"ok", true}, {"request", request}};
                    break;
                }
                // Away is no reason to give up a place in the line: the turn will be
                // granted when it comes. Anything else is the person's to answer.
                const bool turn_will_be_granted = away_grants_input(state_["presence"]) &&
                    !input_queue_place(state_, request, now_ms()).is_null();
                if (stop_when_unavailable && !turn_will_be_granted &&
                    is_one_of(state_["presence"]["state"].get<std::string>(), {"away", "busy"})) {
                    result = {{"ok", true}, {"request", request}, {"wait_reason", "presence"}};
                    break;
                }
                const auto now = now_ms();
                auto remaining = std::max<std::int64_t>(1, request["expires_at"].get<std::int64_t>() - now);
                if (!state_["lease"].is_null())
                    remaining = std::min(remaining, std::max<std::int64_t>(1, state_["lease"]["until"].get<std::int64_t>() - now));
                const auto wake = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining));
                changed_.wait_until(lock, wake);
            }
        } else if (op == "quiet.wait") {
            const auto id = id_field(command);
            const auto seconds = integer_field(command, "wait_seconds", 1, 86400, 86400);
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
            for (;;) {
                if (stopped_) throw std::runtime_error("store_stopped");
                if (expire_locked()) {
                    const auto changed_callback = callback_;
                    std::vector<Json> expired{event("updated")};
                    for (auto& moved : line_events_) expired.push_back(std::move(moved));
                    line_events_.clear();
                    lock.unlock();
                    changed_.notify_all();
                    deliver(changed_callback, expired);
                    lock.lock();
                    continue;
                }
                const auto& window = find_record(state_["quiet_windows"], id);
                if (window["status"] != "waiting" || std::chrono::steady_clock::now() >= deadline) {
                    result = {{"ok", true}, {"quiet", window}};
                    break;
                }
                const auto now = now_ms();
                auto remaining = std::max<std::int64_t>(1, window["expires_at"].get<std::int64_t>() - now);
                for (const auto& other : state_["quiet_windows"]) {
                    if (other["status"] == "open")
                        remaining = std::min(remaining, std::max<std::int64_t>(1, other["until"].get<std::int64_t>() - now));
                }
                const auto wake = std::min(deadline, std::chrono::steady_clock::now() + std::chrono::milliseconds(remaining));
                changed_.wait_until(lock, wake);
            }
        } else {
            if (expire_locked()) events.push_back(event("updated"));
            const bool may_mutate = !is_one_of(op, {"snapshot", "status", "presence.get", "list", "request.result", "job.result", "inbox.result", "note.get", "note.find_message", "note.message.get", "note.trash.list", "note.search", "note.search_locations", "quiet.status"});
            Json previous = may_mutate ? state_ : Json();
            bool dirty = false;
            Json notification;
            std::optional<Json> line_moved;
            try {
                const auto now = now_ms();
                if (op == "snapshot") {
                    Json snapshot = Json::object();
                    for (const auto* field : {"schema", "requests", "jobs", "tasks", "inbox", "agents", "lease", "preferences", "presence", "quiet_windows"})
                        snapshot[field] = state_[field];
                    snapshot["notes"] = Json::array();
                    for (const auto& note : state_["notes"])
                        snapshot["notes"].push_back(note_view(note, true));
                    snapshot["trash"] = Json::array();
                    for (const auto& note : state_["trash"])
                        snapshot["trash"].push_back(note_view(note, true));
                    result = {{"ok", true}, {"state", std::move(snapshot)}};
                } else if (op == "preferences.update") {
                    if (!trusted_ui) throw std::runtime_error("forbidden: GUI settings required");
                    auto preferences = state_["preferences"];
                    for (const auto* field : {"saved_replies", "busy_until", "trash_retention_days"})
                        if (command.contains(field)) preferences[field] = command[field];
                    validate_preferences(preferences);
                    dirty = state_["preferences"] != preferences;
                    state_["preferences"] = std::move(preferences);
                    bool lease_ended = false;
                    dirty = expire_records(state_, now, &lease_ended) || dirty;
                    line_moved = advance_input_line(state_, now, lease_ended, false);
                    result = {{"ok", true}, {"preferences", state_["preferences"]}};
                    notification = event("updated");
                    notification["operation"] = op;
                } else if (op == "presence.get") {
                    result = {{"ok", true}, {"presence", state_["presence"]}};
                } else if (op == "presence.update") {
                    if (!trusted_ui) throw std::runtime_error("forbidden: GUI presence settings required");
                    const auto presence_state = text_field(command, "state", 16, true);
                    if (!is_one_of(presence_state, {"at_pc", "away", "busy"}))
                        throw std::runtime_error("invalid_presence_state");
                    const auto message = text_field(command, "message", 500);
                    dirty = state_["presence"]["state"] != presence_state || state_["presence"]["message"] != message;
                    if (dirty)
                        state_["presence"] = {{"state", presence_state}, {"message", message}, {"updated_at", now}};
                    result = {{"ok", true}, {"presence", state_["presence"]}};
                    notification = event("updated");
                    notification["operation"] = op;
                } else if (op == "status") {
                    const auto count_status = [&](const char* collection, const char* status) {
                        const auto& records = state_[collection];
                        return std::count_if(records.begin(), records.end(), [&](const Json& record) {
                            return record["status"] == status;
                        });
                    };
                    result = {{"ok", true}, {"requests_pending", count_status("requests", "pending")},
                              {"jobs_running", count_status("jobs", "running")},
                              {"tasks_todo", count_status("tasks", "todo")},
                              {"notes", state_["notes"].size()}, {"lease", state_["lease"]},
                              {"presence", state_["presence"]}};
                    // Who else is on this PC right now, for an agent that is starting.
                    Json working = Json::array();
                    for (const auto& agent : state_["agents"]) {
                        if (agent["status"] != "working" || agent.value("archived", false)) continue;
                        working.push_back({{"chat", agent["chat"]}, {"title", agent["title"]},
                                           {"status_text", agent["status_text"]}, {"started_at", agent["started_at"]},
                                           {"updated_at", agent["updated_at"]}});
                    }
                    result["agents"] = std::move(working);
                    Json line = Json::array();
                    for (const auto& request : state_["requests"]) {
                        if (request["kind"] != "input" || request["status"] != "pending") continue;
                        const auto place = input_queue_place(state_, request, now);
                        Json item{{"id", request["id"]}, {"chat", request["chat"]}, {"lease_seconds", request["lease_seconds"]}};
                        if (!place.is_null()) item.update(place);
                        line.push_back(std::move(item));
                    }
                    result["input_queue"] = std::move(line);
                    result["quiet_window"] = quiet_window_view(state_, now);
                } else if (op == "quiet.status") {
                    Json waiting = Json::array();
                    for (const auto& window : state_["quiet_windows"]) {
                        if (window["status"] != "waiting") continue;
                        Json item{{"id", window["id"]}, {"chat", window["chat"]}, {"reason", window["reason"]},
                                  {"minutes", window["minutes"]}, {"expires_at", window["expires_at"]}};
                        item.update(quiet_place(state_, window, now));
                        waiting.push_back(std::move(item));
                    }
                    result = {{"ok", true}, {"quiet_window", quiet_window_view(state_, now)}, {"waiting", std::move(waiting)}};
                } else if (op == "quiet.open") {
                    const auto chat = text_field(command, "chat", 64, true);
                    const auto reason = one_line(command, "reason");
                    const auto minutes = integer_field(command, "minutes", 1, 240, 0, true);
                    const auto timeout = integer_field(command, "timeout_seconds", 1, 86400, 1800);
                    auto& windows = state_["quiet_windows"];
                    // Asking again keeps the place and the clock, so a dropped wait can
                    // be picked up with the same command.
                    const auto mine = std::find_if(windows.begin(), windows.end(), [&](const Json& window) {
                        return window["chat"] == chat && window["status"] != "ended";
                    });
                    if (mine != windows.end()) {
                        result = {{"ok", true}, {"quiet", *mine}, {"already", true}};
                    } else {
                        room_for_record(windows);
                        Json window{{"id", make_id()}, {"chat", chat}, {"reason", reason}, {"minutes", minutes},
                                    {"created_at", now}, {"status", "waiting"}, {"expires_at", now + timeout * 1000}};
                        const bool taken = std::any_of(windows.begin(), windows.end(), [](const Json& other) {
                            return other["status"] != "ended";
                        });
                        if (!taken) open_quiet_window(window, now);
                        windows.push_back(window);
                        trim_quiet_windows(state_);
                        result = {{"ok", true}, {"quiet", window}};
                        notification = event("updated", &window);
                        notification["operation"] = op;
                        dirty = true;
                    }
                } else if (op == "quiet.release") {
                    // Named by id or chat it ends only that one, whether open or still
                    // waiting. With neither it ends whatever is open.
                    auto& windows = state_["quiet_windows"];
                    const bool by_id = command.contains("id"), by_chat = command.contains("chat");
                    const auto id = by_id ? id_field(command) : std::string();
                    const auto chat = by_chat ? text_field(command, "chat", 64, true) : std::string();
                    const auto target = std::find_if(windows.begin(), windows.end(), [&](const Json& window) {
                        if (window["status"] == "ended") return false;
                        if (by_id) return window["id"] == id;
                        if (by_chat) return window["chat"] == chat;
                        return window["status"] == "open";
                    });
                    if (target == windows.end()) {
                        result = {{"ok", true}, {"quiet", nullptr}};
                    } else {
                        end_quiet_window(*target, now, (*target)["status"] == "open" ? "released" : "cancelled");
                        Json ended = *target;
                        advance_quiet_line(state_, now);
                        result = {{"ok", true}, {"quiet", ended}};
                        notification = event("updated", &ended);
                        notification["operation"] = op;
                        dirty = true;
                    }
                } else if (op == "list") {
                    result = list_records(state_, command);
                } else if (op == "note.trash.list") {
                    auto list = command;
                    list["collection"] = "trash";
                    result = list_records(state_, list);
                } else if (op == "note.restore") {
                    const auto id = id_field(command);
                    const auto active = std::find_if(state_["notes"].begin(), state_["notes"].end(),
                        [&](const Json& note) { return note["id"] == id; });
                    if (active != state_["notes"].end()) result = {{"ok", true}, {"note", note_view(*active)}};
                    else {
                        auto& trash = state_["trash"];
                        auto note = find_record(trash, id);
                        note.erase("deleted_at");
                        state_["notes"].push_back(note);
                        trash.erase(std::find_if(trash.begin(), trash.end(), [&](const Json& item) { return item["id"] == id; }));
                        result = {{"ok", true}, {"note", note_view(note)}};
                        notification = event("updated", &note);
                        notification["operation"] = op;
                        dirty = true;
                    }
                } else if (op == "note.purge") {
                    if (!trusted_ui) throw std::runtime_error("forbidden: GUI deletion required");
                    const auto id = id_field(command);
                    auto& trash = state_["trash"];
                    notification = event("updated", &find_record(trash, id));
                    notification["operation"] = op;
                    trash.erase(std::find_if(trash.begin(), trash.end(), [&](const Json& item) { return item["id"] == id; }));
                    result = {{"ok", true}};
                    dirty = true;
                } else if (op == "inbox.result") {
                    result = {{"ok", true}, {"done", find_record(state_["inbox"], id_field(command))}};
                } else if (op == "done") {
                    // One card per chat: finishing again replaces the summary and
                    // moves the record to the newest position.
                    const auto chat = text_field(command, "chat", 64, true);
                    const auto summary = plain_text(text_field(command, "summary", 16000, true));
                    if (!has_text(summary)) throw std::runtime_error("empty_field: summary");
                    auto report = report_path(command);
                    // The check-in may already have named the report.
                    auto& agents = state_["agents"];
                    const auto agent = std::find_if(agents.begin(), agents.end(),
                        [&](const Json& item) { return item["chat"] == chat; });
                    if (agent != agents.end()) {
                        if (report.empty()) report = agent->value("report", "");
                        (*agent)["status"] = "done";
                        (*agent)["finished_at"] = now;
                    }
                    Json completed = Json::array();
                    auto& tasks = state_["tasks"];
                    if (command.contains("task")) {
                        auto& task = find_record(tasks, id_field(Json{{"id", command["task"]}}));
                        if (task["status"] != "done") {
                            task["status"] = "done";
                            task["updated_at"] = now;
                            completed.push_back(task["id"]);
                        }
                    } else {
                        for (auto& task : tasks) {
                            if (task["status"] != "done" && task_matches_chat(task, chat)) {
                                task["status"] = "done";
                                task["updated_at"] = now;
                                completed.push_back(task["id"]);
                            }
                        }
                    }
                    auto& inbox = state_["inbox"];
                    const auto existing = std::find_if(inbox.begin(), inbox.end(),
                        [&](const Json& item) { return item["chat"] == chat; });
                    Json record;
                    if (existing != inbox.end()) {
                        record = *existing;
                        inbox.erase(existing);
                    } else {
                        room_for_record(inbox);
                        record = {{"id", make_id()}, {"chat", chat}, {"created_at", now}};
                    }
                    record["status"] = "done";
                    record["summary"] = summary;
                    record["report"] = report;
                    record["finished_at"] = now;
                    record["updated_at"] = now;
                    record["tasks_completed"] = completed;
                    record.erase("archived");
                    inbox.push_back(record);
                    result = {{"ok", true}, {"done", record}};
                    notification = event("done", &record, summary);
                    notification["title"] = chat;
                    dirty = true;
                } else if (op == "agent.start" || op == "agent.update") {
                    // One record per chat. Starting again replaces the check-in; a
                    // status from a job that never checked in also shows it working.
                    const bool start = op == "agent.start";
                    const auto chat = text_field(command, "chat", 64, true);
                    const auto line = one_line(command, start ? "title" : "status");
                    const auto report = start ? report_path(command) : std::string();
                    auto& agents = state_["agents"];
                    auto agent = std::find_if(agents.begin(), agents.end(),
                        [&](const Json& item) { return item["chat"] == chat; });
                    if (agent == agents.end()) {
                        room_for_record(agents);
                        agents.push_back({{"id", make_id()}, {"chat", chat}, {"title", line}, {"status_text", ""},
                                          {"report", report}, {"created_at", now}, {"started_at", now}});
                        agent = std::prev(agents.end());
                    } else if (start) {
                        (*agent)["title"] = line;
                        (*agent)["status_text"] = "";
                        (*agent)["report"] = report;
                        (*agent)["started_at"] = now;
                    } else {
                        (*agent)["status_text"] = line;
                    }
                    (*agent)["status"] = "working";
                    (*agent)["updated_at"] = now;
                    agent->erase("finished_at");
                    agent->erase("archived");
                    Json record = *agent;
                    result = {{"ok", true}, {"agent", record}};
                    notification = event("updated", &record);
                    notification["operation"] = op;
                    dirty = true;
                } else if (op == "job.result") {
                    result = {{"ok", true}, {"job", find_record(state_["jobs"], id_field(command))}};
                } else if (op == "note.get") {
                    result = read_note(find_record(state_["notes"], id_field(command)), command);
                } else if (op == "note.search") {
                    result = search_notes(state_, command);
                } else if (op == "note.search_locations") {
                    if (!trusted_ui) throw std::runtime_error("forbidden: GUI search required");
                    result = note_search_locations(find_record(state_["notes"], id_field(command)), command);
                } else if (op == "note.message.get") {
                    const auto note_id = id_field(command);
                    const auto& note = find_record(state_["notes"], note_id);
                    const auto message_id = text_field(command, "message_id", 128, true);
                    const auto& messages = note["messages"];
                    const auto found = std::find_if(messages.begin(), messages.end(), [&](const Json& message) {
                        return message["id"] == message_id;
                    });
                    if (found == messages.end()) throw std::runtime_error("message_not_found");
                    result = {{"ok", true}, {"note_id", note["id"]}, {"message", message_view(*found)},
                              {"message_index", static_cast<std::size_t>(found - messages.begin())},
                              {"message_count", messages.size()}};
                } else if (op == "note.find_message") {
                    const auto client_id = message_client_id(command, true);
                    const auto note_id = id_field(command);
                    const auto& note = find_record(state_["notes"], note_id);
                    const auto& messages = note["messages"];
                    const auto found = std::find_if(messages.begin(), messages.end(), [&](const Json& message) {
                        return message.value("client_id", "") == client_id;
                    });
                    result = {{"ok", true}, {"message", found == messages.end() ? Json() : message_view(*found)}};
                } else if (op == "note.append") {
                    auto& note = find_record(state_["notes"], id_field(command));
                    auto& messages = note["messages"];
                    const auto text = text_field(command, "text", max_message_bytes);
                    auto images = note_images(command);
                    require_message_content(text, images);
                    const auto client_id = message_client_id(command);
                    const auto found = client_id.empty() ? messages.end() :
                        std::find_if(messages.begin(), messages.end(), [&](const Json& message) {
                            return message.value("client_id", "") == client_id;
                        });
                    if (found != messages.end()) {
                        const auto& original = found->contains("append_payload") ? (*found)["append_payload"] : *found;
                        if (original["text"] != text || original["images"] != images)
                            throw std::runtime_error("message_client_id_conflict");
                        result = {{"ok", true}, {"note_id", note["id"]}, {"message", message_view(*found)},
                                  {"message_count", messages.size()}};
                    } else {
                        if (messages.size() >= max_note_messages)
                            throw std::runtime_error("note_message_limit_reached");
                        Json message{{"id", make_id()}, {"text", text}, {"images", std::move(images)},
                                     {"created_at", now}, {"revision", 0}};
                        if (!client_id.empty()) message["client_id"] = client_id;
                        messages.push_back(message);
                        note["updated_at"] = now;
                        result = {{"ok", true}, {"note_id", note["id"]}, {"message", std::move(message)},
                                  {"message_count", messages.size()}};
                        notification = event("updated", &note);
                        notification["operation"] = op;
                        dirty = true;
                    }
                } else if (op == "note.message.edit") {
                    auto& note = find_record(state_["notes"], id_field(command));
                    const auto message_id = text_field(command, "message_id", 128, true);
                    auto& messages = note["messages"];
                    auto found = std::find_if(messages.begin(), messages.end(), [&](const Json& message) {
                        return message["id"] == message_id;
                    });
                    if (found == messages.end()) throw std::runtime_error("message_not_found");
                    const auto revision = found->value("revision", std::int64_t{0});
                    const auto expected = integer_field(command, "expected_revision", 0, INT64_MAX, 0, true);
                    if (expected != revision) throw std::runtime_error("message_edit_conflict");
                    if (!command.contains("text")) throw std::runtime_error("missing_field: text");
                    const auto text = text_field(command, "text", max_message_bytes);
                    const auto images = command.contains("images") ? note_images(command) : (*found)["images"];
                    require_message_content(text, images);
                    if ((*found)["text"] != text || (*found)["images"] != images) {
                        if (revision == INT64_MAX) throw std::runtime_error("message_revision_limit_reached");
                        // Client IDs identify the original append, even after a
                        // later edit. Keep that payload private for exact retries.
                        if (found->contains("client_id") && !found->contains("append_payload"))
                            (*found)["append_payload"] = {{"text", (*found)["text"]}, {"images", (*found)["images"]}};
                        (*found)["text"] = text;
                        (*found)["images"] = images;
                        (*found)["revision"] = revision + 1;
                        (*found)["edited_at"] = now;
                        note["updated_at"] = now;
                        notification = event("updated", &note);
                        notification["operation"] = op;
                        dirty = true;
                    }
                    result = {{"ok", true}, {"note_id", note["id"]}, {"message", message_view(*found)},
                              {"message_count", messages.size()}};
                } else if (op == "note.message.remove") {
                    auto& note = find_record(state_["notes"], id_field(command));
                    const auto message_id = text_field(command, "message_id", 128, true);
                    auto& messages = note["messages"];
                    const auto found = std::find_if(messages.begin(), messages.end(), [&](const Json& message) {
                        return message["id"] == message_id;
                    });
                    if (found == messages.end()) throw std::runtime_error("message_not_found");
                    messages.erase(found);
                    note["updated_at"] = now;
                    result = {{"ok", true}, {"note_id", note["id"]}, {"message_count", messages.size()}};
                    notification = event("updated", &note);
                    notification["operation"] = op;
                    dirty = true;
                } else if (op == "request.create") {
                    room_for_record(state_["requests"]);
                    const auto kind = text_field(command, "kind", 16, false, "input");
                    if (!is_one_of(kind, {"input", "question"})) throw std::runtime_error("invalid_request_kind");
                    Json request = {{"id", make_id()}, {"chat", text_field(command, "chat", 64, true)},
                        {"message", text_field(command, "message", 16000, true)}, {"kind", kind},
                        {"created_at", now}, {"expires_at", now + 1000 * integer_field(command, "timeout_seconds", 1, 86400, 1800)},
                        {"status", "pending"}, {"response", ""},
                        {"lease_seconds", integer_field(command, "lease_seconds", 1, 3600, 300)}, {"lease_until", 0}};
                    // Questions need no input, so they never wait behind a lease.
                    const bool input = kind == "input";
                    if (input && !state_["lease"].is_null()) request["queued"] = true;
                    state_["requests"].push_back(request);
                    if (input && !request.contains("queued") && away_grants_input(state_["presence"])) {
                        line_moved = advance_input_line(state_, now, false, true);
                        // An older request took the turn; this one waits behind it.
                        auto& added = state_["requests"].back();
                        if (added["status"] == "pending") added["queued"] = true;
                    }
                    const auto& added = state_["requests"].back();
                    result = {{"ok", true}, {"request", added}};
                    // The person hears about a queued request when its turn comes, and
                    // about a granted one only by seeing who holds the lease.
                    notification = event(input && (added.contains("queued") || added["status"] != "pending") ? "updated" : "request", &added);
                    dirty = true;
                } else if (op == "request.result") {
                    result = {{"ok", true}, {"request", find_record(state_["requests"], id_field(command))}};
                } else if (op == "request.resolve") {
                    if (!trusted_ui) throw std::runtime_error("forbidden: GUI approval required");
                    auto& request = find_record(state_["requests"], id_field(command));
                    if (request["status"] != "pending") throw std::runtime_error("already_resolved");
                    const auto decision = text_field(command, "decision", 16, true);
                    if (!is_one_of(decision, {"approved", "denied", "custom"})) throw std::runtime_error("invalid_decision");
                    const auto response = decision == "custom" ? text_field(command, "text", 16000, true)
                        : (decision == "approved" ? std::string("Yes") : std::string("No"));
                    if (decision == "approved" && request["kind"] == "input") {
                        if (!state_["lease"].is_null()) throw std::runtime_error("input_busy");
                        // A request still waiting its turn cannot overtake the line.
                        if (request.value("queued", false)) throw std::runtime_error("input_queued");
                        grant_lease(state_, request, now);
                    }
                    request["status"] = decision;
                    request["response"] = response;
                    request["resolved_at"] = now;
                    request.erase("queued");
                    result = {{"ok", true}, {"request", request}};
                    notification = event("updated", &request);
                    line_moved = advance_input_line(state_, now, false, false);
                    dirty = true;
                } else if (op == "request.cancel") {
                    auto& request = find_record(state_["requests"], id_field(command));
                    if (request["status"] != "pending") throw std::runtime_error("already_resolved");
                    request["status"] = "cancelled";
                    request["response"] = "";
                    request["resolved_at"] = now;
                    request.erase("queued");
                    result = {{"ok", true}, {"request", request}};
                    notification = event("updated", &request);
                    line_moved = advance_input_line(state_, now, false, false);
                    dirty = true;
                } else if (op == "request.release") {
                    auto& request = find_record(state_["requests"], id_field(command));
                    if (request["kind"] != "input" || !is_one_of(request["status"].get<std::string>(), {"approved", "released"}))
                        throw std::runtime_error("no_active_lease");
                    if (request["status"] == "approved") {
                        if (state_["lease"].is_null() || state_["lease"]["id"] != request["id"])
                            throw std::runtime_error("no_active_lease");
                        state_["lease"] = nullptr;
                        request["status"] = "released";
                        request["released_at"] = now;
                        request["release_reason"] = "released";
                        dirty = true;
                        line_moved = advance_input_line(state_, now, true, false);
                    }
                    result = {{"ok", true}, {"request", request}};
                    notification = event("updated", &request);
                } else if (op == "job.create") {
                    room_for_record(state_["jobs"]);
                    Json job = {{"id", make_id()}, {"chat", text_field(command, "chat", 64, true)},
                        {"title", text_field(command, "title", 160, true)}, {"command", text_field(command, "command", 64000)},
                        {"cwd", text_field(command, "cwd", 32768)}, {"log_path", text_field(command, "log_path", 32768)},
                        {"worker_pid", integer_field(command, "worker_pid", 0, UINT32_MAX, 0)},
                        {"created_at", now}, {"status", "running"}, {"exit_code", nullptr},
                        {"output_tail", ""}, {"resume_text", ""}};
                    state_["jobs"].push_back(job);
                    result = {{"ok", true}, {"job", job}};
                    notification = event("updated", &job);
                    dirty = true;
                } else if (op == "job.started") {
                    auto& job = find_record(state_["jobs"], id_field(command));
                    if (job["status"] != "running") throw std::runtime_error("already_finished");
                    const auto pid = integer_field(command, "worker_pid", 1, UINT32_MAX, 0, true);
                    const auto previous_pid = job.value("worker_pid", std::int64_t{0});
                    if (previous_pid && previous_pid != pid) throw std::runtime_error("worker_already_started");
                    job["worker_pid"] = pid;
                    result = {{"ok", true}, {"job", job}};
                    notification = event("updated", &job);
                    dirty = previous_pid != pid;
                } else if (op == "job.finish") {
                    auto& job = find_record(state_["jobs"], id_field(command));
                    const auto exit_code = integer_field(command, "exit_code", INT32_MIN, UINT32_MAX, 0, true);
                    const auto output = text_field(command, "output_tail", 16000);
                    const auto resume = text_field(command, "resume_text", 16000);
                    const auto error = text_field(command, "error", 16000);
                    if (job["status"] != "running") {
                        if (!is_one_of(job["status"].get<std::string>(), {"completed", "failed"}) ||
                            job["exit_code"] != exit_code || job["output_tail"] != output ||
                            job["resume_text"] != resume || job.value("error", "") != error)
                            throw std::runtime_error("already_finished");
                    } else {
                        job["exit_code"] = exit_code;
                        job["status"] = exit_code == 0 ? "completed" : "failed";
                        job["finished_at"] = now;
                        job["output_tail"] = output;
                        job["resume_text"] = resume;
                        if (command.contains("error")) job["error"] = error;
                        dirty = true;
                    }
                    result = {{"ok", true}, {"job", job}};
                    notification = event("job", &job, job["title"].get<std::string>() +
                        (exit_code == 0 ? " finished." : " failed (exit " + std::to_string(exit_code) + ")."));
                } else if (op == "notify") {
                    room_for_record(state_["jobs"]);
                    Json job = {{"id", make_id()}, {"chat", text_field(command, "chat", 64, true)},
                        {"title", text_field(command, "message", 16000, true)}, {"command", ""}, {"cwd", ""},
                        {"log_path", ""}, {"worker_pid", 0}, {"created_at", now}, {"finished_at", now},
                        {"status", "notice"}, {"exit_code", 0}, {"output_tail", ""},
                        {"resume_text", text_field(command, "resume_text", 16000)}};
                    state_["jobs"].push_back(job);
                    result = {{"ok", true}, {"job", job}};
                    notification = event("job", &job, job["title"].get<std::string>());
                    dirty = true;
                } else if (op == "task.add" || op == "note.add") {
                    const bool task = op == "task.add";
                    auto& records = state_[task ? "tasks" : "notes"];
                    room_for_record(records);
                    if (!task && records.size() + state_["trash"].size() >= max_records)
                        throw std::runtime_error("history_limit_reached");
                    Json record = {{"id", make_id()}, {"title", text_field(command, "title", 160, true)},
                        {"body", text_field(command, "body", 64000)}, {"created_at", now}, {"updated_at", now}};
                    if (task) {
                        record["operator"] = text_field(command, "operator", 64, false, "ME");
                        record["project"] = text_field(command, "project", 160);
                        record["status"] = "todo";
                    } else {
                        record["tags"] = note_tags(command);
                        record["messages"] = Json::array();
                    }
                    records.push_back(record);
                    result = {{"ok", true}, {task ? "task" : "note", task ? record : note_view(record)}};
                    notification = event("updated", &record);
                    if (!task) notification["operation"] = op;
                    dirty = true;
                } else if (op == "task.update" || op == "note.update") {
                    const bool task = op == "task.update";
                    auto& record = find_record(state_[task ? "tasks" : "notes"], id_field(command));
                    if (command.contains("title")) record["title"] = text_field(command, "title", 160, true);
                    if (command.contains("body")) record["body"] = text_field(command, "body", 64000);
                    if (task) {
                        if (command.contains("operator")) record["operator"] = text_field(command, "operator", 64);
                        if (command.contains("project")) record["project"] = text_field(command, "project", 160);
                        if (command.contains("status")) {
                            const auto status = text_field(command, "status", 16, true);
                            if (!is_one_of(status, {"todo", "in_progress", "done"})) throw std::runtime_error("invalid_task_status");
                            record["status"] = status;
                        }
                    } else if (command.contains("tags")) {
                        record["tags"] = note_tags(command);
                    }
                    record["updated_at"] = now;
                    result = {{"ok", true}, {task ? "task" : "note", task ? record : note_view(record)}};
                    notification = event("updated", &record);
                    if (!task) notification["operation"] = op;
                    dirty = true;
                } else if (op == "task.delete" || op == "note.delete") {
                    auto& records = state_[op == "task.delete" ? "tasks" : "notes"];
                    const auto id = id_field(command);
                    const auto& record = find_record(records, id);
                    notification = event("updated", &record);
                    if (op == "note.delete") {
                        auto deleted = record;
                        deleted["deleted_at"] = now;
                        state_["trash"].push_back(std::move(deleted));
                        notification["operation"] = op;
                        result = {{"ok", true}, {"note", note_view(state_["trash"].back())}};
                    } else result = {{"ok", true}};
                    records.erase(std::find_if(records.begin(), records.end(), [&](const Json& item) { return item["id"] == id; }));
                    dirty = true;
                } else if (op == "item.dismiss") {
                    const auto kind = text_field(command, "kind", 16, true);
                    if (!is_one_of(kind, {"request", "job", "inbox", "agent"})) throw std::runtime_error("invalid_item_kind");
                    auto& record = find_record(state_[kind == "request" ? "requests" : kind == "job" ? "jobs" :
                                                      kind == "agent" ? "agents" : "inbox"],
                                               id_field(command));
                    if (record["status"] == "pending" || record["status"] == "running")
                        throw std::runtime_error("item_still_active");
                    record["archived"] = true;
                    result = {{"ok", true}};
                    notification = event("updated", &record);
                    dirty = true;
                } else {
                    throw std::runtime_error("unknown_operation: " + op);
                }
                if (dirty || line_moved) {
                    save_locked();
                    if (dirty) events.push_back(std::move(notification));
                    if (line_moved) events.push_back(std::move(*line_moved));
                }
            } catch (...) {
                if (may_mutate) state_ = std::move(previous);
                throw;
            }
        }
        events.insert(events.end(), line_events_.begin(), line_events_.end());
        line_events_.clear();
        const auto now = now_ms();
        if (result.contains("request") || result.contains("agent") || result.contains("quiet")) {
            result["presence"] = state_["presence"];
            result["quiet_window"] = quiet_window_view(state_, now);
        }
        if (result.contains("request")) {
            auto place = input_queue_place(state_, result["request"], now);
            if (!place.is_null()) result["queue"] = std::move(place);
        } else if (result.contains("quiet") && result["quiet"].is_object() && result["quiet"]["status"] == "waiting") {
            result["queue"] = quiet_place(state_, result["quiet"], now);
        }
    } catch (const std::exception& error) {
        result = {{"ok", false}, {"error", error.what()}};
    } catch (...) {
        result = {{"ok", false}, {"error", "unexpected_store_error"}};
    }
    callback = callback_;
    lock.unlock();
    if (!events.empty()) changed_.notify_all();
    deliver(callback, events);
    return result;
}
} // namespace knock
