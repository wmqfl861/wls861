#include "wls/runtime_resources.hpp"
#include "wls/migrations.hpp"
#include "wls/config_loader.hpp"
#include "wls/schema.hpp"
#include "wls/store.hpp"
#include "test_support.hpp"

#include <sqlite3.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;

#ifndef WLS_SOURCE_DIR
#define WLS_SOURCE_DIR "."
#endif

namespace {
const std::filesystem::path source_root = std::filesystem::absolute(WLS_SOURCE_DIR);

const RuntimeResources& resources() {
    static const auto original = RuntimeResources::from_source_tree(source_root);
    return original;
}

std::string read_fixture(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    CHECK(static_cast<bool>(stream));
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void write_fixture(const std::filesystem::path& path, const std::string& content) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream stream(path, std::ios::binary | std::ios::trunc);
    CHECK(static_cast<bool>(stream));
    stream << content;
    stream.close();
    CHECK(static_cast<bool>(stream));
}

void expect(ErrorCode code, const std::function<void()>& operation) {
    try { operation(); }
    catch (const Error& failure) { CHECK(failure.code() == code); return; }
    throw std::runtime_error("expected resource/registry failure was not thrown");
}

struct Bundle {
    test::TempDir directory;
    std::filesystem::path root{directory.path / "bundle"};
    Json manifest;
    Bundle() : manifest(parse_json_strict(read_fixture(source_root / "resources/runtime-manifest.json"))) {
        for (const auto& asset : manifest.at("assets").as_array()) {
            const auto path = std::string(asset.at("path").as_string());
            write_fixture(root / path, resources().bytes(path));
        }
        save_manifest();
    }
    void save_manifest() { write_fixture(root / "runtime-manifest.json", canonical_json(manifest)); }
};

std::int64_t sql_number(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* database = nullptr;
    CHECK(sqlite3_open_v2(path.c_str(), &database, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK);
    sqlite3_stmt* statement = nullptr;
    CHECK(sqlite3_prepare_v2(database, sql.c_str(), -1, &statement, nullptr) == SQLITE_OK);
    CHECK(sqlite3_step(statement) == SQLITE_ROW);
    const auto value = static_cast<std::int64_t>(sqlite3_column_int64(statement, 0));
    sqlite3_finalize(statement); sqlite3_close(database);
    return value;
}

void sql_exec(const std::filesystem::path& path, const std::string& sql) {
    sqlite3* database = nullptr;
    CHECK(sqlite3_open(path.c_str(), &database) == SQLITE_OK);
    const auto result = sqlite3_exec(database, sql.c_str(), nullptr, nullptr, nullptr);
    sqlite3_close(database);
    CHECK(result == SQLITE_OK);
}

AdmissionInput admission() {
    AdmissionInput result;
    result.tenant_id = "resource-tenant"; result.project_id = "resource-project"; result.root_id = "resource-root";
    result.function_id = "general"; result.idempotency_key = "resource-admission";
    result.original_text = "Preserve the original resource migration fixture.";
    result.config_snapshot = load_config_catalog(source_root / "config/examples/system.yaml",
        source_root / "config/examples/functions", source_root / "prompts").freeze("general").json();
    result.tool_allowlist = {"fs_read", "fs_write"};
    return result;
}

void source_and_bundle_capture_identical_program_bytes() {
    Bundle copy;
    const auto bundle = RuntimeResources::from_bundle(copy.root);
    CHECK(!bundle.source_tree()); CHECK(resources().source_tree());
    CHECK(bundle.diagnostics().at("bundle_sha256") == resources().diagnostics().at("bundle_sha256"));
    CHECK(bundle.diagnostics().at("asset_count") == copy.manifest.at("assets").as_array().size());
    CHECK(bundle.schema_documents() == resources().schema_documents());
    CHECK(bundle.prompt_documents() == resources().prompt_documents());
    for (const auto& registration : registered_store_migrations())
        CHECK(sha256_hex(bundle.bytes(registration.path)) == registration.sha256);
    expect(ErrorCode::InvalidInput, [&] { (void)bundle.bytes("../config/examples/system.yaml"); });
}

void complete_manifest_is_compiled_and_closed() {
    for (const auto mode : {0, 1, 2, 3, 4}) {
        Bundle copy;
        if (mode == 0) copy.manifest.as_object()["unregistered"] = true;
        if (mode == 1) copy.manifest.at("assets").as_array().pop_back();
        if (mode == 2) {
            const auto duplicate = copy.manifest.at("assets").as_array().front();
            copy.manifest.at("assets").as_array().push_back(duplicate);
        }
        if (mode == 3) copy.manifest.at("assets").as_array().front().as_object()["sha256"] = std::string(64, '0');
        if (mode == 4) copy.manifest.as_object()["store_version"] = 999;
        copy.save_manifest();
        expect(ErrorCode::Corrupt, [&] { (void)RuntimeResources::from_bundle(copy.root); });
    }
    Bundle unknown;
    unknown.manifest.as_object()["format"] = "wls-runtime-resources-v999";
    unknown.save_manifest();
    expect(ErrorCode::Unsupported, [&] { (void)RuntimeResources::from_bundle(unknown.root); });
    write_fixture(unknown.root / "runtime-manifest.json", "{\"format\":\"first\",\"format\":\"second\"}");
    expect(ErrorCode::InvalidInput, [&] { (void)RuntimeResources::from_bundle(unknown.root); });
}

void altered_sql_and_self_reported_hash_never_open_store() {
    Bundle copy;
    const auto relative = std::string(registered_store_migrations().back().path);
    const auto altered = resources().bytes(relative) + "\n-- not the compiled SQL\n";
    write_fixture(copy.root / relative, altered);
    const auto path = copy.directory.path / "must-not-exist.sqlite";
    expect(ErrorCode::Corrupt, [&] {
        const auto selected = RuntimeResources::from_bundle(copy.root);
        Store refused(path, MigrationRegistry(selected));
    });
    CHECK(!std::filesystem::exists(path)); CHECK(!std::filesystem::exists(path.string() + ".writer-lock"));
    for (auto& asset : copy.manifest.at("assets").as_array()) if (std::string(asset.at("path").as_string()) == relative) {
        asset.as_object()["sha256"] = sha256_hex(altered);
        asset.as_object()["size_bytes"] = altered.size();
    }
    copy.save_manifest();
    expect(ErrorCode::Corrupt, [&] { (void)RuntimeResources::from_bundle(copy.root); });
}

void missing_and_symbolic_resources_are_rejected() {
    Bundle missing;
    std::filesystem::remove(missing.root / "db/schema.sql");
    expect(ErrorCode::Corrupt, [&] { (void)RuntimeResources::from_bundle(missing.root); });
    Bundle linked;
    std::filesystem::remove(linked.root / "db/schema.sql");
    std::filesystem::create_symlink(source_root / "db/schema.sql", linked.root / "db/schema.sql");
    expect(ErrorCode::Unauthorized, [&] { (void)RuntimeResources::from_bundle(linked.root); });
    Bundle parent;
    std::filesystem::rename(parent.root / "schemas", parent.directory.path / "real-schemas");
    std::filesystem::create_directory_symlink(parent.directory.path / "real-schemas", parent.root / "schemas");
    expect(ErrorCode::Unauthorized, [&] { (void)RuntimeResources::from_bundle(parent.root); });
    Bundle root;
    std::filesystem::create_directory_symlink(root.root, root.directory.path / "root-link");
    expect(ErrorCode::Unauthorized, [&] { (void)RuntimeResources::from_bundle(root.directory.path / "root-link"); });
}

void fifo_and_oversized_inputs_stop_before_data_open() {
    Bundle fifo;
    std::filesystem::remove(fifo.root / "db/schema.sql");
    CHECK(::mkfifo((fifo.root / "db/schema.sql").c_str(), 0600) == 0);
    const auto started = std::chrono::steady_clock::now();
    expect(ErrorCode::Unauthorized, [&] { (void)RuntimeResources::from_bundle(fifo.root); });
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    Bundle large;
    write_fixture(large.root / "db/schema.sql", std::string(1024U * 1024U + 1U, 'x'));
    expect(ErrorCode::Quota, [&] { (void)RuntimeResources::from_bundle(large.root); });
    write_fixture(large.root / "runtime-manifest.json", std::string(64U * 1024U + 1U, ' '));
    expect(ErrorCode::Quota, [&] { (void)RuntimeResources::from_bundle(large.root); });
}

void captured_bytes_survive_later_filesystem_changes() {
    Bundle copy;
    const auto captured = RuntimeResources::from_bundle(copy.root);
    std::filesystem::remove_all(copy.root);
    const SchemaRegistry from_bytes(captured.schema_documents());
    const SchemaRegistry legacy(source_root / "schemas");
    CHECK(from_bytes.schema_names() == legacy.schema_names());
    CHECK(from_bytes.full_schema("plan") == legacy.full_schema("plan"));
    const MigrationRegistry migrations(captured);
    Store created(copy.directory.path / "captured.sqlite", migrations);
    CHECK(created.diagnostics().at("user_version") == 410);
}

void schema_bytes_keep_original_shape_and_reference_guards() {
    auto files = resources().schema_documents();
    files.emplace("../outside.schema.json", files.begin()->second);
    expect(ErrorCode::InvalidInput, [&] { SchemaRegistry refused(files); });
    files = resources().schema_documents();
    files.begin()->second = std::string(1024U * 1024U + 1U, ' ');
    expect(ErrorCode::InvalidInput, [&] { SchemaRegistry refused(files); });
    const SchemaRegistry good(resources().schema_documents());
    expect(ErrorCode::InvalidInput, [&] { good.validate("plan", Object{}); });
}

void executable_discovery_is_relocatable_and_ignores_cwd_and_environment() {
    Bundle copy;
    const auto prefix = copy.directory.path / "prefix";
    std::filesystem::create_directories(prefix / "share");
    std::filesystem::rename(copy.root, prefix / "share/wls861");
    write_fixture(prefix / "bin/wls", "synthetic executable location\n");
    const auto poisoned_cwd = copy.directory.path / "poison";
    write_fixture(poisoned_cwd / "resources/runtime-manifest.json", "poison\n");
    const auto previous = std::filesystem::current_path();
    const char* old_value = ::getenv("WLS_RESOURCE_ROOT");
    const bool had_old = old_value != nullptr;
    const std::string saved = old_value ? old_value : "";
    CHECK(::setenv("WLS_RESOURCE_ROOT", poisoned_cwd.c_str(), 1) == 0);
    std::filesystem::current_path(poisoned_cwd);
    try {
        const auto selected = RuntimeResources::discover(prefix / "bin/wls");
        CHECK(selected.root() == prefix / "share/wls861");
        CHECK(!selected.source_tree());
        const auto moved = copy.directory.path / "relocated";
        std::filesystem::rename(prefix, moved);
        CHECK(RuntimeResources::discover(moved / "bin/wls").root() == moved / "share/wls861");
    } catch (...) {
        std::filesystem::current_path(previous);
        if (had_old) (void)::setenv("WLS_RESOURCE_ROOT", saved.c_str(), 1); else (void)::unsetenv("WLS_RESOURCE_ROOT");
        throw;
    }
    std::filesystem::current_path(previous);
    if (had_old) CHECK(::setenv("WLS_RESOURCE_ROOT", saved.c_str(), 1) == 0); else CHECK(::unsetenv("WLS_RESOURCE_ROOT") == 0);
}

void corrupt_preferred_bundle_does_not_fall_back() {
    Bundle copy;
    const auto prefix = copy.directory.path / "prefix";
    write_fixture(prefix / "bin/wls", "synthetic location\n");
    write_fixture(prefix / "share/wls861/runtime-manifest.json", "{}");
    std::filesystem::create_directories(prefix / "bin/share");
    std::filesystem::rename(copy.root, prefix / "bin/share/wls861");
    expect(ErrorCode::Unsupported, [&] { (void)RuntimeResources::discover(prefix / "bin/wls"); });
    expect(ErrorCode::InvalidInput, [&] { (void)RuntimeResources::discover("relative-executable"); });
}

void registered_prefixes_preserve_old_versions_and_metadata() {
    test::TempDir temporary;
    std::int64_t previous = 401;
    for (const auto& item : registered_store_migrations()) {
        CHECK(item.from_version == previous); previous = item.to_version;
        const MigrationRegistry selected(resources(), item.to_version);
        const auto path = temporary.path / (std::to_string(item.to_version) + ".sqlite");
        {
            Store store(path, selected);
            CHECK(store.diagnostics().at("user_version") == item.to_version);
            CHECK(store.diagnostics().at("foreign_keys") == 1);
            CHECK(store.diagnostics().at("synchronous") == 2);
            CHECK(sql_number(path, "SELECT COUNT(*) FROM " + std::string(item.metadata_table) +
                " WHERE singleton=1 AND store_version=" + std::to_string(item.metadata_version) +
                " AND migration_sha256='" + std::string(item.sha256) + "'") == 1);
        }
        Store reader(path, MigrationRegistry(resources()), StoreOpenMode::ReadOnly);
        CHECK(reader.diagnostics().at("user_version") == item.to_version);
    }
    expect(ErrorCode::Unsupported, [&] { MigrationRegistry refused(resources(), 401); });
    expect(ErrorCode::Unsupported, [&] { MigrationRegistry refused(resources(), 411); });
}

void populated_upgrades_need_explicit_all_request_maintenance() {
    test::TempDir temporary;
    const auto path = temporary.path / "old.sqlite";
    std::string request_id;
    Json original, original_snapshot, original_events;
    {
        Store old(path, resources().bytes("db/schema.sql"), resources().bytes("db/migrations/004_s1_store.sql"));
        request_id = std::string(old.admit(admission()).at("request_id").as_string());
        original = old.request(request_id); original_snapshot = old.request_snapshot(request_id);
        Array events; for (const auto& event : old.events(request_id)) events.push_back(event); original_events = std::move(events);
    }
    const MigrationRegistry latest(resources());
    expect(ErrorCode::Unsupported, [&] { Store refused(path, latest, StoreOpenMode::Takeover, StoreRecoveryScope::admission()); });
    expect(ErrorCode::Unsupported, [&] { Store refused(path, latest, StoreOpenMode::Takeover, StoreRecoveryScope::request(request_id)); });
    {
        Store reader(path, latest, StoreOpenMode::ReadOnly);
        CHECK(reader.diagnostics().at("user_version") == 402);
        CHECK(reader.request(request_id) == original);
        CHECK(reader.request_snapshot(request_id) == original_snapshot);
        Array events; for (const auto& event : reader.events(request_id)) events.push_back(event);
        CHECK(events == original_events);
    }
    {
        Store upgraded(path, latest);
        CHECK(upgraded.diagnostics().at("user_version") == 410);
        CHECK(upgraded.request(request_id).at("config_snapshot") == original.at("config_snapshot"));
        CHECK(upgraded.request_snapshot(request_id).at("texts") == original_snapshot.at("texts"));
    }
}

void legacy_failure_rolls_back_then_registered_upgrade_succeeds() {
    test::TempDir temporary;
    const auto path = temporary.path / "rollback.sqlite";
    {
        Store original(path, MigrationRegistry(resources(), 402));
        CHECK(original.diagnostics().at("user_version") == 402);
    }
    expect(ErrorCode::Storage, [&] {
        Store failed(path, resources().bytes("db/schema.sql"), resources().bytes("db/migrations/004_s1_store.sql"),
            StoreOpenMode::Takeover, resources().bytes("db/migrations/005_worker_tools.sql") +
                "\nINSERT INTO deliberately_absent_table VALUES(1);\n");
    });
    CHECK(sql_number(path, "PRAGMA user_version") == 402);
    CHECK(sql_number(path, "SELECT COUNT(*) FROM sqlite_master WHERE name='worker_tools_metadata'") == 0);
    Store completed(path, MigrationRegistry(resources()));
    CHECK(completed.diagnostics().at("user_version") == 410);
}

void new_entry_keeps_writer_lock_and_unknown_version_guards() {
    test::TempDir temporary;
    const auto path = temporary.path / "lock.sqlite";
    const MigrationRegistry selected(resources());
    {
        Store owner(path, selected);
        expect(ErrorCode::Conflict, [&] { Store second(path, selected); });
        Store observer(path, selected, StoreOpenMode::ReadOnly);
        CHECK(observer.diagnostics().at("read_only").as_bool());
    }
    sql_exec(path, "PRAGMA user_version=999;");
    expect(ErrorCode::Unsupported, [&] { Store refused(path, selected, StoreOpenMode::ReadOnly); });
    CHECK(sql_number(path, "PRAGMA user_version") == 999);
}

void default_prompt_identity_is_separate_from_external_config() {
    const auto original = load_config_catalog(source_root / "config/examples/system.yaml",
        source_root / "config/examples/functions", source_root / "prompts").freeze("general");
    resources().validate_frozen_prompts(original);
    test::TempDir temporary;
    for (const auto& [filename, content] : resources().prompt_documents())
        write_fixture(temporary.path / filename, content + "\nExplicit external prompt variation.\n");
    const auto external = load_config_catalog(source_root / "config/examples/system.yaml",
        source_root / "config/examples/functions", temporary.path).freeze("general");
    CHECK(external.sha256() != original.sha256());
    expect(ErrorCode::Corrupt, [&] { resources().validate_frozen_prompts(external); });
    CHECK(FrozenRequestConfig::restore(original.serialize(), original.sha256()).serialize() == original.serialize());
}

} // namespace

int main() { return test::run({
    {"source and installed bundle capture identical compiled program bytes", source_and_bundle_capture_identical_program_bytes},
    {"manifest rejects omissions extras duplicates and unknown versions", complete_manifest_is_compiled_and_closed},
    {"altered SQL and self-reported hashes fail before Store effects", altered_sql_and_self_reported_hash_never_open_store},
    {"missing leaf parent and root symbolic resources are refused", missing_and_symbolic_resources_are_rejected},
    {"FIFO and oversized resources stop before data reading", fifo_and_oversized_inputs_stop_before_data_open},
    {"captured resource bytes remain valid after source removal", captured_bytes_survive_later_filesystem_changes},
    {"captured Schema registry preserves shape and filename guards", schema_bytes_keep_original_shape_and_reference_guards},
    {"executable-relative discovery relocates and ignores cwd/environment", executable_discovery_is_relocatable_and_ignores_cwd_and_environment},
    {"invalid preferred bundle never falls back to another location", corrupt_preferred_bundle_does_not_fall_back},
    {"all registered prefixes retain original version and durable SQL hashes", registered_prefixes_preserve_old_versions_and_metadata},
    {"populated legacy Store requires explicit all-request upgrade", populated_upgrades_need_explicit_all_request_maintenance},
    {"legacy failing migration rolls back before registered retry", legacy_failure_rolls_back_then_registered_upgrade_succeeds},
    {"registry entry retains writer exclusivity and unknown-version refusal", new_entry_keeps_writer_lock_and_unknown_version_guards},
    {"distributed prompt identity stays separate from external frozen config", default_prompt_identity_is_separate_from_external_config}
}); }
