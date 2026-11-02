// Included by the existing Store suite. These exercise durable behavior through
// the real API; the separate GNU ld serialization-count probe remains optional.
using RawStoreRows = std::vector<std::vector<std::pair<int, std::string>>>;

RawStoreRows original_store_rows(const std::filesystem::path& path, const std::string& query) {
    sqlite3* handle{};
    CHECK(sqlite3_open_v2(path.c_str(), &handle, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    std::unique_ptr<sqlite3, decltype(&sqlite3_close)> db(handle, &sqlite3_close);
    sqlite3_stmt* statement{};
    CHECK(sqlite3_prepare_v2(db.get(), query.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> owned(statement, &sqlite3_finalize);
    RawStoreRows result;
    for (;;) {
        const auto status = sqlite3_step(owned.get());
        CHECK(status == SQLITE_ROW || status == SQLITE_DONE);
        if (status == SQLITE_DONE) return result;
        auto& row = result.emplace_back();
        for (int column = 0; column < sqlite3_column_count(owned.get()); ++column) {
            const auto type = sqlite3_column_type(owned.get(), column);
            const auto* bytes = static_cast<const char*>(sqlite3_column_blob(owned.get(), column));
            const auto size = static_cast<std::size_t>(sqlite3_column_bytes(owned.get(), column));
            row.emplace_back(type, std::string(bytes ? bytes : "", size));
        }
    }
}

Json expected_plan_command(const StoreActor& actor, std::int64_t version,
                           std::int64_t requirements, const Json& body) {
    return Object{{"type", "plan"}, {"expected_request_version", version},
        {"based_on_requirements_revision", requirements}, {"body", body},
        {"actor", Object{{"role", role_name(actor.role)}, {"principal_id", actor.principal_id},
            {"request_id", actor.request_id}, {"task_id", actor.task_id}, {"team_id", actor.team_id},
            {"dispatcher_epoch", actor.dispatcher_epoch}, {"execution_epoch", actor.execution_epoch}}}};
}

void event_original_bytes_hash_and_replay_guards_are_durable() {
    test::TempDir temp;
    const auto path = temp.path / "event-original.sqlite";
    Store store(path, base_sql, migration_sql);
    const auto id = s(store.admit(input("permanent-event-bytes")), "request_id");
    const auto actor = store.host_actor(Role::Dispatcher, id);
    const auto before = store.request(id), original = store.request_snapshot(id), budget = store.budget(id);
    const auto version = n(before, "state_version"), requirements = n(before, "requirements_revision");
    const auto history = original_store_rows(path, "SELECT * FROM events ORDER BY seq");
    const Json body = Object{{"goal", "preserve \\ \"quoted\" 原始字节"},
        {"metadata", Object{{"nested", Array{Object{{"words", Array{"中文", "original"}}, {"count", 2}}}},
            {"absent", nullptr}, {"signed", -1234567890123}, {"unsigned", 18446744073709551615ULL},
            {"decimal", 3.25}, {"negative_zero", -0.0}}}};
    const auto command = expected_plan_command(actor, version, requirements, body);
    const auto bytes = canonical_json(command), hash = sha256_hex(bytes), key = new_id("cmd");
    CHECK(!command.at("actor").as_object().contains("writer_session"));
    const auto published = store.publish_plan(actor, version, requirements, body, key);
    const auto after = store.request(id);
    CHECK(n(after, "state_version") == version + 1 && n(after, "plan_revision") == n(before, "plan_revision") + 1);
    CHECK(after.at("dispatcher_epoch") == before.at("dispatcher_epoch"));
    CHECK(after.at("requirements_revision") == before.at("requirements_revision"));
    CHECK(after.at("config_snapshot_sha256") == before.at("config_snapshot_sha256"));
    CHECK(store.request_snapshot(id) == original && store.budget(id) == budget);
    const auto appended = original_store_rows(path, "SELECT * FROM events ORDER BY seq");
    CHECK(appended.size() == history.size() + 1);
    CHECK(std::equal(history.begin(), history.end(), appended.begin()));
    const auto event = original_store_rows(path, "SELECT payload,command_sha256,result,command_id,actor FROM events WHERE type='plan'");
    CHECK(event.size() == 1 && event.front().size() == 5);
    CHECK(event[0][0].second == bytes && event[0][1].second == hash);
    CHECK(sha256_hex(event[0][0].second) == event[0][1].second);
    CHECK(parse_json_strict(event[0][0].second) == command);
    CHECK(event[0][2].second == canonical_json(published) && parse_json_strict(event[0][2].second) == published);
    CHECK(event[0][3].second == key && event[0][4].second == "dispatcher");
    CHECK(original_store_rows(path, "SELECT body FROM plans")[0][0].second == canonical_json(body));
    const auto plans = original_store_rows(path, "SELECT * FROM plans ORDER BY rowid");
    const auto request_rows = original_store_rows(path, "SELECT * FROM requests ORDER BY rowid");
    const auto frozen_capacity = store.capacity_status(id);
    const auto unchanged = [&] {
        CHECK(original_store_rows(path, "SELECT * FROM events ORDER BY seq") == appended);
        CHECK(original_store_rows(path, "SELECT * FROM plans ORDER BY rowid") == plans);
        CHECK(original_store_rows(path, "SELECT * FROM requests ORDER BY rowid") == request_rows);
        CHECK(store.request(id) == after && store.request_snapshot(id) == original);
        CHECK(store.budget(id) == budget && store.capacity_status(id) == frozen_capacity);
    };
    for (int repeat = 0; repeat != 2; ++repeat) {
        CHECK(store.publish_plan(actor, version, requirements, body, key) == published);
        unchanged();
    }
    auto foreign = actor; foreign.writer_session += "-foreign";
    for (const auto& rejected_key : {key, new_id("cmd")}) {
        error(ErrorCode::Stale, [&] { (void)store.publish_plan(foreign, version, requirements, body, rejected_key); });
        unchanged();
    }
    auto changed = body; changed.at("goal") = "changed intent";
    error(ErrorCode::Conflict, [&] { (void)store.publish_plan(actor, version, requirements, changed, key); });
    unchanged();
    error(ErrorCode::Conflict, [&] { (void)store.publish_plan(actor, version + 1, requirements, body, key); });
    unchanged();
    auto other = actor; other.principal_id += "-other";
    error(ErrorCode::Unauthorized, [&] { (void)store.publish_plan(other, version, requirements, body, key); });
    unchanged();
    error(ErrorCode::Conflict, [&] { (void)store.publish_plan(actor, version, requirements, body, new_id("cmd")); });
    unchanged();
}

void event_encoding_limits_roll_back_plan_fences_and_original_rows() {
    Fixture f; f.ready();
    const auto operation = f.prepare();
    (void)f.store->start_operation(f.actor(Role::Worker), s(operation, "op_id"), n(operation, "state_version"), new_id("start"));
    CHECK(f.store->operation(s(operation, "op_id")).at("status") == "RUNNING");
    const auto actor = f.actor(Role::Dispatcher);
    const auto request = f.store->request(f.request_id), original = f.store->request_snapshot(f.request_id);
    const auto budget = f.store->budget(f.request_id), capacity = f.store->capacity_status(f.request_id);
    const auto version = n(request, "state_version"), requirements = n(request, "requirements_revision");
    const std::vector<std::string> tables{"requests", "request_texts", "request_constraints", "request_budgets",
        "plans", "tasks", "teams", "ops", "events"};
    std::vector<RawStoreRows> before;
    for (const auto& table : tables) before.push_back(original_store_rows(f.path, "SELECT * FROM " + table + " ORDER BY rowid"));
    const auto unchanged = [&] {
        for (std::size_t index = 0; index != tables.size(); ++index)
            CHECK(original_store_rows(f.path, "SELECT * FROM " + tables[index] + " ORDER BY rowid") == before[index]);
        CHECK(f.store->request(f.request_id) == request && f.store->request_snapshot(f.request_id) == original);
        CHECK(f.store->budget(f.request_id) == budget && f.store->capacity_status(f.request_id) == capacity);
        CHECK(f.store->operation(s(operation, "op_id")).at("status") == "RUNNING");
    };
    const Json invalid_utf8 = Object{{"goal", std::string(1, static_cast<char>(0xff))}};
    error(ErrorCode::InvalidInput, [&] { (void)f.store->publish_plan(actor, version, requirements, invalid_utf8, new_id("cmd")); });
    unchanged();
    // Body depth 64 is valid by itself. Its surrounding command adds the
    // 65th container, so the event guard must roll back the already-inserted
    // plan, request CAS and fencing of this real running operation.
    Json nested = 0;
    for (int depth = 0; depth != 63; ++depth) nested = Array{std::move(nested)};
    const Json deep = Object{{"goal", "depth boundary"}, {"nested", nested}};
    CHECK(parse_json_strict(canonical_json(deep)) == deep);
    const auto deep_command = canonical_json(expected_plan_command(actor, version, requirements, deep));
    error(ErrorCode::InvalidInput, [&] { (void)parse_json_strict(deep_command); });
    error(ErrorCode::InvalidInput, [&] { (void)f.store->publish_plan(actor, version, requirements, deep, new_id("cmd")); });
    unchanged();
    constexpr std::size_t limit = 1024U * 1024U;
    const auto empty_body = canonical_json(Object{{"goal", ""}}).size();
    const Json oversized_envelope = Object{{"goal", std::string(limit - empty_body, 'x')}};
    CHECK(canonical_json(oversized_envelope).size() == limit);
    CHECK(parse_json_strict(canonical_json(oversized_envelope), limit) == oversized_envelope);
    CHECK(canonical_json(expected_plan_command(actor, version, requirements, oversized_envelope)).size() > limit);
    error(ErrorCode::Quota, [&] { (void)f.store->publish_plan(actor, version, requirements, oversized_envelope, new_id("cmd")); });
    unchanged();
    // Exact command limit is accepted. This confirms the earlier quota case
    // did not fail at the smaller plan body and did not leave stale cache/CAS.
    const auto empty_command = canonical_json(expected_plan_command(actor, version, requirements, Object{{"goal", ""}})).size();
    const Json boundary = Object{{"goal", std::string(limit - empty_command, 'y')}};
    const auto boundary_bytes = canonical_json(expected_plan_command(actor, version, requirements, boundary));
    CHECK(boundary_bytes.size() == limit && canonical_json(boundary).size() < limit);
    (void)f.store->publish_plan(actor, version, requirements, boundary, new_id("cmd"));
    CHECK(f.rv() == version + 1);
    CHECK(f.store->operation(s(operation, "op_id")).at("status") == "UNKNOWN");
    CHECK(f.store->request_snapshot(f.request_id) == original && f.store->budget(f.request_id) == budget);
    const auto appended = original_store_rows(f.path, "SELECT * FROM events ORDER BY seq");
    CHECK(appended.size() == before.back().size() + 1);
    CHECK(std::equal(before.back().begin(), before.back().end(), appended.begin()));
    const auto event = original_store_rows(f.path, "SELECT payload,command_sha256 FROM events WHERE type='plan' ORDER BY seq DESC LIMIT 1");
    CHECK(event.size() == 1 && event[0][0].second == boundary_bytes);
    CHECK(event[0][1].second == sha256_hex(boundary_bytes));
}
