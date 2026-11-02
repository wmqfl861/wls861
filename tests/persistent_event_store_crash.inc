// Included inside the Store test's anonymous namespace. All barriers live in
// this test process: production Store and migration assets have no test hooks.
using CrashConnection = std::unique_ptr<sqlite3, decltype(&sqlite3_close)>;
using CrashStatement = std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)>;

CrashConnection crash_read_connection(const std::filesystem::path& path) {
    sqlite3* raw{};
    const auto status = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
    CrashConnection connection(raw, sqlite3_close); CHECK(status == SQLITE_OK);
    CHECK(sqlite3_busy_timeout(connection.get(), 1000) == SQLITE_OK); return connection;
}
CrashStatement crash_statement(sqlite3* db, const std::string& query,
                               const std::vector<std::string>& arguments = {}) {
    sqlite3_stmt* raw{};
    const auto status = sqlite3_prepare_v2(db, query.c_str(), -1, &raw, nullptr);
    CrashStatement statement(raw, sqlite3_finalize); CHECK(status == SQLITE_OK);
    for (std::size_t index = 0; index < arguments.size(); ++index)
        CHECK(sqlite3_bind_text(statement.get(), static_cast<int>(index + 1), arguments[index].c_str(),
                               -1, SQLITE_TRANSIENT) == SQLITE_OK);
    return statement;
}
Array crash_rows_on(sqlite3* db, const std::string& query,
                    const std::vector<std::string>& arguments = {}) {
    auto statement = crash_statement(db, query, arguments);
    std::vector<std::string> hashes; int status{};
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        std::string row;
        for (int column = 0; column < sqlite3_column_count(statement.get()); ++column) {
            const auto type = sqlite3_column_type(statement.get(), column);
            const auto* bytes = static_cast<const char*>(sqlite3_column_blob(statement.get(), column));
            const auto size = sqlite3_column_bytes(statement.get(), column);
            row += std::to_string(type) + ':' + std::to_string(size) + ':';
            if (size) row.append(bytes, static_cast<std::size_t>(size));
            row += '|';
        }
        hashes.push_back(sha256_hex(row));
    }
    CHECK(status == SQLITE_DONE); std::sort(hashes.begin(), hashes.end());
    Array result; for (const auto& hash : hashes) result.emplace_back(hash); return result;
}
Array crash_rows(const std::filesystem::path& path, const std::string& query,
                 const std::vector<std::string>& arguments = {}) {
    auto db = crash_read_connection(path); return crash_rows_on(db.get(), query, arguments);
}
std::string crash_database_hash(const std::filesystem::path& path) {
    auto db = crash_read_connection(path);
    CHECK(sqlite3_exec(db.get(), "BEGIN", nullptr, nullptr, nullptr) == SQLITE_OK);
    auto statement = crash_statement(db.get(),
        "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name");
    Object tables; int status{};
    while ((status = sqlite3_step(statement.get())) == SQLITE_ROW) {
        const std::string name(reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0)));
        CHECK(name.find('"') == std::string::npos);
        tables[name] = crash_rows_on(db.get(), "SELECT * FROM \"" + name + "\"");
    }
    CHECK(status == SQLITE_DONE);
    CHECK(sqlite3_exec(db.get(), "COMMIT", nullptr, nullptr, nullptr) == SQLITE_OK);
    return sha256_hex(canonical_json(tables));
}
std::int64_t crash_integer(const std::filesystem::path& path, const std::string& query,
                          const std::vector<std::string>& arguments = {}) {
    auto db = crash_read_connection(path); auto statement = crash_statement(db.get(), query, arguments);
    CHECK(sqlite3_step(statement.get()) == SQLITE_ROW);
    CHECK(sqlite3_column_type(statement.get(), 0) == SQLITE_INTEGER);
    const auto result = static_cast<std::int64_t>(sqlite3_column_int64(statement.get(), 0));
    CHECK(sqlite3_step(statement.get()) == SQLITE_DONE); return result;
}
Json crash_json(const std::filesystem::path& path, const std::string& query,
                const std::vector<std::string>& arguments = {}) {
    auto db = crash_read_connection(path); auto statement = crash_statement(db.get(), query, arguments);
    CHECK(sqlite3_step(statement.get()) == SQLITE_ROW);
    CHECK(sqlite3_column_type(statement.get(), 0) == SQLITE_TEXT);
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement.get(), 0));
    const std::string encoded(bytes, static_cast<std::size_t>(sqlite3_column_bytes(statement.get(), 0)));
    CHECK(sqlite3_step(statement.get()) == SQLITE_DONE); return parse_json_strict(encoded);
}
Object crash_actor_json(const StoreActor& actor) {
    // The session crosses only the parent/child pipe, so the parent can test
    // rejection of the actual dead writer's authority. Never write it to DB.
    return Object{{"role", role_name(actor.role)}, {"principal_id", actor.principal_id},
        {"request_id", actor.request_id}, {"task_id", actor.task_id}, {"team_id", actor.team_id},
        {"dispatcher_epoch", actor.dispatcher_epoch}, {"execution_epoch", actor.execution_epoch},
        {"writer_session", actor.writer_session}};
}
StoreActor crash_actor_from_json(const Json& value) {
    StoreActor actor; actor.role = role_from_string(text(value, "role"));
    actor.principal_id = text(value, "principal_id"); actor.request_id = text(value, "request_id");
    actor.task_id = text(value, "task_id"); actor.team_id = text(value, "team_id");
    actor.dispatcher_epoch = number(value, "dispatcher_epoch"); actor.execution_epoch = number(value, "execution_epoch");
    actor.writer_session = text(value, "writer_session"); return actor;
}
struct CrashPipe {
    int read_fd{-1}, write_fd{-1};
    CrashPipe() {
        int descriptors[2]; CHECK(::pipe(descriptors) == 0);
        read_fd = descriptors[0]; write_fd = descriptors[1];
    }
    ~CrashPipe() { close_read(); close_write(); }
    void close_read() { if (read_fd >= 0) { ::close(read_fd); read_fd = -1; } }
    void close_write() { if (write_fd >= 0) { ::close(write_fd); write_fd = -1; } }
};
void crash_write_bytes(int descriptor, const void* data, std::size_t size) {
    const auto* bytes = static_cast<const char*>(data);
    while (size) {
        const auto written = ::write(descriptor, bytes, size);
        if (written < 0 && errno == EINTR) continue;
        CHECK(written > 0); const auto count = static_cast<std::size_t>(written);
        bytes += count; size -= count;
    }
}
void crash_send(int descriptor, const Json& packet) {
    const auto bytes = canonical_json(packet); CHECK(bytes.size() <= 65536);
    const auto size = static_cast<std::uint32_t>(bytes.size());
    crash_write_bytes(descriptor, &size, sizeof(size));
    crash_write_bytes(descriptor, bytes.data(), bytes.size());
}
void crash_read_bytes(int descriptor, void* data, std::size_t size,
                       std::chrono::steady_clock::time_point deadline) {
    auto* bytes = static_cast<char*>(data);
    while (size) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        CHECK(remaining > 0); pollfd pending{descriptor, POLLIN, 0};
        const auto polled = ::poll(&pending, 1, static_cast<int>(remaining));
        if (polled < 0 && errno == EINTR) continue;
        CHECK(polled == 1); const auto count = ::read(descriptor, bytes, size);
        if (count < 0 && errno == EINTR) continue;
        CHECK(count > 0); const auto received = static_cast<std::size_t>(count);
        bytes += received; size -= received;
    }
}
Json crash_receive(int descriptor) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    std::uint32_t size{}; crash_read_bytes(descriptor, &size, sizeof(size), deadline);
    CHECK(size > 0 && size <= 65536); std::string bytes(size, '\0');
    crash_read_bytes(descriptor, bytes.data(), bytes.size(), deadline);
    auto packet = parse_json_strict(bytes);
    if (packet.at("kind") == "error") throw std::runtime_error("crash child: " + text(packet, "message"));
    CHECK(packet.at("kind") == "barrier"); return packet;
}
struct CrashChild {
    pid_t pid;
    explicit CrashChild(pid_t value) : pid(value) { CHECK(pid > 0); }
    ~CrashChild() {
        if (pid > 0) {
            ::kill(pid, SIGKILL); int status{};
            while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        }
    }
    void kill_and_reap() {
        CHECK(::kill(pid, SIGKILL) == 0); int status{}; pid_t result;
        do { result = ::waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
        CHECK(result == pid); pid = -1;
        CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    }
};
struct CrashBarrier {
    std::string target;
    int ready_fd, hold_fd;
    std::int64_t callbacks{};
    Object facts;
    void checkpoint(const std::filesystem::path& path, Store& store, const std::string& request) {
        facts["checkpoint_sha256"] = crash_database_hash(path); facts["budget"] = store.budget(request);
    }
    void arrive(std::string_view phase) {
        if (phase != target) return;
        facts["kind"] = "barrier"; facts["phase"] = phase; facts["callbacks"] = callbacks;
        crash_send(ready_fd, facts);
        char release{}; ssize_t count;
        do { count = ::read(hold_fd, &release, 1); } while (count < 0 && errno == EINTR);
        ::_exit(92); // Only the parent's known-pid SIGKILL may complete a case.
    }
};
CrashBarrier* active_crash_barrier{};
void crash_sql_barrier(sqlite3_context* context, int count, sqlite3_value** values) noexcept {
    try {
        CHECK(count == 1 && active_crash_barrier);
        const auto* phase = reinterpret_cast<const char*>(sqlite3_value_text(values[0])); CHECK(phase);
        active_crash_barrier->arrive(phase); sqlite3_result_null(context);
    } catch (...) { ::_exit(93); }
}
int install_crash_barrier(sqlite3* database, char**, const sqlite3_api_routines*) {
    return sqlite3_create_function_v2(database, "n5_test_crash_barrier", 1,
        SQLITE_UTF8 | SQLITE_INNOCUOUS, nullptr, crash_sql_barrier, nullptr, nullptr, nullptr);
}
struct CrashPhase {
    std::string name;
    std::string trigger;
    int durable_stage;
    std::int64_t callbacks;
    std::string control;
};
CrashPhase crash_phase(std::string name, std::string boundary, int stage, std::int64_t callbacks = 0) {
    std::string trigger;
    if (!boundary.empty()) trigger = "CREATE TRIGGER n5_test_crash " + boundary +
        " BEGIN SELECT n5_test_crash_barrier('" + name + "'); END";
    return {std::move(name), std::move(trigger), stage, callbacks, {}};
}
ModelResponse crash_model_response(const Bound& value, CrashBarrier& barrier) {
    // The counter is incremented in the actual ModelAdapter::complete entry,
    // before FakeModel runs. Reaching a future or a reservation is not a call.
    class CountedFake final : public ModelAdapter {
    public:
        CountedFake(FakeModelStep step, std::string model, CrashBarrier& state)
            : fake_({std::move(step)}, std::move(model)), state_(state) {}
        AdapterCapabilities capabilities() const override { return fake_.capabilities(); }
        void validate(const ModelRequest& request) const override { fake_.validate(request); }
        ModelResponse complete(ModelRequest request, std::stop_token stop = {}) const override {
            ++state_.callbacks; state_.arrive("W07"); return fake_.complete(std::move(request), stop);
        }
    private:
        FakeModel fake_; CrashBarrier& state_;
    };
    FakeModelStep step; step.role = value.captured.request.role;
    step.role_call_index = value.captured.request.role_call_index;
    step.tool_calls = {{"original-tool", "note", Object{{"text", "one original crash-test note"}}}};
    step.usage.input_tokens = 20; step.usage.output_tokens = 10;
    step.usage.cache_read_tokens = 0; step.usage.cache_write_tokens = 0; step.usage.cost_micro_usd = 0;
    CountedFake adapter(std::move(step), value.captured.request.model.model, barrier);
    return adapter.complete(value.captured.request);
}
void crash_capture_facts(CrashBarrier& barrier, const Bound& value) {
    barrier.facts["actor"] = crash_actor_json(value.captured.actor);
    barrier.facts["attempt_id"] = value.captured.request.attempt_id;
    barrier.facts["call_id"] = value.captured.request.call_id;
    barrier.facts["binding"] = to_json(value.captured.request.binding);
    barrier.facts["request_envelope_sha256"] = sha256_hex(canonical_json(to_json(value.captured.request)));
    barrier.facts["claim_id"] = ""; barrier.facts["link_id"] = "";
    barrier.facts["generation"] = 0; barrier.facts["selected_ids"] = Array{};
}
void crash_child_error(int descriptor, const std::exception& error) noexcept {
    try { crash_send(descriptor, Object{{"kind", "error"}, {"message", error.what()}}); } catch (...) {}
}
void crash_run_child(Fixture& fixture, const CrashPhase& phase, CrashPipe& ready, CrashPipe& hold) {
    ready.close_read(); hold.close_write();
    try {
        CHECK(sqlite3_auto_extension(reinterpret_cast<void (*)()>(install_crash_barrier)) == SQLITE_OK);
        fixture.store = std::make_unique<Store>(fixture.path, MigrationRegistry(resources()),
            StoreOpenMode::Takeover, StoreRecoveryScope::request(fixture.id));
        CrashBarrier barrier{phase.name, ready.write_fd, hold.read_fd, 0, {}};
        active_crash_barrier = &barrier;
        Bound value; OfflineRuntime runtime(*fixture.store, fixture.schemas);
        value.captured = runtime.capture_call(fixture.id, Role::Worker, text(fixture.task(), "task_id"));
        value.reservation = reservation_for(value.captured); crash_capture_facts(barrier, value);
        barrier.checkpoint(fixture.path, *fixture.store, fixture.id);
        if (!phase.trigger.empty()) sql(fixture.path, phase.trigger);
        barrier.arrive("W00");
        value.claim = fixture.store->claim_event_batch(value.captured.actor);
        value.association.claim_id = text(value.claim, "claim_id"); CHECK(!value.association.claim_id.empty());
        value.association.generation = number(value.claim, "generation");
        value.association.expected_state_version = value.captured.expected_state_version;
        value.association.workflow_action = value.captured.workflow_action;
        for (const auto& item : value.claim.at("items").as_array())
            if (item.at("event_requirements_revision") == value.captured.request.binding.requirements_revision)
                value.association.selected_ids.push_back(number(item, "inbox_id"));
        CHECK(value.association.selected_ids.size() == 2);
        barrier.facts["claim_id"] = value.association.claim_id;
        barrier.facts["generation"] = value.association.generation;
        Array selected; for (const auto id : value.association.selected_ids) selected.emplace_back(id);
        barrier.facts["selected_ids"] = selected;
        barrier.facts["claim_rows_sha256"] = sha256_hex(canonical_json(crash_rows(fixture.path,
            "SELECT * FROM n5_event_claims WHERE claim_id=?", {value.association.claim_id})));
        barrier.checkpoint(fixture.path, *fixture.store, fixture.id); barrier.arrive("W02");
        fixture.reserve(value); barrier.facts["link_id"] = text(value.link, "link_id");
        barrier.facts["link_rows_sha256"] = sha256_hex(canonical_json(crash_rows(fixture.path,
            "SELECT * FROM n5_event_links WHERE link_id=?", {text(value.link, "link_id")})));
        barrier.facts["item_rows_sha256"] = sha256_hex(canonical_json(crash_rows(fixture.path,
            "SELECT * FROM n5_event_items WHERE link_id=?", {text(value.link, "link_id")})));
        barrier.checkpoint(fixture.path, *fixture.store, fixture.id); barrier.arrive("W04");
        CHECK(fixture.store->claim_event_dispatch(value.captured.actor, text(value.link, "link_id")).at("dispatch_claimed") == true);
        barrier.checkpoint(fixture.path, *fixture.store, fixture.id); barrier.arrive("W06");
        const auto response = crash_model_response(value, barrier);
        barrier.facts["response_sha256"] = sha256_hex(canonical_json(to_json(response))); barrier.arrive("W08");
        fixture.store->settle_event_observation(text(value.link, "link_id"), response);
        barrier.checkpoint(fixture.path, *fixture.store, fixture.id); barrier.arrive("W10");
        if (!phase.control.empty()) {
            const auto root = fixture.actor(Role::Root);
            const auto version = number(fixture.store->request(fixture.id), "state_version");
            if (phase.control == "amend") fixture.store->amend(root, version, "preserve fixture observations",
                input().constraint_catalog, new_id("control"));
            else fixture.store->cancel_request(root, version, new_id("control"));
            barrier.checkpoint(fixture.path, *fixture.store, fixture.id); barrier.arrive(phase.name);
        }
        CHECK(fixture.store->commit_event_action_and_finish(value.captured.actor, text(value.link, "link_id"), fixture.schemas).at("state") == "CLOSED");
        barrier.checkpoint(fixture.path, *fixture.store, fixture.id); barrier.arrive("W15");
        ::_exit(94); // A missing barrier must fail, never count as a crash.
    } catch (const std::exception& error) { crash_child_error(ready.write_fd, error); ::_exit(95); }
}
void crash_assert_original_facts(const std::filesystem::path& path, const Json& packet) {
    const auto claim = text(packet, "claim_id"), link = text(packet, "link_id");
    if (!claim.empty()) CHECK(sha256_hex(canonical_json(crash_rows(path,
        "SELECT * FROM n5_event_claims WHERE claim_id=?", {claim}))) == text(packet, "claim_rows_sha256"));
    if (!link.empty()) {
        CHECK(sha256_hex(canonical_json(crash_rows(path, "SELECT * FROM n5_event_links WHERE link_id=?", {link}))) == text(packet, "link_rows_sha256"));
        CHECK(sha256_hex(canonical_json(crash_rows(path, "SELECT * FROM n5_event_items WHERE link_id=?", {link}))) == text(packet, "item_rows_sha256"));
    }
}
void process_crash_phase(const CrashPhase& phase) {
    Fixture fixture; fixture.plan(); fixture.ready();
    const auto initial_budget = fixture.store->budget(fixture.id);
    const auto initial_hash = crash_database_hash(fixture.path);
    const auto initial_event_seq = scalar(fixture.path, "SELECT MAX(seq) FROM events");
    const auto original_events = crash_rows(fixture.path, "SELECT * FROM events WHERE seq<=?", {std::to_string(initial_event_seq)});
    const auto original_claims = scalar(fixture.path, "SELECT COUNT(*) FROM n5_event_claims");
    const auto original_links = scalar(fixture.path, "SELECT COUNT(*) FROM n5_event_links");
    const auto original_feedbacks = scalar(fixture.path, "SELECT COUNT(*) FROM model_tool_feedback");
    const auto original_dispatches = scalar(fixture.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'");
    const auto original_settlements = scalar(fixture.path, "SELECT COUNT(*) FROM events WHERE type='settle_attempt'");
    const auto original_closes = scalar(fixture.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE kind='CLOSE'");
    const auto original_attempts = scalar(fixture.path, "SELECT COUNT(*) FROM call_attempts");
    CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM ops") == 0);
    fixture.store.reset(); // Fork has no inherited Store connection or writer lease.
    CrashPipe ready, hold; const auto pid = ::fork(); CHECK(pid >= 0);
    if (pid == 0) crash_run_child(fixture, phase, ready, hold);
    CrashChild child(pid); ready.close_write(); hold.close_read();
    const auto packet = crash_receive(ready.read_fd);
    CHECK(text(packet, "phase") == phase.name); CHECK(number(packet, "callbacks") == phase.callbacks);
    child.kill_and_reap();
    std::cout << "CRASH phase=" << phase.name << " pid=" << pid << " callbacks=" << phase.callbacks
              << " signal=SIGKILL reaped=true checkpoint_sha256=" << text(packet, "checkpoint_sha256") << std::endl;
    const auto stage = phase.durable_stage;
    const auto attempt_id = text(packet, "attempt_id"), link_id = text(packet, "link_id");
    Json original_attempt = nullptr, original_close = nullptr;
    Array committed_events, committed_receipts;
    std::int64_t committed_event_seq{};
    {
        Store reader(fixture.path, MigrationRegistry(resources()), StoreOpenMode::ReadOnly);
        CHECK(reader.diagnostics().at("user_version") == 411);
        CHECK(crash_database_hash(fixture.path) == text(packet, "checkpoint_sha256"));
        CHECK(reader.budget(fixture.id) == packet.at("budget"));
        if (stage == 0) { CHECK(crash_database_hash(fixture.path) == initial_hash); CHECK(reader.budget(fixture.id) == initial_budget); }
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM n5_event_claims") == original_claims + (stage >= 1 ? 1 : 0));
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM n5_event_links") == original_links + (stage >= 2 ? 1 : 0));
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM call_attempts") == original_attempts + (stage >= 2 ? 1 : 0));
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == original_dispatches + (stage >= 3 ? 1 : 0));
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM events WHERE type='settle_attempt'") == original_settlements + (stage >= 4 ? 1 : 0));
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE kind='CLOSE'") == original_closes + (stage == 5 ? 1 : 0));
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM model_tool_feedback") == original_feedbacks + (stage == 5 ? 1 : 0));
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM ops") == (stage == 5 ? 1 : 0));
        CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM n5_event_open_items") == (stage >= 2 && stage < 5 ? 2 : 0));
        crash_assert_original_facts(fixture.path, packet);
        if (stage >= 1) {
            const auto selected = packet.at("selected_ids").as_array(); CHECK(selected.size() == 2);
            for (const auto& id : selected) CHECK(crash_integer(fixture.path,
                "SELECT COUNT(*) FROM event_inbox WHERE inbox_id=? AND status=?", {std::to_string(id.as_int64()), stage == 5 ? "DONE" : "CLAIMED"}) == 1);
        }
        if (stage >= 2) {
            original_attempt = reader.attempt(attempt_id);
            CHECK(sha256_hex(canonical_json(original_attempt.at("request_envelope"))) == text(packet, "request_envelope_sha256"));
            const auto link = reader.event_link_status(link_id);
            CHECK(link.at("completed_ids").as_array().size() == (stage == 5 ? 2U : 0U));
            CHECK(crash_integer(fixture.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE link_id=? AND kind='DISPATCH'", {link_id}) == (stage >= 3 ? 1 : 0));
            CHECK(crash_integer(fixture.path, "SELECT COUNT(*) FROM n5_event_receipts WHERE link_id=? AND kind='SETTLEMENT'", {link_id}) == (stage >= 4 ? 1 : 0));
            if (stage >= 4) {
                CHECK(!original_attempt.at("settlement_sha256").is_null()); CHECK(original_attempt.at("accepted") == 1);
                CHECK(sha256_hex(canonical_json(original_attempt.at("response"))) == text(packet, "response_sha256"));
            } else { CHECK(original_attempt.at("settlement_sha256").is_null()); CHECK(original_attempt.at("response").is_null()); }
            if (stage == 5) {
                original_close = link; CHECK(link.at("state") == "CLOSED");
                CHECK(link.at("completed_ids") == packet.at("selected_ids"));
                CHECK(link.at("released_ids").as_array().empty());
                const auto saved_claim = crash_json(fixture.path, "SELECT fact FROM n5_event_claims WHERE claim_id=?", {text(packet, "claim_id")});
                CHECK(link.at("dispositions").as_array().size() == 2);
                for (const auto& captured_source : saved_claim.at("items").as_array()) {
                    const auto& dispositions = link.at("dispositions").as_array();
                    const auto proof = std::find_if(dispositions.begin(), dispositions.end(), [&](const Json& disposition) {
                        return disposition.at("inbox_id") == captured_source.at("inbox_id");
                    });
                    CHECK(proof != dispositions.end()); CHECK(proof->at("disposition") == "HANDLED");
                    CHECK(text(*proof, "source_sha256") == sha256_hex(canonical_json(captured_source)));
                    CHECK(proof->at("binding") == packet.at("binding")); CHECK(proof->at("op_id") == link.at("result").at("op_id"));
                }
                CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM ops WHERE status='SUCCEEDED'") == 1);
                CHECK(crash_integer(fixture.path,
                    "SELECT COUNT(*) FROM event_inbox i JOIN events e ON e.seq=i.event_seq WHERE e.type='tool_feedback' AND json_extract(e.payload,'$.attempt_id')=? AND i.role='worker' AND i.status='PENDING'",
                    {attempt_id}) == 1);
            }
        }
        if (phase.control == "amend")
            CHECK(number(reader.request(fixture.id), "requirements_revision") == number(packet.at("binding"), "requirements_revision") + 1);
        if (phase.control == "cancel") CHECK(reader.request(fixture.id).at("status") == "CANCELLED");
        committed_event_seq = scalar(fixture.path, "SELECT MAX(seq) FROM events");
        committed_events = crash_rows(fixture.path, "SELECT * FROM events");
        committed_receipts = crash_rows(fixture.path, "SELECT * FROM n5_event_receipts WHERE kind<>'RECOVERY'");
    }
    // The callback function exists only in the killed child. Remove only this
    // named test trigger before opening the recovering writer.
    if (!phase.trigger.empty()) sql(fixture.path, "DROP TRIGGER n5_test_crash");
    fixture.store = std::make_unique<Store>(fixture.path, MigrationRegistry(resources()),
        StoreOpenMode::Takeover, StoreRecoveryScope::request(fixture.id));
    const auto old_actor = crash_actor_from_json(packet.at("actor"));
    const auto recovered_hash = crash_database_hash(fixture.path);
    expect(ErrorCode::Stale, [&] { fixture.store->claim_event_batch(old_actor); });
    if (stage == 1) expect(ErrorCode::Stale, [&] {
        fixture.store->release_event_batch(old_actor, text(packet, "claim_id"), number(packet, "generation"));
    });
    if (stage >= 2) {
        expect(ErrorCode::Stale, [&] { fixture.store->claim_event_dispatch(old_actor, link_id); });
        expect(ErrorCode::Stale, [&] { fixture.store->commit_event_action_and_finish(old_actor, link_id, fixture.schemas); });
    }
    CHECK(crash_database_hash(fixture.path) == recovered_hash);
    CHECK(crash_rows(fixture.path, "SELECT * FROM events WHERE seq<=?", {std::to_string(initial_event_seq)}) == original_events);
    CHECK(crash_rows(fixture.path, "SELECT * FROM events WHERE seq<=?", {std::to_string(committed_event_seq)}) == committed_events);
    CHECK(crash_rows(fixture.path, "SELECT * FROM n5_event_receipts WHERE kind<>'RECOVERY'") == committed_receipts);
    crash_assert_original_facts(fixture.path, packet);
    CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM ops") == (stage == 5 ? 1 : 0));
    CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM model_tool_feedback") == original_feedbacks + (stage == 5 ? 1 : 0));
    if (stage >= 2) {
        const auto recovered_attempt = fixture.store->attempt(attempt_id);
        CHECK(recovered_attempt.at("request_envelope") == original_attempt.at("request_envelope"));
        CHECK(recovered_attempt.at("frozen_binding") == original_attempt.at("frozen_binding"));
        CHECK(recovered_attempt.at("response") == original_attempt.at("response"));
        CHECK(recovered_attempt.at("usage") == original_attempt.at("usage"));
        CHECK(recovered_attempt.at("settlement_sha256") == original_attempt.at("settlement_sha256"));
        for (const auto& id : packet.at("selected_ids").as_array()) CHECK(crash_integer(fixture.path,
            "SELECT COUNT(*) FROM event_inbox WHERE inbox_id=? AND status=?", {std::to_string(id.as_int64()), stage == 5 ? "DONE" : "PENDING"}) == 1);
        if (stage < 4) {
            CHECK(recovered_attempt.at("status") == "UNKNOWN");
            CHECK(recovered_attempt.at("unknown_input") == 1 && recovered_attempt.at("unknown_output") == 1 && recovered_attempt.at("unknown_cost") == 1);
            for (const auto* dimension : {"input", "output", "cost"}) {
                const auto reserved = "reserved_" + std::string(dimension), unknown = "unknown_" + std::string(dimension);
                CHECK(fixture.store->budget(fixture.id).at(reserved) == packet.at("budget").at(reserved));
                CHECK(number(fixture.store->budget(fixture.id), unknown) == number(packet.at("budget"), unknown) + number(original_attempt, reserved));
            }
        } else { CHECK(recovered_attempt == original_attempt); CHECK(fixture.store->budget(fixture.id) == packet.at("budget")); }
        if (stage < 5 && phase.control != "cancel") {
            const auto before_hold = crash_database_hash(fixture.path);
            const auto held = fixture.store->claim_event_batch(fixture.actor(Role::Worker));
            CHECK(held.at("items").as_array().empty()); CHECK(held.at("hold_reason") == "HOLD_UNRESOLVED_ORIGINAL_CALL");
            CHECK(crash_database_hash(fixture.path) == before_hold);
        }
        if (stage == 5) {
            const auto current = fixture.actor(Role::Worker); const auto before_replay = crash_database_hash(fixture.path);
            CHECK(fixture.store->commit_event_action_and_finish(current, link_id, fixture.schemas) == original_close);
            CHECK(fixture.store->claim_event_dispatch(current, link_id).at("dispatch_claimed") == false);
            CHECK(crash_database_hash(fixture.path) == before_replay);
        }
    } else {
        CHECK(fixture.store->budget(fixture.id) == initial_budget);
        if (stage == 1) {
            CHECK(crash_integer(fixture.path, "SELECT COUNT(*) FROM event_actor_claims WHERE claim_id=? AND status='RELEASED'", {text(packet, "claim_id")}) == 1);
            const auto fresh_actor = fixture.actor(Role::Worker); const auto fresh = fixture.store->claim_event_batch(fresh_actor);
            CHECK(text(fresh, "claim_id") != text(packet, "claim_id"));
            for (const auto& id : packet.at("selected_ids").as_array()) CHECK(std::any_of(fresh.at("items").as_array().begin(), fresh.at("items").as_array().end(),
                [&](const Json& item) { return item.at("inbox_id") == id; }));
            fixture.store->release_event_batch(fresh_actor, text(fresh, "claim_id"), number(fresh, "generation"));
            CHECK(fixture.store->budget(fixture.id) == initial_budget);
        }
    }
    CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM call_attempts") == original_attempts + (stage >= 2 ? 1 : 0));
    CHECK(fixture.store->budget(fixture.id).at("attempts") == packet.at("budget").at("attempts"));
    CHECK(scalar(fixture.path, "SELECT COUNT(*) FROM events WHERE type='claim_offline_call'") == original_dispatches + (stage >= 3 ? 1 : 0));
}
void process_crash_claim_reservation_and_dispatch() {
    for (const auto& phase : {
            crash_phase("W00", "", 0),
            crash_phase("W01", "BEFORE INSERT ON n5_event_claims", 0),
            crash_phase("W02", "", 1),
            crash_phase("W03", "BEFORE INSERT ON n5_event_links", 1),
            crash_phase("W04", "", 2),
            crash_phase("W05", "BEFORE INSERT ON n5_event_receipts WHEN NEW.kind='DISPATCH'", 2)}) process_crash_phase(phase);
}
void process_crash_callback_and_observation() {
    for (const auto& phase : {
            crash_phase("W06", "", 3), crash_phase("W07", "", 3, 1),
            crash_phase("W08", "", 3, 1),
            crash_phase("W09", "BEFORE INSERT ON n5_event_receipts WHEN NEW.kind='SETTLEMENT'", 3, 1),
            crash_phase("W10", "", 4, 1)}) process_crash_phase(phase);
}
void process_crash_atomic_action_tail() {
    for (const auto& phase : {
            crash_phase("W11_prepare", "AFTER INSERT ON events WHEN NEW.type='prepare_op'", 4, 1),
            crash_phase("W11_start", "AFTER INSERT ON events WHEN NEW.type='start_op'", 4, 1),
            crash_phase("W12", "BEFORE INSERT ON model_tool_feedback", 4, 1),
            crash_phase("W13", "AFTER INSERT ON model_tool_feedback", 4, 1),
            crash_phase("W14_first_id", "AFTER UPDATE ON event_inbox WHEN NEW.status='DONE' AND NEW.role='worker'", 4, 1),
            crash_phase("W14_close", "BEFORE INSERT ON n5_event_receipts WHEN NEW.kind='CLOSE'", 4, 1),
            crash_phase("W15", "", 5, 1)}) process_crash_phase(phase);
}
void process_crash_control_after_observation() {
    for (const auto* control : {"amend", "cancel"}) {
        auto phase = crash_phase("W16_" + std::string(control), "", 4, 1);
        phase.control = control; process_crash_phase(phase);
    }
}

void process_crash_legacy_feedback_gap_upgrade() {
    test::TempDir directory; const auto path = directory.path / "legacy-crash.sqlite";
    const SchemaRegistry schemas(resources().schema_documents());
    CrashPipe ready, hold; const auto pid = ::fork(); CHECK(pid >= 0);
    if (pid == 0) {
        ready.close_read(); hold.close_write();
        try {
            CHECK(sqlite3_auto_extension(reinterpret_cast<void (*)()>(install_crash_barrier)) == SQLITE_OK);
            Store legacy(path, MigrationRegistry(resources(), 410)); OfflineRuntime runtime(legacy, schemas);
            const auto request = text(legacy.admit(input()), "request_id");
            const auto complete = [&](Role role, const std::string& task_id, const std::string& tool, const Json& args) {
                Bound value; value.captured = runtime.capture_call(request, role, task_id);
                value.reservation = reservation_for(value.captured);
                legacy.reserve_attempt(value.captured.actor, value.captured.request.binding, value.reservation);
                CHECK(legacy.claim_offline_call(value.captured.actor, value.reservation.attempt_id).at("dispatch_claimed") == true);
                FakeModelStep step; step.role = role; step.role_call_index = value.captured.request.role_call_index;
                step.tool_calls = {{"original-tool", tool, args}};
                step.usage.input_tokens = 20; step.usage.output_tokens = 10; step.usage.cache_read_tokens = 0;
                step.usage.cache_write_tokens = 0; step.usage.cost_micro_usd = 0;
                const auto response = FakeModel({step}, value.captured.request.model.model).complete(value.captured.request);
                runtime.accept_independent_response(value.captured, response);
            };
            complete(Role::Dispatcher, "", "submit_plan", plan_body());
            Json alpha = nullptr; for (const auto& task : legacy.tasks(request)) if (task.at("node_id") == "alpha") alpha = task;
            CHECK(!alpha.is_null()); const auto task_id = text(alpha, "task_id");
            complete(Role::TeamLead, task_id, "issue_work_order", order_body());
            auto task = legacy.task(task_id); const auto binding = legacy.binding(task_id);
            const auto actor = legacy.host_actor(Role::Worker, request, binding.team_id);
            legacy.acknowledge(actor, binding, number(task, "state_version"), new_id("ack"));
            Bound value; value.captured = runtime.capture_call(request, Role::Worker, task_id);
            value.reservation = reservation_for(value.captured);
            legacy.reserve_attempt(value.captured.actor, value.captured.request.binding, value.reservation);
            CHECK(legacy.claim_offline_call(value.captured.actor, value.reservation.attempt_id).at("dispatch_claimed") == true);
            CrashBarrier barrier{"W19", ready.write_fd, hold.read_fd, 0, {}};
            active_crash_barrier = &barrier; crash_capture_facts(barrier, value);
            barrier.facts["request_id"] = request;
            const auto response = crash_model_response(value, barrier);
            barrier.facts["response_sha256"] = sha256_hex(canonical_json(to_json(response)));
            sql(path, "CREATE TRIGGER n5_test_crash BEFORE INSERT ON model_tool_feedback BEGIN SELECT n5_test_crash_barrier('W19'); END");
            // The old compatibility path commits settlement and each note
            // operation before reaching this real feedback INSERT.
            runtime.accept_independent_response(value.captured, response); ::_exit(96);
        } catch (const std::exception& error) { crash_child_error(ready.write_fd, error); ::_exit(97); }
    }
    CrashChild child(pid); ready.close_write(); hold.close_read(); const auto packet = crash_receive(ready.read_fd);
    CHECK(packet.at("phase") == "W19"); CHECK(number(packet, "callbacks") == 1); child.kill_and_reap();
    const auto request = text(packet, "request_id"), attempt_id = text(packet, "attempt_id");
    Json attempt, budget; Array original_events, original_ops, original_intents, original_feedbacks;
    {
        Store reader(path, MigrationRegistry(resources()), StoreOpenMode::ReadOnly);
        CHECK(reader.diagnostics().at("user_version") == 410); attempt = reader.attempt(attempt_id); budget = reader.budget(request);
        CHECK(attempt.at("accepted") == 1); CHECK(!attempt.at("settlement_sha256").is_null());
        CHECK(sha256_hex(canonical_json(attempt.at("request_envelope"))) == text(packet, "request_envelope_sha256"));
        CHECK(sha256_hex(canonical_json(attempt.at("response"))) == text(packet, "response_sha256"));
        CHECK(scalar(path, "SELECT COUNT(*) FROM ops WHERE status='SUCCEEDED'") == 1);
        CHECK(crash_integer(path, "SELECT COUNT(*) FROM model_tool_feedback WHERE attempt_id=?", {attempt_id}) == 0);
        CHECK(reader.tool_feedback(attempt_id).is_null());
        original_events = crash_rows(path, "SELECT * FROM events"); original_ops = crash_rows(path, "SELECT * FROM ops");
        original_intents = crash_rows(path, "SELECT * FROM operation_intents");
        original_feedbacks = crash_rows(path, "SELECT * FROM model_tool_feedback");
    }
    const auto last_event_seq = scalar(path, "SELECT MAX(seq) FROM events");
    sql(path, "DROP TRIGGER n5_test_crash");
    {
        Store upgraded(path, MigrationRegistry(resources()));
        CHECK(upgraded.diagnostics().at("user_version") == 411);
        CHECK(upgraded.attempt(attempt_id) == attempt); CHECK(upgraded.budget(request) == budget);
        CHECK(!upgraded.persistent_events_enabled(request)); CHECK(upgraded.request_event_links(request).empty());
        CHECK(scalar(path, "SELECT COUNT(*) FROM n5_event_claims") == 0);
        CHECK(scalar(path, "SELECT COUNT(*) FROM n5_event_links") == 0);
        CHECK(scalar(path, "SELECT COUNT(*) FROM n5_event_items") == 0);
        CHECK(scalar(path, "SELECT COUNT(*) FROM n5_event_receipts") == 0);
        CHECK(scalar(path, "SELECT COUNT(*) FROM n5_event_open_items") == 0);
        CHECK(scalar(path, "SELECT COUNT(*) FROM n5_event_requests") == 0);
        CHECK(upgraded.tool_feedback(attempt_id).is_null());
        CHECK(crash_rows(path, "SELECT * FROM events WHERE seq<=?", {std::to_string(last_event_seq)}) == original_events);
        CHECK(crash_rows(path, "SELECT * FROM ops") == original_ops);
        CHECK(crash_rows(path, "SELECT * FROM operation_intents") == original_intents);
        CHECK(crash_rows(path, "SELECT * FROM model_tool_feedback") == original_feedbacks);
        expect(ErrorCode::Stale, [&] { upgraded.claim_offline_call(crash_actor_from_json(packet.at("actor")), attempt_id); });
        expect(ErrorCode::NotReady, [&] { upgraded.enable_event_driver(upgraded.host_actor(Role::Root, request), {}, new_id("enable")); });
        CHECK(scalar(path, "SELECT COUNT(*) FROM n5_event_receipts") == 0);
    }
    std::cout << "CRASH phase=W19 pid=" << pid << " callbacks=1 signal=SIGKILL reaped=true effect=1 feedback=0 links=0"
              << " envelope_sha256=" << text(packet, "request_envelope_sha256") << std::endl;
}
