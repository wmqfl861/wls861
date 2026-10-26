// Included inside the CLI's anonymous namespace. These entry points are pure
// offline fixture readers, before SQL assets and any Store construction.
void protocol_output(const Json& result) {
    const auto bytes = canonical_json(result);
    if (bytes.size() > 32U * 1024U * 1024U)
        throw Error(ErrorCode::Quota, "offline protocol report exceeds 32 MiB");
    std::cout << bytes << '\n';
}

Json protocol_input_details(const std::filesystem::path& path, std::string_view bytes) {
    auto result = input_path_details(path, "supplied");
    result.as_object()["raw_sha256"] = sha256_hex(bytes);
    result.as_object()["bytes"] = bytes.size();
    return result;
}

std::vector<SseReplayChunk> replay_chunks_from_json(const Json& document,
                                                   const ProviderProfile& profile) {
    if (!document.is_object() || document.as_object().size() != 2 ||
        !document.as_object().contains("format") || !document.as_object().contains("chunks") ||
        document.at("format") != "wls-sse-replay-v1" || !document.at("chunks").is_array())
        throw Error(ErrorCode::InvalidInput, "expected wls-sse-replay-v1 with only format/chunks");
    const auto& items = document.at("chunks").as_array();
    const auto limits = SseReplayLimits{};
    if (items.size() > std::min(limits.max_chunks, profile.limits.max_sse_chunks))
        throw Error(ErrorCode::Quota, "replay chunk count exceeds capacity");
    std::vector<SseReplayChunk> result;
    result.reserve(items.size());
    const auto byte_limit = std::min(limits.max_total_bytes, profile.limits.max_stream_bytes);
    std::size_t total = 0;
    std::int64_t previous = 0;
    for (const auto& item : items) {
        if (!item.is_object() || item.as_object().size() != 3 ||
            !item.as_object().contains("data") || !item.as_object().contains("elapsed_ms") ||
            !item.as_object().contains("cancel_before") || !item.at("data").is_string() ||
            !item.at("cancel_before").is_bool())
            throw Error(ErrorCode::InvalidInput, "replay chunks require data, elapsed_ms, cancel_before");
        const auto elapsed = integer(item.at("elapsed_ms"));
        if (elapsed < previous)
            throw Error(ErrorCode::InvalidInput, "replay arrival times must be nonnegative and nondecreasing");
        previous = elapsed;
        const auto& data = item.at("data").as_string();
        if (data.size() > byte_limit - total)
            throw Error(ErrorCode::Quota, "replay input byte capacity exceeded");
        total += data.size();
        result.push_back({std::string(data), std::chrono::milliseconds(elapsed), item.at("cancel_before").as_bool()});
    }
    return result;
}

int run_protocol_command(const std::string& command, Options& options) {
    const auto profile_path = options.required("--profile");
    const auto request_path = options.required("--request");
    const bool encoding_only = command == "encode-model";
    std::string http_path, stream_path, replay_path;
    bool stream = false;
    std::int64_t http_status = 200;
    std::int64_t chunk_bytes = 4096;
    if (encoding_only) {
        const auto choice = options.take("--stream", "false");
        if (choice != "true" && choice != "false")
            throw Error(ErrorCode::InvalidInput, "--stream must be true or false");
        stream = choice == "true";
    } else {
        http_path = options.take("--http-response");
        stream_path = options.take("--sse-stream");
        replay_path = options.take("--replay");
        const auto modes = static_cast<int>(!http_path.empty()) + static_cast<int>(!stream_path.empty()) +
            static_cast<int>(!replay_path.empty());
        if (modes != 1)
            throw Error(ErrorCode::InvalidInput, "replay needs exactly one HTTP body, SSE stream or timed replay file");
        const bool status_supplied = options.has("--http-status");
        const bool chunk_supplied = options.has("--chunk-bytes");
        http_status = options.number("--http-status", 200, 100);
        chunk_bytes = options.number("--chunk-bytes", 4096, 1);
        if (http_status > 599 || chunk_bytes > 65536 ||
            (status_supplied && http_path.empty()) || (chunk_supplied && stream_path.empty()))
            throw Error(ErrorCode::InvalidInput, "invalid or inapplicable replay HTTP status/chunk size");
        stream = http_path.empty();
    }
    // Reject all options before opening any input, including stray --db.
    options.finish();
    for (const auto& path : {profile_path, request_path, http_path, stream_path, replay_path})
        if (!path.empty()) static_cast<void>(input_path_details(path, "supplied"));
    const auto profile_bytes = read_runtime_file(profile_path, 65536);
    const auto profile = provider_profile_from_json(parse_json_strict(profile_bytes, 65536));
    const auto request_bytes = read_runtime_file(request_path, 4U * 1024U * 1024U);
    const auto request = model_request_from_json(parse_json_strict(request_bytes, 4U * 1024U * 1024U));
    const auto encoded = encode_responses_request(profile, request, stream);
    Object inputs{{"profile", protocol_input_details(profile_path, profile_bytes)},
                  {"request", protocol_input_details(request_path, request_bytes)}};
    Object report{{"format", "wls-offline-protocol-report-v1"},
        {"mode", encoding_only ? "encode_only" : (stream ? "sse_replay" : "http_body_replay")},
        {"network_requests", 0}, {"paid_attempts", 0}, {"tool_dispatches", 0},
        {"credentials_resolved", false}, {"store_opened", false},
        {"encoded_request", to_json(encoded)}};
    if (encoding_only) {
        report["inputs"] = std::move(inputs);
        protocol_output(report);
        return 0;
    }
    ModelResultStatus status;
    if (!http_path.empty()) {
        const auto raw = read_runtime_file(http_path, profile.limits.max_response_bytes);
        inputs["http_body"] = protocol_input_details(http_path, raw);
        inputs["http_status"] = Object{{"origin", "local_fixture_parameter"}, {"value", http_status}};
        const auto decoded = decode_responses_response(profile, request, static_cast<int>(http_status), raw);
        status = decoded.response.status;
        report["result"] = to_json(decoded);
    } else {
        std::vector<SseReplayChunk> chunks;
        const auto limits = SseReplayLimits{};
        if (!stream_path.empty()) {
            const auto raw = read_runtime_file(stream_path, std::min(limits.max_total_bytes, profile.limits.max_stream_bytes));
            inputs["sse_stream"] = protocol_input_details(stream_path, raw);
            inputs["chunk_bytes"] = chunk_bytes;
            const auto width = static_cast<std::size_t>(chunk_bytes);
            const auto count = raw.size() / width + (raw.size() % width != 0 ? 1U : 0U);
            if (count > std::min(limits.max_chunks, profile.limits.max_sse_chunks))
                throw Error(ErrorCode::Quota, "requested splitting exceeds replay chunk capacity");
            chunks.reserve(count);
            for (std::size_t offset = 0; offset < raw.size(); offset += width)
                chunks.push_back({raw.substr(offset, width), std::chrono::milliseconds(0), false});
        } else {
            const auto raw = read_runtime_file(replay_path, 8U * 1024U * 1024U);
            inputs["timed_replay"] = protocol_input_details(replay_path, raw);
            chunks = replay_chunks_from_json(parse_json_strict(raw, 8U * 1024U * 1024U, 16), profile);
        }
        const auto decoded = replay_responses_sse(profile, request, chunks);
        status = decoded.response.status;
        report["result"] = to_json(decoded);
    }
    report["inputs"] = std::move(inputs);
    protocol_output(report);
    return status == ModelResultStatus::Completed ? 0 : 4;
}
