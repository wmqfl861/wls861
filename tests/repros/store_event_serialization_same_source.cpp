// Test-only same-source regression for the Store event write path. This is
// NOT a registered CTest suite and not product code: the translation unit is
// compiled by hand against each version's own headers and production static
// libraries, with GNU ld --wrap redirecting only the canonical_json symbol
// so the probe can count its invocations for one chosen fixture plan
// command. The wrapper always calls the production function unchanged; it
// never intercepts parser results, permission guards, capacity guards or
// deadlines, and it never prints a raw command body.
//
// Store::Impl::event computes sha256_hex(canonical_json(command)) and then
// json_text(command, payload_limit), which canonicalizes the SAME immutable
// command again for the stored payload. A publish_plan command serialized
// once must therefore observe EVENT_COMMAND_SERIALIZATIONS=1. The unmodified
// library is expected to observe 2; that value is observed, never assumed.
// The count CHECK runs last, only after the durable effect, the raw SQLite
// bytes and an independently built byte-exact command expectation are all
// verified; no count value ever short-circuits a validation step.

#define main original_store_tests_main
#include "../store_tests.cpp"
#undef main

#include <string_view>

namespace {
constexpr std::string_view probe_marker = "store-event-serialization-probe-7c1f2b";

struct ProbeState {
    bool counting = false;
    long counted = 0;
};
// One Store used from one thread; no atomic is needed or implied.
ProbeState probe_state;

// Structural identification only. This walk never calls canonical_json (the
// wrapper would recurse), never serializes and never prints the command.
bool probe_contains_marker(const Json& value, int depth) {
    if (depth > 64) return false; // Event command bodies are shallow; stay bounded.
    if (value.is_string()) {
        const auto& text = value.as_string();
        return std::string_view(text.data(), text.size()).find(probe_marker) != std::string_view::npos;
    }
    if (value.is_array()) {
        for (const auto& element : value.as_array())
            if (probe_contains_marker(element, depth + 1)) return true;
        return false;
    }
    if (value.is_object()) {
        for (const auto& member : value.as_object())
            if (probe_contains_marker(member.value(), depth + 1)) return true;
    }
    return false;
}

bool probe_is_counted_plan_command(const Json& value) {
    if (!value.is_object()) return false;
    const auto* type = value.as_object().if_contains("type");
    if (!type || !type->is_string()) return false;
    const auto& name = type->as_string();
    return std::string_view(name.data(), name.size()) == "plan" && probe_contains_marker(value, 0);
}

std::string probe_raw_text(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
    const std::string result(bytes != nullptr ? bytes : "",
        static_cast<std::size_t>(sqlite3_column_bytes(statement, 0)));
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return result;
}

// Deterministic concatenation of the raw TEXT columns of one request's whole
// event history in seq order. This is a raw byte snapshot of stored history,
// not an API view, and it claims no physical database file equality.
std::string probe_raw_event_bytes(const std::filesystem::path& path, const std::string& request_id) {
    sqlite3* db = nullptr;
    CHECK(sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(db, "SELECT seq,type,command_id,command_sha256,payload,result"
                                 " FROM events WHERE request_id=? ORDER BY seq", -1,
                             &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite3_bind_text(statement, 1, request_id.c_str(),
                            static_cast<int>(request_id.size()), SQLITE_STATIC) == SQLITE_OK);
    std::string digest;
    while (true) {
        const auto step = sqlite3_step(statement);
        CHECK(step == SQLITE_ROW || step == SQLITE_DONE);
        if (step == SQLITE_DONE) break;
        for (int column = 0; column < 6; ++column) {
            const auto* bytes = reinterpret_cast<const char*>(sqlite3_column_text(statement, column));
            const auto size = static_cast<std::size_t>(sqlite3_column_bytes(statement, column));
            // Stored payload/result are JSON text, so these separators never
            // occur inside a column value.
            digest.append(bytes != nullptr ? bytes : "", size);
            digest.push_back('\x1f');
        }
        digest.push_back('\x1e');
    }
    sqlite3_finalize(statement);
    sqlite3_close(db);
    return digest;
}
} // namespace

// GNU ld --wrap: undefined references to canonical_json, including the one
// inside the production store.cpp.o, resolve to __wrap_..., and __real_...
// resolves to the original production definition. The extern "C" spellings
// below are the literal wrapped symbol names for
// _ZN3wls2s114canonical_jsonB5cxx11ERKN5boost4json5valueE.
extern "C" std::string __real__ZN3wls2s114canonical_jsonB5cxx11ERKN5boost4json5valueE(const boost::json::value&);
extern "C" std::string __wrap__ZN3wls2s114canonical_jsonB5cxx11ERKN5boost4json5valueE(const boost::json::value& value) {
    if (probe_state.counting && probe_is_counted_plan_command(value)) ++probe_state.counted;
    return __real__ZN3wls2s114canonical_jsonB5cxx11ERKN5boost4json5valueE(value);
}

void plan_command_serialization_probe() {
    // A fresh disposable Store with one admitted request; this probe performs
    // that request's first and only plan publication, so the counted command
    // is unambiguous and no other plan event can exist.
    test::TempDir temp;
    const std::filesystem::path path = temp.path / "state.sqlite";
    Store store(path, base_sql, migration_sql);
    const std::string request_id = s(store.admit(input("probe-serialization")), "request_id");
    const auto dispatcher = store.host_actor(Role::Dispatcher, request_id);
    const auto version = n(store.request(request_id), "state_version");
    const auto requirements = n(store.request(request_id), "requirements_revision");
    const std::string key = new_id("cmd");

    // The plan body is an open object at this Store boundary. The goal keeps
    // the original legal escaped and non-ASCII text behind the unique
    // marker; the small metadata member adds nested object/array values,
    // null, int64/uint64, double and negative zero so the canonical spelling
    // is non-trivial and exactly representable. No nodes member is supplied,
    // so the nodes/brief guard is not touched or bypassed.
    const Json body = Object{
        {"goal", "store event probe \\ \"quoted\" 改为中文 marker " + std::string(probe_marker)},
        {"probe_metadata", Object{
            {"nested", Object{{"words", Array{"原始", "preserved"}}, {"count", 2}}},
            {"absent", nullptr},
            {"int64_value", -1234567890123},
            {"uint64_value", 18446744073709551615ULL},
            {"double_value", 3.25},
            {"negative_zero", -0.0}}}};

    // Independently expected durable command, built from the captured actor,
    // CAS and frozen body before any counting starts. actor_json's original
    // history fields exclude the ephemeral writer_session.
    const Json expected_command = Object{
        {"type", "plan"},
        {"expected_request_version", version},
        {"based_on_requirements_revision", requirements},
        {"actor", Object{
            {"role", role_name(dispatcher.role)},
            {"principal_id", dispatcher.principal_id},
            {"request_id", dispatcher.request_id},
            {"task_id", dispatcher.task_id},
            {"team_id", dispatcher.team_id},
            {"dispatcher_epoch", dispatcher.dispatcher_epoch},
            {"execution_epoch", dispatcher.execution_epoch}}},
        {"body", body}};
    CHECK(!probe_state.counting);
    const std::string expected_bytes = canonical_json(expected_command);
    const std::string expected_hash = sha256_hex(expected_bytes);

    // Durable state before the first publish, including a raw TEXT snapshot
    // of the existing history prefix.
    const auto before_request = store.request(request_id);
    const auto before_snapshot = store.request_snapshot(request_id);
    const auto before_budget = store.budget(request_id);
    const std::string before_digest = probe_raw_event_bytes(path, request_id);
    const auto before_events = raw_number(path, "SELECT count(*) FROM events WHERE request_id='" +
                                                request_id + "'");
    const auto before_total_events = raw_number(path, "SELECT count(*) FROM events");

    probe_state.counting = true;
    probe_state.counted = 0;
    const Json published = store.publish_plan(dispatcher, version, requirements, body, key);
    probe_state.counting = false;
    const long published_count = probe_state.counted;
    std::cout << "EVENT_COMMAND_SERIALIZATIONS=" << published_count << std::endl;

    // The publish must advance exactly one state_version and one
    // plan_revision, and preserve the original epoch, requirements, budget,
    // request snapshot (original texts and configuration identity) and the
    // entire pre-existing raw history prefix.
    const auto after_request = store.request(request_id);
    CHECK(n(after_request, "state_version") == version + 1);
    CHECK(n(after_request, "plan_revision") == n(before_request, "plan_revision") + 1);
    CHECK(n(after_request, "dispatcher_epoch") == n(before_request, "dispatcher_epoch"));
    CHECK(n(after_request, "requirements_revision") == requirements);
    CHECK(s(after_request, "config_snapshot_sha256") == s(before_request, "config_snapshot_sha256"));
    CHECK(store.request_snapshot(request_id) == before_snapshot);
    CHECK(store.budget(request_id) == before_budget);
    const std::string after_digest = probe_raw_event_bytes(path, request_id);
    const auto after_events = raw_number(path, "SELECT count(*) FROM events WHERE request_id='" +
                                               request_id + "'");
    CHECK(after_events == before_events + 1);
    CHECK(after_digest.size() > before_digest.size());
    CHECK(after_digest.compare(0, before_digest.size(), before_digest) == 0);
    CHECK(raw_number(path, "SELECT count(*) FROM events WHERE request_id='" + request_id +
          "' AND type='plan'") == 1);

    // Durable bytes against the independent expectation, not a marker search.
    // Counting stays paused: verification canonicalizes parsed values.
    const std::string by_key =
        "FROM events WHERE request_id='" + request_id + "' AND command_id='" + key + "'";
    CHECK(raw_number(path, "SELECT count(*) " + by_key) == 1);
    const std::string payload = probe_raw_text(path, "SELECT payload " + by_key);
    const std::string result_text = probe_raw_text(path, "SELECT result " + by_key);
    const std::string command_hash = probe_raw_text(path, "SELECT command_sha256 " + by_key);
    CHECK(payload == expected_bytes);
    CHECK(command_hash == expected_hash);
    CHECK(command_hash == sha256_hex(payload));
    const Json recorded_command = parse_json_strict(payload);
    CHECK(recorded_command == expected_command);
    CHECK(recorded_command.at("body") == body);
    CHECK(recorded_command.at("actor") == expected_command.at("actor"));
    CHECK(recorded_command.at("expected_request_version") == version);
    CHECK(recorded_command.at("based_on_requirements_revision") == requirements);
    CHECK(canonical_json(recorded_command) == payload);
    CHECK(s(recorded_command, "type") == "plan");
    CHECK(payload.find(probe_marker) != std::string::npos);
    CHECK(result_text == canonical_json(published));
    CHECK(parse_json_strict(result_text) == published);

    // Everything durable captured once after the first commit. The exact
    // replay and every later rejection must leave all of it byte-identical,
    // including the total event count, so no fresh-key event can escape.
    const auto frozen_request = store.request(request_id);
    const auto frozen_snapshot = store.request_snapshot(request_id);
    const auto frozen_budget = store.budget(request_id);
    const auto frozen_plan = store.plan(request_id);
    const auto frozen_version = n(frozen_request, "state_version");
    const auto frozen_events = raw_number(path, "SELECT count(*) FROM events WHERE request_id='" +
                                                request_id + "'");
    const auto frozen_total_events = raw_number(path, "SELECT count(*) FROM events");
    // The one publish added exactly one durable event globally as well, so a
    // later same-total check cannot mistake the frozen baseline.
    CHECK(frozen_total_events == before_total_events + 1);
    const std::string frozen_digest = probe_raw_event_bytes(path, request_id);
    const auto require_frozen = [&]() {
        CHECK(raw_number(path, "SELECT count(*) FROM events WHERE request_id='" + request_id + "'") ==
              frozen_events);
        CHECK(raw_number(path, "SELECT count(*) FROM events") == frozen_total_events);
        CHECK(probe_raw_event_bytes(path, request_id) == frozen_digest);
        CHECK(probe_raw_text(path, "SELECT payload " + by_key) == payload);
        CHECK(probe_raw_text(path, "SELECT result " + by_key) == result_text);
        CHECK(probe_raw_text(path, "SELECT command_sha256 " + by_key) == command_hash);
        CHECK(n(store.request(request_id), "state_version") == frozen_version);
        CHECK(store.request(request_id) == frozen_request);
        CHECK(store.request_snapshot(request_id) == frozen_snapshot);
        CHECK(store.budget(request_id) == frozen_budget);
        CHECK(store.plan(request_id) == frozen_plan);
    };

    // An exact same-key replay returns the recorded result without rewriting
    // any original event, history or budget. Counted separately; no total is
    // asserted across the whole request chain.
    probe_state.counting = true;
    probe_state.counted = 0;
    const Json replayed = store.publish_plan(dispatcher, version, requirements, body, key);
    probe_state.counting = false;
    std::cout << "EVENT_COMMAND_SERIALIZATIONS_REPLAY=" << probe_state.counted << std::endl;
    CHECK(replayed == published);
    require_frozen();

    // Rejections keep their original guards. A foreign Store session is
    // stale before any replay, both on the exact recorded key and on a fresh
    // key; a reused key with changed intent conflicts.
    auto foreign = dispatcher;
    foreign.writer_session += "-probe-foreign-session";
    error(ErrorCode::Stale, [&] {
        (void)store.publish_plan(foreign, version, requirements, body, key);
    });
    error(ErrorCode::Stale, [&] {
        (void)store.publish_plan(foreign, version, requirements, body, new_id("cmd"));
    });
    auto changed = body;
    changed.as_object()["goal"] = "different original intent";
    error(ErrorCode::Conflict, [&] {
        (void)store.publish_plan(dispatcher, version, requirements, changed, key);
    });
    require_frozen();

    // Only now does the observed serialization count become evidence: one
    // publish of this command must canonicalize it exactly once.
    CHECK(published_count == 1);
}

int main() {
    source_root = WLS_SOURCE_DIR;
    base_sql = read(source_root / "db/schema.sql");
    migration_sql = read(source_root / "db/migrations/004_s1_store.sql");
    tools_migration_sql = read(source_root / "db/migrations/005_worker_tools.sql");
    return test::run({
        {"store event serializes its original plan command once", plan_command_serialization_probe},
    });
}
