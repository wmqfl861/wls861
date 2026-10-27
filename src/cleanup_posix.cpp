#include "wls/cleanup.hpp"
#include "wls/workspace.hpp"
#include "wls/owned_capacity.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <linux/openat2.h>
#include <linux/fs.h>
#include <map>
#include <set>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace wls::s1 {
namespace {
using O = boost::json::object;
using A = boost::json::array;
void need(bool value, ErrorCode code, const char* message) {
    if (!value) throw Error(code, message);
}
[[noreturn]] void os_error(const char* operation) {
    const int value = errno;
    const auto code = value == ELOOP || value == EXDEV || value == ENOTDIR ||
        value == EACCES || value == EPERM ? ErrorCode::Unauthorized :
        value == EEXIST || value == EAGAIN || value == EBUSY ? ErrorCode::Conflict :
        value == ENOSYS || value == EINVAL ? ErrorCode::Unsupported : ErrorCode::Storage;
    throw Error(code, std::string(operation) + ": " + std::strerror(value));
}
class Fd {
public:
    explicit Fd(int value = -1) : value_(value) {}
    ~Fd() { if (value_ >= 0) (void)::close(value_); }
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) (void)::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    int get() const { return value_; }
    int release() { return std::exchange(value_, -1); }
private:
    int value_;
};
std::string text(const Json& value, std::string_view key) {
    need(value.is_object(), ErrorCode::InvalidInput, "expected cleanup object");
    const auto* item = value.as_object().if_contains(key);
    need(item && item->is_string(), ErrorCode::InvalidInput, "expected cleanup string");
    return std::string(item->as_string());
}
void name_ok(std::string_view name) {
    need(!name.empty() && name.size() <= 255 && name != "." && name != ".." &&
         name.find_first_of("/\\") == std::string_view::npos &&
         std::all_of(name.begin(), name.end(), [](unsigned char c) { return c >= 32U && c != 127U; }),
         ErrorCode::InvalidInput, "unsafe cleanup basename");
    (void)parse_json_strict(canonical_json(Json(name)));
}
void entry_name_ok(std::string_view name) {
    // Worker file tools accept bounded relative UTF-8 POSIX names. Backslash
    // and control bytes are literal filename bytes, escaped losslessly in the
    // inventory JSON; they are never passed to a shell. Keep the stricter
    // runtime-owned resource/staging identifier rule in name_ok separately.
    need(!name.empty() && name.size() <= 255 && name != "." && name != ".." &&
         name.find('/') == std::string_view::npos && name.find('\0') == std::string_view::npos,
         ErrorCode::InvalidInput, "unsafe cleanup entry basename");
    (void)parse_json_strict(canonical_json(Json(name)));
}
void path_ok(std::string_view path) {
    need(!path.empty() && path.size() <= 4096, ErrorCode::InvalidInput, "invalid cleanup path");
    std::size_t start = 0;
    while (true) {
        const auto end = path.find('/', start);
        entry_name_ok(path.substr(start, end == std::string_view::npos ? path.size() - start : end - start));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
}
Fd open_at(int parent, std::string_view path, int flags, mode_t mode = 0, bool missing = false) {
    const std::string name(path);
    open_how how{};
    how.flags = static_cast<std::uint64_t>(flags | O_NOFOLLOW | O_CLOEXEC);
    how.mode = mode;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    int value;
    do { value = static_cast<int>(::syscall(SYS_openat2, parent, name.c_str(), &how, sizeof(how))); }
    while (value < 0 && errno == EINTR);
    if (value < 0 && !(missing && errno == ENOENT)) os_error("open owned workspace entry");
    return Fd(value);
}
void sync_fd(int fd) {
    int value;
    do { value = ::fsync(fd); } while (value < 0 && errno == EINTR);
    if (value < 0) os_error("sync owned workspace");
}
void lock_fd(int fd) {
    int value;
    do { value = ::flock(fd, LOCK_EX | LOCK_NB); } while (value < 0 && errno == EINTR);
    if (value < 0) os_error("lock owned workspace");
}
struct statx info(int fd) {
    struct statx result{};
    if (::statx(fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
                STATX_BASIC_STATS | STATX_BTIME | STATX_MNT_ID, &result) != 0)
        os_error("statx owned workspace");
    need((result.stx_mask & (STATX_BTIME | STATX_MNT_ID)) == (STATX_BTIME | STATX_MNT_ID),
         ErrorCode::Unsupported, "cleanup requires birth time and mount identity");
    need(result.stx_uid == ::geteuid() && (result.stx_mode & 0022U) == 0,
         ErrorCode::Unauthorized, "cleanup entry must be private and owned by this uid");
    need(S_ISDIR(result.stx_mode) || (S_ISREG(result.stx_mode) && result.stx_nlink == 1),
         ErrorCode::Unauthorized, "cleanup refuses links and special files");
    return result;
}
Json identity(int fd, const std::string& path) {
    struct stat basic{};
    if (::fstat(fd, &basic) != 0) os_error("stat owned workspace");
    const auto s = info(fd);
    return O{{"path", path}, {"device", static_cast<std::uint64_t>(basic.st_dev)},
             {"inode", s.stx_ino}, {"mount_id", s.stx_mnt_id},
             {"birth_seconds", s.stx_btime.tv_sec}, {"birth_nanoseconds", s.stx_btime.tv_nsec}};
}
bool same_generation(const Json& expected, int fd) {
    return expected == identity(fd, text(expected, "path"));
}
// Inspect special-file type through O_PATH before requesting read access.
// There is no device/FIFO open merely to discover that it is unsupported.
Fd open_data_at(int parent, const std::string& path, bool regular_only = false) {
    auto pinned = open_at(parent, path, O_PATH);
    const auto observed = identity(pinned.get(), path);
    if (regular_only) need(S_ISREG(info(pinned.get()).stx_mode), ErrorCode::Unauthorized,
                           "ownership receipt must be a regular file");
    auto readable = open_at(parent, path, O_RDONLY | O_NONBLOCK);
    need(same_generation(observed, readable.get()), ErrorCode::Conflict, "entry changed before reading");
    return readable;
}
std::vector<std::string> names(int fd) {
    auto scan = open_at(fd, ".", O_RDONLY | O_DIRECTORY);
    DIR* directory = ::fdopendir(scan.release());
    if (!directory) os_error("enumerate owned workspace");
    std::vector<std::string> result;
    try {
        errno = 0;
        while (const auto* entry = ::readdir(directory)) {
            const std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            entry_name_ok(name); result.push_back(name);
            need(result.size() <= 4098, ErrorCode::Quota, "cleanup directory exceeds entry bound");
            errno = 0;
        }
        if (errno != 0) os_error("read owned workspace directory");
    } catch (...) { (void)::closedir(directory); throw; }
    (void)::closedir(directory);
    std::sort(result.begin(), result.end()); return result;
}
std::string read_bytes(int fd, std::uint64_t maximum) {
    const auto before = info(fd);
    need(S_ISREG(before.stx_mode) && before.stx_size <= maximum,
         ErrorCode::Quota, "cleanup file is not a bounded regular file");
    std::string bytes(static_cast<std::size_t>(before.stx_size), '\0');
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto got = ::pread(fd, bytes.data() + offset, bytes.size() - offset, static_cast<off_t>(offset));
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) os_error("read cleanup file");
        need(got > 0, ErrorCode::Conflict, "cleanup file changed while reading");
        offset += static_cast<std::size_t>(got);
    }
    const auto after = info(fd);
    need(before.stx_size == after.stx_size && before.stx_ino == after.stx_ino &&
         before.stx_mtime.tv_sec == after.stx_mtime.tv_sec && before.stx_mtime.tv_nsec == after.stx_mtime.tv_nsec &&
         before.stx_ctime.tv_sec == after.stx_ctime.tv_sec && before.stx_ctime.tv_nsec == after.stx_ctime.tv_nsec,
         ErrorCode::Conflict, "cleanup file changed during observation");
    return bytes;
}
void write_all(int fd, std::string_view bytes) {
    while (!bytes.empty()) {
        const auto count = ::write(fd, bytes.data(), bytes.size());
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) os_error("write ownership receipt");
        bytes.remove_prefix(static_cast<std::size_t>(count));
    }
    sync_fd(fd);
}
const Json& intent_of(const Json& resource) {
    need(resource.is_object() && resource.as_object().contains("intent"), ErrorCode::InvalidInput,
         "resource lacks durable ownership intent");
    return resource.at("intent");
}
bool staged_intent(const Json& intent) {
    const auto* version = intent.as_object().if_contains("version");
    return version && version->is_string() && version->as_string() == "owned-allocation-v2";
}
void validate_intent(const Json& intent) {
    (void)owned_workspace_intent_capacity(intent);
    const auto leaf = text(intent, "resource_id"); name_ok(leaf);
    need(leaf.starts_with("res_") && leaf.size() <= 64, ErrorCode::InvalidInput, "invalid owned resource id");
    need(text(intent, "leaf") == leaf, ErrorCode::InvalidInput, "owned leaf must be the allocated resource id");
    const auto& marker = intent.at("marker");
    need(marker.at("resource_id") == intent.at("resource_id") &&
         marker.at("request_id") == intent.at("request_id") && marker.at("team_id") == intent.at("team_id") &&
         text(marker, "nonce").size() >= 16 && canonical_json(marker).size() <= 16384,
         ErrorCode::InvalidInput, "ownership receipt does not bind this resource");
    if (intent.as_object().contains("version")) {
        need(staged_intent(intent), ErrorCode::InvalidInput, "unsupported owned allocation intent version");
        const auto stage = text(intent, "stage_leaf"); name_ok(stage);
        need(stage.starts_with("stage_") && stage.size() <= 64 && stage != leaf,
             ErrorCode::InvalidInput, "invalid owned allocation staging name");
        for (const auto* field : {"mount_id", "birth_seconds", "birth_nanoseconds"})
            need(intent.at("parent_identity").as_object().contains(field), ErrorCode::InvalidInput,
                 "staged allocation requires the original parent generation");
    }
}
class Handle {
public:
    explicit Handle(const Json& intent) : parent(text(intent.at("parent_identity"), "path"), WorkspaceAccess::ReadOnly),
            parent_fd(parent.dup_root_fd()), leaf(text(intent, "leaf")), marker_bytes(canonical_json(intent.at("marker"))),
            expected_parent(intent.at("parent_identity")) {
        validate_intent(intent);
        recheck_parent();
        lock_fd(parent_fd.get());
    }
    void recheck_parent() const {
        const auto current = parent.root_identity();
        for (const auto* field : {"path", "device", "inode"})
            need(current.at(field) == expected_parent.at(field), ErrorCode::Conflict, "resource parent identity changed");
        if (expected_parent.as_object().contains("mount_id"))
            need(expected_parent == identity(parent_fd.get(), text(expected_parent, "path")),
                 ErrorCode::Conflict, "resource parent generation changed");
    }
    void open_existing(const Json& saved, bool missing_work = false) {
        need(saved.at("parent_identity") == expected_parent, ErrorCode::Conflict, "ownership parent binding changed");
        envelope = open_at(parent_fd.get(), leaf, O_RDONLY | O_DIRECTORY);
        need(same_generation(saved.at("envelope_identity"), envelope.get()), ErrorCode::Conflict, "ownership envelope replaced");
        auto marker = open_data_at(envelope.get(), "owner.json", true);
        need(read_bytes(marker.get(), 16384) == marker_bytes, ErrorCode::Conflict, "ownership receipt changed");
        const auto entries = names(envelope.get());
        need(entries == std::vector<std::string>{"owner.json", "work"} ||
             (missing_work && entries == std::vector<std::string>{"owner.json"}),
             ErrorCode::Conflict, "unexpected ownership envelope contents");
        work = open_at(envelope.get(), "work", O_RDONLY | O_DIRECTORY, 0, missing_work);
        if (work.get() >= 0) {
            need(same_generation(saved.at("workspace_identity"), work.get()), ErrorCode::Conflict, "owned workspace replaced");
            lock_fd(work.get());
        }
        saved_identity = saved;
    }
    void recheck() const {
        recheck_parent();
        auto named = open_at(parent_fd.get(), leaf, O_RDONLY | O_DIRECTORY);
        need(same_generation(saved_identity.at("envelope_identity"), named.get()), ErrorCode::Conflict, "owned envelope moved");
        if (work.get() >= 0) {
            auto current = open_at(envelope.get(), "work", O_RDONLY | O_DIRECTORY);
            need(same_generation(saved_identity.at("workspace_identity"), current.get()), ErrorCode::Conflict, "owned workspace moved");
        }
    }
    Workspace parent;
    Fd parent_fd, envelope, work;
    std::string leaf, marker_bytes;
    Json expected_parent, saved_identity;
};
bool exists_at(int parent, const std::string& name) {
    return open_at(parent, name, O_PATH, 0, true).get() >= 0;
}
Json observed_allocation(Handle& handle) {
    const auto path = text(handle.expected_parent, "path") + "/" + handle.leaf;
    return O{{"parent_identity", handle.expected_parent},
             {"envelope_identity", identity(handle.envelope.get(), path)},
             {"workspace_identity", identity(handle.work.get(), path + "/work")},
             {"marker_sha256", sha256_hex(handle.marker_bytes)}};
}
Json allocation_identity(const Json& receipt, const Json& intent, const std::string& leaf) {
    const auto path = text(intent.at("parent_identity"), "path") + "/" + leaf;
    auto envelope = receipt.at("stage_identity"); envelope.as_object()["path"] = path;
    auto work = receipt.at("workspace_identity"); work.as_object()["path"] = path + "/work";
    return O{{"parent_identity", receipt.at("parent_identity")}, {"envelope_identity", envelope},
             {"workspace_identity", work}, {"marker_sha256", receipt.at("marker_sha256")}};
}
void validate_allocation_receipt(const Json& receipt, const Json& intent) {
    need(receipt.is_object() && text(receipt, "version") == "owned-allocation-receipt-v1" &&
         receipt.at("resource_id") == intent.at("resource_id") &&
         text(receipt, "intent_sha256") == sha256_hex(canonical_json(intent)) &&
         receipt.at("parent_identity") == intent.at("parent_identity") &&
         text(receipt, "marker_sha256") == sha256_hex(canonical_json(intent.at("marker"))) &&
         receipt.at("durable") == Json(true), ErrorCode::Conflict, "allocation receipt differs from original intent");
    const auto path = text(intent.at("parent_identity"), "path") + "/" + text(intent, "stage_leaf");
    need(text(receipt.at("stage_identity"), "path") == path &&
         text(receipt.at("workspace_identity"), "path") == path + "/work",
         ErrorCode::Conflict, "allocation receipt staging paths changed");
}
void check_empty_allocation(Handle& handle) {
    auto marker = open_data_at(handle.envelope.get(), "owner.json", true);
    need(read_bytes(marker.get(), 16384) == handle.marker_bytes, ErrorCode::Conflict, "ownership receipt changed");
    need(names(handle.envelope.get()) == std::vector<std::string>{"owner.json", "work"} &&
         names(handle.work.get()).empty(), ErrorCode::Conflict, "allocation must contain only its receipt and empty work directory");
    handle.recheck();
}
void sync_allocation(Handle& handle) {
    auto marker = open_data_at(handle.envelope.get(), "owner.json", true);
    need(read_bytes(marker.get(), 16384) == handle.marker_bytes, ErrorCode::Conflict, "ownership receipt changed before sync");
    sync_fd(marker.get()); sync_fd(handle.work.get()); sync_fd(handle.envelope.get()); sync_fd(handle.parent_fd.get());
}
Json entry(int root, const std::string& path, CleanupLimits limits, std::uint64_t& bytes) {
    path_ok(path);
    auto fd = open_data_at(root, path);
    const auto stat = info(fd.get());
    O record{{"identity", identity(fd.get(), path)}, {"type", S_ISDIR(stat.stx_mode) ? "directory" : "file"},
             {"mode", stat.stx_mode}};
    if (S_ISREG(stat.stx_mode)) {
        need(stat.stx_size <= limits.max_total_bytes - bytes, ErrorCode::Quota, "cleanup tree exceeds byte capacity");
        const auto content = read_bytes(fd.get(), limits.max_file_bytes);
        need(content.size() <= limits.max_total_bytes - bytes, ErrorCode::Quota, "cleanup tree grew beyond byte capacity");
        bytes += static_cast<std::uint64_t>(content.size());
        record["size_bytes"] = content.size(); record["sha256"] = sha256_hex(content);
    }
    return record;
}
CleanupLimits capacity_limits(const Json& resource, CleanupLimits supplied) {
    need(supplied.max_entries > 0 && supplied.max_entries <= 4096 && supplied.max_depth > 0 && supplied.max_depth <= 32 &&
         supplied.max_file_bytes <= 16U * 1024U * 1024U && supplied.max_total_bytes <= 256U * 1024U * 1024U,
         ErrorCode::InvalidInput, "invalid cleanup limits");
    const auto frozen = owned_cleanup_limits(owned_workspace_intent_capacity(intent_of(resource)));
    supplied.max_entries = std::min(supplied.max_entries, frozen.max_entries);
    supplied.max_depth = std::min(supplied.max_depth, frozen.max_depth);
    supplied.max_file_bytes = std::min(supplied.max_file_bytes, frozen.max_file_bytes);
    supplied.max_total_bytes = std::min(supplied.max_total_bytes, frozen.max_total_bytes);
    return supplied;
}
A scan_tree(int root, CleanupLimits limits) {
    need(limits.max_entries > 0 && limits.max_entries <= 4096 && limits.max_depth > 0 && limits.max_depth <= 32 &&
         limits.max_file_bytes <= 16U * 1024U * 1024U && limits.max_total_bytes <= 256U * 1024U * 1024U,
         ErrorCode::InvalidInput, "invalid cleanup limits");
    std::vector<std::pair<std::string, std::size_t>> pending{{".", 0}};
    A result; std::uint64_t bytes = 0; std::size_t encoded_bytes = 0;
    while (!pending.empty()) {
        const auto [parent, depth] = pending.back(); pending.pop_back();
        auto directory = open_at(root, parent, O_RDONLY | O_DIRECTORY);
        for (const auto& name : names(directory.get())) {
            need(result.size() < limits.max_entries && depth < limits.max_depth, ErrorCode::Quota, "cleanup tree capacity exceeded");
            const auto path = parent == "." ? name : parent + "/" + name;
            auto record = entry(root, path, limits, bytes);
            const auto record_bytes = canonical_json(record).size() + 1U;
            need(record_bytes <= 8U * 1024U * 1024U - encoded_bytes, ErrorCode::Quota,
                 "cleanup inventory encoded-byte capacity exceeded");
            encoded_bytes += record_bytes;
            if (text(record, "type") == "directory") pending.emplace_back(path, depth + 1);
            result.push_back(std::move(record));
        }
    }
    std::sort(result.begin(), result.end(), [](const Json& left, const Json& right) {
        return text(left.at("identity"), "path") < text(right.at("identity"), "path");
    });
    return result;
}
std::map<std::string, Json> frozen_entries(const Json& resource, const Json& inventory) {
    validate_owned_workspace_inventory_capacity(resource, inventory);
    need(inventory.is_object() && inventory.at("resource_id") == resource.at("resource_id") &&
         inventory.at("identity") == resource.at("identity") && inventory.at("entries").is_array() &&
         inventory.at("entries").as_array().size() <= 4096,
         ErrorCode::InvalidInput, "cleanup inventory binding mismatch");
    std::map<std::string, Json> result;
    for (const auto& record : inventory.at("entries").as_array()) {
        const auto path = text(record.at("identity"), "path"); path_ok(path);
        need(result.emplace(path, record).second, ErrorCode::InvalidInput, "duplicate cleanup inventory path");
    }
    return result;
}
Json removed_proof(const Json& resource, const Json& inventory) {
    return O{{"resource_id", resource.at("resource_id")}, {"identity", resource.at("identity")},
             {"inventory_sha256", sha256_hex(canonical_json(inventory))}, {"workspace_absent", true},
             {"envelope_retained", true}, {"durable", true}};
}
}

Json create_owned_workspace(const Json& intent) {
    need(!staged_intent(intent), ErrorCode::InvalidInput, "v2 allocation requires a persisted creation receipt before publication");
    Handle h(intent);
    bool created = false;
    if (::mkdirat(h.parent_fd.get(), h.leaf.c_str(), 0700) == 0) created = true;
    else if (errno != EEXIST) os_error("create ownership envelope");
    h.envelope = open_at(h.parent_fd.get(), h.leaf, O_RDONLY | O_DIRECTORY);
    const auto envelope_info = info(h.envelope.get());
    need(S_ISDIR(envelope_info.stx_mode), ErrorCode::Unauthorized, "ownership envelope is not a directory");
    if (created) {
        auto marker = open_at(h.envelope.get(), "owner.json", O_WRONLY | O_CREAT | O_EXCL, 0600);
        write_all(marker.get(), h.marker_bytes);
    } else {
        auto marker = open_data_at(h.envelope.get(), "owner.json", true);
        need(read_bytes(marker.get(), 16384) == h.marker_bytes, ErrorCode::Conflict,
             "existing directory is not owned by this creation intent");
    }
    const auto existing = names(h.envelope.get());
    need(existing == std::vector<std::string>{"owner.json"} || existing == std::vector<std::string>{"owner.json", "work"},
         ErrorCode::Conflict, "unexpected files in allocation envelope");
    if (::mkdirat(h.envelope.get(), "work", 0700) != 0 && errno != EEXIST) os_error("create owned work directory");
    h.work = open_at(h.envelope.get(), "work", O_RDONLY | O_DIRECTORY); lock_fd(h.work.get());
    need(names(h.work.get()).empty(), ErrorCode::Conflict, "creation retry cannot adopt a populated workspace");
    sync_fd(h.work.get()); sync_fd(h.envelope.get()); sync_fd(h.parent_fd.get());
    const auto parent_identity = h.parent.root_identity();
    const auto path = text(parent_identity, "path") + "/" + h.leaf;
    return O{{"parent_identity", parent_identity}, {"envelope_identity", identity(h.envelope.get(), path)},
             {"workspace_identity", identity(h.work.get(), path + "/work")},
             {"marker_sha256", sha256_hex(h.marker_bytes)}};
}
Json allocation_parent_identity(const std::filesystem::path& path) {
    Workspace parent(path, WorkspaceAccess::ReadOnly);
    Fd fd(parent.dup_root_fd());
    const auto basic = parent.root_identity();
    const auto result = identity(fd.get(), text(basic, "path"));
    need(parent.root_identity() == basic, ErrorCode::Conflict, "allocation parent moved during observation");
    return result;
}
Json stage_owned_workspace(const Json& intent) {
    Handle h(intent);
    need(staged_intent(intent), ErrorCode::InvalidInput, "staging requires a v2 owned allocation intent");
    const auto final_leaf = h.leaf; h.leaf = text(intent, "stage_leaf");
    need(!exists_at(h.parent_fd.get(), final_leaf) && !exists_at(h.parent_fd.get(), h.leaf),
         ErrorCode::Conflict, "unreceipted allocation path exists; preserve residuals and explicitly abandon the intent");
    h.recheck_parent();
    // A failed or interrupted attempt deliberately leaves the original intent
    // PREPARED. No path observed after this call starts can supply a substitute
    // receipt on retry; only this uninterrupted creation may return evidence.
    if (::mkdirat(h.parent_fd.get(), h.leaf.c_str(), 0700) != 0) os_error("create owned allocation stage");
    h.envelope = open_at(h.parent_fd.get(), h.leaf, O_RDONLY | O_DIRECTORY);
    (void)info(h.envelope.get());
    if (::fchmod(h.envelope.get(), 0700) != 0) os_error("set owned allocation stage mode");
    auto marker = open_at(h.envelope.get(), "owner.json", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (::fchmod(marker.get(), 0600) != 0) os_error("set ownership receipt mode");
    write_all(marker.get(), h.marker_bytes);
    if (::mkdirat(h.envelope.get(), "work", 0700) != 0) os_error("create staged work directory");
    h.work = open_at(h.envelope.get(), "work", O_RDONLY | O_DIRECTORY);
    (void)info(h.work.get()); lock_fd(h.work.get());
    if (::fchmod(h.work.get(), 0700) != 0) os_error("set staged workspace mode");
    h.saved_identity = observed_allocation(h);
    check_empty_allocation(h);
    sync_allocation(h);
    check_empty_allocation(h);
    need(!exists_at(h.parent_fd.get(), final_leaf), ErrorCode::Conflict, "allocation final path appeared during staging");
    return O{{"version", "owned-allocation-receipt-v1"}, {"resource_id", intent.at("resource_id")},
             {"intent_sha256", sha256_hex(canonical_json(intent))}, {"parent_identity", intent.at("parent_identity")},
             {"stage_identity", h.saved_identity.at("envelope_identity")},
             {"workspace_identity", h.saved_identity.at("workspace_identity")},
             {"marker_sha256", h.saved_identity.at("marker_sha256")}, {"durable", true}};
}
Json publish_owned_workspace(const Json& resource) {
    const auto& intent = intent_of(resource);
    Handle h(intent);
    need(staged_intent(intent) && text(resource, "status") == "PREPARED", ErrorCode::NotReady,
         "only the original PREPARED v2 allocation can be published");
    need(resource.at("resource_id") == intent.at("resource_id"), ErrorCode::Conflict,
         "allocation resource differs from its durable intent");
    const auto& receipt = resource.at("allocation_receipt");
    validate_allocation_receipt(receipt, intent);
    const auto final_leaf = h.leaf;
    const auto stage_leaf = text(intent, "stage_leaf");
    const bool stage_exists = exists_at(h.parent_fd.get(), stage_leaf);
    const bool final_exists = exists_at(h.parent_fd.get(), final_leaf);
    need(stage_exists != final_exists, ErrorCode::Conflict,
         "allocation receipt requires exactly one original stage or final path");
    h.leaf = stage_exists ? stage_leaf : final_leaf;
    h.open_existing(allocation_identity(receipt, intent, h.leaf));
    check_empty_allocation(h);
    // Re-sync the exact saved objects before and after publication. This also
    // completes a retry after rename/parent-sync succeeded but Store registration
    // failed; no new generation or new receipt is inferred from the final name.
    sync_allocation(h);
    check_empty_allocation(h);
    if (stage_exists) {
        need(!exists_at(h.parent_fd.get(), final_leaf), ErrorCode::Conflict, "allocation final path appeared before publication");
        if (::syscall(SYS_renameat2, h.parent_fd.get(), stage_leaf.c_str(), h.parent_fd.get(),
                      final_leaf.c_str(), RENAME_NOREPLACE) != 0)
            os_error("publish owned allocation without overwrite");
        h.leaf = final_leaf;
        h.saved_identity = allocation_identity(receipt, intent, final_leaf);
    }
    sync_allocation(h);
    check_empty_allocation(h);
    need(!exists_at(h.parent_fd.get(), stage_leaf), ErrorCode::Conflict, "allocation staging path exists after publication");
    return h.saved_identity;
}
Json inspect_owned_workspace(const Json& resource) {
    Handle h(intent_of(resource)); h.open_existing(resource.at("identity")); h.recheck();
    return resource.at("identity");
}
Json inventory_owned_workspace(const Json& resource, CleanupLimits limits) {
    limits = capacity_limits(resource, limits);
    Handle h(intent_of(resource)); h.open_existing(resource.at("identity"));
    const auto records = scan_tree(h.work.get(), limits); h.recheck();
    Json result = O{{"resource_id", resource.at("resource_id")}, {"identity", resource.at("identity")}, {"entries", records}};
    const auto policy = owned_workspace_intent_capacity(intent_of(resource));
    if (!policy.is_null()) result.as_object()["capacity_policy"] = policy;
    validate_owned_workspace_inventory_capacity(resource, result);
    return result;
}
Json erase_owned_workspace(const Json& resource, const Json& inventory, std::size_t max_unlinks, CleanupLimits limits) {
    need(max_unlinks > 0 && max_unlinks <= 4096, ErrorCode::InvalidInput, "cleanup pass limit must be 1..4096");
    const auto expected = frozen_entries(resource, inventory);
    limits = capacity_limits(resource, limits);
    Handle h(intent_of(resource)); h.open_existing(resource.at("identity"), true);
    if (h.work.get() < 0) {
        h.recheck(); sync_fd(h.envelope.get()); sync_fd(h.parent_fd.get());
        return O{{"complete", true}, {"removed_entries", 0}, {"proof", removed_proof(resource, inventory)}};
    }
    auto current = scan_tree(h.work.get(), limits);
    for (const auto& record : current) {
        const auto found = expected.find(text(record.at("identity"), "path"));
        need(found != expected.end() && found->second == record, ErrorCode::Conflict,
             "cleanup refuses new, replaced or changed inventory entries");
    }
    h.recheck();
    // Reverse lexical order places every child before its parent, including
    // names with UTF-8 bytes. Actual parents are reopened beneath the pinned fd.
    std::reverse(current.begin(), current.end());
    std::size_t removed = 0;
    for (const auto& record : current) {
        if (removed == max_unlinks) break;
        const auto path = text(record.at("identity"), "path");
        const auto split = path.rfind('/');
        const auto parent = split == std::string::npos ? "." : path.substr(0, split);
        const auto name = split == std::string::npos ? path : path.substr(split + 1);
        auto parent_fd = open_at(h.work.get(), parent, O_RDONLY | O_DIRECTORY);
        if (parent != ".") need(same_generation(expected.at(parent).at("identity"), parent_fd.get()),
                                ErrorCode::Conflict, "cleanup parent was replaced");
        std::uint64_t checked_bytes = 0;
        need(entry(h.work.get(), path, limits, checked_bytes) == record, ErrorCode::Conflict, "cleanup entry changed before unlink");
        const auto flags = text(record, "type") == "directory" ? AT_REMOVEDIR : 0;
        if (::unlinkat(parent_fd.get(), name.c_str(), flags) != 0) os_error("unlink owned inventory entry");
        sync_fd(parent_fd.get()); ++removed;
    }
    if (removed == current.size() && removed < max_unlinks) {
        h.recheck(); need(names(h.work.get()).empty(), ErrorCode::Conflict, "owned workspace is not empty");
        sync_fd(h.work.get());
        if (::unlinkat(h.envelope.get(), "work", AT_REMOVEDIR) != 0) os_error("remove owned workspace root");
        sync_fd(h.envelope.get()); sync_fd(h.parent_fd.get());
        return O{{"complete", true}, {"removed_entries", removed + 1}, {"proof", removed_proof(resource, inventory)}};
    }
    return O{{"complete", false}, {"removed_entries", removed}, {"proof", nullptr}};
}
Json verify_owned_workspace_removed(const Json& resource, const Json& inventory) {
    (void)frozen_entries(resource, inventory);
    Handle h(intent_of(resource)); h.open_existing(resource.at("identity"), true);
    need(h.work.get() < 0, ErrorCode::NotReady, "owned workspace still exists");
    h.recheck();
    sync_fd(h.envelope.get()); sync_fd(h.parent_fd.get());
    return removed_proof(resource, inventory);
}
} // namespace wls::s1
