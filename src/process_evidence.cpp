#include "wls/process_evidence.hpp"
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;
constexpr std::size_t max_entries = 256, max_snapshot_bytes = 49152;
constexpr std::uint64_t max_bytes = 16U * 1024U * 1024U;
void check(bool b, ErrorCode code, const char* message) { if (!b) throw Error(code, message); }
[[noreturn]] void failed(const char* message) {
    throw Error(ErrorCode::Storage, std::string(message) + ": " + std::strerror(errno));
}
struct Fd {
    int value;
    explicit Fd(int v) : value(v) { if (v < 0) failed("workspace observation open"); }
    ~Fd() { if (value >= 0) ::close(value); }
    Fd(const Fd&) = delete; Fd& operator=(const Fd&) = delete;
};
struct statx inspect(int fd) {
    struct statx s{};
    constexpr unsigned mask = STATX_BASIC_STATS | STATX_BTIME | STATX_MNT_ID;
    if (::syscall(SYS_statx, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT, mask, &s) != 0)
        failed("workspace observation statx");
    check((s.stx_mask & mask) == mask, ErrorCode::Unsupported, "workspace observation requires complete statx generation");
    return s;
}
Json generation(const struct statx& s) {
    return Object{{"dev_major", s.stx_dev_major}, {"dev_minor", s.stx_dev_minor}, {"inode", s.stx_ino},
        {"mount_id", s.stx_mnt_id}, {"btime_s", s.stx_btime.tv_sec}, {"btime_ns", s.stx_btime.tv_nsec}};
}
Json metadata(const struct statx& s) {
    return Object{{"generation", generation(s)}, {"mode", s.stx_mode}, {"uid", s.stx_uid}, {"gid", s.stx_gid},
        {"nlink", s.stx_nlink}, {"size", s.stx_size}, {"mtime_s", s.stx_mtime.tv_sec}, {"mtime_ns", s.stx_mtime.tv_nsec},
        {"ctime_s", s.stx_ctime.tv_sec}, {"ctime_ns", s.stx_ctime.tv_nsec}};
}
int child_fd(int parent, const std::string& name) {
    open_how how{};
    how.flags = O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    return static_cast<int>(::syscall(SYS_openat2, parent, name.c_str(), &how, sizeof(how)));
}
Json scan(int root) {
    const auto identity = inspect(root);
    check(S_ISDIR(identity.stx_mode), ErrorCode::Unauthorized, "observation root must be a directory");
    const auto safe = [&](const struct statx& s) {
        check(s.stx_uid == ::geteuid() && (s.stx_mode & 0022U) == 0 && s.stx_nlink > 0 &&
            s.stx_mnt_id == identity.stx_mnt_id && s.stx_dev_major == identity.stx_dev_major && s.stx_dev_minor == identity.stx_dev_minor,
            ErrorCode::Unauthorized, "unsafe workspace observation owner, mode or mount");
        check(S_ISDIR(s.stx_mode) || (S_ISREG(s.stx_mode) && s.stx_nlink == 1), ErrorCode::Unauthorized,
            "workspace observation accepts only private directories and single-link files");
    };
    safe(identity);
    Array entries;
    std::uint64_t bytes = 0;
    const auto walk = [&](const auto& self, int directory, const std::string& prefix, std::size_t depth) -> void {
        check(depth <= 32, ErrorCode::Quota, "workspace observation depth exceeded");
        const auto original = inspect(directory); safe(original);
        Fd readable(::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        DIR* ptr = ::fdopendir(readable.value);
        if (!ptr) failed("workspace observation enumerate");
        readable.value = -1;
        const auto close_dir = [](DIR* p) { ::closedir(p); };
        std::unique_ptr<DIR, decltype(close_dir)> stream(ptr, close_dir);
        std::vector<std::string> names;
        for (;;) {
            errno = 0; const auto* item = ::readdir(stream.get());
            if (!item) { if (errno) failed("workspace observation readdir"); break; }
            const std::string name(item->d_name);
            if (name == "." || name == "..") continue;
            names.push_back(name);
            check(names.size() + entries.size() <= max_entries, ErrorCode::Quota, "workspace observation entry cap exceeded");
        }
        std::sort(names.begin(), names.end());
        for (const auto& name : names) {
            const auto path = prefix.empty() ? name : prefix + "/" + name;
            check(path.size() <= 4096 && entries.size() < max_entries, ErrorCode::Quota, "workspace observation path/entry cap exceeded");
            Fd child(child_fd(directory, name));
            const auto st = inspect(child.value); safe(st);
            Object item{{"path", path}, {"type", S_ISDIR(st.stx_mode) ? "directory" : "file"}, {"metadata", metadata(st)}};
            if (S_ISREG(st.stx_mode)) {
                check(st.stx_size <= max_bytes - bytes, ErrorCode::Quota, "workspace observation byte cap exceeded");
                std::string content; content.reserve(static_cast<std::size_t>(st.stx_size));
                std::array<char, 8192> buffer{};
                for (;;) {
                    const auto n = ::read(child.value, buffer.data(), buffer.size());
                    if (n < 0) { if (errno == EINTR) continue; failed("workspace observation read"); }
                    if (!n) break;
                    check(static_cast<std::size_t>(n) <= max_bytes - bytes - content.size(), ErrorCode::Quota,
                        "workspace observation changed beyond byte cap");
                    content.append(buffer.data(), static_cast<std::size_t>(n));
                }
                check(content.size() == st.stx_size && metadata(st) == metadata(inspect(child.value)), ErrorCode::Conflict,
                    "workspace file changed during observation");
                bytes += content.size(); item["sha256"] = sha256_hex(content);
            }
            entries.emplace_back(std::move(item));
            check(canonical_json(entries).size() <= max_snapshot_bytes, ErrorCode::Quota, "workspace observation encoded cap exceeded");
            if (S_ISDIR(st.stx_mode)) self(self, child.value, path, depth + 1);
            const auto after = inspect(child.value); safe(after);
            check(metadata(st) == metadata(after), ErrorCode::Conflict, "workspace entry changed during observation");
        }
        check(metadata(original) == metadata(inspect(directory)), ErrorCode::Conflict, "workspace directory changed during observation");
    };
    walk(walk, root, "", 0);
    Json result = Object{{"format", "wls-workspace-observation/1"}, {"root", generation(identity)},
        {"root_metadata", metadata(identity)}, {"complete", true}, {"total_bytes", bytes}, {"entries", entries}};
    check(canonical_json(result).size() <= max_snapshot_bytes, ErrorCode::Quota, "workspace observation encoded cap exceeded");
    return result;
}
bool flag(const Json& j, std::string_view key) {
    const auto* p = j.is_object() ? j.as_object().if_contains(key) : nullptr;
    return p && p->is_bool() && p->as_bool();
}
bool equals(const Json& j, std::string_view key, std::string_view value) {
    const auto* p = j.is_object() ? j.as_object().if_contains(key) : nullptr;
    return p && p->is_string() && p->as_string() == value;
}
}
Json observe_process_workspace(int root_fd) {
    // The exclusive cooperative lock spans both scans and the actual command
    // in ProcessRunner. Same-UID malicious host code remains outside the model.
    auto before = scan(root_fd);
    check(before == scan(root_fd), ErrorCode::Conflict, "workspace changed between complete observation passes");
    return before;
}
Json process_workspace_changes(const Json& before, const Json& after) {
    check(flag(before, "complete") && flag(after, "complete") && before.at("root") == after.at("root"),
        ErrorCode::Conflict, "incomplete observations or different workspace generations");
    std::map<std::string, Json> old_entries, new_entries;
    const auto collect = [](const Json& value, auto& target) {
        check(equals(value, "format", "wls-workspace-observation/1") && value.at("entries").is_array() &&
            value.at("entries").as_array().size() <= max_entries && canonical_json(value).size() <= max_snapshot_bytes,
            ErrorCode::InvalidInput, "invalid bounded workspace observation");
        for (const auto& entry : value.at("entries").as_array()) {
            const auto path = std::string(entry.at("path").as_string());
            check(target.emplace(path, entry).second, ErrorCode::InvalidInput, "duplicate observation path");
        }
    };
    collect(before, old_entries); collect(after, new_entries);
    Array changes;
    for (const auto& [path, old] : old_entries) {
        const auto found = new_entries.find(path);
        if (found == new_entries.end()) changes.emplace_back(Object{{"path", path}, {"kind", "removed"}});
        else if (found->second != old) changes.emplace_back(Object{{"path", path}, {"kind", "changed"}});
    }
    for (const auto& [path, entry] : new_entries) {
        (void)entry;
        if (!old_entries.contains(path)) changes.emplace_back(Object{{"path", path}, {"kind", "added"}});
    }
    return Object{{"complete", true}, {"before", before}, {"after", after},
        {"before_sha256", sha256_hex(canonical_json(before))}, {"after_sha256", sha256_hex(canonical_json(after))},
        {"changes", changes}, {"root_metadata_changed", before.at("root_metadata") != after.at("root_metadata")}, {"effects_undone", false}};
}
std::string process_operation_outcome(const ProcessResult& p) {
    if (!p.spawned && p.status != ProcessStatus::CleanupUnknown)
        return p.status == ProcessStatus::Cancelled ? "CANCELLED" : "FAILED";
    const auto& e = p.evidence;
    if (!p.strong_isolation || !p.all_descendants_exited || !p.launcher_exec_confirmed ||
        !equals(e, "network", "none") || !equals(e, "supervision", "cgroup-v2") ||
        !equals(e, "descendants_proof", "cgroup.events populated=0") ||
        !flag(e, "workspace_mount_verified") || !flag(e, "namespaces_verified") || !flag(e, "command_start_released")) return "UNKNOWN";
    try {
        const auto& observation = e.at("workspace_observation");
        if (observation != process_workspace_changes(observation.at("before"), observation.at("after"))) return "UNKNOWN";
    } catch (const std::exception&) { return "UNKNOWN"; }
    switch (p.status) {
        case ProcessStatus::Exited: return !p.exit_code ? "UNKNOWN" : *p.exit_code == 0 ? "SUCCEEDED" : "FAILED";
        case ProcessStatus::Signalled: return p.signal ? "FAILED" : "UNKNOWN";
        case ProcessStatus::TimedOut: return "TIMED_OUT";
        case ProcessStatus::Cancelled: return "CANCELLED";
        case ProcessStatus::OutputLimit: return "FAILED";
        default: return "UNKNOWN";
    }
}
}
