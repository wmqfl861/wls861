#include "wls/store.hpp"
#include "wls/config_loader.hpp"
#include "wls/model.hpp"
#include "test_support.hpp"

#include <openssl/evp.h>
#include <sqlite3.h>
#include <algorithm>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>

namespace {
using namespace wls::s1;
using wls::Role;
using Object = boost::json::object;
using Array = boost::json::array;
const std::filesystem::path source(WLS_SOURCE_DIR);
std::string base, migration, tools, events;
std::string read(const std::filesystem::path& p) {
    std::ifstream stream(p, std::ios::binary); CHECK(stream.good());
    return {std::istreambuf_iterator<char>(stream), {}};
}
std::string text(const Json& v, std::string_view k) { return std::string(v.at(k).as_string()); }
std::int64_t num(const Json& v, std::string_view k) {
    return v.at(k).is_int64() ? v.at(k).as_int64() : static_cast<std::int64_t>(v.at(k).as_uint64());
}
void fails(ErrorCode expected, const std::function<void()>& operation) {
    try { operation(); } catch (const Error& e) { CHECK(e.code() == expected); return; }
    throw std::runtime_error("expected rejection");
}
void execute(const std::filesystem::path& path, const std::string& statement) {
    sqlite3* db = nullptr; CHECK(sqlite3_open(path.c_str(), &db) == SQLITE_OK);
    const auto code = sqlite3_exec(db, statement.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(db); CHECK(code == SQLITE_OK);
}
std::string decode(std::string encoded) {
    if (encoded.empty()) return {};
    std::string bytes(encoded.size() / 4 * 3, '\0');
    const auto size = EVP_DecodeBlock(reinterpret_cast<unsigned char*>(bytes.data()),
        reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
    CHECK(size >= 0);
    const auto padding = static_cast<std::size_t>(encoded.back() == '=') +
        static_cast<std::size_t>(encoded[encoded.size() - 2] == '=');
    bytes.resize(static_cast<std::size_t>(size) - padding); return bytes;
}
struct Fixture {
    test::TempDir temp;
    std::filesystem::path path{temp.path / "runtime.sqlite"};
    std::unique_ptr<Store> store;
    std::string request;
    FrozenRequestConfig config{load_config_catalog(source / "config/examples/system.yaml",
        source / "config/examples/functions", source / "prompts").freeze("general")};
    Json brief = Object{{"goal", "preserve originals"}, {"constraint_refs", Array{"c1"}},
        {"constraints_verbatim", Array{"preserve originals"}}};
    Fixture() {
        reopen();
        AdmissionInput input; input.tenant_id = "tenant"; input.project_id = "project";
        input.root_id = "root"; input.function_id = "general"; input.idempotency_key = new_id("key");
        input.original_text = "preserve originals"; input.config_snapshot = config.json();
        input.constraint_catalog = Array{Object{{"constraint_id", "c1"}, {"text", input.original_text}, {"source_revision", 1}}};
        input.tool_allowlist = {"note", "ack_change"}; input.budget = {600, 2000000, 200000, 0};
        request = text(store->admit(input), "request_id");
        Array nodes;
        for (const auto* id : {"alpha", "beta"}) nodes.push_back(Object{{"node_id", id},
            {"kind", "team_task"}, {"required", true}, {"depends_on", Array{}}, {"brief", brief}});
        store->apply_independent_plan(store->host_actor(Role::Dispatcher, request),
            num(store->request(request), "state_version"), 1, Object{{"nodes", nodes}}, Object{}, new_id("plan"));
        for (const auto* node : {"alpha", "beta"}) {
            const auto id = tid(node);
            store->issue_instruction(actor(node, Role::TeamLead), store->binding(id),
                num(store->task(id), "state_version"), brief, new_id("instruction"));
            store->acknowledge(actor(node), store->binding(id), num(store->task(id), "state_version"), new_id("ack"));
        }
    }
    void reopen(StoreOpenMode mode = StoreOpenMode::Takeover) {
        store.reset(); store = std::make_unique<Store>(path, base, migration, mode, tools, events);
    }
    std::string tid(const std::string& node = "alpha") const {
        for (const auto& task : store->tasks(request)) if (text(task, "node_id") == node) return text(task, "task_id");
        throw std::runtime_error("missing task");
    }
    StoreActor actor(const std::string& node = "alpha", Role role = Role::Worker) const {
        return store->host_actor(role, request, store->binding(tid(node)).team_id);
    }
    std::string add(const Json& result = Object{{"original", "unchanged"}},
                    const std::string& node = "alpha", bool feedback = true) {
        const auto a = actor(node); const auto b = store->binding(a.task_id);
        ContextInput context; context.role = Role::Worker; context.model = config.resolve(Role::Worker);
        const auto profile = config.model_profile(Role::Worker);
        context.adapter_version = text(profile, "adapter_version"); context.protocol_version = text(profile, "protocol");
        context.system_prompt = "history fixture"; context.role_prompt = "Worker";
        context.function_prompt = "preserve originals"; context.task_contract = "complete original test contract";
        ModelRequest call; call.call_id = new_id("call"); call.attempt_id = new_id("attempt");
        call.role = Role::Worker; call.binding = b; call.config_snapshot_id = config.sha256(); call.model = context.model;
        call.context = ContextCompiler().compile(context); call.max_output_tokens = 50;
        call.reserved_input_tokens = 100; call.reserved_output_tokens = 50;
        AttemptReservation reserve; reserve.call_id = call.call_id; reserve.attempt_id = call.attempt_id;
        reserve.config_snapshot_sha256 = config.sha256(); reserve.model_ref = call.model.model;
        reserve.max_output_tokens = 50; reserve.reserved_input_tokens = 100; reserve.reserved_output_tokens = 50;
        reserve.request_envelope = to_json(call); store->reserve_attempt(a, b, reserve);
        const Json response = Object{{"text", "original model output"}, {"tool_call_id", "reused-source-id"}};
        store->settle_attempt(call.attempt_id, b, config.sha256(), "COMPLETED",
            Object{{"input_tokens", 1}, {"output_tokens", 1}, {"cost_micro_usd", 0}}, response);
        if (feedback) store->record_tool_feedback(call.attempt_id, b, response, result);
        return call.attempt_id;
    }
    Json page(std::int64_t after = 0, std::int64_t through = 0, std::int64_t limit = 32) {
        return store->attempt_history_page(actor(), after, through, limit);
    }
    std::string hash(const std::string& id) { return text(store->tool_feedback(id), "record_sha256"); }
};

void empty_and_bounds() {
    Fixture f; const auto empty = f.page();
    CHECK(empty.at("items").as_array().empty() && !empty.at("has_more").as_bool());
    CHECK(num(empty, "through_sequence") == 0 && empty.at("next_after_sequence").is_null());
    for (const auto limit : {std::int64_t(0), std::int64_t(65), INT64_MAX})
        fails(ErrorCode::InvalidInput, [&] { f.page(0, 0, limit); });
    fails(ErrorCode::InvalidInput, [&] { f.page(-1); });
    fails(ErrorCode::InvalidInput, [&] { f.page(0, -1); });
    fails(ErrorCode::Unauthorized, [&] { f.store->attempt_history_page(f.store->host_actor(Role::Root, f.request)); });
}
void over_256_with_fixed_snapshot() {
    Fixture f; std::set<std::string> originals;
    for (int i = 0; i < 270; ++i) {
        originals.insert(f.add(Object{{"ordinal", i}}));
        if (i % 20 == 0) f.add(Object{{"foreign", true}}, "beta");
    }
    fails(ErrorCode::Quota, [&] { f.store->attempt_history(f.actor()); });
    auto page = f.page(0, 0, 17); const auto through = num(page, "through_sequence");
    const auto late = f.add(Object{{"appended_later", true}});
    std::set<std::string> seen; std::int64_t last = 0;
    while (true) {
        CHECK(num(page, "through_sequence") == through);
        CHECK(page.at("items").as_array().size() <= 17);
        for (const auto& item : page.at("items").as_array()) {
            CHECK(num(item, "sequence") > last); last = num(item, "sequence");
            CHECK(!item.as_object().contains("request_envelope") && !item.as_object().contains("response"));
            CHECK(!item.at("feedback_missing").as_bool());
            CHECK(!item.at("accepted_is_business_success").as_bool());
            CHECK(seen.insert(text(item, "attempt_id")).second);
        }
        if (!page.at("has_more").as_bool()) break;
        CHECK(num(page, "next_after_sequence") == last);
        page = f.page(last, through, 17);
    }
    CHECK(seen == originals && !seen.contains(late));
    CHECK(f.page(through).at("items").as_array().size() == 1);
    const auto tail = f.store->attempt_history_tail(f.actor(), 4);
    CHECK(num(tail, "total_count") == 271 && num(tail, "omitted_count") == 267);
    CHECK(tail.at("items").as_array().size() == 4);
    CHECK(text(tail.at("items").as_array().back(), "attempt_id") == late);
    CHECK(num(tail, "through_sequence") == num(tail.at("items").as_array().back(), "sequence"));
    CHECK(f.store->attempt_history_tail(f.actor("beta"), 4).at("principal_id") != tail.at("principal_id"));

}
void ownership_and_cursor_guards() {
    Fixture f; const auto own = f.add(); const auto other = f.add(Object{}, "beta");
    const auto seq = num(f.store->attempt_history_page(f.actor("beta")).at("items").as_array().front(), "sequence");
    fails(ErrorCode::Unauthorized, [&] { f.page(seq); });
    fails(ErrorCode::Unauthorized, [&] { f.page(0, seq); });
    fails(ErrorCode::Unauthorized, [&] { f.store->read_history_feedback(f.actor(), other, f.hash(other), "result"); });
    fails(ErrorCode::Unauthorized, [&] { f.store->read_history_feedback(f.actor("alpha", Role::TeamLead), own, f.hash(own), "result"); });
    auto forged = f.actor(); forged.principal_id = f.actor("beta").principal_id;
    fails(ErrorCode::Unauthorized, [&] { f.store->attempt_history_page(forged); });
}
void original_bytes_and_sections() {
    Fixture f;
    const Json result = Object{{"text", "中文原文😀"}, {"binary_base64", "AP9BQkM="},
        {"large", std::string(61100, 'z')}, {"null", std::string("a\0b", 3)}};
    const auto id = f.add(result); const auto hash = f.hash(id);
    for (const auto* section : {"result", "response"}) {
        const auto original = canonical_json(f.store->tool_feedback(id).at(section));
        std::string reassembled; std::int64_t offset = 0;
        while (true) {
            const auto page = f.store->read_history_feedback(f.actor(), id, hash, section, offset, 1001);
            const auto bytes = decode(text(page, "content_base64"));
            CHECK(bytes.size() <= 1001 && num(page, "bytes_returned") == static_cast<std::int64_t>(bytes.size()));
            CHECK(text(page, "section_sha256") == sha256_hex(original));
            CHECK(num(page, "size_bytes") == static_cast<std::int64_t>(original.size()));
            CHECK(page.at("original_binding") == f.store->tool_feedback(id).at("original_binding"));
            reassembled += bytes;
            if (page.at("eof").as_bool()) { CHECK(page.at("next_offset").is_null()); break; }
            offset = num(page, "next_offset"); CHECK(offset == static_cast<std::int64_t>(reassembled.size()));
        }
        CHECK(reassembled == original);
        const auto end = f.store->read_history_feedback(f.actor(), id, hash, section, static_cast<std::int64_t>(original.size()), 1);
        CHECK(end.at("eof").as_bool() && text(end, "content_base64").empty());
        fails(ErrorCode::InvalidInput, [&] { f.store->read_history_feedback(f.actor(), id, hash, section, INT64_MAX); });
    }
}
void invalid_reads_and_missing_feedback() {
    Fixture f; const auto id = f.add(); const auto hash = f.hash(id);
    fails(ErrorCode::InvalidInput, [&] { f.store->read_history_feedback(f.actor(), id, "not-a-hash", "result"); });
    fails(ErrorCode::Conflict, [&] { f.store->read_history_feedback(f.actor(), id, std::string(64, '0'), "result"); });
    fails(ErrorCode::InvalidInput, [&] { f.store->read_history_feedback(f.actor(), id, hash, "request_envelope"); });
    fails(ErrorCode::InvalidInput, [&] { f.store->read_history_feedback(f.actor(), id, hash, "result", -1); });
    for (const auto limit : {0, 16385}) fails(ErrorCode::InvalidInput,
        [&] { f.store->read_history_feedback(f.actor(), id, hash, "result", 0, limit); });
    const auto missing = f.add(Object{}, "alpha", false);
    const auto items = f.page().at("items").as_array();
    CHECK(items.back().at("feedback_missing").as_bool() && items.back().at("record_sha256").is_null());
    fails(ErrorCode::NotReady, [&] { f.store->read_history_feedback(f.actor(), missing, std::string(64, '0'), "result"); });
}
void readonly_reopen_keeps_original_binding() {
    Fixture f; const auto id = f.add(); const auto old_actor = f.actor();
    const auto hash = f.hash(id); const auto original = f.store->read_history_feedback(old_actor, id, hash, "result");
    const auto request = f.store->request(f.request); const auto budget = f.store->budget(f.request);
    f.reopen(StoreOpenMode::ReadOnly);
    CHECK(f.store->read_history_feedback(f.actor(), id, hash, "result") == original);
    CHECK(f.store->request(f.request) == request && f.store->budget(f.request) == budget);
    f.reopen();
    CHECK(f.store->read_history_feedback(f.actor(), id, hash, "result") == original);
    fails(ErrorCode::Stale, [&] { f.store->attempt_history_page(old_actor); });
    fails(ErrorCode::Stale, [&] { f.store->read_history_feedback(old_actor, id, hash, "result"); });
}
void tampering_rejected() {
    Fixture f; const auto id = f.add(); const auto hash = f.hash(id);
    // Damage only this disposable database, then restore the immutable guard.
    execute(f.path, "DROP TRIGGER model_tool_feedback_immutable; UPDATE model_tool_feedback SET result='{}' WHERE attempt_id='" + id +
        "'; CREATE TRIGGER model_tool_feedback_immutable BEFORE UPDATE ON model_tool_feedback "
        "BEGIN SELECT RAISE(ABORT, 'immutable model tool feedback'); END;");
    fails(ErrorCode::Corrupt, [&] { f.store->read_history_feedback(f.actor(), id, hash, "result"); });
}
void self_consistent_fingerprint_is_not_authority() {
    Fixture f; const auto id = f.add();
    const auto old = f.store->tool_feedback(id);
    auto binding = old.at("original_binding");
    binding.as_object()["requirements_revision"] = 42;
    const Json changed = Object{{"attempt_id", id}, {"original_binding", binding},
        {"response", old.at("response")}, {"result", old.at("result")}};
    const auto forged_hash = sha256_hex(canonical_json(changed));
    execute(f.path, "DROP TRIGGER model_tool_feedback_immutable; UPDATE model_tool_feedback SET original_binding='" +
        canonical_json(binding) + "',record_sha256='" + forged_hash + "' WHERE attempt_id='" + id +
        "'; CREATE TRIGGER model_tool_feedback_immutable BEFORE UPDATE ON model_tool_feedback "
        "BEGIN SELECT RAISE(ABORT, 'immutable model tool feedback'); END;");
    fails(ErrorCode::Corrupt, [&] { f.store->read_history_feedback(f.actor(), id, forged_hash, "result"); });
}
void snapshot_membership_does_not_fabricate_missing_observations() {
    Fixture f; const auto id = f.add(Object{}, "alpha", false);
    const auto page = f.page();
    CHECK(page.at("items").as_array().front().at("feedback_missing").as_bool());
    const auto attempt = f.store->attempt(id);
    const auto original = execution_binding_from_json(attempt.at("frozen_binding"));
    f.store->record_tool_feedback(id, original, attempt.at("response"), Object{{"observed_later", true}});
    const auto updated = f.page(0, num(page, "through_sequence"));
    CHECK(updated.at("items").as_array().size() == 1);
    CHECK(!updated.at("items").as_array().front().at("feedback_missing").as_bool());
    CHECK(text(updated.at("items").as_array().front(), "attempt_id") == id);
    const auto empty = f.page(num(page, "through_sequence"), num(page, "through_sequence"));
    CHECK(empty.at("items").as_array().empty() && !empty.at("has_more").as_bool());
}
void reads_do_not_clear_wait_or_unknown() {
    Fixture f; const auto id = f.add(); const auto actor = f.actor(); const auto binding = f.store->binding(actor.task_id);
    const auto prepared = f.store->prepare_operation(actor, binding, num(f.store->task(actor.task_id), "state_version"),
        new_id("effect"), "note", Object{{"text", "fixture"}}, "none", new_id("prepare"));
    f.store->start_operation(actor, text(prepared, "op_id"), num(prepared, "state_version"), new_id("start"));
    f.reopen(); const auto current = f.actor();
    const auto before = f.store->operation(text(prepared, "op_id"));
    CHECK(before.at("status") == "UNKNOWN");
    const auto budget = f.store->budget(f.request);
    static_cast<void>(f.store->attempt_history_page(current));
    static_cast<void>(f.store->read_history_feedback(current, id, f.hash(id), "result"));
    CHECK(f.store->operation(text(prepared, "op_id")) == before && f.store->budget(f.request) == budget);
    const auto wait = f.store->wait_worker(current, f.store->binding(current.task_id),
        num(f.store->task(current.task_id), "state_version"), "explicit wait", new_id("wait"));
    static_cast<void>(f.store->read_history_feedback(current, id, f.hash(id), "result"));
    CHECK(f.store->worker_wait(current.task_id) == wait);
}
} // namespace
int main() {
    base = read(source / "db/schema.sql"); migration = read(source / "db/migrations/004_s1_store.sql");
    tools = read(source / "db/migrations/005_worker_tools.sql"); events = read(source / "db/migrations/006_event_runtime.sql");
    return test::run({
        {"empty index and hard bounds reject invalid requests", empty_and_bounds},
        {"270 attempts paginate completely without legacy cap or append drift", over_256_with_fixed_snapshot},
        {"actor and cursor scopes never cross requests teams or roles", ownership_and_cursor_guards},
        {"Unicode binary and 61KB results reassemble exact immutable JSON bytes", original_bytes_and_sections},
        {"invalid hashes sections offsets and absent feedback fail explicitly", invalid_reads_and_missing_feedback},
        {"read-only and takeover preserve original facts and fence old actors", readonly_reopen_keeps_original_binding},
        {"corrupted immutable feedback is rejected", tampering_rejected},
        {"self-consistent receipt tampering cannot replace original attempt binding", self_consistent_fingerprint_is_not_authority},
        {"snapshot freezes membership not a fabricated settled observation", snapshot_membership_does_not_fabricate_missing_observations},
        {"history reads do not clear waits UNKNOWN or consume budget", reads_do_not_clear_wait_or_unknown}
    });
}
