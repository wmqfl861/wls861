#include "wls/process_evidence.hpp"
#include "test_support.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <map>
using namespace wls::s1;
using Object = boost::json::object;
namespace {
struct Fixture {
    test::TempDir directory;
    int fd{::open(directory.path.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
    ~Fixture() { if (fd >= 0) ::close(fd); }
    void write(const std::string& name, const std::string& bytes) {
        std::ofstream file(directory.path / name, std::ios::binary); CHECK(file.good()); file << bytes; file.close(); CHECK(file.good());
    }
    Json snapshot() { CHECK(fd >= 0); return observe_process_workspace(fd); }
};
ProcessResult strong_fixture(const Json& before, const Json& after) {
    // A typed classifier fixture, NOT an executed/attested strong sandbox.
    ProcessResult result; result.status = ProcessStatus::Exited; result.exit_code = 0;
    result.spawned = result.launcher_exec_confirmed = result.strong_isolation = result.all_descendants_exited = true;
    result.evidence = Object{{"network", "none"}, {"supervision", "cgroup-v2"},
        {"descendants_proof", "cgroup.events populated=0"}, {"workspace_mount_verified", true},
        {"namespaces_verified", true}, {"command_start_released", true},
        {"workspace_observation", process_workspace_changes(before, after)}};
    return result;
}
void complete_binary_changes_are_captured() {
    Fixture f; f.write("keep", "unchanged"); f.write("changed", "old"); f.write("remove", "old");
    const auto before = f.snapshot();
    f.write("changed", "new"); f.write("binary", std::string("a\0b", 3));
    CHECK(::unlink((f.directory.path / "remove").c_str()) == 0);
    const auto after = f.snapshot();
    const auto diff = process_workspace_changes(before, after);
    std::map<std::string, std::string> kinds;
    for (const auto& entry : diff.at("changes").as_array()) kinds.emplace(std::string(entry.at("path").as_string()), std::string(entry.at("kind").as_string()));
    CHECK(kinds.size() == 3 && kinds.at("binary") == "added" && kinds.at("changed") == "changed" && kinds.at("remove") == "removed");
    CHECK(diff.at("effects_undone") == false);
    CHECK(std::string(diff.at("before_sha256").as_string()) == sha256_hex(canonical_json(before)));
    CHECK(std::string(diff.at("after_sha256").as_string()) == sha256_hex(canonical_json(after)));
    CHECK(after.at("total_bytes") == 15); // unchanged(9), new(3), binary(3)
    CHECK(before == diff.at("before"));
}
void statuses_require_complete_proof() {
    Fixture f; const auto before = f.snapshot(); f.write("partial-output", "effect happened"); const auto after = f.snapshot();
    auto p = strong_fixture(before, after);
    CHECK(process_operation_outcome(p) == "SUCCEEDED");
    p.exit_code = 1; CHECK(process_operation_outcome(p) == "FAILED");
    p.status = ProcessStatus::TimedOut; CHECK(process_operation_outcome(p) == "TIMED_OUT");
    p.status = ProcessStatus::OutputLimit; CHECK(process_operation_outcome(p) == "FAILED");
    p.status = ProcessStatus::Cancelled; CHECK(process_operation_outcome(p) == "CANCELLED");
    p.status = ProcessStatus::Signalled; p.signal = 9; CHECK(process_operation_outcome(p) == "FAILED");
    p.signal.reset(); CHECK(process_operation_outcome(p) == "UNKNOWN");
    p.status = ProcessStatus::CleanupUnknown; CHECK(process_operation_outcome(p) == "UNKNOWN");
    p.status = ProcessStatus::SpawnFailed; CHECK(process_operation_outcome(p) == "UNKNOWN");
    p.status = ProcessStatus::Exited; p.exit_code.reset(); CHECK(process_operation_outcome(p) == "UNKNOWN");
}
void missing_or_contradictory_proofs_stay_unknown() {
    Fixture f; auto s = f.snapshot(); const auto valid = strong_fixture(s, s);
    for (const auto* field : {"network", "supervision", "descendants_proof", "workspace_mount_verified", "namespaces_verified", "command_start_released", "workspace_observation"}) {
        auto p = valid; p.evidence.as_object().erase(field); CHECK(process_operation_outcome(p) == "UNKNOWN");
    }
    for (int mode = 0; mode < 7; ++mode) {
        auto p = valid;
        if (mode == 0) p.strong_isolation = false;
        if (mode == 1) p.all_descendants_exited = false;
        if (mode == 2) p.launcher_exec_confirmed = false;
        if (mode == 3) p.evidence.as_object()["network"] = "host";
        if (mode == 4) p.evidence.at("workspace_observation").as_object()["before_sha256"] = "forged";
        if (mode == 5) p.evidence.at("workspace_observation").at("after").as_object()["complete"] = false;
        if (mode == 6) p.evidence.at("workspace_observation").at("changes").as_array().push_back(Object{{"path", "invented"}, {"kind", "removed"}});
        CHECK(process_operation_outcome(p) == "UNKNOWN");
    }
}
void refused_before_fork_has_no_ambiguous_effect() {
    ProcessResult p; CHECK(process_operation_outcome(p) == "FAILED");
    p.status = ProcessStatus::Cancelled; CHECK(process_operation_outcome(p) == "CANCELLED");
    p.status = ProcessStatus::CleanupUnknown; CHECK(process_operation_outcome(p) == "UNKNOWN");
}
void unsafe_or_partial_trees_are_rejected() {
    for (int mode = 0; mode < 5; ++mode) {
        Fixture f; f.write("file", "x");
        if (mode == 0) CHECK(::symlink("file", (f.directory.path / "link").c_str()) == 0);
        if (mode == 1) CHECK(::link((f.directory.path / "file").c_str(), (f.directory.path / "link").c_str()) == 0);
        if (mode == 2) CHECK(::chmod((f.directory.path / "file").c_str(), 0660) == 0);
        if (mode == 3) CHECK(::mkfifo((f.directory.path / "pipe").c_str(), 0600) == 0);
        if (mode == 4) CHECK(::truncate((f.directory.path / "file").c_str(), 16 * 1024 * 1024 + 1) == 0);
        test::throws<Error>([&] { (void)f.snapshot(); });
    }
}
void observations_are_bounded_and_root_specific() {
    Fixture f, other; const auto empty = f.snapshot();
    test::throws<Error>([&] { (void)process_workspace_changes(empty, other.snapshot()); });
    for (int i = 0; i < 257; ++i) f.write("entry-" + std::to_string(i), "");
    test::throws<Error>([&] { (void)f.snapshot(); });
}
void root_metadata_changes_are_retained() {
    Fixture f; const auto before = f.snapshot();
    CHECK(::chmod(f.directory.path.c_str(), 0500) == 0);
    const auto after = f.snapshot(); const auto diff = process_workspace_changes(before, after);
    CHECK(diff.at("root_metadata_changed") == true);
    CHECK(diff.at("changes").as_array().empty());
    CHECK(diff.at("before").at("root_metadata") != diff.at("after").at("root_metadata"));
    CHECK(::chmod(f.directory.path.c_str(), 0700) == 0);
}
void replacement_with_same_bytes_is_known_change() {
    Fixture f; f.write("result", "same"); const auto before = f.snapshot();
    CHECK(::rename((f.directory.path / "result").c_str(), (f.directory.path / "old").c_str()) == 0);
    f.write("result", "same");
    const auto after = f.snapshot(); const auto diff = process_workspace_changes(before, after);
    CHECK(diff.at("changes").as_array().size() == 2);
    CHECK(process_operation_outcome(strong_fixture(before, after)) == "SUCCEEDED");
}
}
int main() { return test::run({
    {"bounded full workspace snapshots retain byte hashes and exact changes", complete_binary_changes_are_captured},
    {"known exit/failure/timeout/output/cancel semantics require complete proof", statuses_require_complete_proof},
    {"every missing or contradictory proof remains UNKNOWN", missing_or_contradictory_proofs_stay_unknown},
    {"pre-fork refusal and pre-effect cancellation are definite", refused_before_fork_has_no_ambiguous_effect},
    {"links writable modes special files and byte overflow never become partial snapshots", unsafe_or_partial_trees_are_rejected},
    {"entry bounds and root generations are enforced", observations_are_bounded_and_root_specific},
    {"root directory mode changes are retained independently of file changes", root_metadata_changes_are_retained},
    {"same-content inode replacement is recorded as a change", replacement_with_same_bytes_is_known_change}}); }
