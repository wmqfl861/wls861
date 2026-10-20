#include "wls/delivery.hpp"
#include "test_support.hpp"

#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <barrier>
#include <cerrno>
#include <cstddef>
#include <fstream>
#include <functional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Test executable only: every successful fsync is the real Linux syscall. The
// module has no environment switch or injected production durability bypass.
namespace fault {
thread_local unsigned int file_syncs = 0;
thread_local unsigned int directory_syncs = 0;
thread_local unsigned int fail_file_on = 0;
thread_local unsigned int fail_directory_on = 0;
thread_local std::function<void()> after_file_sync;
thread_local std::function<void()> after_directory_sync;
void clear() {
    file_syncs = 0; directory_syncs = 0; fail_file_on = 0; fail_directory_on = 0;
    after_file_sync = {}; after_directory_sync = {};
}
struct Reset { ~Reset() { clear(); } };
}
extern "C" int fsync(int descriptor) {
    struct stat info{};
    if (::fstat(descriptor, &info) != 0) return -1;
    if (S_ISREG(info.st_mode) && ++fault::file_syncs == fault::fail_file_on) { errno = EIO; return -1; }
    if (S_ISDIR(info.st_mode) && ++fault::directory_syncs == fault::fail_directory_on) { errno = EIO; return -1; }
    const int result = static_cast<int>(::syscall(SYS_fsync, descriptor));
    auto& callback = S_ISREG(info.st_mode) ? fault::after_file_sync : fault::after_directory_sync;
    if (result == 0 && callback) {
        auto action = std::move(callback);
        callback = {};
        try { action(); } catch (...) { errno = EIO; return -1; }
    }
    return result;
}

namespace {
using namespace wls::s1;
using Object = boost::json::object;
using Array = boost::json::array;

class Fixture {
public:
    Fixture() : root(temporary.path / "deliveries") {
        std::filesystem::create_directory(root);
        CHECK(::chmod(root.c_str(), 0700) == 0);
    }
    ~Fixture() {
        // Only this self-created temporary fixture is unsealed for teardown.
        // Default traversal never follows a planted symlink outside the tree.
        std::error_code ignored;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(
                 temporary.path, std::filesystem::directory_options::skip_permission_denied, ignored)) {
            if (entry.is_directory(ignored) && !entry.is_symlink(ignored)) (void)::chmod(entry.path().c_str(), 0700);
        }
    }
    test::TempDir temporary;
    std::filesystem::path root;
};

void host_write(const std::filesystem::path& path, std::string_view bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    if (!output) throw std::runtime_error("test fixture write failed");
}
std::string host_read(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) throw std::runtime_error("test fixture read failed");
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
std::string string_at(const Json& value, std::string_view name) {
    return std::string(value.as_object().at(name).as_string());
}
bool bool_at(const Json& value, std::string_view name) { return value.as_object().at(name).as_bool(); }
std::size_t entry_count(const std::filesystem::path& path) {
    return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(path),
                                                 std::filesystem::directory_iterator()));
}
struct stat stat_path(const std::filesystem::path& path) {
    struct stat info{};
    CHECK(::lstat(path.c_str(), &info) == 0);
    return info;
}
std::string base64(std::string_view bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto a = static_cast<unsigned char>(bytes[i]);
        const auto b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0U;
        const auto c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0U;
        const auto word = (static_cast<unsigned int>(a) << 16U) | (b << 8U) | c;
        result.push_back(alphabet[(word >> 18U) & 63U]);
        result.push_back(alphabet[(word >> 12U) & 63U]);
        result.push_back(i + 1 < bytes.size() ? alphabet[(word >> 6U) & 63U] : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[word & 63U] : '=');
    }
    return result;
}

void refresh_metadata(Json& plan) {
    Array entries;
    for (const auto& file : plan.as_object().at("files").as_array())
        entries.emplace_back(Object{{"name", file.as_object().at("name")},
            {"sha256", file.as_object().at("sha256")}, {"size_bytes", file.as_object().at("size_bytes")}});
    plan.as_object().at("manifest").as_object()["files"] = entries;
    plan.as_object()["manifest_sha256"] = sha256_hex(canonical_json(plan.as_object().at("manifest")));
}
Json plan_for(const DeliveryDirectory& directory,
              std::vector<std::pair<std::string, std::string>> contents = {{"result.txt", "original result\n"}}) {
    std::sort(contents.begin(), contents.end());
    const auto id = new_id("dlv");
    Array files;
    for (const auto& [name, bytes] : contents)
        files.emplace_back(Object{{"name", name}, {"sha256", sha256_hex(bytes)},
            {"size_bytes", static_cast<std::uint64_t>(bytes.size())}, {"content_base64", base64(bytes)}});
    Json result = Object{{"delivery_id", id}, {"root_identity", directory.root_identity()},
        {"manifest", Object{{"delivery_id", id}, {"schema_version", 1},
            {"requirements", Array{"complete original requirements\n中文", "full amendment text"}},
            {"acceptance", Object{{"source", "host fixture only"}, {"business_assertion", false}}},
            {"experience", Object{{"status", "pending"}}}, {"unknown_effects", Array{}},
            {"limitations", Array{"synthetic fixture, not product acceptance"}}}},
        {"manifest_sha256", ""}, {"files", files}};
    refresh_metadata(result);
    return result;
}
std::filesystem::path package_path(const Fixture& fixture, const Json& plan) {
    return fixture.root / string_at(plan, "delivery_id");
}
void rejected(const DeliveryMutationResult& result, ErrorCode code) {
    CHECK(result.status == DeliveryMutationStatus::EffectNotStarted);
    CHECK(result.error_code == code);
    CHECK(result.publication.is_null());
}
void success(const DeliveryMutationResult& result, bool reused) {
    CHECK(result.status == DeliveryMutationStatus::Succeeded);
    CHECK(!result.error_code);
    CHECK(bool_at(result.publication, "verified"));
    CHECK(bool_at(result.publication, "durable"));
    CHECK(bool_at(result.publication, "reused") == reused);
}

void complete_original_bytes_and_read_only_inspect() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const std::string binary = std::string("\0", 1) + static_cast<char>(0xff) + "a";
    const auto plan = plan_for(directory, {{"binary.bin", binary}, {"empty", ""}, {"text.txt", "中文\n"}});
    fault::clear(); fault::Reset reset;
    const auto result = directory.publish(plan);
    success(result, false);
    CHECK(fault::file_syncs == 4 && fault::directory_syncs == 2);
    const auto package = package_path(fixture, plan);
    const auto manifest = canonical_json(plan.as_object().at("manifest"));
    CHECK(host_read(package / "manifest.json") == manifest);
    CHECK(host_read(package / "binary.bin") == binary);
    CHECK(host_read(package / "empty").empty());
    CHECK(host_read(package / "text.txt") == "中文\n");
    CHECK(entry_count(fixture.root) == 1 && entry_count(package) == 4);
    CHECK((stat_path(package).st_mode & 07777) == 0500);
    CHECK((stat_path(package / "binary.bin").st_mode & 07777) == 0400);
    CHECK(string_at(result.publication, "manifest_sha256") == sha256_hex(manifest));
    CHECK(string_at(result.publication, "files_sha256") ==
          sha256_hex(canonical_json(plan.as_object().at("manifest").as_object().at("files"))));
    CHECK(string_at(result.publication, "path") == package.string());
    CHECK(result.publication.as_object().at("root_identity") == directory.root_identity());
    fault::clear();
    const auto inspected = directory.inspect(plan);
    CHECK(bool_at(inspected, "verified") && !bool_at(inspected, "durable"));
    CHECK(fault::file_syncs == 0 && fault::directory_syncs == 0);
    const auto before = stat_path(package / "manifest.json");
    success(directory.publish(plan), true);
    CHECK(fault::file_syncs == 4 && fault::directory_syncs == 2);
    CHECK(before.st_ino == stat_path(package / "manifest.json").st_ino);
    CHECK(entry_count(fixture.root) == 1);
}

void frozen_bytes_do_not_read_the_live_source() {
    Fixture fixture;
    const auto source = fixture.temporary.path / "source.txt";
    host_write(source, "captured original");
    DeliveryDirectory directory(fixture.root);
    const auto plan = plan_for(directory, {{"result.txt", host_read(source)}});
    host_write(source, "later live bytes");
    success(directory.publish(plan), false);
    CHECK(host_read(package_path(fixture, plan) / "result.txt") == "captured original");
    CHECK(host_read(source) == "later live bytes");
}

void strict_plan_paths_and_canonical_base64() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto original = plan_for(directory, {{"one", "a"}});
    const std::vector<std::string> invalid_names{"", ".", "..", "manifest.json", "../escape", "/absolute",
        "nested/file", "nested\\file", "name\n", std::string("nul\0name", 8),
        "\xc2\x85", std::string(256, 'n')};
    for (const auto& name : invalid_names) {
        auto plan = original;
        plan.as_object().at("files").as_array()[0].as_object()["name"] = name;
        refresh_metadata(plan);
        rejected(directory.publish(plan), ErrorCode::InvalidInput);
    }
    for (const std::string encoded : {"YQ", "YQ===", "YR==", "YQ=Z", "YQ==AAAA", "!!!!", "YQ==\n"}) {
        auto plan = original;
        plan.as_object().at("files").as_array()[0].as_object()["content_base64"] = encoded;
        const auto result = directory.publish(plan);
        CHECK(result.status == DeliveryMutationStatus::EffectNotStarted);
        CHECK(result.publication.is_null());
    }
    auto plan = original;
    // Invalid UTF-8 cannot itself be canonicalized into a new manifest; pass
    // the invalid file name directly to the module's input validation.
    plan.as_object().at("files").as_array()[0].as_object()["name"] =
        std::string("bad") + static_cast<char>(0xff);
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    plan = original;
    plan.as_object()["unrecognized"] = true;
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    plan = original;
    plan.as_object()["delivery_id"] = "../../escape";
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    plan = original;
    plan.as_object()["manifest_sha256"] = std::string(64, 'a');
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    plan = original;
    plan.as_object().at("manifest").as_object()["delivery_id"] = new_id("dlv");
    refresh_metadata(plan);
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    plan = original;
    plan.as_object().at("files").as_array()[0].as_object()["size_bytes"] = 0;
    refresh_metadata(plan);
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    CHECK(entry_count(fixture.root) == 0);
}

void manifest_coverage_names_and_aggregate_bounds() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto original = plan_for(directory, {{"a", "abc"}, {"b", "def"}});
    auto plan = original;
    auto& files = plan.as_object().at("files").as_array();
    std::swap(files[0], files[1]);
    refresh_metadata(plan);
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    plan = original;
    plan.as_object().at("files").as_array()[1].as_object()["name"] = "a";
    refresh_metadata(plan);
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    plan = original;
    plan.as_object().at("manifest").as_object().at("files").as_array().pop_back();
    plan.as_object()["manifest_sha256"] = sha256_hex(canonical_json(plan.as_object().at("manifest")));
    rejected(directory.publish(plan), ErrorCode::InvalidInput);
    DeliveryLimits limits;
    limits.max_files = 1;
    rejected(DeliveryDirectory(fixture.root, limits).publish(original), ErrorCode::Quota);
    limits = {};
    limits.max_file_bytes = 2;
    rejected(DeliveryDirectory(fixture.root, limits).publish(original), ErrorCode::Quota);
    limits = {};
    limits.max_manifest_bytes = 32;
    rejected(DeliveryDirectory(fixture.root, limits).publish(original), ErrorCode::Quota);
    limits = {};
    limits.max_total_bytes = canonical_json(original.as_object().at("manifest")).size() + 5;
    rejected(DeliveryDirectory(fixture.root, limits).publish(original), ErrorCode::Quota);
    CHECK(entry_count(fixture.root) == 0);
    limits.max_total_bytes += 1;
    success(DeliveryDirectory(fixture.root, limits).publish(original), false);
}

void pinned_root_identity_and_unsafe_roots() {
    test::throws<Error>([] { DeliveryDirectory invalid("/"); });
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto original = plan_for(directory);
    auto wrong = original;
    wrong.as_object().at("root_identity").as_object()["inode"] =
        wrong.as_object().at("root_identity").as_object().at("inode").to_number<std::uint64_t>() + 1;
    rejected(directory.publish(wrong), ErrorCode::Stale);
    const auto alias = fixture.temporary.path / "alias";
    CHECK(::symlink(fixture.root.c_str(), alias.c_str()) == 0);
    test::throws<Error>([&] { DeliveryDirectory invalid(alias); });
    CHECK(::chmod(fixture.root.c_str(), 0777) == 0);
    test::throws<Error>([&] { DeliveryDirectory invalid(fixture.root); });
    CHECK(::chmod(fixture.root.c_str(), 0700) == 0);
    const auto moved = fixture.temporary.path / "moved";
    std::filesystem::rename(fixture.root, moved);
    std::filesystem::create_directory(fixture.root);
    CHECK(directory.publish(original).status == DeliveryMutationStatus::EffectNotStarted);
    test::throws<Error>([&] { (void)directory.inspect(original); });
    CHECK(entry_count(fixture.root) == 0 && entry_count(moved) == 0);
}

void same_identity_different_content_never_overwrites() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto original = plan_for(directory, {{"result", "first bytes"}});
    success(directory.publish(original), false);
    auto changed = plan_for(directory, {{"result", "other bytes"}});
    changed.as_object()["delivery_id"] = original.as_object().at("delivery_id");
    changed.as_object().at("manifest").as_object()["delivery_id"] = original.as_object().at("delivery_id");
    refresh_metadata(changed);
    rejected(directory.publish(changed), ErrorCode::Conflict);
    const auto package = package_path(fixture, original);
    CHECK(host_read(package / "result") == "first bytes");
    CHECK(host_read(package / "manifest.json") == canonical_json(original.as_object().at("manifest")));
    CHECK(entry_count(fixture.root) == 1);
}

void unsafe_published_entries_and_tampering() {
    for (const std::string attack : {"symlink", "hardlink", "fifo", "directory", "extra", "missing", "bytes", "writable"}) {
        Fixture fixture;
        DeliveryDirectory directory(fixture.root);
        const auto plan = plan_for(directory, {{"result", "safe bytes"}});
        success(directory.publish(plan), false);
        const auto package = package_path(fixture, plan);
        const auto target = package / "result";
        CHECK(::chmod(package.c_str(), 0700) == 0);
        if (attack == "symlink" || attack == "fifo" || attack == "directory") {
            std::filesystem::remove(target);
            if (attack == "symlink") {
                const auto external = fixture.temporary.path / "external";
                host_write(external, "private bytes");
                CHECK(::symlink(external.c_str(), target.c_str()) == 0);
            } else if (attack == "fifo") CHECK(::mkfifo(target.c_str(), 0400) == 0);
            else std::filesystem::create_directory(target);
        } else if (attack == "hardlink") {
            CHECK(::link(target.c_str(), (fixture.temporary.path / "second-link").c_str()) == 0);
        } else if (attack == "extra") host_write(package / "foreign", "must be retained");
        else if (attack == "missing") std::filesystem::remove(target);
        else if (attack == "writable") CHECK(::chmod(target.c_str(), 0600) == 0);
        else {
            CHECK(::chmod(target.c_str(), 0600) == 0);
            host_write(target, "evil bytes");
            CHECK(::chmod(target.c_str(), 0400) == 0);
        }
        CHECK(::chmod(package.c_str(), 0500) == 0);
        test::throws<Error>([&] { (void)directory.inspect(plan); });
        CHECK(directory.publish(plan).status != DeliveryMutationStatus::Succeeded);
        CHECK(entry_count(fixture.root) == 1);
        if (attack == "extra") CHECK(host_read(package / "foreign") == "must be retained");
        if (attack == "bytes") CHECK(host_read(target) == "evil bytes");
    }
}

void planted_final_directory_or_symlink_is_not_adopted() {
    for (const bool symbolic : {false, true}) {
        Fixture fixture;
        DeliveryDirectory directory(fixture.root);
        const auto plan = plan_for(directory);
        const auto package = package_path(fixture, plan);
        if (symbolic) CHECK(::symlink(fixture.temporary.path.c_str(), package.c_str()) == 0);
        else std::filesystem::create_directory(package);
        rejected(directory.publish(plan), ErrorCode::Unauthorized);
        CHECK(entry_count(fixture.root) == 1);
    }
}

void cooperating_publishers_share_one_exact_package() {
    Fixture fixture;
    DeliveryDirectory first(fixture.root);
    DeliveryDirectory second(fixture.root);
    const auto plan = plan_for(first);
    std::barrier start(3);
    std::array<DeliveryMutationResult, 2> results;
    std::thread left([&] { start.arrive_and_wait(); results[0] = first.publish(plan); });
    std::thread right([&] { start.arrive_and_wait(); results[1] = second.publish(plan); });
    start.arrive_and_wait();
    left.join(); right.join();
    CHECK(results[0].status == DeliveryMutationStatus::Succeeded);
    CHECK(results[1].status == DeliveryMutationStatus::Succeeded);
    CHECK(bool_at(results[0].publication, "reused") != bool_at(results[1].publication, "reused"));
    CHECK(entry_count(fixture.root) == 1);
    CHECK(host_read(package_path(fixture, plan) / "result.txt") == "original result\n");
}

void prepublication_sync_failures_remove_only_owned_stage() {
    for (const unsigned int fail_file_on : {1U, 2U, 0U}) {
        Fixture fixture;
        DeliveryDirectory directory(fixture.root);
        const auto plan = plan_for(directory);
        fault::clear(); fault::Reset reset;
        fault::fail_file_on = fail_file_on;
        fault::fail_directory_on = fail_file_on == 0 ? 1U : 0U;
        const auto result = directory.publish(plan);
        rejected(result, ErrorCode::Storage);
        CHECK(bool_at(result.evidence, "cleanup_complete"));
        CHECK(!bool_at(result.evidence, "rename_attempted"));
        CHECK(!bool_at(result.evidence, "staging_remaining"));
        CHECK(entry_count(fixture.root) == 0);
        fault::clear();
        success(directory.publish(plan), false);
    }
}

void postrename_sync_failure_survives_and_recovers() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto plan = plan_for(directory);
    fault::clear(); fault::Reset reset;
    fault::fail_directory_on = 2;
    const auto unknown = directory.publish(plan);
    CHECK(unknown.status == DeliveryMutationStatus::OutcomeUnknown);
    CHECK(unknown.error_code == ErrorCode::UnknownOutcome);
    CHECK(unknown.publication.is_null());
    CHECK(bool_at(unknown.evidence, "rename_attempted") && bool_at(unknown.evidence, "renamed"));
    CHECK(!bool_at(unknown.evidence, "durable") && !bool_at(unknown.evidence, "staging_remaining"));
    const auto package = package_path(fixture, plan);
    const auto prior = stat_path(package);
    fault::clear();
    DeliveryDirectory reopened(fixture.root);
    const auto observed = reopened.inspect(plan);
    CHECK(bool_at(observed, "verified") && !bool_at(observed, "durable"));
    CHECK(fault::file_syncs == 0 && fault::directory_syncs == 0);
    success(reopened.publish(plan), true);
    CHECK(fault::file_syncs == 2 && fault::directory_syncs == 2);
    CHECK(stat_path(package).st_ino == prior.st_ino && entry_count(fixture.root) == 1);
}

void failed_recovery_sync_does_not_claim_durability() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto plan = plan_for(directory);
    success(directory.publish(plan), false);
    fault::clear(); fault::Reset reset;
    fault::fail_file_on = 1;
    const auto result = directory.publish(plan);
    CHECK(result.status == DeliveryMutationStatus::OutcomeUnknown);
    CHECK(result.publication.is_null());
    CHECK(!bool_at(result.evidence, "durable"));
    CHECK(!bool_at(directory.inspect(plan), "durable"));
    fault::clear();
    success(directory.publish(plan), true);
}

void published_file_change_before_final_verification_is_unknown() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto plan = plan_for(directory, {{"result", "safe bytes"}});
    const auto target = package_path(fixture, plan) / "result";
    fault::clear(); fault::Reset reset;
    fault::after_directory_sync = [&] {
        // The first directory sync is the private stage, before publication.
        CHECK(!std::filesystem::exists(package_path(fixture, plan)));
        fault::after_directory_sync = [&] {
            // The second is the root after rename: replace bytes before the
            // publisher's final verification, without changing their length.
            CHECK(::chmod(target.c_str(), 0600) == 0);
            host_write(target, "evil bytes");
            CHECK(::chmod(target.c_str(), 0400) == 0);
        };
    };
    const auto result = directory.publish(plan);
    CHECK(result.status == DeliveryMutationStatus::OutcomeUnknown);
    CHECK(result.publication.is_null());
    CHECK(bool_at(result.evidence, "renamed"));
    CHECK(host_read(target) == "evil bytes");
    CHECK(entry_count(fixture.root) == 1);
    test::throws<Error>([&] { (void)directory.inspect(plan); });
}

void foreign_stage_entry_is_retained_and_never_published() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto plan = plan_for(directory);
    std::filesystem::path stage;
    fault::clear(); fault::Reset reset;
    fault::after_file_sync = [&] {
        CHECK(!std::filesystem::exists(package_path(fixture, plan)));
        CHECK(entry_count(fixture.root) == 1);
        stage = std::filesystem::directory_iterator(fixture.root)->path();
        host_write(stage / "foreign-user-bytes", "must not delete");
    };
    const auto result = directory.publish(plan);
    CHECK(result.status == DeliveryMutationStatus::OutcomeUnknown);
    CHECK(!bool_at(result.evidence, "rename_attempted"));
    CHECK(bool_at(result.evidence, "staging_remaining"));
    CHECK(host_read(stage / "foreign-user-bytes") == "must not delete");
    CHECK(!std::filesystem::exists(package_path(fixture, plan)));
    fault::clear();
    success(directory.publish(plan), false);
    CHECK(entry_count(fixture.root) == 2);
    CHECK(host_read(stage / "foreign-user-bytes") == "must not delete");
}

void replacement_stage_file_is_retained() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto plan = plan_for(directory);
    std::filesystem::path stage;
    fault::clear(); fault::Reset reset;
    fault::after_file_sync = [&] {
        stage = std::filesystem::directory_iterator(fixture.root)->path();
        std::filesystem::rename(stage / "manifest.json", fixture.temporary.path / "original-manifest");
        host_write(stage / "manifest.json", "foreign replacement");
    };
    const auto result = directory.publish(plan);
    CHECK(result.status == DeliveryMutationStatus::OutcomeUnknown);
    CHECK(bool_at(result.evidence, "staging_remaining"));
    CHECK(host_read(stage / "manifest.json") == "foreign replacement");
    CHECK(!std::filesystem::exists(package_path(fixture, plan)));
}

void moving_root_during_staging_retains_uncertain_files() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto plan = plan_for(directory);
    const auto moved = fixture.temporary.path / "moved";
    fault::clear(); fault::Reset reset;
    fault::after_file_sync = [&] {
        std::filesystem::rename(fixture.root, moved);
        std::filesystem::create_directory(fixture.root);
    };
    const auto result = directory.publish(plan);
    CHECK(result.status == DeliveryMutationStatus::OutcomeUnknown);
    CHECK(bool_at(result.evidence, "staging_remaining"));
    CHECK(!bool_at(result.evidence, "rename_attempted"));
    CHECK(entry_count(fixture.root) == 0 && entry_count(moved) == 1);
}

void rename_syscall_failure_preserves_orphan_for_reconciliation() {
    Fixture fixture;
    DeliveryDirectory directory(fixture.root);
    const auto plan = plan_for(directory);
    const auto child = ::fork();
    CHECK(child >= 0);
    if (child == 0) {
        const std::array<sock_filter, 4> filter{{
            BPF_STMT(BPF_LD | BPF_W | BPF_ABS, offsetof(seccomp_data, nr)),
            BPF_JUMP(BPF_JMP | BPF_JEQ | BPF_K, static_cast<unsigned int>(SYS_renameat2), 0, 1),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EIO),
            BPF_STMT(BPF_RET | BPF_K, SECCOMP_RET_ALLOW)
        }};
        sock_fprog program{static_cast<unsigned short>(filter.size()), const_cast<sock_filter*>(filter.data())};
        if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0 || ::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) != 0)
            ::_exit(2);
        const auto result = directory.publish(plan);
        const bool expected = result.status == DeliveryMutationStatus::OutcomeUnknown &&
            bool_at(result.evidence, "rename_attempted") && !bool_at(result.evidence, "renamed") &&
            bool_at(result.evidence, "staging_remaining");
        ::_exit(expected ? 0 : 3);
    }
    int status = 0;
    CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
    CHECK(!std::filesystem::exists(package_path(fixture, plan)) && entry_count(fixture.root) == 1);
    const auto orphan = std::filesystem::directory_iterator(fixture.root)->path();
    const auto original_bytes = host_read(orphan / "manifest.json");
    success(directory.publish(plan), false);
    CHECK(entry_count(fixture.root) == 2);
    CHECK(host_read(orphan / "manifest.json") == original_bytes);
}
} // namespace

int main() {
    return test::run({
        {"complete original bytes and pure inspection", complete_original_bytes_and_read_only_inspect},
        {"frozen bytes do not reread live sources", frozen_bytes_do_not_read_the_live_source},
        {"strict paths and canonical base64", strict_plan_paths_and_canonical_base64},
        {"manifest coverage and aggregate bounds", manifest_coverage_names_and_aggregate_bounds},
        {"pinned root identity and unsafe roots", pinned_root_identity_and_unsafe_roots},
        {"same ID different content never overwrites", same_identity_different_content_never_overwrites},
        {"unsafe published entries and tampering", unsafe_published_entries_and_tampering},
        {"planted final paths are not adopted", planted_final_directory_or_symlink_is_not_adopted},
        {"cooperating publishers share one package", cooperating_publishers_share_one_exact_package},
        {"prepublication sync failures clean owned stages", prepublication_sync_failures_remove_only_owned_stage},
        {"postrename sync failure and recovery", postrename_sync_failure_survives_and_recovers},
        {"failed recovery sync does not claim durability", failed_recovery_sync_does_not_claim_durability},
        {"postpublication tampering is not accepted", published_file_change_before_final_verification_is_unknown},
        {"foreign staging entries are retained", foreign_stage_entry_is_retained_and_never_published},
        {"replacement staged files are retained", replacement_stage_file_is_retained},
        {"root movement retains uncertain files", moving_root_during_staging_retains_uncertain_files},
        {"rename failure preserves orphan stage", rename_syscall_failure_preserves_orphan_for_reconciliation}
    });
}
