#include "wls/owned_capacity.hpp"
#include "test_support.hpp"

#include <array>
#include <csignal>
#include <fstream>
#include <optional>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

// Private syscall interposition observes only one disposable fixture's staged
// regular file in the child. No production hook or power-loss claim is added.
namespace capacity_fault { thread_local std::string kill_after_stage_sync; }
extern "C" int fsync(int fd) {
    const auto result = static_cast<int>(::syscall(SYS_fsync, fd));
    if (result == 0 && !capacity_fault::kill_after_stage_sync.empty()) {
        struct stat info{};
        std::array<char, 8192> name{};
        const auto descriptor = "/proc/self/fd/" + std::to_string(fd);
        const auto length = ::readlink(descriptor.c_str(), name.data(), name.size());
        if (::fstat(fd, &info) == 0 && S_ISREG(info.st_mode) && length > 0 &&
            static_cast<std::size_t>(length) < name.size() &&
            std::string_view(name.data(), static_cast<std::size_t>(length)).starts_with(capacity_fault::kill_after_stage_sync))
            (void)::kill(::getpid(), SIGKILL);
    }
    return result;
}

namespace {
using namespace wls::s1;
using O = boost::json::object;
using A = boost::json::array;
constexpr std::uint64_t mib = 1024U * 1024U;
Json policy(std::size_t entries = 4095, std::uint64_t bytes = 240U * mib,
            std::size_t inventory = 8U * 1024U * 1024U) {
    return O{{"version", "owned-capacity-v1"}, {"max_entries", entries},
        {"max_total_bytes", bytes}, {"max_inventory_bytes", inventory}};
}
void rejects(ErrorCode code, const std::function<void()>& action) {
    try { action(); }
    catch (const Error& error) {
        if (error.code() != code) throw std::runtime_error(std::string("wrong error: ") + error.what());
        return;
    }
    throw std::runtime_error("expected rejection was not thrown");
}
void refused(const WorkspaceMutationResult& result) {
    CHECK(result.status == WorkspaceMutationStatus::EffectNotStarted);
    CHECK(result.error_code == ErrorCode::Quota);
    CHECK(result.result.is_null());
}
struct Fixture {
    test::TempDir temp;
    Json capacity, intent, resource;
    std::filesystem::path work;
    explicit Fixture(Json selected = policy()) : capacity(std::move(selected)) {
        Workspace parent(temp.path, WorkspaceAccess::ReadOnly);
        const auto id = new_id("res");
        intent = O{{"resource_id", id}, {"leaf", id}, {"request_id", "req_fixture"}, {"team_id", "team_fixture"},
            {"parent_identity", parent.root_identity()}, {"marker", O{{"resource_id", id}, {"request_id", "req_fixture"},
                {"team_id", "team_fixture"}, {"nonce", new_id("nonce")}}}};
        if (!capacity.is_null()) {
            intent.as_object()["capacity_policy"] = capacity;
            intent.at("marker").as_object()["capacity_policy_sha256"] = sha256_hex(canonical_json(capacity));
        }
        resource = O{{"resource_id", id}, {"intent", intent}, {"identity", create_owned_workspace(intent)}};
        work = temp.path / id / "work";
    }
    void seed(std::string_view name, std::string_view bytes = "") {
        std::ofstream out(work / std::string(name), std::ios::binary);
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size())); CHECK(out.good());
    }
    std::size_t count() const {
        return static_cast<std::size_t>(std::distance(std::filesystem::directory_iterator(work), std::filesystem::directory_iterator{}));
    }
    Workspace writer() const { return Workspace(work, WorkspaceAccess::ReadWrite, owned_workspace_limits(capacity)); }
};

void legacy_gap_is_reproduced() {
    Fixture f(nullptr);
    for (std::size_t i = 0; i < 4096; ++i) f.seed("item-" + std::to_string(i));
    auto writer = f.writer(); CHECK(writer.limits().max_entries == 20000);
    CHECK(writer.write_atomic("legal-4097th", "x").status == WorkspaceMutationStatus::Succeeded);
    rejects(ErrorCode::Quota, [&] { (void)inventory_owned_workspace(f.resource); });
    CHECK(f.count() == 4097); CHECK(std::filesystem::exists(f.work / "legal-4097th"));
}

void hard_entry_boundary_closes_out() {
    Fixture f;
    for (std::size_t i = 0; i < 4094; ++i) f.seed("item-" + std::to_string(i));
    auto writer = f.writer();
    CHECK(writer.write_atomic("last", "old").status == WorkspaceMutationStatus::Succeeded);
    CHECK(f.count() == 4095);
    refused(writer.write_atomic("extra", "new")); refused(writer.mkdir("extra-dir"));
    CHECK(f.count() == 4095);
    CHECK(writer.write_atomic("last", "new", sha256_hex("old")).status == WorkspaceMutationStatus::Succeeded);
    const auto inventory = inventory_owned_workspace(f.resource);
    CHECK(inventory.at("entries").as_array().size() == 4095);
    CHECK(inventory.at("capacity_policy") == f.capacity);
    const auto done = erase_owned_workspace(f.resource, inventory, 4096);
    CHECK(done.at("complete") == true && done.at("removed_entries") == 4096);
    CHECK(!std::filesystem::exists(f.work));
}

void byte_boundary_is_effect_free() {
    Fixture f(policy(8, 8)); auto writer = f.writer();
    CHECK(writer.write_atomic("full", "12345678").status == WorkspaceMutationStatus::Succeeded);
    refused(writer.write_atomic("extra", "x"));
    refused(writer.write_atomic("full", "123456789", sha256_hex("12345678")));
    refused(writer.patch("full", sha256_hex("12345678"), 0, "1", "99"));
    CHECK(f.count() == 1); CHECK(writer.read("full").at("sha256") == Json(sha256_hex("12345678")));
    CHECK(writer.patch("full", sha256_hex("12345678"), 0, "1", "9").status == WorkspaceMutationStatus::Succeeded);
    CHECK(erase_owned_workspace(f.resource, inventory_owned_workspace(f.resource)).at("complete") == true);
}

void encoded_budget_counts_escaped_names() {
    std::vector<std::string> names;
    std::size_t budget = owned_inventory_root_budget + owned_inventory_stage_budget;
    for (std::size_t i = 0; i < 50; ++i) {
        names.push_back("entry-" + std::to_string(i) + std::string(190, '\x01'));
        budget += owned_inventory_entry_budget(names.back());
    }
    CHECK(budget >= 256U * 1024U);
    Fixture f(policy(200, 1024, budget)); auto writer = f.writer();
    for (const auto& name : names) CHECK(writer.write_atomic(name, "x").status == WorkspaceMutationStatus::Succeeded);
    CHECK(f.count() == names.size());
    refused(writer.write_atomic("extra", "x")); refused(writer.mkdir("extra-dir"));
    CHECK(f.count() == names.size());
    CHECK(writer.write_atomic(names.back(), "y", sha256_hex("x")).status == WorkspaceMutationStatus::Succeeded);
    const auto inventory = inventory_owned_workspace(f.resource);
    CHECK(canonical_json(inventory).size() <= budget);
    CHECK(erase_owned_workspace(f.resource, inventory).at("complete") == true);
}

void interrupted_replace_retains_bounded_stage() {
    Fixture f(policy(4095, 240U * mib));
    for (std::size_t i = 0; i < 4080; ++i) f.seed("item-" + std::to_string(i));
    const std::string old_bytes(static_cast<std::size_t>(16U * mib), 'a');
    // Exercise the actual hard content boundary as well as the hard entry
    // boundary: fifteen legal 16 MiB files plus one interrupted replacement.
    for (std::size_t i = 0; i < 14; ++i) f.seed("bulk-" + std::to_string(i), old_bytes);
    f.seed("victim", old_bytes);
    const auto child = ::fork(); CHECK(child >= 0);
    if (child == 0) {
        capacity_fault::kill_after_stage_sync = f.work.string() + "/.wls-tmp-";
        auto writer = f.writer();
        (void)writer.write_atomic("victim", std::string(old_bytes.size(), 'b'), sha256_hex(old_bytes));
        ::_exit(7);
    }
    int status = 0; CHECK(::waitpid(child, &status, 0) == child);
    CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    CHECK(f.count() == 4096);
    auto writer = f.writer();
    CHECK(writer.read("victim").at("sha256") == Json(sha256_hex(old_bytes)));
    refused(writer.write_atomic("another", "x"));
    const auto inventory = inventory_owned_workspace(f.resource);
    CHECK(inventory.at("entries").as_array().size() == 4096);
    std::uint64_t observed = 0;
    std::size_t stages = 0;
    for (const auto& record : inventory.at("entries").as_array()) {
        if (record.at("type") == "file") observed += record.at("size_bytes").to_number<std::uint64_t>();
        if (record.at("identity").at("path").as_string().starts_with(".wls-tmp-")) ++stages;
    }
    CHECK(stages == 1 && observed == 256U * mib);
    CHECK(erase_owned_workspace(f.resource, inventory, 4096).at("complete") == false);
    CHECK(erase_owned_workspace(f.resource, inventory, 1).at("complete") == true);
}

void long_parent_reserves_representable_stage() {
    Fixture f; auto writer = f.writer();
    std::string parent;
    for (std::size_t i = 0; i < 16; ++i) {
        if (!parent.empty()) parent += '/';
        parent += std::string(250, 'd');
        CHECK(writer.mkdir(parent).status == WorkspaceMutationStatus::Succeeded);
    }
    parent += '/' + std::string(45, 'e');
    CHECK(parent.size() == 4061);
    CHECK(writer.mkdir(parent).status == WorkspaceMutationStatus::Succeeded);
    refused(writer.write_atomic(parent + "/x", "x"));
    const auto inventory = inventory_owned_workspace(f.resource);
    CHECK(inventory.at("entries").as_array().size() == 17);
    CHECK(erase_owned_workspace(f.resource, inventory).at("complete") == true);
}

void original_inventory_survives_partial_pass() {
    Fixture f(policy(3)); auto writer = f.writer();
    CHECK(writer.write_atomic("a", "a").status == WorkspaceMutationStatus::Succeeded);
    CHECK(writer.write_atomic("z", "z").status == WorkspaceMutationStatus::Succeeded);
    const auto inventory = inventory_owned_workspace(f.resource);
    const auto hash = sha256_hex(canonical_json(inventory));
    CHECK(erase_owned_workspace(f.resource, inventory, 1).at("complete") == false);
    f.seed("new", "unauthorized");
    rejects(ErrorCode::Conflict, [&] { (void)erase_owned_workspace(f.resource, inventory); });
    CHECK(std::filesystem::exists(f.work / "a"));
    std::filesystem::remove(f.work / "new");
    const auto done = erase_owned_workspace(f.resource, inventory);
    CHECK(done.at("proof").at("inventory_sha256") == Json(hash));
}

void identity_replacement_is_not_capacity_authority() {
    Fixture f(policy(1)); auto writer = f.writer();
    CHECK(writer.write_atomic("one", "same").status == WorkspaceMutationStatus::Succeeded);
    const auto inventory = inventory_owned_workspace(f.resource);
    std::filesystem::rename(f.work / "one", f.temp.path / "original"); f.seed("one", "same");
    rejects(ErrorCode::Conflict, [&] { (void)erase_owned_workspace(f.resource, inventory); });
    CHECK(std::filesystem::exists(f.work / "one")); CHECK(std::filesystem::exists(f.temp.path / "original"));
}

void frozen_policy_and_limits_cannot_widen() {
    Fixture f(policy(1)); auto writer = f.writer();
    writer.bind_limits(owned_workspace_limits(f.capacity));
    writer.bind_limits(owned_workspace_limits(f.capacity));
    rejects(ErrorCode::Conflict, [&] { writer.bind_limits(owned_workspace_limits(policy(2))); });
    CHECK(writer.write_atomic("one", "x").status == WorkspaceMutationStatus::Succeeded);
    auto inventory = inventory_owned_workspace(f.resource);
    inventory.at("capacity_policy").as_object()["max_entries"] = 2;
    rejects(ErrorCode::Conflict, [&] { (void)erase_owned_workspace(f.resource, inventory); });
    auto corrupted = f.resource;
    corrupted.at("intent").as_object().erase("capacity_policy");
    rejects(ErrorCode::Conflict, [&] { (void)inventory_owned_workspace(corrupted); });
    CHECK(f.count() == 1);
}

void legacy_inventory_shape_and_replay_are_unchanged() {
    Fixture f(nullptr); f.seed("one", "x"); f.seed("two", "y");
    const auto inventory = inventory_owned_workspace(f.resource);
    CHECK(inventory.as_object().size() == 3 && !inventory.as_object().contains("capacity_policy"));
    const auto original = canonical_json(inventory);
    CHECK(erase_owned_workspace(f.resource, inventory, 1).at("complete") == false);
    CHECK(erase_owned_workspace(f.resource, parse_json_strict(original)).at("complete") == true);
    CHECK(canonical_json(inventory) == original);
}

void policy_bounds_do_not_expand_cleanup() {
    const auto capacity = policy(); const auto cleanup = owned_cleanup_limits(capacity);
    CHECK(cleanup.max_entries == 4096 && cleanup.max_total_bytes == 256U * mib);
    for (const auto* key : {"max_entries", "max_total_bytes", "max_inventory_bytes"}) {
        auto excessive = capacity; excessive.as_object()[key] = excessive.at(key).to_number<std::uint64_t>() + 1U;
        rejects(ErrorCode::InvalidInput, [&] { validate_owned_workspace_capacity_policy(excessive); });
    }
    auto extra = capacity; extra.as_object()["allow_run"] = true;
    rejects(ErrorCode::InvalidInput, [&] { validate_owned_workspace_capacity_policy(extra); });
    CHECK(owned_workspace_limits(nullptr).max_entries == 20000);
    CHECK(owned_workspace_limits(nullptr).max_inventory_bytes == 0);
}
} // namespace

int main() {
    if (::geteuid() == 0) { std::cerr << "owned capacity tests require an ordinary uid\n"; return 2; }
    return test::run({
        {"legacy 4097-entry legal tree reproduces cleanup gap", legacy_gap_is_reproduced},
        {"4095-entry boundary rejects extra effects and completes cleanup", hard_entry_boundary_closes_out},
        {"exact total-byte boundary rejects writes and patches before effects", byte_boundary_is_effect_free},
        {"exact encoded inventory budget counts escaped names", encoded_budget_counts_escaped_names},
        {"SIGKILL after replacement stage sync remains cleanup-bounded", interrupted_replace_retains_bounded_stage},
        {"long parent cannot create an unrepresentable stage path", long_parent_reserves_representable_stage},
        {"partial pass resumes only the original immutable inventory", original_inventory_survives_partial_pass},
        {"replacement identity is refused even within capacity", identity_replacement_is_not_capacity_authority},
        {"frozen policy and bound Workspace limits cannot widen", frozen_policy_and_limits_cannot_widen},
        {"legacy inventory keeps exact bytes across replay", legacy_inventory_shape_and_replay_are_unchanged},
        {"new policy never expands cleanup hard limits", policy_bounds_do_not_expand_cleanup}});
}
