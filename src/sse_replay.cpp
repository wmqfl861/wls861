#include "wls/sse_replay.hpp"

#include "wls/protocol_codec.hpp"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

class ReplayFailure : public std::runtime_error {
public:
    ReplayFailure(std::string code, const std::string& message)
        : std::runtime_error(message), code_(std::move(code)) {}
    const std::string& code() const noexcept { return code_; }
private:
    std::string code_;
};

void require(bool condition, const std::string& message) {
    if (!condition) throw ReplayFailure("invalid_sse", message);
}

void within(bool condition, const std::string& message) {
    if (!condition) throw ReplayFailure("stream_limit", message);
}

const Object& object(const Json& value) {
    require(value.is_object(), "SSE event and nested records must be JSON objects");
    return value.as_object();
}

const Json& field(const Object& source, std::string_view key) {
    const auto* value = source.if_contains(key);
    require(value != nullptr, "missing SSE field: " + std::string(key));
    return *value;
}

std::string text(const Object& source, std::string_view key) {
    const auto& value = field(source, key);
    require(value.is_string(), "SSE field must be a string: " + std::string(key));
    return std::string(value.as_string());
}

std::string identifier(const Object& source, std::string_view key) {
    auto value = text(source, key);
    require(!value.empty() && value.size() <= 256 && value.find('\0') == std::string::npos,
            "SSE identity is empty, contains NUL or exceeds its bound");
    return value;
}

std::uint64_t integer(const Object& source, std::string_view key) {
    const auto& value = field(source, key);
    if (value.is_int64() && value.as_int64() >= 0) return static_cast<std::uint64_t>(value.as_int64());
    if (value.is_uint64() && value.as_uint64() <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        return value.as_uint64();
    throw ReplayFailure("invalid_sse", "SSE index or sequence must be a bounded nonnegative integer");
}

const Array& array(const Object& source, std::string_view key) {
    const auto& value = field(source, key);
    require(value.is_array(), "SSE field must be an array: " + std::string(key));
    return value.as_array();
}

void allowed(const Object& value, const std::set<std::string>& names) {
    for (const auto& entry : value) {
        if (!names.contains(std::string(entry.key())))
            throw ReplayFailure("unsupported_output", "SSE output contains a field outside the implemented text/function subset");
    }
}

void empty_optional_array(const Object& value, std::string_view key) {
    if (const auto* found = value.if_contains(key)) {
        require(found->is_array(), "SSE output annotation/logprob field must be an array");
        if (!found->as_array().empty())
            throw ReplayFailure("unsupported_output", "SSE output annotations and logprobs are outside the implemented subset");
    }
}

Json normalize_part(const Json& value) {
    const auto& part = object(value);
    allowed(part, {"type", "text", "annotations", "logprobs"});
    if (text(part, "type") != "output_text")
        throw ReplayFailure("unsupported_output", "only output_text content is implemented by SSE replay");
    empty_optional_array(part, "annotations");
    empty_optional_array(part, "logprobs");
    return Object{{"type", "output_text"}, {"text", text(part, "text")}};
}

Json normalize_item(const Json& value, bool completed) {
    const auto& item = object(value);
    const auto type = text(item, "type");
    const auto id = identifier(item, "id");
    if (type == "message") {
        allowed(item, {"id", "type", "role", "status", "content"});
        require(text(item, "role") == "assistant", "SSE output message role must be assistant");
        const auto status = text(item, "status");
        require(completed ? status == "completed" :
                (status == "in_progress" || status == "incomplete" || status == "completed"),
                "SSE message status does not match its lifecycle");
        Array content;
        for (const auto& part : array(item, "content")) content.push_back(normalize_part(part));
        return Object{{"id", id}, {"type", type}, {"role", "assistant"},
                      {"status", status}, {"content", std::move(content)}};
    }
    if (type == "function_call") {
        allowed(item, {"id", "type", "call_id", "name", "arguments", "status"});
        const auto* status = item.if_contains("status");
        if (status) {
            require(status->is_string(), "SSE function status must be a string");
            const auto observed = std::string(status->as_string());
            require(completed ? observed == "completed" :
                    (observed == "in_progress" || observed == "incomplete" || observed == "completed"),
                    "SSE function status does not match its lifecycle");
        }
        // Function status is optional in the provider schema. Its absence
        // cannot hide different IDs, names or argument bytes.
        return Object{{"id", id}, {"type", type}, {"call_id", identifier(item, "call_id")},
                      {"name", identifier(item, "name")}, {"arguments", text(item, "arguments")}};
    }
    throw ReplayFailure("unsupported_output", "SSE replay implements only message/output_text and function_call outputs");
}

bool valid_utf8(std::string_view value) {
    for (std::size_t i = 0; i < value.size();) {
        const auto first = static_cast<unsigned char>(value[i++]);
        if (first < 0x80U) continue;
        std::uint32_t point = 0;
        std::uint32_t minimum = 0;
        std::size_t continuation = 0;
        if (first >= 0xc2U && first <= 0xdfU) { point = first & 0x1fU; minimum = 0x80U; continuation = 1; }
        else if (first >= 0xe0U && first <= 0xefU) { point = first & 0x0fU; minimum = 0x800U; continuation = 2; }
        else if (first >= 0xf0U && first <= 0xf4U) { point = first & 0x07U; minimum = 0x10000U; continuation = 3; }
        else return false;
        if (continuation > value.size() - i) return false;
        for (std::size_t j = 0; j < continuation; ++j) {
            const auto next = static_cast<unsigned char>(value[i++]);
            if ((next & 0xc0U) != 0x80U) return false;
            point = (point << 6U) | (next & 0x3fU);
        }
        if (point < minimum || point > 0x10ffffU || (point >= 0xd800U && point <= 0xdfffU)) return false;
    }
    return true;
}

// WHATWG framing (CR/LF/CRLF, one leading BOM, comments, multiline data,
// persistent id, ignored retry/unknown fields). Malformed UTF-8 is refused
// instead of replacing bytes, so original provider bytes stay unambiguous.
class Frames {
public:
    using Receiver = std::function<void(std::string_view, std::string_view)>;
    Frames(const SseReplayLimits& limits, Receiver receive)
        : limits_(limits), receive_(std::move(receive)) {}

    void feed(std::string_view bytes, std::stop_token stop) {
        for (const char character : bytes) {
            if (stop.stop_requested()) throw ReplayFailure("cancelled", "offline SSE replay cancelled");
            ++parsed_bytes;
            if (after_cr_) {
                after_cr_ = false;
                if (character == '\n') continue;
            }
            if (character == '\r' || character == '\n') {
                process_line();
                after_cr_ = character == '\r';
            } else {
                within(line_.size() < limits_.max_line_bytes, "SSE line exceeds its byte bound");
                line_.push_back(character);
            }
        }
    }

    bool has_pending_event() {
        // An unterminated final comment does not create a data event. Do not
        // promote an unterminated data field or JSON body into a final event.
        if (!line_.empty() && line_.front() == ':') {
            process_line();
        }
        return !line_.empty() || !data_.empty() || !event_.empty();
    }
    std::size_t events{};
    std::size_t comments{};
    std::size_t ignored_fields{};
    std::size_t retry_fields{};
    std::size_t parsed_bytes{};
    std::string last_event_id;

private:
    void process_line() {
        if (first_line_) {
            first_line_ = false;
            if (line_.starts_with("\xef\xbb\xbf")) line_.erase(0, 3);
        }
        require(valid_utf8(line_), "SSE input is not valid UTF-8");
        if (line_.empty()) {
            if (event_bytes_ != 0)
                within(limits_.max_event_bytes - event_bytes_ >= 2,
                       "SSE event delimiter exceeds its byte bound");
            if (!data_.empty()) {
                within(events < limits_.max_events, "SSE event count exceeds its bound");
                ++events;
                data_.pop_back(); // Remove only the final data-field newline.
                receive_(event_, data_);
            }
            data_.clear();
            event_.clear();
            event_bytes_ = 0;
            return;
        }
        // Charge two bytes for each terminator, including LF-only input. This
        // conservatively bounds the actual CRLF wire envelope before dispatch.
        within(line_.size() < limits_.max_event_bytes &&
               limits_.max_event_bytes - line_.size() >= 2 &&
               line_.size() + 2 <= limits_.max_event_bytes - event_bytes_,
               "SSE event exceeds its byte bound");
        event_bytes_ += line_.size() + 2;
        if (line_.front() == ':') { ++comments; line_.clear(); return; }
        const auto colon = line_.find(':');
        const std::string_view name(line_.data(), colon == std::string::npos ? line_.size() : colon);
        std::string_view value = colon == std::string::npos ? std::string_view{} :
            std::string_view(line_).substr(colon + 1);
        if (!value.empty() && value.front() == ' ') value.remove_prefix(1);
        if (name == "data") {
            within(value.size() < limits_.max_event_bytes &&
                   value.size() + 1 <= limits_.max_event_bytes - data_.size(),
                   "SSE event data exceeds its byte bound");
            data_.append(value);
            data_.push_back('\n');
        } else if (name == "event") event_ = value;
        else if (name == "id") {
            if (value.find('\0') == std::string_view::npos) {
                within(value.size() <= 256, "SSE event ID exceeds its byte bound");
                last_event_id = value;
            }
        } else if (name == "retry") ++retry_fields; // Never authorizes a retry.
        else ++ignored_fields;
        line_.clear();
    }

    const SseReplayLimits& limits_;
    Receiver receive_;
    std::string line_;
    std::string data_;
    std::string event_;
    std::size_t event_bytes_{};
    bool first_line_{true};
    bool after_cr_{false};
};

struct PartState {
    std::string text;
    bool text_done{false};
    bool part_done{false};
};

struct ItemState {
    std::string id;
    std::string type;
    std::string call_id;
    std::string name;
    std::string arguments;
    std::vector<PartState> parts;
    bool arguments_done{false};
    bool done{false};
    Json done_item = nullptr;
};

class Events {
public:
    explicit Events(const SseReplayLimits& limits, std::size_t max_depth)
        : limits_(limits), max_depth_(max_depth) {}

    void accept(std::string_view event_name, std::string_view data);
    Json observations() const;
    bool terminal{false};
    std::string response_id;
    std::string terminal_type;
    Json terminal_response = nullptr;
    std::optional<std::uint64_t> last_sequence;
    std::set<std::string> observed_types;
    std::size_t obfuscation_fields{};
    std::size_t obfuscation_bytes{};

private:
    ItemState& item(const Object& event);
    PartState& part(const Object& event);
    void response_start(const Object& event, bool first);
    void output_added(const Object& event);
    void content_added(const Object& event);
    void text_delta(const Object& event, bool done);
    void content_done(const Object& event);
    void arguments(const Object& event, bool done);
    void output_done(const Object& event);
    void finish(const Object& event, const std::string& type);
    void matches(const ItemState& state, const Json& value, bool completed) const;

    const SseReplayLimits& limits_;
    std::size_t max_depth_;
    bool created_{false};
    bool in_progress_{false};
    std::string model_;
    std::vector<ItemState> items_;
    std::set<std::string> item_ids_;
    std::set<std::string> call_ids_;
    std::size_t text_bytes_{};
    std::size_t argument_bytes_{};
};

void Events::accept(std::string_view event_name, std::string_view data) {
    Json decoded;
    try { decoded = parse_json_strict(data, limits_.max_event_bytes, max_depth_); }
    catch (const Error&) { throw ReplayFailure("invalid_sse", "SSE data is not one strict bounded JSON event"); }
    const auto& event = object(decoded);
    const auto type = text(event, "type");
    require(event_name.empty() || event_name == "message" || event_name == type,
            "SSE event field disagrees with its JSON type");
    if (terminal) throw ReplayFailure("event_after_terminal", "SSE data arrived after a terminal response");
    const auto sequence = integer(event, "sequence_number");
    if (!last_sequence) require(sequence <= 1, "SSE initial sequence must be zero or one");
    else require(*last_sequence < static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) &&
                 sequence == *last_sequence + 1, "SSE sequence is repeated, reordered or missing an event");
    last_sequence = sequence;
    observed_types.insert(type);
    std::set<std::string> fields{"type", "sequence_number"};
    if (type == "response.created" || type == "response.in_progress" ||
        type == "response.completed" || type == "response.failed" || type == "response.incomplete")
        fields.insert("response");
    else if (type == "response.output_item.added" || type == "response.output_item.done")
        fields.insert({"output_index", "item", "response_id"});
    else if (type == "response.content_part.added" || type == "response.content_part.done")
        fields.insert({"output_index", "item_id", "content_index", "part"});
    else if (type == "response.output_text.delta" || type == "response.output_text.done") {
        fields.insert({"output_index", "item_id", "content_index", "logprobs"});
        fields.insert(type == "response.output_text.delta" ? "delta" : "text");
        if (type == "response.output_text.delta") fields.insert("obfuscation");
    } else if (type == "response.function_call_arguments.delta" || type == "response.function_call_arguments.done") {
        fields.insert({"output_index", "item_id", "response_id"});
        fields.insert(type == "response.function_call_arguments.delta" ? "delta" : "arguments");
        if (type == "response.function_call_arguments.delta") fields.insert("obfuscation");
    } else throw ReplayFailure("unsupported_event", "SSE event type is outside the implemented Responses subset");
    for (const auto& entry : event) {
        if (!fields.contains(std::string(entry.key())))
            throw ReplayFailure("unsupported_event", "SSE event field is outside the implemented Responses subset");
    }
    // Responses includes optional random padding on delta events by default.
    // Preserve it in raw_stream and its wire budgets, never in model text or
    // function arguments. Other event kinds and unknown fields still refuse.
    if (const auto* padding = event.if_contains("obfuscation")) {
        require(padding->is_string(), "SSE delta obfuscation must be a string");
        ++obfuscation_fields;
        obfuscation_bytes += padding->as_string().size();
    }
    if (event.contains("response_id")) {
        const auto linked_response = identifier(event, "response_id");
        require(linked_response.size() <= 128 &&
                std::all_of(linked_response.begin(), linked_response.end(), [](char character) {
                    const auto byte = static_cast<unsigned char>(character);
                    return byte >= 33 && byte <= 126;
                }), "SSE response_id must be a bounded provider token");
        require(created_ && linked_response == response_id,
                "SSE response_id differs from the response.created identity");
    }
    if (type == "response.created") { response_start(event, true); return; }
    require(created_, "SSE must begin with response.created");
    if (type == "response.in_progress") response_start(event, false);
    else if (type == "response.output_item.added") output_added(event);
    else if (type == "response.content_part.added") content_added(event);
    else if (type == "response.output_text.delta") text_delta(event, false);
    else if (type == "response.output_text.done") text_delta(event, true);
    else if (type == "response.content_part.done") content_done(event);
    else if (type == "response.function_call_arguments.delta") arguments(event, false);
    else if (type == "response.function_call_arguments.done") arguments(event, true);
    else if (type == "response.output_item.done") output_done(event);
    else if (type == "response.completed" || type == "response.failed" || type == "response.incomplete")
        finish(event, type);
    else throw ReplayFailure("unsupported_event", "SSE event type is outside the implemented Responses subset");
}

void Events::response_start(const Object& event, bool first) {
    const auto& response = object(field(event, "response"));
    require(text(response, "object") == "response" && text(response, "status") == "in_progress",
            "SSE response start must be an in_progress response object");
    require(array(response, "output").empty(), "SSE resumed or prepopulated response is outside the implemented subset");
    if (first) {
        require(!created_, "SSE response.created cannot repeat");
        response_id = identifier(response, "id");
        model_ = identifier(response, "model");
        created_ = true;
    } else {
        require(!in_progress_ && items_.empty(), "SSE response.in_progress is repeated or follows output");
        require(identifier(response, "id") == response_id && identifier(response, "model") == model_,
                "SSE response identity changed during the attempt");
        in_progress_ = true;
    }
}

ItemState& Events::item(const Object& event) {
    const auto index = integer(event, "output_index");
    require(index < items_.size(), "SSE output index has not been added");
    auto& result = items_[static_cast<std::size_t>(index)];
    require(!result.done, "SSE modifies an already completed output item");
    if (event.contains("item_id"))
        require(identifier(event, "item_id") == result.id, "SSE item ID disagrees with its output index");
    return result;
}

PartState& Events::part(const Object& event) {
    auto& state = item(event);
    require(identifier(event, "item_id") == state.id, "SSE content item ID disagrees with its output index");
    require(state.type == "message", "SSE text/content event targets a non-message output");
    const auto index = integer(event, "content_index");
    require(index < state.parts.size(), "SSE content index has not been added");
    return state.parts[static_cast<std::size_t>(index)];
}

void Events::output_added(const Object& event) {
    const auto index = integer(event, "output_index");
    require(index == items_.size(), "SSE output indices must be introduced in order without gaps");
    within(items_.size() < limits_.max_output_items, "SSE output item count exceeds its bound");
    const auto& original = field(event, "item");
    const auto normalized = normalize_item(original, false);
    const auto& source = normalized.as_object();
    ItemState state;
    state.id = identifier(source, "id");
    state.type = text(source, "type");
    require(item_ids_.insert(state.id).second, "SSE output item ID is reused");
    const auto& original_object = object(original);
    if (const auto* status = original_object.if_contains("status"))
        require(status->is_string() && status->as_string() == "in_progress",
                "SSE newly added item must be in progress");
    if (state.type == "message") require(array(source, "content").empty(), "new SSE message content must be empty");
    else {
        within(call_ids_.size() < limits_.max_tool_calls, "SSE tool call count exceeds its bound");
        state.call_id = identifier(source, "call_id");
        state.name = identifier(source, "name");
        require(call_ids_.insert(state.call_id).second, "SSE provider function call ID is reused");
        require(text(source, "arguments").empty(), "new SSE function arguments must be empty");
    }
    items_.push_back(std::move(state));
}

void Events::content_added(const Object& event) {
    auto& state = item(event);
    require(identifier(event, "item_id") == state.id, "SSE content item ID disagrees with its output index");
    require(state.type == "message", "SSE content part targets a non-message output");
    const auto index = integer(event, "content_index");
    require(index == state.parts.size(), "SSE content indices must be introduced in order without gaps");
    within(state.parts.size() < limits_.max_content_parts, "SSE content part count exceeds its bound");
    const auto normalized = normalize_part(field(event, "part"));
    require(text(normalized.as_object(), "text").empty(), "new SSE output_text part must be empty");
    state.parts.emplace_back();
}

void Events::text_delta(const Object& event, bool done) {
    auto& state = part(event);
    require(!state.text_done && !state.part_done, "SSE text is already final");
    empty_optional_array(event, "logprobs");
    const auto value = text(event, done ? "text" : "delta");
    if (done) {
        require(value == state.text, "SSE text.done disagrees with the accumulated deltas");
        state.text_done = true;
    } else {
        within(value.size() <= limits_.max_text_bytes - text_bytes_, "SSE accumulated text exceeds its bound");
        state.text.append(value);
        text_bytes_ += value.size();
    }
}

void Events::content_done(const Object& event) {
    auto& state = part(event);
    require(state.text_done && !state.part_done, "SSE content.done requires one prior text.done");
    const auto normalized = normalize_part(field(event, "part"));
    require(text(normalized.as_object(), "text") == state.text,
            "SSE content.done disagrees with the accumulated text");
    state.part_done = true;
}

void Events::arguments(const Object& event, bool done) {
    auto& state = item(event);
    require(identifier(event, "item_id") == state.id, "SSE function item ID disagrees with its output index");
    require(state.type == "function_call" && !state.arguments_done,
            "SSE function arguments target a non-function or already final item");
    const auto value = text(event, done ? "arguments" : "delta");
    if (done) {
        require(value == state.arguments, "SSE arguments.done disagrees with the accumulated deltas");
        Json parsed;
        try { parsed = parse_json_strict(value, limits_.max_tool_argument_bytes, max_depth_); }
        catch (const Error&) { throw ReplayFailure("invalid_sse", "SSE final function arguments are not strict bounded JSON"); }
        require(parsed.is_object(), "SSE function arguments must be a JSON object");
        state.arguments_done = true;
    } else {
        within(value.size() <= limits_.max_tool_argument_bytes - argument_bytes_,
               "SSE accumulated function arguments exceed their shared bound");
        state.arguments.append(value);
        argument_bytes_ += value.size();
    }
}

void Events::matches(const ItemState& state, const Json& value, bool completed) const {
    const auto normalized = normalize_item(value, completed);
    const auto& candidate = normalized.as_object();
    require(identifier(candidate, "id") == state.id && text(candidate, "type") == state.type,
            "SSE output snapshot changed its item identity or type");
    if (state.type == "message") {
        const auto& content = array(candidate, "content");
        require(content.size() == state.parts.size(), "SSE output snapshot has unobserved or missing content parts");
        for (std::size_t index = 0; index < content.size(); ++index) {
            const auto& expected = state.parts[index];
            if (completed) require(expected.text_done && expected.part_done,
                                   "SSE completed message contains unfinished content");
            require(text(object(content[index]), "text") == expected.text,
                    "SSE output snapshot disagrees with accumulated text");
        }
    } else {
        require(identifier(candidate, "call_id") == state.call_id && identifier(candidate, "name") == state.name,
                "SSE output snapshot changed its function call identity or name");
        if (completed) require(state.arguments_done, "SSE completed function has unfinished arguments");
        require(text(candidate, "arguments") == state.arguments,
                "SSE output snapshot disagrees with accumulated function arguments");
    }
}

void Events::output_done(const Object& event) {
    auto& state = item(event);
    const auto& value = field(event, "item");
    matches(state, value, true);
    state.done_item = normalize_item(value, true);
    state.done = true;
}

void Events::finish(const Object& event, const std::string& type) {
    const auto& value = field(event, "response");
    const auto& response = object(value);
    require(text(response, "object") == "response" && identifier(response, "id") == response_id &&
            identifier(response, "model") == model_, "SSE terminal response identity changed");
    const auto status = type.substr(std::string("response.").size());
    require(text(response, "status") == status, "SSE terminal event and response status disagree");
    const auto& output = array(response, "output");
    require(output.size() == items_.size(), "SSE terminal response has unobserved or missing output items");
    const bool completed = type == "response.completed";
    for (std::size_t index = 0; index < output.size(); ++index) {
        const auto& state = items_[index];
        matches(state, output[index], completed);
        if (completed) require(state.done, "SSE completed response contains an unfinished output item");
        if (state.done)
            require(normalize_item(output[index], true) == state.done_item,
                    "SSE terminal response changed an output_item.done snapshot");
    }
    terminal_type = type;
    terminal_response = value;
    terminal = true;
}

Json Events::observations() const {
    Array outputs;
    for (std::size_t index = 0; index < items_.size(); ++index) {
        const auto& state = items_[index];
        Object observed{{"output_index", index}, {"item_id", state.id}, {"type", state.type},
                        {"item_done", state.done}};
        if (state.type == "message") {
            Array content;
            for (std::size_t part_index = 0; part_index < state.parts.size(); ++part_index) {
                const auto& part_state = state.parts[part_index];
                content.emplace_back(Object{{"content_index", part_index}, {"text_bytes", part_state.text.size()},
                    {"text_sha256", sha256_hex(part_state.text)}, {"text_done", part_state.text_done},
                    {"part_done", part_state.part_done}});
            }
            observed["content"] = std::move(content);
        } else {
            observed["call_id"] = state.call_id;
            observed["name"] = state.name;
            observed["arguments_bytes"] = state.arguments.size();
            observed["arguments_sha256"] = sha256_hex(state.arguments);
            observed["arguments_done"] = state.arguments_done;
        }
        outputs.push_back(std::move(observed));
    }
    return outputs;
}

SseReplayLimits effective_limits(const ProviderProfile& profile, SseReplayLimits supplied) {
    // The caller may narrow the frozen profile bounds but cannot enlarge them.
    const SseReplayLimits hard;
    const auto positive = [](std::size_t value, std::size_t maximum) {
        if (value == 0 || value > maximum)
            throw Error(ErrorCode::InvalidInput, "SSE replay limit is outside the supported hard bounds");
    };
    positive(supplied.max_total_bytes, hard.max_total_bytes);
    positive(supplied.max_line_bytes, hard.max_line_bytes);
    positive(supplied.max_event_bytes, hard.max_event_bytes);
    positive(supplied.max_events, hard.max_events);
    positive(supplied.max_chunks, hard.max_chunks);
    positive(supplied.max_output_items, hard.max_output_items);
    positive(supplied.max_tool_calls, hard.max_tool_calls);
    positive(supplied.max_content_parts, hard.max_content_parts);
    positive(supplied.max_text_bytes, hard.max_text_bytes);
    positive(supplied.max_tool_argument_bytes, hard.max_tool_argument_bytes);
    const auto& frozen = profile.limits;
    supplied.max_total_bytes = std::min(supplied.max_total_bytes, frozen.max_stream_bytes);
    supplied.max_line_bytes = std::min(supplied.max_line_bytes, frozen.max_sse_line_bytes);
    supplied.max_event_bytes = std::min(supplied.max_event_bytes, frozen.max_sse_event_bytes);
    supplied.max_events = std::min(supplied.max_events, frozen.max_sse_events);
    supplied.max_chunks = std::min(supplied.max_chunks, frozen.max_sse_chunks);
    supplied.max_output_items = std::min(supplied.max_output_items, frozen.max_output_items);
    supplied.max_tool_calls = std::min(supplied.max_tool_calls, frozen.max_tool_calls);
    supplied.max_text_bytes = std::min(supplied.max_text_bytes, frozen.max_text_bytes);
    supplied.max_tool_argument_bytes = std::min(supplied.max_tool_argument_bytes, frozen.max_tool_argument_bytes);
    return supplied;
}

ModelResponse original_response(const ModelRequest& request) {
    ModelResponse result;
    result.call_id = request.call_id;
    result.attempt_id = request.attempt_id;
    result.role_call_index = request.role_call_index;
    result.role = request.role;
    result.binding = request.binding;
    result.config_snapshot_id = request.config_snapshot_id;
    return result;
}

void fail(ModelResponse& response, std::string code, std::string message) {
    response.status = code == "cancelled" || code == "timeout" || code == "stream_interrupted" ?
        ModelResultStatus::Interrupted : ModelResultStatus::Error;
    response.error_code = std::move(code);
    response.error_message = std::move(message);
    response.text.clear();
    response.tool_calls.clear();
    response.usage = {};
}

std::string base64(std::string_view input) {
    constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve(((input.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < input.size(); i += 3) {
        const auto first = static_cast<unsigned char>(input[i]);
        const auto second = i + 1 < input.size() ? static_cast<unsigned char>(input[i + 1]) : 0U;
        const auto third = i + 2 < input.size() ? static_cast<unsigned char>(input[i + 2]) : 0U;
        const auto value = (static_cast<std::uint32_t>(first) << 16U) |
            (static_cast<std::uint32_t>(second) << 8U) | static_cast<std::uint32_t>(third);
        output.push_back(alphabet[(value >> 18U) & 63U]);
        output.push_back(alphabet[(value >> 12U) & 63U]);
        output.push_back(i + 1 < input.size() ? alphabet[(value >> 6U) & 63U] : '=');
        output.push_back(i + 2 < input.size() ? alphabet[value & 63U] : '=');
    }
    return output;
}

Json replay_record(const ResponsesSseReplay& replay) {
    return Object{{"record_kind", "local_consistency_only"},
        {"response", to_json(replay.response)}, {"evidence", replay.evidence},
        {"raw_stream_base64", base64(replay.raw_stream)},
        {"raw_stream_sha256", sha256_hex(replay.raw_stream)},
        {"terminal_response", replay.terminal_response}};
}

} // namespace

ResponsesSseReplay replay_responses_sse(const ProviderProfile& profile,
                                      const ModelRequest& request,
                                      const std::vector<SseReplayChunk>& chunks,
                                      SseReplayLimits limits,
                                      std::stop_token stop) {
    // Encoding performs the same complete frozen request/profile validation as
    // HTTP replay, without touching transport or credentials.
    const auto encoded = encode_responses_request(profile, request, true);
    limits = effective_limits(profile, limits);
    auto elapsed = std::chrono::milliseconds(0);
    if (chunks.size() <= limits.max_chunks) {
        for (const auto& chunk : chunks) {
            if (chunk.elapsed.count() < 0 || chunk.elapsed < elapsed)
                throw Error(ErrorCode::InvalidInput, "SSE recorded arrival times must be nonnegative and nondecreasing");
            elapsed = chunk.elapsed;
        }
    }
    ResponsesSseReplay result;
    result.response = original_response(request);
    Events events(limits, profile.limits.max_json_depth);
    std::optional<ModelResponse> terminal_decoded;
    std::optional<ModelUsage> terminal_usage;
    Json terminal_codec_evidence = nullptr;
    Frames frames(limits, [&](std::string_view type, std::string_view data) {
        events.accept(type, data);
        if (!events.terminal) return;
        const auto terminal_bytes = canonical_json(events.terminal_response);
        within(terminal_bytes.size() <= profile.limits.max_response_bytes,
               "SSE terminal response exceeds the HTTP response bound");
        const auto decoded = decode_responses_response(profile, request, 200, terminal_bytes);
        terminal_decoded = decoded.response;
        terminal_codec_evidence = decoded.evidence;
        // A later stream/cancellation error cannot erase a valid terminal's
        // already reported metering. Retaining usage never retains its tools.
        // Invalid/unsupported/mismatched terminal payloads are not a source of
        // verified usage for this recovery path.
        if (decoded.response.status == ModelResultStatus::Completed ||
            (events.terminal_type == "response.failed" && decoded.response.error_code == "provider_failed") ||
            (events.terminal_type == "response.incomplete" && decoded.response.error_code == "response_incomplete"))
            terminal_usage = decoded.response.usage;
    });
    std::size_t consumed_chunks = 0;
    elapsed = std::chrono::milliseconds(0);
    bool complete = false;
    try {
        within(chunks.size() <= limits.max_chunks, "SSE chunk count exceeds its bound");
        if (stop.stop_requested()) throw ReplayFailure("cancelled", "offline SSE replay cancelled before reading input");
        for (const auto& chunk : chunks) {
            elapsed = chunk.elapsed;
            if (stop.stop_requested() || chunk.cancel_before)
                throw ReplayFailure("cancelled", "offline SSE replay cancelled before the next recorded read");
            if (chunk.elapsed >= request.timeout)
                throw ReplayFailure("timeout", "SSE chunk arrived at or after the original attempt deadline");
            within(chunk.bytes.size() <= limits.max_total_bytes - result.raw_stream.size(),
                   "SSE input exceeds its total byte bound");
            result.raw_stream.append(chunk.bytes);
            ++consumed_chunks;
            frames.feed(chunk.bytes, stop);
        }
        if (stop.stop_requested()) throw ReplayFailure("cancelled", "offline SSE replay cancelled before completion");
        if (frames.has_pending_event()) {
            if (events.terminal)
                throw ReplayFailure("event_after_terminal", "SSE contains pending data after its terminal response");
            throw ReplayFailure("stream_interrupted", "SSE ended before its final event delimiter");
        }
        if (!events.terminal)
            throw ReplayFailure("stream_interrupted", "SSE ended without a complete terminal response");
        require(terminal_decoded.has_value(), "SSE terminal response was not decoded");
        result.response = *terminal_decoded;
        complete = true;
    } catch (const ReplayFailure& error) {
        fail(result.response, error.code(), error.what());
    } catch (const Error&) {
        fail(result.response, "invalid_sse", "SSE terminal response failed the shared protocol decoder");
    }
    if (!complete && terminal_usage) result.response.usage = *terminal_usage;
    result.terminal_response = events.terminal_response;
    Array types;
    for (const auto& type : events.observed_types) types.emplace_back(type);
    result.evidence = Object{
        {"format", "wls-responses-sse-replay-v1"}, {"offline", true},
        {"network_requests", 0}, {"retry_attempts", 0}, {"tools_dispatched", 0}, {"business_accepted", false},
        {"profile_sha256", encoded.profile_sha256},
        {"logical_request_sha256", encoded.logical_request_sha256},
        {"encoded_request_body_sha256", encoded.body_sha256},
        {"raw_stream_sha256", sha256_hex(result.raw_stream)},
        {"retained_stream_bytes", result.raw_stream.size()}, {"parsed_stream_bytes", frames.parsed_bytes},
        {"input_chunks", chunks.size()}, {"consumed_chunks", consumed_chunks},
        {"events", frames.events}, {"comment_lines", frames.comments},
        {"ignored_sse_fields", frames.ignored_fields}, {"ignored_retry_fields", frames.retry_fields},
        {"observed_obfuscation_fields", events.obfuscation_fields},
        {"observed_obfuscation_bytes", events.obfuscation_bytes},
        {"last_event_id", frames.last_event_id}, {"provider_response_id", events.response_id},
        {"last_sequence_number", events.last_sequence ? Json(*events.last_sequence) : Json(nullptr)},
        {"observed_event_types", std::move(types)}, {"terminal_event", events.terminal_type},
        {"terminal_received", events.terminal}, {"stream_complete", complete},
        {"terminal_usage_validated", terminal_usage.has_value()},
        {"terminal_usage_preserved_after_stream_error", !complete && terminal_usage.has_value()},
        {"response_usable", complete && result.response.status == ModelResultStatus::Completed},
        {"timing_source", "caller_supplied_elapsed_ms"}, {"elapsed_ms", elapsed.count()},
        {"deadline_ms", request.timeout.count()}, {"partial_output_observations", events.observations()},
        {"terminal_codec", std::move(terminal_codec_evidence)},
        {"limitations", Array{"offline supplied bytes; no network or billing validation",
            "text and function_call event subset only; unsupported events refuse",
            "invalid UTF-8 refuses without replacement; original bytes remain retained",
            "partial output observations are not dispatchable tool calls",
            "host current-version, tool schema and authorization checks remain required"}}
    };
    result.record_sha256 = sha256_hex(canonical_json(replay_record(result)));
    return result;
}

Json to_json(const ResponsesSseReplay& replay) {
    auto record = replay_record(replay);
    if (replay.record_sha256 != sha256_hex(canonical_json(record)))
        throw Error(ErrorCode::Corrupt, "SSE replay report changed after its local consistency fingerprint was recorded");
    record.as_object()["record_sha256"] = replay.record_sha256;
    return record;
}

} // namespace wls::s1


