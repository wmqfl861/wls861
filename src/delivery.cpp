#include "wls/delivery.hpp"
#include "wls/workspace.hpp"

#include <linux/fs.h>
#include <linux/openat2.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wls::s1 {
namespace {
using Object = boost::json::object;
using Array = boost::json::array;

void require(bool condition, ErrorCode code, std::string_view message) {
    if (!condition) throw Error(code, std::string(message));
}

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

Fd open_resolved(int root, std::string_view path, int flags,
                 mode_t mode = 0, bool missing_ok = false) {
    const std::string name(path);
    open_how how{};
    how.flags = static_cast<std::uint64_t>(flags | O_CLOEXEC | O_NOFOLLOW);
    how.mode = mode;
    how.resolve = RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV;
    int result;
    do { result = static_cast<int>(::syscall(SYS_openat2, root, name.c_str(), &how, sizeof(how))); }
    while (result < 0 && errno == EINTR);
    if (result < 0 && !(missing_ok && errno == ENOENT)) fail_errno("open delivery entry");
    return Fd(result);
}

struct stat stat_fd(int descriptor) {
    struct stat info{};
    if (::fstat(descriptor, &info) != 0) fail_errno("inspect delivery descriptor");
    return info;
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
void sync_fd(int descriptor, std::string_view action) {
    int result;
    do { result = ::fsync(descriptor); } while (result < 0 && errno == EINTR);
    if (result < 0) fail_errno(action);
}

class DirectoryLock {
public:
    DirectoryLock(int root, bool exclusive) : descriptor_(open_resolved(root, ".", O_RDONLY | O_DIRECTORY)) {
        int result;
        do { result = ::flock(descriptor_.get(), exclusive ? LOCK_EX : LOCK_SH); }
        while (result < 0 && errno == EINTR);
        if (result < 0) fail_errno("lock delivery directory");
    }
private:
    Fd descriptor_;
};

void exact_fields(const Json& value, std::initializer_list<std::string_view> names) {
    require(value.is_object() && value.as_object().size() == names.size(), ErrorCode::InvalidInput,
            "delivery object has missing or unexpected fields");
    for (const auto name : names)
        require(value.as_object().contains(name), ErrorCode::InvalidInput, "delivery object is missing a field");
}
std::string string_field(const Json& value, std::string_view name) {
    const auto* field = value.as_object().if_contains(name);
    require(field && field->is_string(), ErrorCode::InvalidInput, "delivery field must be a string");
    return std::string(field->as_string());
}
std::uint64_t uint_field(const Json& value, std::string_view name) {
    const auto* field = value.as_object().if_contains(name);
    require(field && (field->is_uint64() || (field->is_int64() && field->as_int64() >= 0)),
            ErrorCode::InvalidInput, "delivery field must be a nonnegative integer");
    return field->to_number<std::uint64_t>();
}
bool digest_valid(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// JSON strings created directly by a trusted host still need bounded UTF-8
// checks before canonicalization. Names additionally reject C0/C1 controls.
bool utf8(std::string_view bytes, bool no_controls = false) {
    std::size_t position = 0;
    while (position < bytes.size()) {
        const auto first = static_cast<unsigned char>(bytes[position++]);
        std::uint32_t codepoint = first;
        unsigned int remaining = 0;
        std::uint32_t minimum = 0;
        if (first >= 0x80U) {
            if (first >= 0xc2U && first <= 0xdfU) { remaining = 1; minimum = 0x80U; codepoint = first & 0x1fU; }
            else if (first >= 0xe0U && first <= 0xefU) { remaining = 2; minimum = 0x800U; codepoint = first & 0x0fU; }
            else if (first >= 0xf0U && first <= 0xf4U) { remaining = 3; minimum = 0x10000U; codepoint = first & 0x07U; }
            else return false;
            if (remaining > bytes.size() - position) return false;
            for (unsigned int i = 0; i < remaining; ++i) {
                const auto next = static_cast<unsigned char>(bytes[position++]);
                if ((next & 0xc0U) != 0x80U) return false;
                codepoint = (codepoint << 6U) | (next & 0x3fU);
            }
            if (codepoint < minimum || codepoint > 0x10ffffU ||
                (codepoint >= 0xd800U && codepoint <= 0xdfffU)) return false;
        }
        if (no_controls && (codepoint < 0x20U || (codepoint >= 0x7fU && codepoint <= 0x9fU))) return false;
    }
    return true;
}
bool safe_name(std::string_view name) {
    return !name.empty() && name.size() <= 255 && name != "." && name != ".." &&
           name != "manifest.json" && name.find('/') == std::string_view::npos &&
           name.find('\\') == std::string_view::npos && utf8(name, true);
}

void bounded_json(const Json& value, std::size_t maximum) {
    std::vector<std::pair<const Json*, std::size_t>> pending{{&value, 0}};
    std::size_t bytes = 0;
    std::size_t nodes = 0;
    const auto account = [&](std::string_view text) {
        require(utf8(text), ErrorCode::InvalidInput, "delivery manifest contains invalid UTF-8");
        require(text.size() <= maximum - bytes, ErrorCode::Quota, "delivery manifest exceeds capacity");
        bytes += text.size();
    };
    while (!pending.empty()) {
        const auto [current, depth] = pending.back();
        pending.pop_back();
        require(depth <= 64 && ++nodes <= maximum, ErrorCode::Quota,
                "delivery manifest depth or node count exceeds capacity");
        if (current->is_object()) {
            require(current->as_object().size() <= maximum - nodes, ErrorCode::Quota,
                    "delivery manifest has too many members");
            for (const auto& member : current->as_object()) {
                account(member.key());
                pending.emplace_back(&member.value(), depth + 1);
            }
        } else if (current->is_array()) {
            require(current->as_array().size() <= maximum - nodes, ErrorCode::Quota,
                    "delivery manifest has too many array elements");
            for (const auto& item : current->as_array()) pending.emplace_back(&item, depth + 1);
        } else if (current->is_string()) account(current->as_string());
        else if (current->is_double())
            require(std::isfinite(current->as_double()), ErrorCode::InvalidInput,
                    "delivery manifest contains a non-finite number");
    }
}

std::string decode_base64(std::string_view encoded, std::uint64_t maximum) {
    require(encoded.size() % 4 == 0, ErrorCode::InvalidInput, "delivery base64 has invalid length");
    require(encoded.size() <= ((maximum + 2) / 3) * 4, ErrorCode::Quota,
            "encoded delivery file exceeds capacity");
    const auto digit = [](char c) -> unsigned int {
        if (c >= 'A' && c <= 'Z') return static_cast<unsigned int>(c - 'A');
        if (c >= 'a' && c <= 'z') return static_cast<unsigned int>(c - 'a' + 26);
        if (c >= '0' && c <= '9') return static_cast<unsigned int>(c - '0' + 52);
        if (c == '+') return 62;
        if (c == '/') return 63;
        throw Error(ErrorCode::InvalidInput, "delivery base64 contains an invalid character");
    };
    std::string result;
    result.reserve(encoded.size() / 4 * 3);
    for (std::size_t i = 0; i < encoded.size(); i += 4) {
        const auto first = digit(encoded[i]);
        const auto second = digit(encoded[i + 1]);
        const bool third_padding = encoded[i + 2] == '=';
        const bool fourth_padding = encoded[i + 3] == '=';
        require((!third_padding || fourth_padding) &&
                ((!third_padding && !fourth_padding) || i + 4 == encoded.size()),
                ErrorCode::InvalidInput, "delivery base64 has invalid padding");
        const auto third = third_padding ? 0U : digit(encoded[i + 2]);
        const auto fourth = fourth_padding ? 0U : digit(encoded[i + 3]);
        require((!third_padding || (second & 15U) == 0U) &&
                (!fourth_padding || third_padding || (third & 3U) == 0U),
                ErrorCode::InvalidInput, "delivery base64 is not canonical");
        const auto word = (first << 18U) | (second << 12U) | (third << 6U) | fourth;
        result.push_back(static_cast<char>((word >> 16U) & 255U));
        if (!third_padding) result.push_back(static_cast<char>((word >> 8U) & 255U));
        if (!fourth_padding) result.push_back(static_cast<char>(word & 255U));
    }
    require(result.size() <= maximum, ErrorCode::Quota, "decoded delivery file exceeds capacity");
    return result;
}

struct File {
    std::string name;
    std::string bytes;
    std::string digest;
};
struct Plan {
    std::string delivery_id;
    std::string manifest_bytes;
    std::string manifest_digest;
    std::string files_digest;
    std::vector<File> files;
};
struct FileObservation {
    std::string name;
    Fd descriptor;
    struct stat info{};
};
struct OwnedFile {
    std::string name;
    struct stat info{};
};
} // namespace

struct DeliveryDirectory::Impl {
    Workspace anchor;
    Fd root;
    Json identity;
    struct stat root_info{};
    DeliveryLimits limits;
    mutable std::mutex mutex;

    Impl(const std::filesystem::path& path, DeliveryLimits policy)
        : anchor(path, WorkspaceAccess::ReadOnly), root(anchor.dup_root_fd()),
          identity(anchor.root_identity()), root_info(stat_fd(root.get())), limits(policy) {
        require(string_field(identity, "path") != "/", ErrorCode::Unauthorized,
                "the filesystem root cannot be a delivery directory");
        require(limits.max_files > 0 && limits.max_files <= 4096 &&
                limits.max_file_bytes > 0 && limits.max_file_bytes <= 64U * 1024U * 1024U &&
                limits.max_total_bytes > 0 && limits.max_total_bytes <= 256U * 1024U * 1024U &&
                limits.max_manifest_bytes > 0 && limits.max_manifest_bytes <= 16U * 1024U * 1024U,
                ErrorCode::InvalidInput, "delivery limits are outside supported bounds");
    }

    void check_root() const {
        require(anchor.root_identity() == identity && same_inode(stat_fd(root.get()), root_info),
                ErrorCode::Stale, "delivery root identity changed");
    }

    Plan parse_plan(const Json& value) const {
        exact_fields(value, {"delivery_id", "root_identity", "manifest", "manifest_sha256", "files"});
        const auto& object = value.as_object();
        Plan plan;
        plan.delivery_id = string_field(value, "delivery_id");
        require(is_typed_id(plan.delivery_id, "dlv"), ErrorCode::InvalidInput, "invalid delivery ID");
        require(string_field(identity, "path").size() + 1 + plan.delivery_id.size() <= 4096,
                ErrorCode::Quota, "published delivery path exceeds capacity");
        const auto& selected_root = object.at("root_identity");
        exact_fields(selected_root, {"path", "device", "inode"});
        (void)string_field(selected_root, "path");
        (void)uint_field(selected_root, "device");
        (void)uint_field(selected_root, "inode");
        require(selected_root == identity, ErrorCode::Stale, "plan selects a different delivery root");
        const auto& manifest = object.at("manifest");
        require(manifest.is_object(), ErrorCode::InvalidInput, "delivery manifest must be an object");
        require(string_field(manifest, "delivery_id") == plan.delivery_id, ErrorCode::InvalidInput,
                "manifest delivery ID does not match the plan");
        bounded_json(manifest, limits.max_manifest_bytes);
        plan.manifest_bytes = canonical_json(manifest);
        require(plan.manifest_bytes.size() <= limits.max_manifest_bytes, ErrorCode::Quota,
                "canonical delivery manifest exceeds capacity");
        plan.manifest_digest = string_field(value, "manifest_sha256");
        require(digest_valid(plan.manifest_digest) && sha256_hex(plan.manifest_bytes) == plan.manifest_digest,
                ErrorCode::InvalidInput, "delivery manifest hash does not match its complete bytes");
        const auto& file_values = object.at("files");
        require(file_values.is_array(), ErrorCode::InvalidInput, "delivery files must be an array");
        require(file_values.as_array().size() <= limits.max_files, ErrorCode::Quota,
                "delivery file count exceeds capacity");
        std::uint64_t total = plan.manifest_bytes.size();
        require(total <= limits.max_total_bytes, ErrorCode::Quota, "delivery total bytes exceed capacity");
        Array metadata;
        std::string previous;
        for (const auto& item : file_values.as_array()) {
            exact_fields(item, {"name", "sha256", "size_bytes", "content_base64"});
            File file;
            file.name = string_field(item, "name");
            require(safe_name(file.name) && (previous.empty() || previous < file.name),
                    ErrorCode::InvalidInput, "delivery names must be safe, unique and sorted");
            previous = file.name;
            const auto size = uint_field(item, "size_bytes");
            require(size <= limits.max_file_bytes && size <= limits.max_total_bytes - total,
                    ErrorCode::Quota, "delivery file or total bytes exceed capacity");
            const auto encoded = string_field(item, "content_base64");
            file.bytes = decode_base64(encoded, limits.max_file_bytes);
            file.digest = string_field(item, "sha256");
            require(file.bytes.size() == size && digest_valid(file.digest) &&
                    sha256_hex(file.bytes) == file.digest, ErrorCode::InvalidInput,
                    "delivery file hash or size does not match its complete bytes");
            total += size;
            metadata.emplace_back(Object{{"name", file.name}, {"sha256", file.digest}, {"size_bytes", size}});
            plan.files.push_back(std::move(file));
        }
        const auto* manifest_files = manifest.as_object().if_contains("files");
        require(manifest_files && *manifest_files == metadata, ErrorCode::InvalidInput,
                "manifest files must exactly cover the sorted delivery file metadata");
        plan.files_digest = sha256_hex(canonical_json(metadata));
        return plan;
    }

    void check_directory(const struct stat& info, bool sealed) const {
        require(S_ISDIR(info.st_mode) && info.st_uid == ::geteuid() && info.st_dev == root_info.st_dev &&
                info.st_nlink > 0 && (info.st_mode & 0022) == 0 &&
                (!sealed || (info.st_mode & 07777) == 0500),
                ErrorCode::Unauthorized, "delivery directory identity, ownership or mode is unsafe");
    }
    void check_file(const struct stat& info) const {
        require(S_ISREG(info.st_mode) && info.st_nlink == 1 && info.st_uid == ::geteuid() &&
                info.st_dev == root_info.st_dev && info.st_size >= 0 && (info.st_mode & 07777) == 0400,
                ErrorCode::Unauthorized, "delivery file type, ownership, link count or mode is unsafe");
    }

    std::vector<std::string> entries(int directory) const {
        Fd listing = open_resolved(directory, ".", O_RDONLY | O_DIRECTORY);
        DIR* raw = ::fdopendir(listing.get());
        if (!raw) fail_errno("open delivery directory stream");
        (void)listing.release();
        const auto close_directory = [](DIR* value) { (void)::closedir(value); };
        std::unique_ptr<DIR, decltype(close_directory)> stream(raw, close_directory);
        std::vector<std::string> names;
        while (true) {
            errno = 0;
            const auto* entry = ::readdir(stream.get());
            if (!entry) {
                if (errno != 0) fail_errno("enumerate delivery directory");
                break;
            }
            const std::string name(entry->d_name);
            if (name == "." || name == "..") continue;
            require(names.size() <= limits.max_files, ErrorCode::Quota,
                    "delivery directory contains too many entries");
            names.push_back(name);
        }
        std::sort(names.begin(), names.end());
        return names;
    }

    FileObservation observe_file(int directory, const File& expected,
                                 bool synchronize, bool& durability_started,
                                 ErrorCode mismatch) const {
        Fd inspected = open_resolved(directory, expected.name, O_PATH);
        const auto before = stat_fd(inspected.get());
        check_file(before);
        require(static_cast<std::uint64_t>(before.st_size) == expected.bytes.size(), mismatch,
                "published delivery file has a different size");
        Fd opened = open_resolved(directory, expected.name, O_RDONLY | O_NONBLOCK | O_NOCTTY);
        require(same_observation(before, stat_fd(opened.get())), ErrorCode::Conflict,
                "delivery file changed while opening");
        std::string bytes(expected.bytes.size(), '\0');
        std::size_t offset = 0;
        while (offset < bytes.size()) {
            const auto count = ::pread(opened.get(), bytes.data() + offset, bytes.size() - offset,
                                       static_cast<off_t>(offset));
            if (count < 0) { if (errno == EINTR) continue; fail_errno("read delivery file"); }
            require(count > 0, ErrorCode::Conflict, "delivery file was truncated while reading");
            offset += static_cast<std::size_t>(count);
        }
        char trailing{};
        ssize_t extra;
        do { extra = ::pread(opened.get(), &trailing, 1, static_cast<off_t>(offset)); }
        while (extra < 0 && errno == EINTR);
        if (extra < 0) fail_errno("read delivery file boundary");
        require(extra == 0, ErrorCode::Conflict, "delivery file grew while reading");
        require(bytes == expected.bytes && sha256_hex(bytes) == expected.digest, mismatch,
                "published delivery file does not match the frozen bytes");
        if (synchronize) {
            durability_started = true;
            sync_fd(opened.get(), "sync existing delivery file");
        }
        require(same_observation(before, stat_fd(opened.get())), ErrorCode::Conflict,
                "delivery file changed during verification");
        return {expected.name, std::move(opened), before};
    }

    void verify(const Plan& plan, std::string_view name, bool synchronize,
                bool& durability_started) const {
        check_root();
        Fd inspected = open_resolved(root.get(), name, O_PATH);
        const auto before = stat_fd(inspected.get());
        check_directory(before, true);
        Fd directory = open_resolved(root.get(), name, O_RDONLY | O_DIRECTORY);
        require(same_observation(before, stat_fd(directory.get())), ErrorCode::Conflict,
                "delivery package changed while opening");
        std::vector<std::string> expected_names{"manifest.json"};
        for (const auto& file : plan.files) expected_names.push_back(file.name);
        std::sort(expected_names.begin(), expected_names.end());
        require(entries(directory.get()) == expected_names, ErrorCode::Corrupt,
                "delivery package contains missing or unexpected entries");
        std::vector<FileObservation> observations;
        observations.push_back(observe_file(directory.get(),
            {"manifest.json", plan.manifest_bytes, plan.manifest_digest}, synchronize,
            durability_started, ErrorCode::Conflict));
        for (const auto& file : plan.files)
            observations.push_back(observe_file(directory.get(), file, synchronize,
                                                durability_started, ErrorCode::Corrupt));
        if (synchronize) {
            durability_started = true;
            sync_fd(directory.get(), "sync existing delivery package");
            sync_fd(root.get(), "sync delivery root");
        }
        check_root();
        require(entries(directory.get()) == expected_names && same_observation(before, stat_fd(directory.get())),
                ErrorCode::Conflict, "delivery package changed during verification");
        Fd current = open_resolved(root.get(), name, O_PATH);
        require(same_observation(before, stat_fd(current.get())), ErrorCode::Conflict,
                "delivery package pathname changed during verification");
        for (const auto& observed : observations) {
            Fd named = open_resolved(directory.get(), observed.name, O_PATH);
            require(same_observation(observed.info, stat_fd(observed.descriptor.get())) &&
                    same_observation(observed.info, stat_fd(named.get())), ErrorCode::Conflict,
                    "delivery file changed before verification completed");
        }
    }

    Json publication(const Plan& plan, bool durable, bool reused) const {
        return Object{{"delivery_id", plan.delivery_id}, {"root_identity", identity},
            {"path", string_field(identity, "path") + "/" + plan.delivery_id},
            {"manifest_sha256", plan.manifest_digest}, {"files_sha256", plan.files_digest},
            {"verified", true}, {"durable", durable}, {"reused", reused}};
    }

    void create_file(int directory, const File& file, std::vector<OwnedFile>& owned) const {
        Fd descriptor = open_resolved(directory, file.name, O_WRONLY | O_CREAT | O_EXCL, 0600);
        const auto created = stat_fd(descriptor.get());
        owned.push_back({file.name, created});
        std::size_t offset = 0;
        while (offset < file.bytes.size()) {
            const auto count = ::write(descriptor.get(), file.bytes.data() + offset, file.bytes.size() - offset);
            if (count < 0) { if (errno == EINTR) continue; fail_errno("write staged delivery file"); }
            require(count > 0, ErrorCode::Storage, "zero-length delivery write");
            offset += static_cast<std::size_t>(count);
        }
        if (::fchmod(descriptor.get(), 0400) != 0) fail_errno("seal delivery file permissions");
        sync_fd(descriptor.get(), "sync staged delivery file");
        const auto now = stat_fd(descriptor.get());
        check_file(now);
        require(same_inode(created, now) && static_cast<std::uint64_t>(now.st_size) == file.bytes.size(),
                ErrorCode::Conflict, "staged delivery file identity or size changed");
        Fd named = open_resolved(directory, file.name, O_PATH);
        require(same_observation(now, stat_fd(named.get())), ErrorCode::Conflict,
                "staged delivery file pathname changed");
    }

    // There is deliberately no recursive/prefix cleanup. Check the entire
    // current stage against this call's inode inventory before unlinking any
    // entry. A foreign entry, replacement or root drift retains the stage.
    bool remove_owned_stage(std::string_view name, int directory, const struct stat& created,
                            const std::vector<OwnedFile>& owned) const noexcept {
        try {
            check_root();
            const auto current_directory = stat_fd(directory);
            check_directory(current_directory, false);
            Fd named = open_resolved(root.get(), name, O_PATH);
            if (!same_inode(created, current_directory) || !same_inode(created, stat_fd(named.get()))) return false;
            const auto names = entries(directory);
            if (names.size() != owned.size()) return false;
            for (const auto& entry : names) {
                const auto found = std::find_if(owned.begin(), owned.end(), [&](const OwnedFile& item) {
                    return item.name == entry;
                });
                if (found == owned.end()) return false;
                Fd file = open_resolved(directory, entry, O_PATH);
                const auto info = stat_fd(file.get());
                if (!same_inode(found->info, info) || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
                    info.st_uid != ::geteuid()) return false;
            }
            if (::fchmod(directory, 0700) != 0) return false;
            for (const auto& file : owned) {
                Fd current = open_resolved(directory, file.name, O_PATH);
                if (!same_inode(file.info, stat_fd(current.get()))) return false;
                if (::unlinkat(directory, file.name.c_str(), 0) != 0) return false;
            }
            sync_fd(directory, "sync removed staged delivery files");
            check_root();
            Fd still_named = open_resolved(root.get(), name, O_PATH);
            if (!same_inode(created, stat_fd(still_named.get())) || !entries(directory).empty()) return false;
            const std::string stage(name);
            if (::unlinkat(root.get(), stage.c_str(), AT_REMOVEDIR) != 0) return false;
            sync_fd(root.get(), "sync removed delivery stage");
            return true;
        } catch (...) { return false; }
    }

    DeliveryMutationResult publish(const Json& value) {
        std::lock_guard lock(mutex);
        DeliveryMutationResult result;
        std::string phase = "validate";
        std::string stage;
        Fd stage_directory;
        struct stat stage_info{};
        std::vector<OwnedFile> owned;
        bool created = false;
        bool rename_attempted = false;
        bool renamed = false;
        bool durable = false;
        bool recovery_sync_started = false;
        // Keep the root lock through exception cleanup, not just through rename.
        std::unique_ptr<DirectoryLock> directory_lock;
        ErrorCode cause = ErrorCode::Storage;
        std::string message;
        try {
            const auto plan = parse_plan(value);
            directory_lock = std::make_unique<DirectoryLock>(root.get(), true);
            check_root();
            Fd existing = open_resolved(root.get(), plan.delivery_id, O_PATH, 0, true);
            if (existing.get() >= 0) {
                phase = "verify_existing";
                verify(plan, plan.delivery_id, true, recovery_sync_started);
                result.status = DeliveryMutationStatus::Succeeded;
                result.publication = publication(plan, true, true);
                result.evidence = Object{{"phase", "complete"}, {"reused", true}, {"durable", true},
                    {"rename_attempted", false}, {"renamed", false}, {"staging_remaining", false}};
                return result;
            }
            phase = "create_stage";
            stage = ".wls-delivery-stage-" + new_id("stg");
            if (::mkdirat(root.get(), stage.c_str(), 0700) != 0) fail_errno("create delivery stage");
            created = true;
            stage_directory = open_resolved(root.get(), stage, O_RDONLY | O_DIRECTORY);
            stage_info = stat_fd(stage_directory.get());
            check_directory(stage_info, false);
            phase = "write_files";
            owned.reserve(plan.files.size() + 1);
            create_file(stage_directory.get(), {"manifest.json", plan.manifest_bytes, plan.manifest_digest}, owned);
            for (const auto& file : plan.files) create_file(stage_directory.get(), file, owned);
            phase = "sync_stage";
            if (::fchmod(stage_directory.get(), 0500) != 0) fail_errno("seal delivery package permissions");
            sync_fd(stage_directory.get(), "sync staged delivery package");
            phase = "verify_stage";
            bool unused = false;
            verify(plan, stage, false, unused);
            Fd staged = open_resolved(root.get(), stage, O_PATH);
            require(same_inode(stage_info, stat_fd(staged.get())), ErrorCode::Conflict,
                    "delivery staging directory was replaced");
            phase = "rename";
            rename_attempted = true;
            const auto renamed_result = ::syscall(SYS_renameat2, root.get(), stage.c_str(),
                                                   root.get(), plan.delivery_id.c_str(), RENAME_NOREPLACE);
            if (renamed_result != 0) fail_errno("publish delivery package");
            renamed = true;
            phase = "sync_root";
            sync_fd(root.get(), "sync published delivery root");
            durable = true;
            phase = "verify_published";
            verify(plan, plan.delivery_id, false, unused);
            Fd published = open_resolved(root.get(), plan.delivery_id, O_PATH);
            require(same_inode(stage_info, stat_fd(published.get())), ErrorCode::Conflict,
                    "published delivery directory identity changed");
            result.status = DeliveryMutationStatus::Succeeded;
            result.publication = publication(plan, true, false);
            result.evidence = Object{{"phase", "complete"}, {"reused", false}, {"durable", true},
                {"rename_attempted", true}, {"renamed", true}, {"staging_remaining", false}};
            return result;
        } catch (const Error& error) {
            cause = error.code(); message = error.what();
        } catch (const std::exception& error) {
            cause = ErrorCode::Storage; message = error.what();
        }
        bool cleanup_complete = !created || renamed;
        if (created && !rename_attempted && stage_directory.get() >= 0)
            cleanup_complete = remove_owned_stage(stage, stage_directory.get(), stage_info, owned);
        const bool unknown = rename_attempted || recovery_sync_started || !cleanup_complete;
        result.status = unknown ? DeliveryMutationStatus::OutcomeUnknown : DeliveryMutationStatus::EffectNotStarted;
        result.error_code = unknown ? ErrorCode::UnknownOutcome : cause;
        result.evidence = Object{{"phase", phase}, {"cause", error_name(cause)}, {"message", message},
            {"rename_attempted", rename_attempted}, {"renamed", renamed}, {"durable", durable},
            {"cleanup_complete", cleanup_complete}, {"staging_remaining", created && !renamed && !cleanup_complete},
            {"staging_path", stage.empty() ? Json(nullptr) : Json(string_field(identity, "path") + "/" + stage)}};
        return result;
    }
};

std::string_view delivery_mutation_status_name(DeliveryMutationStatus status) noexcept {
    switch (status) {
    case DeliveryMutationStatus::Succeeded: return "succeeded";
    case DeliveryMutationStatus::EffectNotStarted: return "effect_not_started";
    case DeliveryMutationStatus::OutcomeUnknown: return "outcome_unknown";
    }
    return "outcome_unknown";
}

DeliveryDirectory::DeliveryDirectory(const std::filesystem::path& root, DeliveryLimits limits)
    : impl_(std::make_unique<Impl>(root, limits)) {}
DeliveryDirectory::~DeliveryDirectory() = default;
DeliveryDirectory::DeliveryDirectory(DeliveryDirectory&&) noexcept = default;
DeliveryDirectory& DeliveryDirectory::operator=(DeliveryDirectory&&) noexcept = default;
Json DeliveryDirectory::root_identity() const {
    std::lock_guard lock(impl_->mutex);
    impl_->check_root();
    return impl_->identity;
}
DeliveryMutationResult DeliveryDirectory::publish(const Json& frozen_plan) { return impl_->publish(frozen_plan); }
Json DeliveryDirectory::inspect(const Json& frozen_plan) const {
    std::lock_guard lock(impl_->mutex);
    const auto plan = impl_->parse_plan(frozen_plan);
    DirectoryLock directory_lock(impl_->root.get(), false);
    bool unused = false;
    impl_->verify(plan, plan.delivery_id, false, unused);
    return impl_->publication(plan, false, true);
}

Json DeliveryDirectory::verify_and_sync_existing(const Json& frozen_plan) const {
    std::lock_guard lock(impl_->mutex);
    const auto plan = impl_->parse_plan(frozen_plan);
    DirectoryLock directory_lock(impl_->root.get(), true);
    bool sync_started = false;
    impl_->verify(plan, plan.delivery_id, true, sync_started);
    return impl_->publication(plan, true, true);
}

} // namespace wls::s1
