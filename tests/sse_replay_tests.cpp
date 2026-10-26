#include "wls/sse_replay.hpp"
#include "protocol_test_support.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <functional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
using namespace wls::s1;
using namespace std::chrono_literals;
namespace fixture = wls::test::protocol;
using Object = boost::json::object;
using Array = boost::json::array;
using EventList = std::vector<Json>;

Json part(std::string content) {
    return Object{{"type", "output_text"}, {"text", std::move(content)}, {"annotations", Array{}}};
}

Json response_start() {
    auto response = fixture::response_with_output({});
    response.as_object()["status"] = "in_progress";
    response.as_object()["usage"] = nullptr;
    return response;
}

void add(EventList& events, std::string type, Object data) {
    data["type"] = std::move(type);
    data["sequence_number"] = events.size();
    events.emplace_back(std::move(data));
}

void resequence(EventList& events, std::size_t first = 0) {
    for (std::size_t index = 0; index < events.size(); ++index)
        events[index].as_object()["sequence_number"] = first + index;
}

EventList text_events(std::string content = "中😀 preserved") {
    EventList events;
    add(events, "response.created", {{"response", response_start()}});
    add(events, "response.in_progress", {{"response", response_start()}});
    add(events, "response.output_item.added", {{"output_index", 0}, {"item", Object{
        {"id", "msg_offline_fixture"}, {"type", "message"}, {"role", "assistant"},
        {"status", "in_progress"}, {"content", Array{}}}}});
    const Object location{{"item_id", "msg_offline_fixture"}, {"output_index", 0}, {"content_index", 0}};
    auto data = location; data["part"] = part(""); add(events, "response.content_part.added", data);
    data = location; data["delta"] = content; data["logprobs"] = Array{};
    add(events, "response.output_text.delta", data);
    data = location; data["text"] = content; data["logprobs"] = Array{};
    add(events, "response.output_text.done", data);
    data = location; data["part"] = part(content); add(events, "response.content_part.done", data);
    const auto terminal = fixture::text_response(content);
    add(events, "response.output_item.done", {{"output_index", 0},
        {"item", terminal.at("output").as_array()[0]}});
    add(events, "response.completed", {{"response", terminal}});
    return events;
}

EventList tool_events(std::string arguments = "{\"message\":\"fixture\"}") {
    EventList events;
    add(events, "response.created", {{"response", response_start()}});
    add(events, "response.output_item.added", {{"output_index", 0}, {"item", Object{
        {"id", "fc_offline_fixture"}, {"type", "function_call"}, {"call_id", "call_provider_fixture"},
        {"name", "note"}, {"arguments", ""}, {"status", "in_progress"}}}});
    const Object location{{"item_id", "fc_offline_fixture"}, {"output_index", 0}};
    auto data = location; data["delta"] = arguments.substr(0, arguments.size() / 2);
    add(events, "response.function_call_arguments.delta", data);
    data = location; data["delta"] = arguments.substr(arguments.size() / 2);
    add(events, "response.function_call_arguments.delta", data);
    data = location; data["arguments"] = arguments; add(events, "response.function_call_arguments.done", data);
    auto terminal = fixture::tool_response();
    terminal.as_object().at("output").as_array()[0].as_object()["arguments"] = arguments;
    add(events, "response.output_item.done", {{"output_index", 0},
        {"item", terminal.at("output").as_array()[0]}});
    add(events, "response.completed", {{"response", terminal}});
    return events;
}

std::string stream(const EventList& events, std::string_view newline = "\n", bool event_fields = true) {
    std::string bytes;
    for (const auto& event : events) {
        if (event_fields) {
            bytes += "event: "; bytes += event.at("type").as_string(); bytes += newline;
        }
        bytes += "data: "; bytes += canonical_json(event); bytes += newline; bytes += newline;
    }
    return bytes;
}

ResponsesSseReplay replay(const EventList& events, SseReplayLimits limits = {}) {
    return replay_responses_sse(fixture::profile(), fixture::request(), {{stream(events), 0ms}}, limits);
}

void expect_rejected(const EventList& events, std::string_view code = {}) {
    const auto result = replay(events);
    CHECK(result.response.status != ModelResultStatus::Completed);
    CHECK(result.response.tool_calls.empty());
    if (!code.empty()) CHECK(result.response.error_code == code);
    CHECK(result.response.binding == fixture::request().binding);
    CHECK(result.response.call_id == fixture::request().call_id);
    CHECK(result.response.attempt_id == fixture::request().attempt_id);
    CHECK(!result.evidence.at("response_usable").as_bool());
    CHECK(result.evidence.at("network_requests") == 0);
    CHECK(result.evidence.at("retry_attempts") == 0);
}

void text_completion_is_independent_of_every_two_chunk_split() {
    const auto profile = fixture::profile();
    const auto request = fixture::request();
    const std::string bytes = "\xef\xbb\xbf" + stream(text_events(), "\r\n");
    const auto reference = replay_responses_sse(profile, request, {{bytes, 0ms}});
    CHECK(reference.response.status == ModelResultStatus::Completed);
    CHECK(reference.response.text == "中😀 preserved");
    CHECK(reference.response.usage.input_tokens == 9);
    CHECK(reference.response.usage.cache_read_tokens == 2);
    CHECK(!reference.response.usage.cost_micro_usd);
    const auto expected_events = text_events().size();
    for (std::size_t offset = 0; offset <= bytes.size(); ++offset) {
        const auto actual = replay_responses_sse(profile, request,
            {{bytes.substr(0, offset), 0ms}, {bytes.substr(offset), 1ms}});
        CHECK(to_json(actual.response) == to_json(reference.response));
        CHECK(actual.raw_stream == bytes);
        CHECK(actual.evidence.at("stream_complete").as_bool());
        CHECK(actual.evidence.at("events") == expected_events);
    }
}

void one_byte_reads_keep_bom_utf8_and_crlf_intact() {
    const auto bytes = std::string("\xef\xbb\xbf") + stream(text_events("字符😀"), "\r\n");
    std::vector<SseReplayChunk> chunks;
    for (const char byte : bytes) chunks.push_back({std::string(1, byte), 0ms});
    const auto result = replay_responses_sse(fixture::profile(), fixture::request(), chunks);
    CHECK(result.response.status == ModelResultStatus::Completed);
    CHECK(result.response.text == "字符😀");
    CHECK(result.raw_stream == bytes);
}

void all_line_endings_and_multiline_data_are_supported() {
    for (const auto ending : {"\n", "\r", "\r\n"}) {
        const auto bytes = stream(text_events("line variants"), ending, false);
        CHECK(replay_responses_sse(fixture::profile(), fixture::request(), {{bytes, 0ms}}).response.text == "line variants");
    }
    std::string bytes = "\xef\xbb\xbf: comment only\r\n\r\nid: retained-id\r\nretry: 1\r\nunknown-field: ignored\r\n";
    for (const auto& event : text_events("two data lines")) {
        auto json = canonical_json(event);
        const auto comma = json.find(',');
        CHECK(comma != std::string::npos);
        bytes += "data:" + json.substr(0, comma + 1) + "\r\ndata: " + json.substr(comma + 1) + "\r\n\r\n";
    }
    bytes += ": unterminated final comment";
    const auto result = replay_responses_sse(fixture::profile(), fixture::request(), {{bytes, 0ms}});
    CHECK(result.response.status == ModelResultStatus::Completed);
    CHECK(result.response.text == "two data lines");
    CHECK(result.evidence.at("last_event_id") == "retained-id");
    CHECK(result.evidence.at("ignored_retry_fields") == 1);
    CHECK(result.evidence.at("ignored_sse_fields") == 1);
    CHECK(result.evidence.at("comment_lines") == 2);
    CHECK(result.evidence.at("retry_attempts") == 0);
}

void complete_function_arguments_are_not_dispatched_by_replay() {
    const auto result = replay(tool_events("{\"message\":\"精确😀\"}"));
    CHECK(result.response.status == ModelResultStatus::Completed);
    CHECK(result.response.tool_calls.size() == 1);
    CHECK(result.response.tool_calls[0].call_id == "call_provider_fixture");
    CHECK(result.response.tool_calls[0].arguments.at("message") == "精确😀");
    CHECK(result.evidence.at("tools_dispatched") == 0);
    require_complete_response(fixture::request(), result.response);
    auto changed = fixture::request();
    ++changed.binding.task_revision;
    ::test::throws<Error>([&] { validate_response(changed, result.response); });
}

void truncated_streams_never_expose_partial_tools_or_invent_usage() {
    auto events = tool_events();
    events.resize(4);
    auto result = replay(events);
    CHECK(result.response.status == ModelResultStatus::Interrupted);
    CHECK(result.response.error_code == "stream_interrupted");
    CHECK(result.response.tool_calls.empty());
    CHECK(!result.response.usage.input_tokens && !result.response.usage.cost_micro_usd);
    CHECK(!result.evidence.at("partial_output_observations").as_array().empty());
    auto bytes = stream(tool_events());
    bytes.pop_back(); // A terminal JSON object without a blank delimiter is not an event.
    result = replay_responses_sse(fixture::profile(), fixture::request(), {{bytes, 0ms}});
    CHECK(result.response.status == ModelResultStatus::Interrupted);
    CHECK(!result.evidence.at("terminal_received").as_bool());
    CHECK(result.response.tool_calls.empty());
    CHECK(result.raw_stream == bytes);
}

void cancellation_is_deterministic_before_and_during_input() {
    std::stop_source cancellation;
    cancellation.request_stop();
    const auto bytes = stream(tool_events());
    const auto before = replay_responses_sse(fixture::profile(), fixture::request(), {{bytes, 0ms}}, {}, cancellation.get_token());
    CHECK(before.response.status == ModelResultStatus::Interrupted && before.response.error_code == "cancelled");
    CHECK(before.raw_stream.empty());
    auto partial = tool_events(); partial.resize(3);
    const auto prefix = stream(partial);
    const auto middle = replay_responses_sse(fixture::profile(), fixture::request(),
        {{prefix, 1ms}, {"", 2ms, true}, {bytes, 3ms}});
    CHECK(middle.response.status == ModelResultStatus::Interrupted && middle.response.error_code == "cancelled");
    CHECK(middle.raw_stream == prefix);
    CHECK(middle.response.tool_calls.empty() && !middle.response.usage.input_tokens);
    CHECK(middle.response.binding == fixture::request().binding);
    CHECK(middle.evidence.at("consumed_chunks") == 1);
}

void deadline_boundary_refuses_late_terminal_without_sleeping() {
    auto request = fixture::request(); request.timeout = 30ms;
    const auto all = tool_events();
    EventList prefix(all.begin(), all.end() - 1);
    const EventList terminal{all.back()};
    const auto on_time = replay_responses_sse(fixture::profile(), request,
        {{stream(prefix), 0ms}, {stream(terminal), 29ms}});
    CHECK(on_time.response.status == ModelResultStatus::Completed);
    for (const auto arrival : {30ms, 31ms}) {
        const auto late = replay_responses_sse(fixture::profile(), request,
            {{stream(prefix), 1ms}, {stream(terminal), arrival}});
        CHECK(late.response.status == ModelResultStatus::Interrupted && late.response.error_code == "timeout");
        CHECK(late.response.tool_calls.empty() && !late.response.usage.cost_micro_usd);
        CHECK(late.raw_stream == stream(prefix));
        CHECK(late.response.binding == request.binding);
    }
}

void invalid_caller_timing_profile_and_limits_refuse() {
    const auto profile = fixture::profile(); const auto request = fixture::request();
    ::test::throws<Error>([&] { (void)replay_responses_sse(profile, request, {{"", -1ms}}); });
    ::test::throws<Error>([&] { (void)replay_responses_sse(profile, request, {{"", 2ms}, {"", 1ms}}); });
    auto limits = SseReplayLimits{}; limits.max_events = 0;
    ::test::throws<Error>([&] { (void)replay_responses_sse(profile, request, {}, limits); });
    auto disabled = profile; disabled.streaming = false;
    ::test::throws<Error>([&] { (void)replay_responses_sse(disabled, request, {}); });
    auto fake = request; fake.context.protocol_version = "fake-v1";
    ::test::throws<Error>([&] { (void)replay_responses_sse(profile, fake, {}); });
}

void unsupported_events_and_done_sentinel_are_not_silently_ignored() {
    auto events = text_events();
    events[4].as_object()["type"] = "response.reasoning_summary_text.delta";
    expect_rejected(events, "unsupported_event");
    const auto bytes = stream(EventList{text_events().front()}) + "data: [DONE]\n\n";
    const auto result = replay_responses_sse(fixture::profile(), fixture::request(), {{bytes, 0ms}});
    CHECK(result.response.status == ModelResultStatus::Error && result.response.error_code == "invalid_sse");
    CHECK(result.raw_stream == bytes && result.response.tool_calls.empty());
}

void duplicate_missing_reordered_and_invalid_sequences_refuse() {
    for (int variant = 0; variant < 5; ++variant) {
        auto events = text_events();
        if (variant == 0) events[3].as_object()["sequence_number"] = 2;
        if (variant == 1) events[3].as_object()["sequence_number"] = 4;
        if (variant == 2) events[3].as_object()["sequence_number"] = -1;
        if (variant == 3) events[3].as_object()["sequence_number"] = 3.5;
        if (variant == 4) events[0].as_object()["sequence_number"] = 2;
        expect_rejected(events, "invalid_sse");
    }
    auto one_based = text_events(); resequence(one_based, 1);
    CHECK(replay(one_based).response.status == ModelResultStatus::Completed);
}

void response_item_and_content_identity_cannot_drift() {
    for (int variant = 0; variant < 8; ++variant) {
        auto events = text_events();
        if (variant == 0) events.back().as_object().at("response").as_object()["id"] = "resp_other";
        if (variant == 1) events.back().as_object().at("response").as_object()["model"] = "other-model";
        if (variant == 2) events[4].as_object()["item_id"] = "msg_other";
        if (variant == 3) events[4].as_object().erase("item_id");
        if (variant == 4) events[2].as_object()["output_index"] = 1;
        if (variant == 5) events[3].as_object()["content_index"] = 1;
        if (variant == 6) events[4].as_object()["output_index"] = 1;
        if (variant == 7) events[4].as_object()["content_index"] = 1;
        expect_rejected(events, "invalid_sse");
    }
}

void every_done_and_terminal_text_must_equal_original_deltas() {
    for (int variant = 0; variant < 6; ++variant) {
        auto events = text_events();
        if (variant == 0) events[5].as_object()["text"] = "changed";
        if (variant == 1) events[6].as_object().at("part").as_object()["text"] = "changed";
        if (variant == 2) events[7].as_object().at("item").as_object().at("content").as_array()[0].as_object()["text"] = "changed";
        if (variant == 3) events[8].as_object().at("response").as_object().at("output").as_array()[0].as_object().at("content").as_array()[0].as_object()["text"] = "changed";
        if (variant == 4) events[8].as_object().at("response").as_object()["output"] = Array{};
        if (variant == 5) events[8].as_object().at("response").as_object()["status"] = "incomplete";
        expect_rejected(events, "invalid_sse");
    }
}

void function_json_and_snapshots_are_complete_exact_and_unique() {
    for (const auto& malformed : {std::string("{\"message\":"), std::string("[1]"),
                                 std::string("{\"message\":1,\"message\":2}")})
        expect_rejected(tool_events(malformed), "invalid_sse");
    for (int variant = 0; variant < 5; ++variant) {
        auto events = tool_events();
        if (variant == 0) events[4].as_object()["arguments"] = "{}";
        if (variant == 1) events[5].as_object().at("item").as_object()["arguments"] = "{}";
        if (variant == 2) events[5].as_object().at("item").as_object()["call_id"] = "changed_call";
        if (variant == 3) events.back().as_object().at("response").as_object().at("output").as_array()[0].as_object()["name"] = "other";
        if (variant == 4) events[3].as_object()["item_id"] = "other_item";
        expect_rejected(events, "invalid_sse");
    }
}

void missing_or_duplicate_lifecycle_steps_do_not_complete() {
    for (int variant = 0; variant < 5; ++variant) {
        auto events = text_events();
        if (variant == 0) events.erase(events.begin());
        if (variant == 1) events.erase(events.begin() + 3);
        if (variant == 2) events.erase(events.begin() + 5);
        if (variant == 3) events.erase(events.begin() + 7);
        if (variant == 4) events.insert(events.begin() + 7, events[6]);
        resequence(events);
        expect_rejected(events, "invalid_sse");
    }
    auto tool = tool_events(); tool.erase(tool.begin() + 4); resequence(tool);
    expect_rejected(tool, "invalid_sse");
}

EventList parallel_tools() {
    const auto first = tool_events();
    auto second = tool_events("{\"message\":\"second\"}");
    for (auto& event : second) {
        auto& source = event.as_object();
        if (source.contains("output_index")) source["output_index"] = 1;
        if (source.contains("item_id")) source["item_id"] = "fc_second";
        if (source.contains("item")) {
            source.at("item").as_object()["id"] = "fc_second";
            source.at("item").as_object()["call_id"] = "call_second";
        }
    }
    EventList result{first[0], first[1], second[1], first[2], second[2], first[3], second[3],
                     first[4], second[4], first[5], second[5]};
    const auto terminal = fixture::response_with_output(Array{first[5].at("item"), second[5].at("item")});
    add(result, "response.completed", {{"response", terminal}});
    resequence(result);
    return result;
}

void interleaved_tool_deltas_preserve_each_provider_identity() {
    const auto result = replay(parallel_tools());
    CHECK(result.response.status == ModelResultStatus::Completed && result.response.tool_calls.size() == 2);
    CHECK(result.response.tool_calls[0].call_id == "call_provider_fixture");
    CHECK(result.response.tool_calls[0].arguments.at("message") == "fixture");
    CHECK(result.response.tool_calls[1].call_id == "call_second");
    CHECK(result.response.tool_calls[1].arguments.at("message") == "second");
    for (const auto key : {"id", "call_id"}) {
        auto events = parallel_tools();
        events[2].as_object().at("item").as_object()[key] = events[1].at("item").at(key);
        expect_rejected(events, "invalid_sse");
    }
}

void unsupported_output_content_and_metadata_refuse() {
    for (int variant = 0; variant < 5; ++variant) {
        auto events = text_events();
        if (variant == 0) events[2].as_object().at("item").as_object()["type"] = "reasoning";
        if (variant == 1) events[3].as_object().at("part").as_object()["type"] = "refusal";
        if (variant == 2) events[5].as_object()["logprobs"] = Array{Object{{"token", "ignored"}}};
        if (variant == 3) events[6].as_object().at("part").as_object()["annotations"] = Array{Object{{"type", "url_citation"}}};
        if (variant == 4) events[2].as_object().at("item").as_object()["unsupported_extension"] = true;
        expect_rejected(events, "unsupported_output");
    }
}

void failed_and_incomplete_terminal_keep_known_usage_but_no_tools() {
    for (const auto status : {"failed", "incomplete"}) {
        auto events = tool_events(); events.resize(3);
        auto partial = fixture::tool_response().at("output").as_array()[0];
        partial.as_object()["status"] = "in_progress";
        partial.as_object()["arguments"] = events.back().at("delta");
        auto terminal = fixture::response_with_output(Array{partial});
        terminal.as_object()["status"] = status;
        if (std::string_view(status) == "failed")
            terminal.as_object()["error"] = Object{{"code", "server_error"}, {"message", "fixture failure"}};
        else terminal.as_object()["incomplete_details"] = Object{{"reason", "max_output_tokens"}};
        add(events, "response." + std::string(status), {{"response", terminal}});
        const auto result = replay(events);
        CHECK(result.response.status != ModelResultStatus::Completed);
        CHECK(result.response.tool_calls.empty());
        CHECK(result.response.usage.input_tokens == 9);
        CHECK(!result.response.usage.cost_micro_usd);
        CHECK(result.evidence.at("terminal_received").as_bool());
        CHECK(result.evidence.at("stream_complete").as_bool());
        CHECK(!result.evidence.at("response_usable").as_bool());
    }
}

void shared_http_decoder_keeps_unknown_usage_and_refuses_invalid_usage() {
    auto events = tool_events();
    events.back().as_object().at("response").as_object()["usage"] = nullptr;
    auto result = replay(events);
    CHECK(result.response.status == ModelResultStatus::Completed);
    CHECK(!result.response.usage.input_tokens && !result.response.usage.output_tokens);
    CHECK(!result.response.usage.cache_read_tokens && !result.response.usage.cost_micro_usd);
    events = tool_events();
    events.back().as_object().at("response").as_object().at("usage").as_object()["input_tokens"] = -1;
    result = replay(events);
    CHECK(result.response.status == ModelResultStatus::Error);
    CHECK(result.response.tool_calls.empty());
    CHECK(result.evidence.at("stream_complete").as_bool());
}

void all_stream_read_and_payload_bounds_are_enforced() {
    const auto events = text_events();
    const auto reject_limit = [&](const SseReplayLimits& limits) {
        const auto result = replay(events, limits);
        CHECK(result.response.status == ModelResultStatus::Error);
        CHECK(result.response.error_code == "stream_limit");
        CHECK(result.response.tool_calls.empty());
    };
    auto limits = SseReplayLimits{}; limits.max_total_bytes = 16; reject_limit(limits);
    limits = {}; limits.max_line_bytes = 16; reject_limit(limits);
    limits = {}; limits.max_event_bytes = 16; reject_limit(limits);
    limits = {}; limits.max_events = 1; reject_limit(limits);
    limits = {}; limits.max_text_bytes = 1; reject_limit(limits);
    limits = {}; limits.max_chunks = 1;
    const auto bytes = stream(events);
    const auto chunks = replay_responses_sse(fixture::profile(), fixture::request(),
        {{bytes.substr(0, 1), 0ms}, {bytes.substr(1), 0ms}}, limits);
    CHECK(chunks.response.error_code == "stream_limit" && chunks.raw_stream.empty());
    limits = {}; limits.max_tool_argument_bytes = 1;
    CHECK(replay(tool_events(), limits).response.error_code == "stream_limit");
    limits = {}; limits.max_output_items = 1;
    CHECK(replay(parallel_tools(), limits).response.error_code == "stream_limit");
    limits = {}; limits.max_tool_calls = 1;
    CHECK(replay(parallel_tools(), limits).response.error_code == "stream_limit");
}

void profile_stream_limits_cannot_be_enlarged_by_replay_arguments() {
    auto profile = fixture::profile();
    profile.limits.max_sse_events = 1;
    const auto result = replay_responses_sse(profile, fixture::request(), {{stream(text_events()), 0ms}});
    CHECK(result.response.error_code == "stream_limit");
    CHECK(!result.evidence.at("response_usable").as_bool());
}

void content_parts_are_individually_indexed_and_bounded() {
    auto events = text_events("first"); events.resize(7);
    const Object location{{"item_id", "msg_offline_fixture"}, {"output_index", 0}, {"content_index", 1}};
    auto data = location; data["part"] = part(""); add(events, "response.content_part.added", data);
    data = location; data["delta"] = "second"; add(events, "response.output_text.delta", data);
    data = location; data["text"] = "second"; add(events, "response.output_text.done", data);
    data = location; data["part"] = part("second"); add(events, "response.content_part.done", data);
    auto terminal = fixture::text_response("first");
    terminal.as_object().at("output").as_array()[0].as_object().at("content").as_array().push_back(part("second"));
    add(events, "response.output_item.done", {{"output_index", 0}, {"item", terminal.at("output").as_array()[0]}});
    add(events, "response.completed", {{"response", terminal}});
    CHECK(replay(events).response.status == ModelResultStatus::Completed);
    auto limits = SseReplayLimits{}; limits.max_content_parts = 1;
    CHECK(replay(events, limits).response.error_code == "stream_limit");
}

void invalid_utf8_and_duplicate_json_are_preserved_losslessly() {
    const std::string invalid = "data: " + std::string(1, static_cast<char>(0xff)) + "\n\n";
    const auto result = replay_responses_sse(fixture::profile(), fixture::request(), {{invalid, 0ms}});
    CHECK(result.response.status == ModelResultStatus::Error && result.response.error_code == "invalid_sse");
    CHECK(result.raw_stream == invalid);
    const auto encoded = to_json(result);
    CHECK(encoded.at("raw_stream_base64") == "ZGF0YTog/woK");
    CHECK(std::string(encoded.at("raw_stream_sha256").as_string()) == sha256_hex(invalid));
    CHECK(parse_json_strict(canonical_json(encoded)).is_object());
    const std::string repeated = "data: {\"type\":\"response.created\",\"type\":\"response.completed\",\"sequence_number\":0}\n\n";
    CHECK(replay_responses_sse(fixture::profile(), fixture::request(), {{repeated, 0ms}}).response.error_code == "invalid_sse");
}

void event_type_fields_and_pending_or_complete_tail_cannot_disagree() {
    const auto events = text_events();
    auto bytes = stream(events);
    bytes.replace(0, std::string("event: response.created").size(), "event: response.completed");
    CHECK(replay_responses_sse(fixture::profile(), fixture::request(), {{bytes, 0ms}}).response.error_code == "invalid_sse");
    for (const auto& tail : {stream(EventList{events.back()}), std::string("data: {\"type\":\"unknown\"}")}) {
        const auto result = replay_responses_sse(fixture::profile(), fixture::request(), {{stream(events) + tail, 0ms}});
        CHECK(result.response.status == ModelResultStatus::Error);
        CHECK(result.response.error_code == "event_after_terminal");
        CHECK(result.response.tool_calls.empty());
        CHECK(result.evidence.at("terminal_received").as_bool());
        CHECK(!result.evidence.at("stream_complete").as_bool());
    }
}

void empty_input_is_an_interruption_with_original_tuple() {
    const auto request = fixture::request();
    const auto result = replay_responses_sse(fixture::profile(), request, {});
    CHECK(result.response.status == ModelResultStatus::Interrupted);
    CHECK(result.response.error_code == "stream_interrupted");
    CHECK(result.response.binding == request.binding);
    CHECK(result.response.config_snapshot_id == request.config_snapshot_id);
    CHECK(result.response.role_call_index == request.role_call_index);
    CHECK(result.response.call_id == request.call_id && result.response.attempt_id == request.attempt_id);
    CHECK(result.response.tool_calls.empty() && !result.response.usage.input_tokens);
    CHECK(result.evidence.at("events") == 0 && result.evidence.at("network_requests") == 0);
}

void optional_empty_metadata_and_function_status_normalize_consistently() {
    auto events = text_events();
    events[6].as_object().at("part").as_object().erase("annotations");
    CHECK(replay(events).response.status == ModelResultStatus::Completed);
    events = tool_events();
    events[1].as_object().at("item").as_object().erase("status");
    events[5].as_object().at("item").as_object().erase("status");
    CHECK(replay(events).response.status == ModelResultStatus::Completed);
}

void optional_provider_response_ids_bind_to_created_response() {
    auto events = tool_events();
    const std::vector<std::size_t> linked{1, 2, 3, 4, 5};
    for (const auto index : linked) events[index].as_object()["response_id"] = "resp_offline_fixture";
    CHECK(replay(events).response.status == ModelResultStatus::Completed);
    for (const auto index : linked) {
        for (const auto& wrong : {Json("resp_other"), Json(17), Json(std::string(129, 'x')), Json("resp bad")}) {
            auto changed = events;
            changed[index].as_object()["response_id"] = wrong;
            expect_rejected(changed, "invalid_sse");
        }
    }
    auto message = text_events();
    message[2].as_object()["response_id"] = "resp_offline_fixture";
    message[7].as_object()["response_id"] = "resp_offline_fixture";
    CHECK(replay(message).response.status == ModelResultStatus::Completed);
    message[4].as_object()["response_id"] = "resp_offline_fixture";
    expect_rejected(message, "unsupported_event"); // Not a declared extension for text.delta.
}

void validated_terminal_usage_survives_later_stream_rejection() {
    auto request = fixture::request(); request.timeout = 30ms;
    const auto events = tool_events();
    const auto bytes = stream(events);
    const auto check = [&](const ResponsesSseReplay& result) {
        CHECK(result.response.status != ModelResultStatus::Completed);
        CHECK(result.response.text.empty() && result.response.tool_calls.empty());
        CHECK(result.response.usage.input_tokens == 9 && result.response.usage.output_tokens == 3);
        CHECK(result.response.usage.cache_read_tokens == 2 && result.response.usage.cache_write_tokens == 1);
        CHECK(!result.response.usage.cost_micro_usd);
        CHECK(result.response.binding == request.binding && result.response.attempt_id == request.attempt_id);
        CHECK(result.evidence.at("terminal_usage_validated").as_bool());
        CHECK(result.evidence.at("terminal_usage_preserved_after_stream_error").as_bool());
        CHECK(!result.evidence.at("response_usable").as_bool());
    };
    check(replay_responses_sse(fixture::profile(), request,
        {{bytes + stream(EventList{events.back()}), 0ms}}));
    check(replay_responses_sse(fixture::profile(), request,
        {{bytes + "data: {\"type\":\"extra\"}", 0ms}}));
    check(replay_responses_sse(fixture::profile(), request,
        {{bytes, 0ms}, {"", 1ms, true}}));
    check(replay_responses_sse(fixture::profile(), request,
        {{bytes, 0ms}, {"", 30ms}}));

    auto invalid = events;
    invalid.back().as_object().at("response").as_object().at("usage").as_object()["input_tokens"] = -1;
    const auto invalid_result = replay_responses_sse(fixture::profile(), request,
        {{stream(invalid), 0ms}, {"", 1ms, true}});
    CHECK(invalid_result.response.status == ModelResultStatus::Interrupted);
    CHECK(!invalid_result.response.usage.input_tokens && !invalid_result.response.usage.output_tokens);
    CHECK(!invalid_result.evidence.at("terminal_usage_validated").as_bool());

    auto failed = tool_events(); failed.resize(1);
    auto terminal = fixture::response_with_output({});
    terminal.as_object()["status"] = "failed";
    terminal.as_object()["error"] = Object{{"code", "server_error"}, {"message", "fixture failure"}};
    add(failed, "response.failed", {{"response", terminal}});
    check(replay_responses_sse(fixture::profile(), request,
        {{stream(failed), 0ms}, {"", 1ms, true}}));
}

void replay_record_detects_mutated_response_evidence_and_raw_bytes() {
    const auto original = replay(tool_events());
    const auto recorded = to_json(original);
    CHECK(recorded.at("record_kind") == "local_consistency_only");
    CHECK(std::string(recorded.at("record_sha256").as_string()) == original.record_sha256);
    for (int variant = 0; variant < 7; ++variant) {
        auto changed = original;
        if (variant == 0) changed.response.tool_calls[0].arguments.as_object()["message"] = "mutated";
        if (variant == 1) changed.response.usage.input_tokens = 999;
        if (variant == 2) changed.response.binding.task_revision += 1;
        if (variant == 3) changed.evidence.as_object()["network_requests"] = 1;
        if (variant == 4) changed.terminal_response.as_object()["id"] = "resp_mutated";
        if (variant == 5) changed.raw_stream += ": changed\n";
        if (variant == 6) {
            changed.raw_stream += ": changed\n";
            changed.evidence.as_object()["raw_stream_sha256"] = sha256_hex(changed.raw_stream);
            changed.evidence.as_object()["retained_stream_bytes"] = changed.raw_stream.size();
        }
        ::test::throws<Error>([&] { (void)to_json(changed); });
    }
    auto partial = tool_events(); partial.resize(3);
    const auto interrupted = replay(partial);
    CHECK(parse_json_strict(canonical_json(to_json(interrupted))).is_object());
}

} // namespace

int main() {
    return ::test::run({
        {"SSE every two-chunk split preserves exact UTF-8 text, usage and identity", text_completion_is_independent_of_every_two_chunk_split},
        {"SSE bytewise reads preserve split BOM, Unicode and CRLF", one_byte_reads_keep_bom_utf8_and_crlf_intact},
        {"SSE CR, LF, CRLF, multiline data, comments and metadata are bounded framing", all_line_endings_and_multiline_data_are_supported},
        {"SSE completed function retains original IDs without dispatching tools", complete_function_arguments_are_not_dispatched_by_replay},
        {"SSE truncation never exposes partial tools or invented usage", truncated_streams_never_expose_partial_tools_or_invent_usage},
        {"SSE deterministic cancellation before and within argument deltas", cancellation_is_deterministic_before_and_during_input},
        {"SSE exact deadline rejects late terminal without real time waits", deadline_boundary_refuses_late_terminal_without_sleeping},
        {"SSE invalid caller timing, profile and bounds fail before replay", invalid_caller_timing_profile_and_limits_refuse},
        {"SSE unsupported events and chat DONE sentinel cannot silently complete", unsupported_events_and_done_sentinel_are_not_silently_ignored},
        {"SSE duplicate, missing, reordered and malformed sequences refuse", duplicate_missing_reordered_and_invalid_sequences_refuse},
        {"SSE response, item and content identities remain bound", response_item_and_content_identity_cannot_drift},
        {"SSE every text done and terminal snapshot matches its deltas", every_done_and_terminal_text_must_equal_original_deltas},
        {"SSE function JSON and snapshots are complete and exact", function_json_and_snapshots_are_complete_exact_and_unique},
        {"SSE missing and duplicate lifecycle transitions cannot complete", missing_or_duplicate_lifecycle_steps_do_not_complete},
        {"SSE interleaved function deltas preserve independent provider IDs", interleaved_tool_deltas_preserve_each_provider_identity},
        {"SSE unsupported content and metadata refuse explicitly", unsupported_output_content_and_metadata_refuse},
        {"SSE failed and incomplete terminal preserve known usage without tools", failed_and_incomplete_terminal_keep_known_usage_but_no_tools},
        {"SSE uses the HTTP decoder for nullable and invalid usage", shared_http_decoder_keeps_unknown_usage_and_refuses_invalid_usage},
        {"SSE total, line, event, chunk, item, tool and payload budgets stop replay", all_stream_read_and_payload_bounds_are_enforced},
        {"SSE caller limits cannot enlarge frozen profile event bounds", profile_stream_limits_cannot_be_enlarged_by_replay_arguments},
        {"SSE content parts have separate bounded indices", content_parts_are_individually_indexed_and_bounded},
        {"SSE invalid UTF-8 and duplicate JSON remain lossless failures", invalid_utf8_and_duplicate_json_are_preserved_losslessly},
        {"SSE event mismatch and any post-terminal data refuse", event_type_fields_and_pending_or_complete_tail_cannot_disagree},
        {"SSE empty input interrupts while retaining the original host tuple", empty_input_is_an_interruption_with_original_tuple},
        {"SSE optional empty metadata and function status remain compatible", optional_empty_metadata_and_function_status_normalize_consistently},
        {"SSE optional provider response_id links bind known item/function events", optional_provider_response_ids_bind_to_created_response},
        {"SSE validated terminal usage survives later data, cancel and deadline errors", validated_terminal_usage_survives_later_stream_rejection},
        {"SSE local record detects later changes to response, tuple, evidence and bytes", replay_record_detects_mutated_response_evidence_and_raw_bytes}
    });
}


