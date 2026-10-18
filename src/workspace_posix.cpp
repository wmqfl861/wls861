#include "wls/workspace.hpp"

#include <linux/fs.h>
#include <linux/openat2.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <dirent.h>
#include <fcntl.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace wls::s1 {
namespace {
class Fd {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
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
    int get() const noexcept { return value_; }
    int release() noexcept { return std::exchange(value_, -1); }
private:
    int value_;
};

ErrorCode errno_code(int value) {
    if (value == ENOSYS || value == EINVAL || value == E2BIG) return ErrorCode::Unsupported;
    if (value == ELOOP || value == EXDEV || value == EACCES || value == EPERM || value == ENOTDIR)
        return ErrorCode::Unauthorized;
    if (value == EAGAIN || value == EEXIST || value == EBUSY) return ErrorCode::Conflict;
    if (value == ENOSPC || value == EDQUOT || value == EFBIG) return ErrorCode::Quota;
    if (value == ENOENT || value == ENAMETOOLONG) return ErrorCode::InvalidInput;
    return ErrorCode::Storage;
}
[[noreturn]] void fail_errno(std::string_view action, int value = errno) {
    throw Error(errno_code(value), std::string(action) + ": " + std::strerror(value));
}

bool utf8(std::string_view text) {
    std::size_t i = 0;
    while (i < text.size()) {
        const auto first = static_cast<unsigned char>(text[i++]);
        if (first < 0x80U) continue;
        unsigned int rest = 0;
        std::uint32_t value = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2U && first <= 0xdfU) { rest = 1; value = first & 0x1fU; minimum = 0x80U; }
        else if (first >= 0xe0U && first <= 0xefU) { rest = 2; value = first & 0x0fU; minimum = 0x800U; }
        else if (first >= 0xf0U && first <= 0xf4U) { rest = 3; value = first & 0x07U; minimum = 0x10000U; }
        else return false;
        if (rest > text.size() - i) return false;
        for (unsigned int j = 0; j < rest; ++j) {
            const auto next = static_cast<unsigned char>(text[i++]);
            if ((next & 0xc0U) != 0x80U) return false;
            value = (value << 6U) | (next & 0x3fU);
        }
        if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU)) return false;
    }
    return true;
}

std::string base64(std::string_view bytes) {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string result;
    result.reserve(((bytes.size() + 2U) / 3U) * 4U);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const auto first = static_cast<unsigned char>(bytes[i]);
        const auto second = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0U;
        const auto third = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0U;
        const auto value = (static_cast<unsigned int>(first) << 16U) | (second << 8U) | third;
        result.push_back(alphabet[(value >> 18U) & 63U]);
        result.push_back(alphabet[(value >> 12U) & 63U]);
        result.push_back(i + 1 < bytes.size() ? alphabet[(value >> 6U) & 63U] : '=');
        result.push_back(i + 2 < bytes.size() ? alphabet[value & 63U] : '=');
    }
    return result;
}

Json text_or_null(std::string_view bytes) { return utf8(bytes) ? Json(bytes) : Json(nullptr); }
std::string join_path(std::string_view parent, std::string_view name) {
    return parent == "." ? std::string(name) : std::string(parent) + "/" + std::string(name);
}

struct stat stat_fd(int fd) {
    struct stat value{};
    if (::fstat(fd, &value) != 0) fail_errno("inspect workspace descriptor");
    return value;
}
bool same_inode(const struct stat& left, const struct stat& right) {
    return left.st_dev == right.st_dev && left.st_ino == right.st_ino;
}
bool same_observation(const struct stat& left, const struct stat& right) {
    return same_inode(left, right) && left.st_size == right.st_size && left.st_mode == right.st_mode &&
           left.st_nlink == right.st_nlink && left.st_uid == right.st_uid &&
           left.st_mtim.tv_sec == right.st_mtim.tv_sec && left.st_mtim.tv_nsec == right.st_mtim.tv_nsec &&
           left.st_ctim.tv_sec == right.st_ctim.tv_sec && left.st_ctim.tv_nsec == right.st_ctim.tv_nsec;
}

Fd open_resolved(int root, std::string_view path, int flags, mode_t mode = 0, bool missing_ok = false) {
    const std::string name(path);
    open_how how{};
    how.flags = static_cast<std::uint64_t>(flags | O_CLOEXEC | O_NOFOLLOW);
    how.mode = mode;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    int result;
    do { result = static_cast<int>(::syscall(SYS_openat2, root, name.c_str(), &how, sizeof(how))); }
    while (result < 0 && errno == EINTR);
    if (result < 0 && !(missing_ok && errno == ENOENT)) fail_errno("openat2 workspace path");
    return Fd(result);
}

void sync_fd(int fd, std::string_view action) {
    int result;
    do { result = ::fsync(fd); } while (result < 0 && errno == EINTR);
    if (result != 0) fail_errno(action);
}

class DirectoryLock {
public:
    DirectoryLock(int root, bool exclusive) : fd_(open_resolved(root, ".", O_RDONLY | O_DIRECTORY)) {
        int result;
        do { result = ::flock(fd_.get(), exclusive ? LOCK_EX : LOCK_SH); } while (result < 0 && errno == EINTR);
        if (result != 0) fail_errno("lock workspace directory");
    }
private:
    Fd fd_;
};

Fd open_root(const std::filesystem::path& supplied) {
    const auto native = supplied.string();
    if (native.empty() || native.find('\0') != std::string::npos || !utf8(native))
        throw Error(ErrorCode::InvalidInput, "workspace root must be a UTF-8 directory path");
    std::error_code error;
    const auto absolute = std::filesystem::absolute(supplied, error);
    if (error) throw Error(ErrorCode::InvalidInput, "cannot resolve workspace root: " + error.message());
    Fd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.get() < 0) fail_errno("open host root directory");
    for (const auto& part : absolute.relative_path()) {
        const auto component = part.string();
        if (component.empty() || component == ".") continue;
        if (component == "..") throw Error(ErrorCode::Unauthorized, "workspace root must not contain '..'");
        Fd next(::openat(current.get(), component.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        if (next.get() < 0) fail_errno("open configured workspace root component");
        current = std::move(next);
    }
    return current;
}

std::string canonical_fd_path(int fd) {
    const auto link = "/proc/self/fd/" + std::to_string(fd);
    std::array<char, 4097> bytes{};
    const auto count = ::readlink(link.c_str(), bytes.data(), bytes.size());
    if (count < 0) fail_errno("inspect canonical workspace root");
    if (static_cast<std::size_t>(count) >= bytes.size()) throw Error(ErrorCode::Quota, "workspace root path exceeds limit");
    std::string result(bytes.data(), static_cast<std::size_t>(count));
    if (result.empty() || result.front() != '/' || result.ends_with(" (deleted)") || !utf8(result))
        throw Error(ErrorCode::Conflict, "workspace root is not a linked UTF-8 host directory");
    return result;
}

void valid_digest(std::string_view digest) {
    if (digest.size() != 64 || !std::all_of(digest.begin(), digest.end(), [](char value) {
        return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
    })) throw Error(ErrorCode::InvalidInput, "expected_sha256 must be 64 lowercase hexadecimal characters");
}

struct Snapshot {
    bool exists{false};
    struct stat metadata{};
    std::string bytes;
    std::string digest;
};
struct Entry { std::string path; std::string name; struct stat metadata{}; };
struct Inventory { std::vector<Entry> files; std::uint64_t bytes{0}; std::size_t entries{0}; };
struct PatchSpec { std::uint64_t offset; std::string_view expected; };
} // namespace

struct Workspace::Impl {
    Fd root;
    WorkspaceAccess access;
    WorkspaceLimits limits;
    struct stat identity{};
    std::string canonical_path;
    mutable std::mutex mutex;

    Impl(const std::filesystem::path& path, WorkspaceAccess selected, WorkspaceLimits bounds)
        : root(open_root(path)), access(selected), limits(bounds), identity(stat_fd(root.get())),
          canonical_path(canonical_fd_path(root.get())) {
        constexpr std::uint64_t mib = 1024U * 1024U;
        if ((access != WorkspaceAccess::ReadOnly && access != WorkspaceAccess::ReadWrite) ||
            limits.max_file_bytes == 0 || limits.max_file_bytes > 256U * mib || limits.max_total_bytes == 0 ||
            limits.max_total_bytes > 65536U * mib || limits.max_read_bytes == 0 || limits.max_read_bytes > 64U * mib ||
            limits.max_write_bytes == 0 || limits.max_write_bytes > 256U * mib || limits.max_entries == 0 ||
            limits.max_entries > 1000000 || limits.max_list_entries == 0 || limits.max_list_entries > 10000 ||
            limits.max_search_files == 0 || limits.max_search_files > 1000000 || limits.max_search_bytes == 0 ||
            limits.max_search_bytes > 65536U * mib || limits.max_search_matches == 0 || limits.max_search_matches > 100000 ||
            limits.max_path_bytes == 0 || limits.max_path_bytes > 4096 || limits.max_depth == 0 || limits.max_depth > 128)
            throw Error(ErrorCode::InvalidInput, "invalid workspace capacity limits");
        check_directory(identity);
        const auto probe = open_resolved(root.get(), ".", O_RDONLY | O_DIRECTORY);
        if (!same_inode(identity, stat_fd(probe.get()))) throw Error(ErrorCode::Conflict, "workspace root changed");
    }

    void check_directory(const struct stat& info) const {
        if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || info.st_dev != identity.st_dev ||
            (info.st_mode & (S_IWGRP | S_IWOTH)) != 0 || info.st_nlink == 0)
            throw Error(ErrorCode::Unauthorized, "workspace directory must be owned, linked and not writable by other users");
    }
    void check_file(const struct stat& info) const {
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_uid != ::geteuid() || info.st_dev != identity.st_dev ||
            (info.st_mode & (S_IWGRP | S_IWOTH)) != 0)
            throw Error(ErrorCode::Unauthorized, "workspace requires owned single-link ordinary files");
        if (info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) > limits.max_file_bytes)
            throw Error(ErrorCode::Quota, "workspace file exceeds capacity");
    }
    void check_root() const {
        const auto now = stat_fd(root.get());
        check_directory(now);
        if (!same_inode(identity, now) || canonical_fd_path(root.get()) != canonical_path)
            throw Error(ErrorCode::Conflict, "workspace identity or host location changed");
    }
    std::string path(std::string_view value, bool allow_root) const {
        if (allow_root && value == ".") return ".";
        if (value.empty() || value.size() > limits.max_path_bytes || value.front() == '/' ||
            value.find('\0') != std::string_view::npos || !utf8(value))
            throw Error(ErrorCode::InvalidInput, "workspace path must be bounded relative UTF-8");
        std::size_t depth = 0;
        std::size_t start = 0;
        while (start <= value.size()) {
            const auto end = value.find('/', start);
            const auto part = value.substr(start, end == std::string_view::npos ? value.size() - start : end - start);
            if (part.empty() || part == "." || part == "..")
                throw Error(ErrorCode::Unauthorized, "workspace path contains empty, '.' or '..' component");
            if (part.starts_with(".wls-tmp-"))
                throw Error(ErrorCode::Unauthorized, "workspace staging names are reserved");
            if (++depth > limits.max_depth || part.size() > 255)
                throw Error(ErrorCode::Quota, "workspace path depth or component exceeds capacity");
            if (end == std::string_view::npos) break;
            start = end + 1;
        }
        return std::string(value);
    }

    // Validate each ancestor as well as relying on kernel resolution. O_PATH
    // checks the final inode before any ordinary file/device open is attempted.
    Fd inspected(std::string_view name, bool missing_ok = false) const {
        std::size_t slash = name.find('/');
        while (slash != std::string_view::npos) {
            const auto ancestor = open_resolved(root.get(), name.substr(0, slash), O_PATH | O_DIRECTORY);
            check_directory(stat_fd(ancestor.get()));
            slash = name.find('/', slash + 1);
        }
        auto fd = open_resolved(root.get(), name, O_PATH, 0, missing_ok);
        if (fd.get() >= 0) {
            const auto info = stat_fd(fd.get());
            if (S_ISDIR(info.st_mode)) check_directory(info); else check_file(info);
        }
        return fd;
    }

    Snapshot snapshot(std::string_view name, bool missing_ok = false) const {
        auto observed = inspected(name, missing_ok);
        if (observed.get() < 0) return {};
        const auto before = stat_fd(observed.get());
        check_file(before);
        auto fd = open_resolved(root.get(), name, O_RDONLY | O_NONBLOCK | O_NOCTTY);
        const auto opened = stat_fd(fd.get());
        check_file(opened);
        if (!same_observation(before, opened)) throw Error(ErrorCode::Conflict, "workspace file changed while opening");
        Snapshot value;
        value.exists = true;
        value.metadata = opened;
        value.bytes.resize(static_cast<std::size_t>(opened.st_size));
        std::size_t position = 0;
        while (position < value.bytes.size()) {
            const auto count = ::pread(fd.get(), value.bytes.data() + position, value.bytes.size() - position,
                                       static_cast<off_t>(position));
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) fail_errno("read workspace file");
            if (count == 0) throw Error(ErrorCode::Conflict, "workspace file shrank while reading");
            position += static_cast<std::size_t>(count);
        }
        char extra{};
        ssize_t count;
        do { count = ::pread(fd.get(), &extra, 1, static_cast<off_t>(value.bytes.size())); } while (count < 0 && errno == EINTR);
        if (count < 0) fail_errno("check workspace file end");
        if (count != 0 || !same_observation(opened, stat_fd(fd.get())))
            throw Error(ErrorCode::Conflict, "workspace file changed while reading");
        const auto current = inspected(name);
        if (!same_observation(opened, stat_fd(current.get())))
            throw Error(ErrorCode::Conflict, "workspace file path changed while reading");
        value.digest = sha256_hex(value.bytes);
        return value;
    }

    std::vector<Entry> directory(std::string_view name) const {
        const auto observed = inspected(name);
        const auto before = stat_fd(observed.get());
        check_directory(before);
        auto opened = open_resolved(root.get(), name, O_RDONLY | O_DIRECTORY);
        if (!same_observation(before, stat_fd(opened.get()))) throw Error(ErrorCode::Conflict, "workspace directory changed");
        DIR* raw = ::fdopendir(opened.get());
        if (!raw) fail_errno("read workspace directory");
        (void)opened.release();
        const std::unique_ptr<DIR, int(*)(DIR*)> stream(raw, ::closedir);
        std::vector<Entry> entries;
        while (true) {
            errno = 0;
            const auto* item = ::readdir(stream.get());
            if (!item) {
                if (errno != 0) fail_errno("enumerate workspace directory");
                break;
            }
            const std::string child(item->d_name);
            if (child == "." || child == "..") continue;
            if (!utf8(child)) throw Error(ErrorCode::InvalidInput, "workspace filename is not UTF-8");
            if (entries.size() >= limits.max_entries) throw Error(ErrorCode::Quota, "workspace directory entry capacity exceeded");
            const auto logical = join_path(name, child);
            if (logical.size() > limits.max_path_bytes ||
                static_cast<std::size_t>(std::count(logical.begin(), logical.end(), '/')) + 1U > limits.max_depth)
                throw Error(ErrorCode::Quota, "workspace path capacity exceeded");
            const auto fd = inspected(logical);
            entries.push_back({logical, child, stat_fd(fd.get())});
        }
        if (!same_observation(before, stat_fd(::dirfd(stream.get()))))
            throw Error(ErrorCode::Conflict, "workspace directory changed while enumerating");
        std::sort(entries.begin(), entries.end(), [](const Entry& left, const Entry& right) { return left.name < right.name; });
        return entries;
    }

    void collect(std::string_view name, std::size_t depth, Inventory& inventory) const {
        if (depth > limits.max_depth) throw Error(ErrorCode::Quota, "workspace traversal depth exceeded");
        for (auto& entry : directory(name)) {
            if (++inventory.entries > limits.max_entries) throw Error(ErrorCode::Quota, "workspace entry capacity exceeded");
            if (S_ISDIR(entry.metadata.st_mode)) collect(entry.path, depth + 1, inventory);
            else {
                const auto size = static_cast<std::uint64_t>(entry.metadata.st_size);
                if (size > limits.max_total_bytes || inventory.bytes > limits.max_total_bytes - size)
                    throw Error(ErrorCode::Quota, "workspace total byte capacity exceeded");
                inventory.bytes += size;
                inventory.files.push_back(std::move(entry));
            }
        }
    }

    WorkspaceMutationResult mutate(std::string_view supplied_path, std::string_view supplied_bytes,
                                   const std::optional<std::string>& expected, std::optional<PatchSpec> patch_spec) {
        WorkspaceMutationResult output;
        std::unique_lock<std::mutex> thread_lock;
        std::unique_ptr<DirectoryLock> process_lock;
        Fd parent;
        Fd staged;
        struct stat staged_identity{};
        bool stage_exists = false;
        bool rename_attempted = false;
        bool renamed = false;
        bool durable = false;
        std::string phase = "validate";
        std::string logical;
        std::string parent_name;
        std::string leaf;
        std::string temporary;
        std::string bytes;
        Snapshot previous;
        boost::json::object evidence;
        try {
            if (access != WorkspaceAccess::ReadWrite) throw Error(ErrorCode::Unauthorized, "workspace is read-only");
            logical = path(supplied_path, false);
            if (expected) valid_digest(*expected);
            if (patch_spec && patch_spec->expected.empty())
                throw Error(ErrorCode::InvalidInput, "patch expected bytes must be nonempty");
            if (supplied_bytes.size() > limits.max_write_bytes || supplied_bytes.size() > limits.max_file_bytes)
                throw Error(ErrorCode::Quota, "workspace write exceeds capacity");
            const auto separator = logical.rfind('/');
            parent_name = separator == std::string::npos ? "." : logical.substr(0, separator);
            leaf = separator == std::string::npos ? logical : logical.substr(separator + 1);
            thread_lock = std::unique_lock<std::mutex>(mutex);
            process_lock = std::make_unique<DirectoryLock>(root.get(), true);
            check_root();
            const auto checked_parent = inspected(parent_name);
            const auto parent_identity = stat_fd(checked_parent.get());
            check_directory(parent_identity);
            parent = open_resolved(root.get(), parent_name, O_RDONLY | O_DIRECTORY);
            if (!same_inode(parent_identity, stat_fd(parent.get()))) throw Error(ErrorCode::Conflict, "workspace parent changed");
            previous = snapshot(logical, true);
            if ((!expected && previous.exists) || (expected && (!previous.exists || previous.digest != *expected)))
                throw Error(ErrorCode::Conflict, "workspace destination does not match creation/CAS precondition");
            if (patch_spec) {
                if (!previous.exists || patch_spec->offset > previous.bytes.size() ||
                    patch_spec->expected.size() > previous.bytes.size() - static_cast<std::size_t>(patch_spec->offset) ||
                    previous.bytes.compare(static_cast<std::size_t>(patch_spec->offset), patch_spec->expected.size(), patch_spec->expected) != 0)
                    throw Error(ErrorCode::Conflict, "patch byte range does not match expected bytes");
                const auto first_match = previous.bytes.find(patch_spec->expected);
                if (first_match != static_cast<std::size_t>(patch_spec->offset) ||
                    previous.bytes.find(patch_spec->expected, first_match + 1) != std::string::npos)
                    throw Error(ErrorCode::Conflict, "patch expected bytes must have exactly one match in the complete file");
                const auto remainder = previous.bytes.size() - patch_spec->expected.size();
                if (remainder > limits.max_write_bytes || supplied_bytes.size() > limits.max_write_bytes - remainder ||
                    remainder > limits.max_file_bytes || supplied_bytes.size() > limits.max_file_bytes - remainder)
                    throw Error(ErrorCode::Quota, "patched file exceeds capacity");
                bytes = previous.bytes;
                bytes.replace(static_cast<std::size_t>(patch_spec->offset), patch_spec->expected.size(), supplied_bytes);
            } else bytes.assign(supplied_bytes);

            Inventory inventory;
            collect(".", 1, inventory);
            const auto old_size = static_cast<std::uint64_t>(previous.bytes.size());
            if (inventory.bytes < old_size || bytes.size() > limits.max_total_bytes ||
                inventory.bytes - old_size > limits.max_total_bytes - bytes.size() ||
                (!previous.exists && inventory.entries >= limits.max_entries))
                throw Error(ErrorCode::Quota, "projected workspace capacity exceeded");
            evidence = {{"path", logical}, {"before_sha256", previous.exists ? Json(previous.digest) : Json(nullptr)},
                        {"after_sha256", sha256_hex(bytes)}, {"size_bytes", static_cast<std::uint64_t>(bytes.size())},
                        {"root_device", static_cast<std::uint64_t>(identity.st_dev)},
                        {"root_inode", static_cast<std::uint64_t>(identity.st_ino)}};

            phase = "stage";
            temporary = ".wls-tmp-" + new_id("tmp");
            evidence["temporary_path"] = join_path(parent_name, temporary);
            staged = open_resolved(parent.get(), temporary, O_WRONLY | O_CREAT | O_EXCL, 0600);
            stage_exists = true;
            staged_identity = stat_fd(staged.get());
            check_file(staged_identity);
            std::size_t written = 0;
            while (written < bytes.size()) {
                const auto count = ::write(staged.get(), bytes.data() + written, bytes.size() - written);
                if (count < 0 && errno == EINTR) continue;
                if (count < 0) fail_errno("write staged workspace file");
                if (count == 0) throw Error(ErrorCode::Storage, "staged workspace write made no progress");
                written += static_cast<std::size_t>(count);
            }
            const auto permissions = previous.exists ? static_cast<mode_t>(previous.metadata.st_mode & 0777U) : static_cast<mode_t>(0600);
            if (::fchmod(staged.get(), permissions) != 0) fail_errno("set staged workspace permissions");
            sync_fd(staged.get(), "sync staged workspace file");
            phase = "recheck";
            check_root();
            const auto live_parent = inspected(parent_name);
            if (!same_inode(parent_identity, stat_fd(live_parent.get())))
                throw Error(ErrorCode::Conflict, "workspace parent path changed before commit");
            const auto current = snapshot(logical, true);
            if (current.exists != previous.exists || (current.exists &&
                (!same_observation(current.metadata, previous.metadata) || current.digest != previous.digest)))
                throw Error(ErrorCode::Conflict, "workspace destination changed before commit");
            const auto stage_at_name = open_resolved(parent.get(), temporary, O_PATH);
            const auto stage_now = stat_fd(stage_at_name.get());
            check_file(stage_now);
            if (!same_inode(stage_now, staged_identity) || stage_now.st_size != static_cast<off_t>(bytes.size()))
                throw Error(ErrorCode::Conflict, "workspace staging file changed before commit");

            // Once the rename syscall is attempted every failure is classified
            // conservatively as UNKNOWN. Never restore/overwrite the target in
            // an error handler: SQLite settlement is a separate host operation.
            phase = "rename";
            rename_attempted = true;
            const auto rename_flags = previous.exists ? 0U : static_cast<unsigned int>(RENAME_NOREPLACE);
            if (::syscall(SYS_renameat2, parent.get(), temporary.c_str(), parent.get(), leaf.c_str(), rename_flags) != 0)
                fail_errno("publish workspace file");
            renamed = true;
            stage_exists = false;
            phase = "directory_sync";
            sync_fd(parent.get(), "sync workspace parent directory");
            durable = true;
            phase = "verify";
            const auto published = snapshot(logical);
            if (!same_inode(published.metadata, staged_identity) || published.digest != sha256_hex(bytes))
                throw Error(ErrorCode::Conflict, "workspace published file changed before result");
            evidence["renamed"] = true;
            evidence["durable"] = true;
            evidence["temporary_remaining"] = false;
            evidence["temporary_cleanup_complete"] = true;
            output.status = WorkspaceMutationStatus::Succeeded;
            output.evidence = std::move(evidence);
            output.result = boost::json::object{{"path", logical}, {"sha256", published.digest},
                {"size_bytes", static_cast<std::uint64_t>(bytes.size())}, {"created", !previous.exists},
                {"previous_sha256", previous.exists ? Json(previous.digest) : Json(nullptr)}};
            return output;
        } catch (const std::exception& error) {
            const auto* known = dynamic_cast<const Error*>(&error);
            const auto cause = known ? known->code() : ErrorCode::Storage;
            bool cleanup_complete = !stage_exists;
            // Before rename only this invocation's inode may be unlinked. If
            // cleanup cannot be proved, retain UNKNOWN and expose its name.
            if (stage_exists && !rename_attempted) {
                try {
                    const auto candidate = open_resolved(parent.get(), temporary, O_PATH, 0, true);
                    if (candidate.get() < 0) throw Error(ErrorCode::Conflict, "staging inode disappeared before cleanup");
                    if (!same_inode(stat_fd(candidate.get()), staged_identity))
                        throw Error(ErrorCode::Conflict, "staging name no longer belongs to operation");
                    if (::unlinkat(parent.get(), temporary.c_str(), 0) != 0) fail_errno("remove failed workspace staging file");
                    stage_exists = false;
                    sync_fd(parent.get(), "sync failed staging cleanup");
                    cleanup_complete = true;
                } catch (const std::exception& cleanup_error) {
                    evidence["cleanup_error"] = cleanup_error.what();
                }
            }
            const bool unknown = rename_attempted || !cleanup_complete;
            output.status = unknown ? WorkspaceMutationStatus::OutcomeUnknown : WorkspaceMutationStatus::EffectNotStarted;
            output.error_code = unknown ? ErrorCode::UnknownOutcome : cause;
            evidence["path"] = logical;
            evidence["phase"] = phase;
            evidence["error"] = error.what();
            evidence["cause_code"] = error_name(cause);
            evidence["rename_attempted"] = rename_attempted;
            evidence["renamed"] = renamed;
            evidence["durable"] = durable;
            evidence["temporary_remaining"] = stage_exists;
            evidence["temporary_cleanup_complete"] = cleanup_complete;
            output.evidence = std::move(evidence);
            output.result = nullptr;
            return output;
        }
    }
};

std::string_view workspace_mutation_status_name(WorkspaceMutationStatus status) noexcept {
    switch (status) {
    case WorkspaceMutationStatus::Succeeded: return "succeeded";
    case WorkspaceMutationStatus::EffectNotStarted: return "effect_not_started";
    case WorkspaceMutationStatus::OutcomeUnknown: return "outcome_unknown";
    }
    return "invalid_workspace_status";
}

Workspace::Workspace(const std::filesystem::path& root, WorkspaceAccess access, WorkspaceLimits limits)
    : impl_(std::make_unique<Impl>(root, access, limits)) {}
Workspace::~Workspace() = default;
Workspace::Workspace(Workspace&&) noexcept = default;
Workspace& Workspace::operator=(Workspace&&) noexcept = default;

Json Workspace::root_identity() const {
    std::lock_guard lock(impl_->mutex);
    impl_->check_root();
    return boost::json::object{{"path", impl_->canonical_path}, {"device", static_cast<std::uint64_t>(impl_->identity.st_dev)},
                              {"inode", static_cast<std::uint64_t>(impl_->identity.st_ino)}};
}
int Workspace::dup_root_fd() const {
    std::lock_guard lock(impl_->mutex);
    impl_->check_root();
    const int fd = ::fcntl(impl_->root.get(), F_DUPFD_CLOEXEC, 3);
    if (fd < 0) fail_errno("duplicate workspace root descriptor");
    return fd;
}

Json Workspace::read(std::string_view path, std::uint64_t offset, std::size_t max_bytes) const {
    const auto logical = impl_->path(path, false);
    if (max_bytes == 0 || max_bytes > impl_->limits.max_read_bytes) throw Error(ErrorCode::Quota, "read page exceeds capacity");
    std::lock_guard lock(impl_->mutex);
    const DirectoryLock directory_lock(impl_->root.get(), false);
    impl_->check_root();
    const auto data = impl_->snapshot(logical);
    if (offset > data.bytes.size()) throw Error(ErrorCode::InvalidInput, "read offset is past end of file");
    const auto begin = static_cast<std::size_t>(offset);
    const auto page = std::string_view(data.bytes).substr(begin, std::min(max_bytes, data.bytes.size() - begin));
    const auto next = offset + page.size();
    return boost::json::object{{"path", logical}, {"offset", offset}, {"bytes_returned", static_cast<std::uint64_t>(page.size())},
        {"size_bytes", static_cast<std::uint64_t>(data.bytes.size())}, {"sha256", data.digest},
        {"content", text_or_null(page)}, {"content_base64", base64(page)}, {"eof", next == data.bytes.size()},
        {"next_offset", next == data.bytes.size() ? Json(nullptr) : Json(next)}};
}

Json Workspace::list(std::string_view path, std::string_view after_name, std::size_t limit) const {
    const auto logical = impl_->path(path, true);
    if (limit == 0 || limit > impl_->limits.max_list_entries) throw Error(ErrorCode::Quota, "list page exceeds capacity");
    if (!after_name.empty() && (after_name.size() > 255 || after_name.find('/') != std::string_view::npos ||
        after_name.find('\0') != std::string_view::npos || after_name == "." || after_name == ".." || !utf8(after_name)))
        throw Error(ErrorCode::InvalidInput, "list cursor must be a filename");
    std::lock_guard lock(impl_->mutex);
    const DirectoryLock directory_lock(impl_->root.get(), false);
    impl_->check_root();
    const auto entries = impl_->directory(logical);
    boost::json::array output;
    std::string last;
    bool more = false;
    for (const auto& entry : entries) {
        if (entry.name <= after_name) continue;
        if (output.size() == limit) { more = true; break; }
        output.emplace_back(boost::json::object{{"name", entry.name}, {"type", S_ISDIR(entry.metadata.st_mode) ? "directory" : "file"},
            {"size_bytes", S_ISREG(entry.metadata.st_mode) ? Json(static_cast<std::uint64_t>(entry.metadata.st_size)) : Json(nullptr)}});
        last = entry.name;
    }
    return boost::json::object{{"path", logical}, {"entries", std::move(output)}, {"has_more", more},
        {"next_after_name", more ? Json(last) : Json(nullptr)}, {"snapshot", false}};
}

Json Workspace::search(std::string_view path, std::string_view literal, std::size_t max_matches) const {
    const auto logical = impl_->path(path, true);
    if (literal.empty() || literal.size() > 65536) throw Error(ErrorCode::InvalidInput, "search literal must contain 1 to 65536 bytes");
    if (max_matches == 0 || max_matches > impl_->limits.max_search_matches) throw Error(ErrorCode::Quota, "search match limit exceeds capacity");
    std::lock_guard lock(impl_->mutex);
    const DirectoryLock directory_lock(impl_->root.get(), false);
    impl_->check_root();
    const auto root = impl_->inspected(logical);
    const auto info = stat_fd(root.get());
    Inventory inventory;
    if (S_ISDIR(info.st_mode)) impl_->collect(logical, 1, inventory);
    else { inventory.files.push_back({logical, logical, info}); inventory.bytes = static_cast<std::uint64_t>(info.st_size); }
    if (inventory.files.size() > impl_->limits.max_search_files || inventory.bytes > impl_->limits.max_search_bytes)
        throw Error(ErrorCode::Quota, "search file/byte budget cannot cover selected scope");
    std::sort(inventory.files.begin(), inventory.files.end(), [](const Entry& left, const Entry& right) { return left.path < right.path; });
    boost::json::array matches;
    std::uint64_t examined_bytes = 0;
    std::size_t examined_files = 0;
    bool truncated = false;
    for (const auto& entry : inventory.files) {
        const auto data = impl_->snapshot(entry.path);
        if (!same_observation(entry.metadata, data.metadata)) throw Error(ErrorCode::Conflict, "search scope changed after enumeration");
        examined_bytes += data.bytes.size();
        ++examined_files;
        std::size_t start = 0;
        std::size_t line_start = 0;
        std::uint64_t line = 1;
        while (true) {
            const auto found = data.bytes.find(literal, start);
            if (found == std::string::npos) break;
            if (matches.size() == max_matches) { truncated = true; break; }
            line += static_cast<std::uint64_t>(std::count(data.bytes.begin() + static_cast<std::ptrdiff_t>(line_start),
                data.bytes.begin() + static_cast<std::ptrdiff_t>(found), '\n'));
            line_start = found;
            const auto excerpt_start = found > 64 ? found - 64 : 0;
            const auto excerpt = std::string_view(data.bytes).substr(excerpt_start, 160);
            matches.emplace_back(boost::json::object{{"path", entry.path}, {"byte_offset", static_cast<std::uint64_t>(found)},
                {"line", line}, {"sha256", data.digest}, {"excerpt", text_or_null(excerpt)}, {"excerpt_base64", base64(excerpt)}});
            start = found + 1;
        }
        if (truncated) break;
    }
    return boost::json::object{{"path", logical}, {"matches", std::move(matches)}, {"complete", !truncated},
        {"truncated", truncated}, {"limit_reason", truncated ? Json("max_matches") : Json(nullptr)},
        {"files_examined", static_cast<std::uint64_t>(examined_files)}, {"bytes_examined", examined_bytes}};
}

WorkspaceMutationResult Workspace::write_atomic(std::string_view path, std::string_view bytes,
                                                 std::optional<std::string> expected_sha256) {
    return impl_->mutate(path, bytes, expected_sha256, std::nullopt);
}
WorkspaceMutationResult Workspace::patch(std::string_view path, std::string_view expected_sha256,
                                          std::uint64_t byte_offset, std::string_view expected_bytes,
                                          std::string_view replacement) {
    return impl_->mutate(path, replacement, std::string(expected_sha256), PatchSpec{byte_offset, expected_bytes});
}

} // namespace wls::s1
