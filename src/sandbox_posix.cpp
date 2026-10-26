#include "wls/sandbox.hpp"
#include "wls/process_evidence.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <climits>
#include <csignal>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <functional>
#include <linux/close_range.h>
#include <linux/magic.h>
#include <linux/memfd.h>
#include <linux/stat.h>
#include <memory>
#include <openssl/evp.h>
#include <poll.h>
#include <set>
#include <sstream>
#include <sys/prctl.h>
#include <sys/file.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/statvfs.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <utility>

#include "sandbox_doctor.inc"

namespace wls::s1 {
namespace {

using Clock = std::chrono::steady_clock;
using Milliseconds = std::chrono::milliseconds;
using sandbox_detail::DiagnosticError;
using sandbox_detail::diagnostic_error;
using sandbox_detail::RuntimeReadBudget;

class Fd {
public:
    explicit Fd(int value = -1) noexcept : value_(value) {}
    ~Fd() { if (value_ >= 0) ::close(value_); }
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    Fd(Fd&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Fd& operator=(Fd&& other) noexcept {
        if (this != &other) {
            if (value_ >= 0) ::close(value_);
            value_ = std::exchange(other.value_, -1);
        }
        return *this;
    }
    int get() const noexcept { return value_; }
    int release() noexcept { return std::exchange(value_, -1); }
    explicit operator bool() const noexcept { return value_ >= 0; }
private:
    int value_;
};

[[noreturn]] void system_error(std::string_view action, int number = errno) {
    throw Error(ErrorCode::Storage, std::string(action) + ": " + std::strerror(number));
}

[[noreturn]] void diagnostic_system_error(std::string_view action, std::string_view stage,
                                        int number = errno) {
    diagnostic_error(ErrorCode::Storage, sandbox_detail::errno_reason(number), stage,
                     std::string(action) + ": " + std::strerror(number), number);
}

void add_error(std::string& existing, std::string_view error) {
    if (existing.size() >= 4096) return;
    if (!existing.empty()) existing += "; ";
    existing.append(error.substr(0, 4096 - existing.size()));
}

Fd duplicate_fd(int fd, int minimum = 3) {
    const int value = ::fcntl(fd, F_DUPFD_CLOEXEC, minimum);
    if (value < 0) system_error("duplicate file descriptor");
    return Fd(value);
}

void write_all(int fd, std::string_view bytes) {
    std::size_t offset = 0;
    while (offset < bytes.size()) {
        const auto count = ::write(fd, bytes.data() + offset, bytes.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) system_error("write", count == 0 ? EIO : errno);
        offset += static_cast<std::size_t>(count);
    }
}

std::array<Fd, 2> make_pipe() {
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC) != 0) system_error("pipe2");
    std::array<Fd, 2> pipe{Fd(descriptors[0]), Fd(descriptors[1])};
    const int flags = ::fcntl(descriptors[0], F_GETFL);
    if (flags < 0 || ::fcntl(descriptors[0], F_SETFL, flags | O_NONBLOCK) < 0)
        system_error("nonblocking pipe read");
    return pipe;
}

std::array<Fd, 2> make_start_pipe() {
    int descriptors[2];
    if (::pipe2(descriptors, O_CLOEXEC) != 0) system_error("create blocking start pipe");
    // This read end MUST stay blocking: bwrap's --block-fd ignores read errors.
    // The parent also keeps a private read reference, so its one-byte release
    // cannot raise SIGPIPE if the blocked child exits concurrently.
    return {Fd(descriptors[0]), Fd(descriptors[1])};
}

bool bounded_text(std::string_view value, std::size_t limit) {
    return value.size() <= limit && value.find('\0') == std::string_view::npos;
}

std::vector<std::string> path_parts(std::string_view path, bool absolute) {
    if (!bounded_text(path, 4096) || path.empty() ||
        (path.front() == '/') != absolute)
        diagnostic_error(ErrorCode::InvalidInput, "path_refused", "path", "invalid sandbox path");
    std::vector<std::string> parts;
    std::size_t at = absolute ? 1 : 0;
    while (at < path.size()) {
        const auto end = path.find('/', at);
        const auto part = path.substr(at, end == std::string_view::npos ? path.size() - at : end - at);
        if (part.empty() || part == "." || part == ".." || part.size() > 255)
            diagnostic_error(ErrorCode::InvalidInput, "path_refused", "path", "sandbox paths reject dot, parent and empty components");
        parts.emplace_back(part);
        if (parts.size() > 64) diagnostic_error(ErrorCode::InvalidInput, "path_refused", "path", "sandbox path nesting exceeds bound");
        if (end == std::string_view::npos) break;
        at = end + 1;
        if (at == path.size()) diagnostic_error(ErrorCode::InvalidInput, "path_refused", "path", "sandbox path has trailing separator");
    }
    return parts;
}

Fd open_absolute(const std::filesystem::path& path, bool directory) {
    const auto parts = path_parts(path.string(), true);
    Fd current(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC));
    if (!current) diagnostic_system_error("open filesystem root", "open");
    if (parts.empty() && !directory)
        diagnostic_error(ErrorCode::InvalidInput, "file_type_refused", "path", "runtime source cannot be the filesystem root");
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const bool leaf = i + 1 == parts.size();
        const int flags = O_CLOEXEC | O_NOFOLLOW | (leaf && !directory ? O_RDONLY | O_NONBLOCK : O_PATH | O_DIRECTORY);
        Fd next(::openat(current.get(), parts[i].c_str(), flags));
        if (!next) {
            const int number = errno;
            // O_DIRECTORY | O_NOFOLLOW may report ENOTDIR for a symlink.
            // Classify the actual rejected entry, never infer it from text.
            struct stat entry{};
            if ((number == ELOOP || number == ENOTDIR) &&
                ::fstatat(current.get(), parts[i].c_str(), &entry, AT_SYMLINK_NOFOLLOW) == 0 &&
                S_ISLNK(entry.st_mode))
                diagnostic_error(ErrorCode::Storage, "symlink_refused", "open",
                                 "open sandbox path without symlinks: symbolic link refused", number);
            diagnostic_system_error("open sandbox path without symlinks", "open", number);
        }
        current = std::move(next);
    }
    return current;
}

#ifndef WLS_SANDBOX_SUPERVISOR_TEST_ONLY
Fd open_relative_directory(int root, std::string_view path) {
    auto current = duplicate_fd(root);
    if (path.empty() || path == ".") return current;
    for (const auto& part : path_parts(path, false)) {
        Fd next(::openat(current.get(), part.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!next) system_error("open workspace cwd without symlinks");
        current = std::move(next);
    }
    return current;
}

void check_workspace(int fd) {
    struct stat info{};
    if (::fstat(fd, &info) != 0) system_error("stat workspace");
    if (!S_ISDIR(info.st_mode) || info.st_uid != ::geteuid() || (info.st_mode & 0022) != 0)
        throw Error(ErrorCode::Unauthorized, "workspace must be an owned directory without group/other write access");
    struct stat root{};
    if (::stat("/", &root) != 0) system_error("stat host root");
    if (info.st_dev == root.st_dev && info.st_ino == root.st_ino)
        throw Error(ErrorCode::Unauthorized, "host root cannot be a workspace");
}

std::string read_at(int root, const char* name, std::size_t maximum = 16384) {
    Fd file(::openat(root, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!file) system_error(std::string("open ") + name);
    std::string text;
    std::array<char, 4096> buffer{};
    for (;;) {
        const auto count = ::read(file.get(), buffer.data(), buffer.size());
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) system_error(std::string("read ") + name);
        if (count == 0) break;
        if (static_cast<std::size_t>(count) > maximum - text.size())
            throw Error(ErrorCode::InvalidInput, "sandbox probe input exceeds bound");
        text.append(buffer.data(), static_cast<std::size_t>(count));
    }
    return text;
}

void write_at(int root, const char* name, std::string_view value) {
    Fd file(::openat(root, name, O_WRONLY | O_NOFOLLOW | O_CLOEXEC));
    if (!file) system_error(std::string("open ") + name);
    write_all(file.get(), value);
}

#endif

bool hash_string(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

struct PinnedFile {
    Fd fd;
    std::string sha256;
    mode_t mode{};
    std::uint64_t size{};
};

// Runtime bytes are copied into a sealed memfd. The verified copy is the only
// source passed to bwrap, so neither pathname replacement nor later writes to
// the original inode can change this operation's runtime content.
PinnedFile pin_file(const std::filesystem::path& path, std::string_view expected,
                    bool launcher, bool copy, RuntimeReadBudget* budget = nullptr,
                    boost::json::object* observation = nullptr) {
    if (!expected.empty() && !hash_string(expected))
        diagnostic_error(ErrorCode::InvalidInput, "hash_invalid", "expected_hash", "runtime file hash must be lowercase SHA-256");
    auto source = open_absolute(path, false);
    struct stat before{};
    if (::fstat(source.get(), &before) != 0) diagnostic_system_error("stat runtime file", "stat");
    constexpr std::uint64_t max_file = sandbox_detail::runtime_max_file_bytes;
    if (observation != nullptr) {
        (*observation)["device"] = static_cast<std::uint64_t>(before.st_dev);
        (*observation)["inode"] = static_cast<std::uint64_t>(before.st_ino);
        (*observation)["uid"] = static_cast<std::uint64_t>(before.st_uid);
        (*observation)["mode"] = static_cast<std::uint64_t>(before.st_mode);
        (*observation)["declared_bytes"] = static_cast<std::int64_t>(before.st_size);
        (*observation)["bytes_read"] = 0;
        (*observation)["sha256"] = nullptr;
        (*observation)["identity_verified"] = false;
    }
    if (!S_ISREG(before.st_mode))
        diagnostic_error(ErrorCode::Unauthorized, "file_type_refused", "stat", "runtime source must be an ordinary file");
    if (before.st_size < 0 || static_cast<std::uint64_t>(before.st_size) > max_file)
        diagnostic_error(ErrorCode::Unauthorized, "read_budget_exceeded", "size", "runtime source exceeds the 64 MiB file bound");
    if ((before.st_mode & (S_ISUID | S_ISGID | 0022)) != 0 ||
        (before.st_uid != 0 && before.st_uid != ::geteuid()))
        diagnostic_error(ErrorCode::Unauthorized, "owner_mode_refused", "owner_mode", "runtime source must be a bounded, trusted ordinary file without writable/shared privilege bits");
    if (launcher && before.st_uid != 0)
        diagnostic_error(ErrorCode::Unauthorized, "owner_mode_refused", "launcher_owner", "bubblewrap must be a root-owned executable without setuid/setgid");
    if (launcher && (before.st_mode & 0111) == 0)
        diagnostic_error(ErrorCode::Unauthorized, "launcher_not_executable", "launcher_mode", "bubblewrap must be a root-owned executable without setuid/setgid");
    if (budget != nullptr) budget->preflight(static_cast<std::uint64_t>(before.st_size));
    std::array<unsigned char, 4> elf{};
    bool launcher_magic_verified = !launcher;
    if (launcher && budget == nullptr) {
        if (::pread(source.get(), elf.data(), elf.size(), 0) != 4 ||
            elf != std::array<unsigned char, 4>{0x7f, 'E', 'L', 'F'})
            diagnostic_error(ErrorCode::Unauthorized, "launcher_not_elf", "launcher_elf", "bubblewrap launcher must be an ELF executable");
        launcher_magic_verified = true;
    }
    Fd frozen;
    if (copy) {
        const auto value = ::syscall(SYS_memfd_create, "wls-runtime", MFD_CLOEXEC | MFD_ALLOW_SEALING);
        if (value < 0) system_error("memfd_create");
        frozen = Fd(static_cast<int>(value));
    }
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> digest(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!digest || EVP_DigestInit_ex(digest.get(), EVP_sha256(), nullptr) != 1)
        throw Error(ErrorCode::Storage, "initialize runtime SHA-256");
    std::array<char, 65536> buffer{};
    std::uint64_t total = 0;
    for (;;) {
        // A diagnostic reader never consumes bytes outside its shared budget.
        // Its final fstat still detects growth/truncation and metadata changes.
        if (budget != nullptr && total == static_cast<std::uint64_t>(before.st_size)) break;
        const auto maximum = budget == nullptr ? buffer.size() : static_cast<std::size_t>(
            std::min<std::uint64_t>(buffer.size(), static_cast<std::uint64_t>(before.st_size) - total));
        const auto count = ::read(source.get(), buffer.data(), maximum);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) diagnostic_system_error("read runtime file", "read");
        if (count == 0) break;
        if (budget != nullptr) budget->charge(static_cast<std::uint64_t>(count));
        total += static_cast<std::uint64_t>(count);
        if (observation != nullptr) (*observation)["bytes_read"] = total;
        if (!launcher_magic_verified) {
            if (count < 4 || buffer[0] != '\x7f' || buffer[1] != 'E' || buffer[2] != 'L' || buffer[3] != 'F')
                diagnostic_error(ErrorCode::Unauthorized, "launcher_not_elf", "launcher_elf", "bubblewrap launcher must be an ELF executable");
            launcher_magic_verified = true;
        }
        if (total > max_file) diagnostic_error(ErrorCode::InvalidInput, "read_budget_exceeded", "read", "runtime file grew beyond bound");
        if (EVP_DigestUpdate(digest.get(), buffer.data(), static_cast<std::size_t>(count)) != 1)
            throw Error(ErrorCode::Storage, "hash runtime file");
        if (copy) write_all(frozen.get(), std::string_view(buffer.data(), static_cast<std::size_t>(count)));
    }
    if (!launcher_magic_verified)
        diagnostic_error(ErrorCode::Unauthorized, "launcher_not_elf", "launcher_elf", "bubblewrap launcher must be an ELF executable");
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest_bytes{};
    unsigned int digest_size = 0;
    if (EVP_DigestFinal_ex(digest.get(), digest_bytes.data(), &digest_size) != 1 || digest_size != 32)
        throw Error(ErrorCode::Storage, "finish runtime SHA-256");
    constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    for (unsigned int i = 0; i < digest_size; ++i) {
        actual += hex[digest_bytes[i] >> 4];
        actual += hex[digest_bytes[i] & 15];
    }
    if (observation != nullptr) (*observation)["sha256"] = actual;
    struct stat after{};
    if (::fstat(source.get(), &after) != 0) diagnostic_system_error("restat runtime file", "restat");
    if (before.st_size != after.st_size || before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
        before.st_mtim.tv_nsec != after.st_mtim.tv_nsec || before.st_ctim.tv_sec != after.st_ctim.tv_sec ||
        before.st_ctim.tv_nsec != after.st_ctim.tv_nsec || total != static_cast<std::uint64_t>(before.st_size))
        diagnostic_error(ErrorCode::Conflict, "source_changed", "restat", "runtime file changed while being pinned");
    if (observation != nullptr) (*observation)["identity_verified"] = true;
    if (!expected.empty() && actual != expected)
        diagnostic_error(ErrorCode::Conflict, "hash_mismatch", "hash", "runtime file does not match the frozen SHA-256");
    if (copy) {
        if (::lseek(frozen.get(), 0, SEEK_SET) < 0 ||
            ::fcntl(frozen.get(), F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) < 0)
            system_error("seal runtime content");
    }
    return {copy ? std::move(frozen) : std::move(source), std::move(actual), before.st_mode, total};
}

void validate_destination(std::string_view path) {
    (void)path_parts(path, true);
    const auto prefix = [&](std::string_view root) { return path.starts_with(root) && path.size() > root.size(); };
    if (!prefix("/usr/bin/") && !prefix("/usr/lib/") && !prefix("/bin/") &&
        !prefix("/lib/") && !prefix("/lib64/") && !prefix("/usr/lib64/") &&
        path != "/etc/ld.so.cache" && path != "/etc/nsswitch.conf" &&
        path != "/etc/passwd" && path != "/etc/group")
        diagnostic_error(ErrorCode::Unauthorized, "destination_refused", "destination", "runtime mount destination is outside the fixed runtime whitelist");
}

std::string mountinfo_path(std::string_view encoded) {
    std::string decoded;
    for (std::size_t i = 0; i < encoded.size(); ++i) {
        if (encoded[i] != '\\') { decoded += encoded[i]; continue; }
        if (i + 3 >= encoded.size())
            throw Error(ErrorCode::Corrupt, "truncated mountinfo path escape");
        unsigned int value = 0;
        for (std::size_t j = 1; j <= 3; ++j) {
            const char digit = encoded[i + j];
            if (digit < '0' || digit > '7')
                throw Error(ErrorCode::Corrupt, "invalid mountinfo path escape");
            value = value * 8 + static_cast<unsigned int>(digit - '0');
        }
        if (value == 0 || value > 255)
            throw Error(ErrorCode::Corrupt, "invalid mountinfo path byte");
        decoded += static_cast<char>(value);
        i += 3;
    }
    if (decoded.empty() || decoded.front() != '/' || decoded.size() > 4096)
        throw Error(ErrorCode::Corrupt, "invalid mountinfo absolute path");
    return decoded;
}

void verify_workspace_mounts(std::string_view mountinfo) {
    if (!bounded_text(mountinfo, 262144) || mountinfo.empty())
        throw Error(ErrorCode::Corrupt, "sandbox mountinfo is empty or exceeds bound");
    std::istringstream lines{std::string(mountinfo)};
    std::size_t records = 0;
    std::size_t workspace_mounts = 0;
    for (std::string line; std::getline(lines, line); ) {
        if (++records > 1024) throw Error(ErrorCode::Corrupt, "sandbox mount count exceeds bound");
        std::istringstream row(line);
        std::string id, parent, device, root, encoded_point, options;
        if (!(row >> id >> parent >> device >> root >> encoded_point >> options))
            throw Error(ErrorCode::Corrupt, "incomplete sandbox mountinfo record");
        const auto point = mountinfo_path(encoded_point);
        // bwrap binds directories recursively. A same-device bind mount is
        // still outside the workspace boundary and must not evade this check.
        if (point.starts_with("/work/"))
            throw Error(ErrorCode::Unauthorized, "workspace contains a nested mount");
        bool separator = false;
        bool propagation = false;
        for (std::string field; row >> field; ) {
            if (field == "-") { separator = true; break; }
            if (field.starts_with("shared:") || field.starts_with("master:") ||
                field.starts_with("propagate_from:")) propagation = true;
        }
        std::string filesystem, source, super_options, extra;
        if (!separator || !(row >> filesystem >> source >> super_options) || row >> extra)
            throw Error(ErrorCode::Corrupt, "malformed sandbox mountinfo record");
        if (point == "/work") {
            ++workspace_mounts;
            // A slave bind can receive later host submounts even after this
            // snapshot. Require a private workspace mount; never repair the
            // host's propagation settings from this runner.
            if (propagation)
                throw Error(ErrorCode::Unauthorized, "workspace mount can receive host mount propagation");
            const std::string flags = "," + options + ",";
            if (flags.find(",rw,") == std::string::npos || flags.find(",nosuid,") == std::string::npos ||
                flags.find(",nodev,") == std::string::npos)
                throw Error(ErrorCode::Unauthorized, "workspace mount lacks required rw/nosuid/nodev flags");
        }
    }
    if (workspace_mounts != 1)
        throw Error(ErrorCode::Unauthorized, "sandbox must contain exactly one workspace mount");
}

struct statx workspace_node(int directory, const char* name) {
    struct statx info{};
    constexpr unsigned int required = STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_UID | STATX_INO | STATX_MNT_ID;
    if (::syscall(SYS_statx, directory, name, AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW | AT_NO_AUTOMOUNT,
                  STATX_BASIC_STATS | STATX_MNT_ID, &info) != 0)
        system_error("inspect workspace inode and mount identity");
    if ((info.stx_mask & required) != required)
        throw Error(ErrorCode::Unsupported, "workspace audit requires kernel inode and mount identities");
    return info;
}

std::size_t verify_workspace_tree(int root) {
    const auto root_info = workspace_node(root, "");
    const auto check = [&](const struct statx& info) {
        if (info.stx_uid != ::geteuid() || (info.stx_mode & 0022U) != 0 ||
            info.stx_mnt_id != root_info.stx_mnt_id || info.stx_nlink == 0)
            throw Error(ErrorCode::Unauthorized, "workspace tree contains an unsafe owner, writable mode or mount boundary");
        if (!S_ISDIR(info.stx_mode) && (!S_ISREG(info.stx_mode) || info.stx_nlink != 1))
            throw Error(ErrorCode::Unauthorized, "workspace tree permits only directories and single-link ordinary files");
    };
    check(root_info);
    if (!S_ISDIR(root_info.stx_mode)) throw Error(ErrorCode::Unauthorized, "workspace audit root is not a directory");
    std::size_t entries = 0;
    const auto walk = [&](const auto& self, int parent, std::size_t depth, std::size_t path_bytes) -> void {
        Fd readable(::openat(parent, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
        if (!readable) system_error("open workspace audit directory");
        DIR* opened = ::fdopendir(readable.get());
        if (opened == nullptr) system_error("enumerate workspace audit directory");
        (void)readable.release();
        const auto close_directory = [](DIR* value) { (void)::closedir(value); };
        std::unique_ptr<DIR, decltype(close_directory)> directory(opened, close_directory);
        for (;;) {
            errno = 0;
            const auto* entry = ::readdir(directory.get());
            if (entry == nullptr) {
                if (errno != 0) system_error("read workspace audit directory");
                break;
            }
            const std::string_view name(entry->d_name);
            if (name == "." || name == "..") continue;
            if (++entries > 20000 || depth >= 32 || path_bytes + name.size() > 4096)
                throw Error(ErrorCode::Quota, "workspace audit exceeds entry, depth or path bound");
            const auto info = workspace_node(::dirfd(directory.get()), entry->d_name);
            check(info);
            if (S_ISDIR(info.stx_mode)) {
                Fd child(::openat(::dirfd(directory.get()), entry->d_name,
                                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
                if (!child) system_error("pin workspace audit subdirectory");
                const auto pinned = workspace_node(child.get(), "");
                check(pinned);
                if (pinned.stx_ino != info.stx_ino || pinned.stx_mnt_id != info.stx_mnt_id)
                    throw Error(ErrorCode::Conflict, "workspace directory changed during its audit");
                self(self, child.get(), depth + 1, path_bytes + name.size() + 1);
            }
        }
    };
    walk(walk, root, 0, 0);
    return entries;
}

#ifndef WLS_SANDBOX_SUPERVISOR_TEST_ONLY
void validate_policy_limits(const SandboxPolicy& policy) {
    if (policy.runtime_files.size() > 128 || policy.memory_max_bytes < 16777216 ||
        policy.memory_max_bytes > 1099511627776ULL || policy.pids_max < 4 || policy.pids_max > 65536 ||
        policy.cpu_period_us < 1000 || policy.cpu_period_us > 1000000 ||
        policy.cpu_quota_us < 1000 || policy.cpu_quota_us > 1000000000 ||
        policy.max_timeout < Milliseconds(1) || policy.max_timeout > Milliseconds(3600000) ||
        policy.cleanup_timeout < Milliseconds(50) || policy.cleanup_timeout > Milliseconds(10000) ||
        policy.max_output_bytes == 0 || policy.max_output_bytes > 16777216)
        diagnostic_error(ErrorCode::InvalidInput, "policy_unsupported", "policy", "sandbox policy limit is outside supported bounds");
}

void validate_policy(const SandboxPolicy& policy) {
    validate_policy_limits(policy);
    (void)path_parts(policy.bwrap_path.string(), true);
    if (!policy.cgroup_root.empty()) {
        (void)path_parts(policy.cgroup_root.string(), true);
        if (policy.cgroup_root == "/" || policy.cgroup_root == "/sys/fs/cgroup")
            diagnostic_error(ErrorCode::Unauthorized, "path_refused", "cgroup_root", "supply a dedicated delegated cgroup directory, not the hierarchy root");
    }
    std::set<std::string> destinations;
    for (const auto& file : policy.runtime_files) {
        (void)path_parts(file.source.string(), true);
        validate_destination(file.destination);
        if (!destinations.insert(file.destination).second)
            diagnostic_error(ErrorCode::InvalidInput, "destination_duplicate", "destination", "duplicate runtime mount destination");
    }
}

void validate_request(const ProcessRequest& request, const SandboxPolicy& policy) {
    if (request.argv.empty() || request.argv.size() > 128 || request.env.size() > 64 ||
        request.timeout < Milliseconds(1) || request.timeout > policy.max_timeout ||
        request.output_limit_bytes == 0 || request.output_limit_bytes > policy.max_output_bytes)
        throw Error(ErrorCode::InvalidInput, "process arguments or limits exceed the frozen sandbox policy");
    (void)path_parts(request.argv.front(), true);
    if (!request.operation_id.empty() && !is_typed_id(request.operation_id, "op"))
        throw Error(ErrorCode::InvalidInput, "process operation identity must be host-issued op ULID");
    std::size_t bytes = 0;
    for (const auto& arg : request.argv) {
        if (!bounded_text(arg, 32768)) throw Error(ErrorCode::InvalidInput, "process argument is oversized or contains NUL");
        bytes += arg.size() + 1;
    }
    for (const auto& [key, value] : request.env) {
        if (key.empty() || key.size() > 128 ||
            !((key.front() >= 'A' && key.front() <= 'Z') || (key.front() >= 'a' && key.front() <= 'z') || key.front() == '_') ||
            !std::all_of(key.begin(), key.end(), [](char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
            }) || !bounded_text(value, 16384))
            throw Error(ErrorCode::InvalidInput, "process environment must contain explicit bounded key/value pairs");
        if (key == "HOME" || key == "TMPDIR" || key == "PWD" || key.starts_with("LD_"))
            throw Error(ErrorCode::Unauthorized, "process environment cannot override sandbox home, cwd, tmp or launcher loader policy");
        bytes += key.size() + value.size() + 2;
    }
    if (bytes > 131072) throw Error(ErrorCode::InvalidInput, "combined process arguments exceed bound");
    if (!request.cwd.empty() && request.cwd != ".") (void)path_parts(request.cwd, false);
}
#endif

struct ChildMapping { int source; int target; };
struct ChildSpec {
    int executable_fd{-1};
    int working_directory_fd{-1};
    int cgroup_procs_fd{-1};
    std::vector<std::string> argv;
    std::vector<std::string> env;
    std::vector<ChildMapping> mappings;
    Milliseconds timeout{1000};
    Milliseconds cleanup_timeout{500};
    std::size_t output_limit{65536};
};

// Parent-only hooks. Production supplies cgroup.kill / cgroup.events. The test
// TU may exercise this internal supervisor using fixed trusted fixtures, but
// no unsandboxed entry point exists in the production header or CLI.
struct DescendantControl {
    std::function<void(pid_t)> kill_all;
    std::function<bool()> empty;
    bool authoritative{false};
};

struct SupervisionResult {
    ProcessResult process;
    std::string status_stream;
    bool child_reaped{false};
    bool status_overflow{false};
};

struct ChildError { int stage; int number; };

[[noreturn]] void child_error(int fd, int stage, int number) noexcept {
    const ChildError error{stage, number};
    const char* bytes = reinterpret_cast<const char*>(&error);
    std::size_t offset = 0;
    while (offset < sizeof(error)) {
        const auto count = ::write(fd, bytes + offset, sizeof(error) - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        offset += static_cast<std::size_t>(count);
    }
    _exit(126);
}

// All allocations, argv construction and descriptor duplication happen before
// fork. The child only performs async-signal-safe operations/syscalls until
// execveat, and writes a fixed small error record on failure.
SupervisionResult supervise(const ChildSpec& spec, DescendantControl control,
                            std::stop_token stop, int status_read_fd = -1,
                            int owned_status_write_fd = -1,
                            const std::function<void(std::string_view, Clock::time_point)>& start_gate = {},
                            bool* observed_spawn = nullptr) {
    Fd status_writer(owned_status_write_fd);
    SupervisionResult result;
    auto& process = result.process;
    if (stop.stop_requested()) {
        process.status = ProcessStatus::Cancelled;
        process.all_descendants_exited = true;
        return result;
    }
    auto stdout_pipe = make_pipe();
    auto stderr_pipe = make_pipe();
    auto error_pipe = make_pipe();
    Fd null_input(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    if (!null_input) system_error("open null stdin");
    // Fixed child targets occupy 0..300. Duplicates above 512 cannot collide.
    std::vector<Fd> owned;
    std::vector<ChildMapping> mappings;
    const auto add_mapping = [&](int source, int target) {
        owned.push_back(duplicate_fd(source, 512));
        mappings.push_back({owned.back().get(), target});
    };
    add_mapping(null_input.get(), 0);
    add_mapping(stdout_pipe[1].get(), 1);
    add_mapping(stderr_pipe[1].get(), 2);
    add_mapping(error_pipe[1].get(), 3);
    for (const auto& mapping : spec.mappings) add_mapping(mapping.source, mapping.target);
    const auto executable = duplicate_fd(spec.executable_fd, 512);
    Fd working_directory;
    if (spec.working_directory_fd >= 0) working_directory = duplicate_fd(spec.working_directory_fd, 512);
    Fd cgroup_procs;
    if (spec.cgroup_procs_fd >= 0) cgroup_procs = duplicate_fd(spec.cgroup_procs_fd, 512);
    std::vector<char*> args;
    std::vector<char*> environment;
    for (const auto& arg : spec.argv) args.push_back(const_cast<char*>(arg.c_str()));
    for (const auto& item : spec.env) environment.push_back(const_cast<char*>(item.c_str()));
    args.push_back(nullptr);
    environment.push_back(nullptr);
    const auto parent = ::getpid();
    const auto started = Clock::now();
    const pid_t child = ::fork();
    if (child < 0) system_error("fork process launcher");
    if (child == 0) {
        const int original_error = owned[3].get();
        // The workload must create objects acceptable to the same workspace
        // checks on its next call, regardless of the host shell's umask.
        ::umask(0077);
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL, 0, 0, 0) != 0 || ::getppid() != parent)
            child_error(original_error, 1, errno == 0 ? ESRCH : errno);
        if (::syscall(SYS_close_range, 3U, UINT_MAX, CLOSE_RANGE_CLOEXEC) != 0)
            child_error(original_error, 2, errno);
        if (::setpgid(0, 0) != 0) child_error(original_error, 3, errno);
        if (::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) child_error(original_error, 4, errno);
        sigset_t empty_mask;
        ::sigemptyset(&empty_mask);
        if (::sigprocmask(SIG_SETMASK, &empty_mask, nullptr) != 0) child_error(original_error, 5, errno);
        struct sigaction reset{};
        reset.sa_handler = SIG_DFL;
        ::sigemptyset(&reset.sa_mask);
        for (int signal = 1; signal < NSIG; ++signal) {
            if (signal != SIGKILL && signal != SIGSTOP) (void)::sigaction(signal, &reset, nullptr);
        }
        struct rlimit core{0, 0};
        if (::setrlimit(RLIMIT_CORE, &core) != 0) child_error(original_error, 6, errno);
        if (cgroup_procs && ::write(cgroup_procs.get(), "0", 1) != 1)
            child_error(original_error, 7, errno == 0 ? EIO : errno);
        if (working_directory && ::fchdir(working_directory.get()) != 0)
            child_error(original_error, 8, errno);
        for (const auto& mapping : mappings) {
            if (::dup2(mapping.source, mapping.target) < 0) child_error(original_error, 9, errno);
        }
        if (::fcntl(3, F_SETFD, FD_CLOEXEC) != 0) child_error(3, 10, errno);
        ::syscall(SYS_execveat, executable.get(), "", args.data(), environment.data(), AT_EMPTY_PATH);
        child_error(3, 11, errno);
    }
    process.spawned = true;
    if (observed_spawn != nullptr) *observed_spawn = true;
    stdout_pipe[1] = Fd();
    stderr_pipe[1] = Fd();
    error_pipe[1] = Fd();
    status_writer = Fd();
    owned.clear();
    // A pinned pidfd avoids PID reuse while terminating the direct child.
    Fd child_pidfd(static_cast<int>(::syscall(SYS_pidfd_open, child, 0)));
    std::optional<ProcessStatus> reason;
    std::optional<Clock::time_point> cleanup_deadline;
    std::optional<Clock::time_point> reaped_at;
    bool kill_sent = false;
    bool stdout_open = true;
    bool stderr_open = true;
    bool error_open = true;
    bool status_open = status_read_fd >= 0;
    bool descendants_empty = false;
    std::string error_bytes;
    int wait_status = 0;
    const auto begin_cleanup = [&] {
        if (!cleanup_deadline) cleanup_deadline = Clock::now() + spec.cleanup_timeout;
        // Retry the cgroup kill while it remains populated. The direct child
        // can be between fork and its cgroup join when cancellation begins.
        try { if (control.kill_all) control.kill_all(child); }
        catch (const std::exception& error) { add_error(process.error, error.what()); }
        if (!kill_sent && !result.child_reaped) {
            if (child_pidfd) {
                if (::syscall(SYS_pidfd_send_signal, child_pidfd.get(), SIGKILL, nullptr, 0) != 0 && errno != ESRCH)
                    add_error(process.error, "cannot signal launcher through pidfd");
            } else {
                // An unreaped owned child cannot have its PID reused.
                if (::kill(child, SIGKILL) != 0 && errno != ESRCH) add_error(process.error, "cannot kill owned launcher");
            }
        }
        kill_sent = true;
    };
    const auto drain = [&](int fd, std::string& text, bool& open, std::size_t cap, bool output) {
        if (!open) return;
        std::array<char, 4096> buffer{};
        // Bound work per pipe per loop so an endless writer cannot starve the
        // deadline/cancel/cgroup checks or the other output stream.
        for (int reads = 0; reads < 16; ++reads) {
            const auto count = ::read(fd, buffer.data(), buffer.size());
            if (count < 0 && errno == EINTR) continue;
            if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
            if (count <= 0) {
                open = false;
                if (count < 0) {
                    add_error(process.error, "read process pipe failed");
                    if (!reason) reason = ProcessStatus::CleanupUnknown;
                }
                return;
            }
            const auto used = output ? process.stdout_text.size() + process.stderr_text.size() : text.size();
            const auto available = used >= cap ? 0 : cap - used;
            const auto bytes = static_cast<std::size_t>(count);
            text.append(buffer.data(), std::min(bytes, available));
            if (bytes > available) {
                if (output) {
                    process.output_truncated = true;
                    if (!reason) reason = ProcessStatus::OutputLimit;
                } else {
                    if (fd == status_read_fd) result.status_overflow = true;
                    if (!reason) reason = ProcessStatus::CleanupUnknown;
                    add_error(process.error, "process control pipe exceeded bound");
                }
                return;
            }
        }
    };
    for (;;) {
        drain(stdout_pipe[0].get(), process.stdout_text, stdout_open, spec.output_limit, true);
        drain(stderr_pipe[0].get(), process.stderr_text, stderr_open, spec.output_limit, true);
        drain(error_pipe[0].get(), error_bytes, error_open, sizeof(ChildError), false);
        if (status_open) drain(status_read_fd, result.status_stream, status_open, 16384, false);
        if (!reason && stop.stop_requested()) reason = ProcessStatus::Cancelled;
        if (!reason && Clock::now() >= started + spec.timeout) reason = ProcessStatus::TimedOut;
        if (!reason && start_gate) {
            try { start_gate(result.status_stream, started + spec.timeout); }
            catch (const std::exception& error) {
                add_error(process.error, error.what());
                reason = ProcessStatus::SpawnFailed;
            }
        }
        if (!result.child_reaped) {
            const auto waited = ::waitpid(child, &wait_status, WNOHANG);
            if (waited == child) {
                result.child_reaped = true;
                reaped_at = Clock::now();
            }
            else if (waited < 0 && errno != EINTR) {
                add_error(process.error, "owned launcher wait status is unavailable");
                if (!reason) reason = ProcessStatus::CleanupUnknown;
            }
        }
        descendants_empty = false;
        try { descendants_empty = control.authoritative && control.empty && control.empty(); }
        catch (const std::exception& error) {
            add_error(process.error, error.what());
            if (!reason) reason = ProcessStatus::CleanupUnknown;
        }
        if (!error_open && error_bytes.empty()) process.launcher_exec_confirmed = true;
        if (!error_bytes.empty() && !reason) reason = ProcessStatus::SpawnFailed;
        const auto now = Clock::now();
        if (!reason && stop.stop_requested()) reason = ProcessStatus::Cancelled;
        if (!reason && now >= started + spec.timeout) reason = ProcessStatus::TimedOut;
        if (reason) begin_cleanup();
        // Reaping the main process does not prove detached descendants exited.
        if (result.child_reaped && !cleanup_deadline) cleanup_deadline = now + spec.cleanup_timeout;
        if (result.child_reaped && !stdout_open && !stderr_open && !error_open && !status_open &&
            (descendants_empty || !control.authoritative)) break;
        if (cleanup_deadline && now >= *cleanup_deadline) {
            begin_cleanup();
            break;
        }
        if (result.child_reaped && control.authoritative && !descendants_empty && reaped_at &&
            now >= *reaped_at + std::min(Milliseconds(50), spec.cleanup_timeout / 2)) {
            // An exit-0 launcher with background descendants is not evidence
            // that those descendants finished their writes. Terminate them,
            // retain the uncertainty, and require outcome reconciliation.
            if (!reason) reason = ProcessStatus::CleanupUnknown;
            begin_cleanup();
        }
        std::array<pollfd, 4> polls{{
            {stdout_open ? stdout_pipe[0].get() : -1, POLLIN, 0},
            {stderr_open ? stderr_pipe[0].get() : -1, POLLIN, 0},
            {error_open ? error_pipe[0].get() : -1, POLLIN, 0},
            {status_open ? status_read_fd : -1, POLLIN, 0}
        }};
        if (::poll(polls.data(), static_cast<nfds_t>(polls.size()), 10) < 0 && errno != EINTR) {
            add_error(process.error, "poll process pipes failed");
            if (!reason) reason = ProcessStatus::CleanupUnknown;
        }
    }
    // Never block indefinitely waiting for a D-state process after SIGKILL.
    // Its cgroup remains as a durable recovery target when populated != 0.
    if (!result.child_reaped) {
        const auto waited = ::waitpid(child, &wait_status, WNOHANG);
        result.child_reaped = waited == child;
    }
    descendants_empty = false;
    try { descendants_empty = control.authoritative && control.empty && control.empty(); }
    catch (const std::exception& error) {
        add_error(process.error, error.what());
        reason = ProcessStatus::CleanupUnknown;
    }
    process.all_descendants_exited = result.child_reaped && descendants_empty;
    if (result.child_reaped) {
        if (WIFEXITED(wait_status)) process.exit_code = WEXITSTATUS(wait_status);
        if (WIFSIGNALED(wait_status)) process.signal = WTERMSIG(wait_status);
    }
    if (!error_bytes.empty()) {
        if (error_bytes.size() == sizeof(ChildError)) {
            ChildError error{};
            std::memcpy(&error, error_bytes.data(), sizeof(error));
            add_error(process.error, "launcher setup stage " + std::to_string(error.stage) + ": " + std::strerror(error.number));
        } else add_error(process.error, "partial launcher failure record");
    }
    const bool cleanup_unknown = !result.child_reaped ||
        (control.authoritative && !process.all_descendants_exited) || stdout_open || stderr_open || status_open;
    process.status = cleanup_unknown ? ProcessStatus::CleanupUnknown :
        reason.value_or(process.signal ? ProcessStatus::Signalled : ProcessStatus::Exited);
    boost::json::object evidence{
        {"supervision", control.authoritative ? "cgroup-v2" : "trusted-fixture-only"},
        {"launcher_reaped", result.child_reaped}, {"kill_sent", kill_sent},
        {"descendants_proof", process.all_descendants_exited ? "cgroup.events populated=0" :
            control.authoritative ? "unconfirmed" : "unavailable"},
        {"stdout_sha256", sha256_hex(process.stdout_text)},
        {"stderr_sha256", sha256_hex(process.stderr_text)},
        {"retained_bytes", process.stdout_text.size() + process.stderr_text.size()},
        {"elapsed_ms", std::chrono::duration_cast<Milliseconds>(Clock::now() - started).count()},
        {"side_effects_undone", false}
    };
    if (reason) evidence["termination_reason"] = process_status_name(*reason);
    process.evidence = std::move(evidence);
    return result;
}

// Public definitions are omitted only when the regression TU includes the
// implementation to test its private supervisor. The production library is
// always built without this macro and exports only the strong ProcessRunner.
#ifndef WLS_SANDBOX_SUPERVISOR_TEST_ONLY

class Cgroup {
public:
    Cgroup(const SandboxPolicy& policy, std::string_view operation_id)
        : root_(open_absolute(policy.cgroup_root, true)),
          name_("wls-op-" + sha256_hex(operation_id).substr(0, 32)) {
        struct statfs filesystem{};
        if (::fstatfs(root_.get(), &filesystem) != 0) system_error("stat cgroup filesystem");
        if (filesystem.f_type != CGROUP2_SUPER_MAGIC)
            throw Error(ErrorCode::Unsupported, "delegated root is not cgroup v2");
        const auto enabled = read_at(root_.get(), "cgroup.subtree_control");
        if (!sandbox_detail::missing_controllers(enabled).empty())
            throw Error(ErrorCode::Unsupported, "delegated cgroup lacks enabled cpu/memory/pids controllers");
        if (!sandbox_detail::cgroup_is_domain(read_at(root_.get(), "cgroup.type")))
            throw Error(ErrorCode::Unsupported, "delegated cgroup must be a domain");
        if (::mkdirat(root_.get(), name_.c_str(), 0700) != 0) {
            if (errno == EEXIST) throw Error(ErrorCode::UnknownOutcome, "operation cgroup already exists; reconcile it before any retry");
            system_error("create operation cgroup");
        }
        created_ = true;
        try {
            directory_ = Fd(::openat(root_.get(), name_.c_str(), O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC));
            if (!directory_) system_error("open operation cgroup");
            write_at(directory_.get(), "memory.max", std::to_string(policy.memory_max_bytes));
            write_at(directory_.get(), "memory.swap.max", "0");
            write_at(directory_.get(), "memory.oom.group", "1");
            write_at(directory_.get(), "pids.max", std::to_string(policy.pids_max));
            write_at(directory_.get(), "cpu.max", std::to_string(policy.cpu_quota_us) + " " + std::to_string(policy.cpu_period_us));
            kill_ = Fd(::openat(directory_.get(), "cgroup.kill", O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
            procs_ = Fd(::openat(directory_.get(), "cgroup.procs", O_WRONLY | O_CLOEXEC | O_NOFOLLOW));
            if (!kill_ || !procs_) system_error("open cgroup lifecycle controls");
            if (!empty()) throw Error(ErrorCode::UnknownOutcome, "new operation cgroup is unexpectedly populated");
        } catch (...) {
            (void)::unlinkat(root_.get(), name_.c_str(), AT_REMOVEDIR);
            created_ = false;
            throw;
        }
    }
    ~Cgroup() {
        if (!created_) return;
        // Exception unwinding also attempts containment. An unkillable or
        // unconfirmed group survives rmdir for later host reconciliation.
        try { if (!empty() && kill_) kill_all(); } catch (...) {}
        // rmdir refuses populated groups. Never recursively remove unknown
        // state or delete other operation groups during this destructor.
        (void)::unlinkat(root_.get(), name_.c_str(), AT_REMOVEDIR);
    }
    int procs() const noexcept { return procs_.get(); }
    const std::string& name() const noexcept { return name_; }
    void kill_all() {
        if (::lseek(kill_.get(), 0, SEEK_SET) < 0) system_error("seek cgroup kill control");
        write_all(kill_.get(), "1");
    }
    bool empty() const {
        const auto events = read_at(directory_.get(), "cgroup.events");
        std::istringstream stream(events);
        for (std::string key, value; stream >> key >> value; ) {
            if (key == "populated") {
                if (value == "0") return true;
                if (value == "1") return false;
                break;
            }
        }
        throw Error(ErrorCode::Corrupt, "cgroup.events has no valid populated state");
    }
private:
    Fd root_;
    Fd directory_;
    Fd kill_;
    Fd procs_;
    std::string name_;
    bool created_{false};
};

std::string base64(std::string_view bytes) {
    if (bytes.empty()) return {};
    std::string encoded(4 * ((bytes.size() + 2) / 3), '\0');
    const int size = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(encoded.data()),
        reinterpret_cast<const unsigned char*>(bytes.data()), static_cast<int>(bytes.size()));
    if (size < 0 || static_cast<std::size_t>(size) != encoded.size())
        throw Error(ErrorCode::Storage, "encode process output");
    return encoded;
}

std::string display_text(std::string_view bytes) {
    // Preserve ordinary UTF-8 exactly; malformed byte sequences are replaced
    // only in the display field. Base64 fields retain the original bytes.
    std::string result;
    for (std::size_t i = 0; i < bytes.size();) {
        const auto first = static_cast<unsigned char>(bytes[i]);
        std::size_t length = 1;
        std::uint32_t codepoint = first;
        if (first >= 0xc2 && first <= 0xdf) { length = 2; codepoint = first & 0x1fU; }
        else if (first >= 0xe0 && first <= 0xef) { length = 3; codepoint = first & 0x0fU; }
        else if (first >= 0xf0 && first <= 0xf4) { length = 4; codepoint = first & 0x07U; }
        bool valid = first < 0x80 || length > 1;
        if (i + length > bytes.size()) valid = false;
        if (valid) for (std::size_t j = 1; j < length; ++j) {
            const auto next = static_cast<unsigned char>(bytes[i + j]);
            if ((next & 0xc0U) != 0x80U) { valid = false; break; }
            codepoint = (codepoint << 6U) | (next & 0x3fU);
        }
        if ((length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
            (length == 4 && codepoint < 0x10000) || (codepoint >= 0xd800 && codepoint <= 0xdfff) || codepoint > 0x10ffff)
            valid = false;
        if (valid) { result.append(bytes.substr(i, length)); i += length; }
        else { result += "\xef\xbf\xbd"; ++i; }
    }
    return result;
}

#endif
} // namespace

#ifndef WLS_SANDBOX_SUPERVISOR_TEST_ONLY

std::string_view process_status_name(ProcessStatus status) noexcept {
    switch (status) {
    case ProcessStatus::Refused: return "REFUSED";
    case ProcessStatus::Exited: return "EXITED";
    case ProcessStatus::Signalled: return "SIGNALLED";
    case ProcessStatus::TimedOut: return "TIMED_OUT";
    case ProcessStatus::Cancelled: return "CANCELLED";
    case ProcessStatus::OutputLimit: return "OUTPUT_LIMIT";
    case ProcessStatus::SpawnFailed: return "SPAWN_FAILED";
    case ProcessStatus::CleanupUnknown: return "CLEANUP_UNKNOWN";
    }
    return "CLEANUP_UNKNOWN";
}

Json to_json(const ProcessResult& result) {
    boost::json::object value{
        {"status", process_status_name(result.status)}, {"spawned", result.spawned},
        {"launcher_exec_confirmed", result.launcher_exec_confirmed},
        {"strong_isolation", result.strong_isolation},
        {"all_descendants_exited", result.all_descendants_exited},
        {"output_truncated", result.output_truncated},
        {"stdout_text", display_text(result.stdout_text)}, {"stderr_text", display_text(result.stderr_text)},
        {"stdout_base64", base64(result.stdout_text)}, {"stderr_base64", base64(result.stderr_text)},
        {"error", result.error}, {"cgroup_name", result.cgroup_name}, {"evidence", result.evidence},
        {"exit_code", result.exit_code ? Json(*result.exit_code) : Json(nullptr)},
        {"signal", result.signal ? Json(*result.signal) : Json(nullptr)}
    };
    return value;
}

// R2a.3 read-only structured diagnostics are defined below.
namespace {

struct DoctorText {
    std::string value;
    bool truncated{false};
    bool transformed{false};
};

DoctorText doctor_display(std::string_view text, std::size_t maximum = 4096) {
    // Bound the work before UTF-8 display conversion, including trusted C++
    // callers which can construct policy strings larger than CLI inputs.
    auto prefix_bytes = std::min(text.size(), maximum);
    if (prefix_bytes < text.size())
        while (prefix_bytes > 0 && (static_cast<unsigned char>(text[prefix_bytes]) & 0xc0U) == 0x80U) --prefix_bytes;
    const auto prefix = text.substr(0, prefix_bytes);
    auto shown = display_text(prefix);
    bool truncated = prefix_bytes != text.size();
    const bool transformed = shown != prefix;
    if (shown.size() > maximum) {
        auto length = maximum;
        while (length > 0 && (static_cast<unsigned char>(shown[length]) & 0xc0U) == 0x80U) --length;
        shown.resize(length);
        truncated = true;
    }
    return {std::move(shown), truncated, transformed};
}

std::string doctor_text(std::string_view text, std::size_t maximum = 4096) {
    return doctor_display(text, maximum).value;
}

boost::json::object doctor_check(std::string_view id, std::string_view kind,
        std::string_view path, bool required, Json expected = boost::json::object{},
        std::string_view destination = {}, std::string_view path_kind = "explicit_path") {
    const auto shown_path = doctor_display(path);
    const auto shown_destination = doctor_display(destination);
    return {{"id", id}, {"kind", kind}, {"path", shown_path.value},
            {"path_kind", path_kind}, {"path_truncated", shown_path.truncated},
            {"path_display_transformed", shown_path.transformed},
            {"destination", shown_destination.value}, {"destination_truncated", shown_destination.truncated},
            {"destination_display_transformed", shown_destination.transformed},
            {"stage", "prerequisite"}, {"status", "observed_ok"},
            {"required_for_prerequisites", required}, {"expected", std::move(expected)},
            {"observed", boost::json::object{}}, {"reasons", boost::json::array{}}};
}

void doctor_reason(boost::json::object& check, std::string_view code,
        std::string_view message, std::string_view stage,
        std::optional<int> number = std::nullopt) {
    check["stage"] = stage;
    if (code == "read_budget_exceeded" || code == "dependency_not_checked" || code == "operation_not_created")
        check["status"] = "not_checked";
    else if (code == "path_unreadable" || code == "probe_io_failed") check["status"] = "unavailable";
    else check["status"] = "blocked";
    check.at("reasons").as_array().push_back(sandbox_detail::reason(code, message, number));
}

void doctor_failure(boost::json::object& check, const std::exception& error) {
    if (const auto* diagnostic = dynamic_cast<const DiagnosticError*>(&error))
        doctor_reason(check, diagnostic->reason, diagnostic->what(), diagnostic->stage, diagnostic->number);
    else doctor_reason(check, "probe_io_failed", error.what(), "probe");
}

void doctor_append(SandboxDoctor& result, boost::json::object check) {
    const auto status = check.at("status").as_string();
    const bool required = check.at("required_for_prerequisites").as_bool();
    if (status == "not_checked") {
        ++result.not_checked_count;
        if (required) result.complete = false;
    }
    if (required && status != "observed_ok") {
        for (const auto& item : check.at("reasons").as_array())
            result.issues.emplace_back(item.at("message").as_string());
    }
    result.checks.as_array().push_back(std::move(check));
}

std::string doctor_read(int fd, std::size_t maximum) {
    std::string text;
    std::array<char, 4096> buffer{};
    while (text.size() < maximum) {
        const auto count = ::read(fd, buffer.data(), std::min(buffer.size(), maximum - text.size()));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) diagnostic_system_error("read diagnostic control", "read");
        if (count == 0) return text;
        text.append(buffer.data(), static_cast<std::size_t>(count));
    }
    diagnostic_error(ErrorCode::Quota, "read_budget_exceeded", "read",
                     "diagnostic control did not reach EOF within its bounded read");
}

std::string doctor_read_at(int root, const char* name) {
    Fd file(::openat(root, name, O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK));
    if (!file) diagnostic_system_error(std::string("open ") + name, "open");
    return doctor_read(file.get(), 16384);
}

std::uint64_t doctor_count(const boost::json::object& value, std::string_view key) {
    const auto* found = value.if_contains(key);
    if (found == nullptr) return 0;
    if (found->is_uint64()) return found->as_uint64();
    if (found->is_int64() && found->as_int64() >= 0) return static_cast<std::uint64_t>(found->as_int64());
    return 0;
}

} // namespace

void bound_sandbox_doctor_report(Json& report) {
    using namespace sandbox_detail;
    auto& object = report.as_object();
    // The budget fields may be absent in a caller-supplied report. Reserve
    // before retaining references to its nested checks/issues values.
    object.reserve(object.size() + 8);
    auto& checks = object.at("checks").as_array();
    auto& issues = object.at("issues").as_array();
    const bool previously_truncated = object.if_contains("report_truncated") != nullptr && object.at("report_truncated").as_bool();
    bool truncated = previously_truncated;
    bool budget_counted = previously_truncated;
    std::uint64_t omitted = doctor_count(object, "omitted_check_count");
    auto not_checked = doctor_count(object, "not_checked_count");
    const auto remove_check = [&] {
        const auto& item = checks.back().as_object();
        if (item.at("status") != "not_checked") ++not_checked;
        checks.pop_back();
        ++omitted;
        truncated = true;
    };
    while (checks.size() > doctor_max_checks) remove_check();
    for (auto& item : checks) {
        auto& reasons = item.at("reasons").as_array();
        if (reasons.size() > doctor_max_reasons) {
            const auto removed = reasons.size() - doctor_max_reasons;
            reasons.resize(doctor_max_reasons);
            item.as_object()["omitted_reason_count"] = removed;
            truncated = true;
        }
        // Adding omitted_reason_count may relocate this check's members.
        for (auto& explanation : item.at("reasons").as_array()) {
            if (const auto* marked = explanation.as_object().if_contains("message_truncated"); marked != nullptr && marked->as_bool())
                truncated = true;
            auto& message = explanation.at("message");
            if (message.as_string().size() > doctor_max_error_bytes) {
                message = bounded_message(message.as_string());
                explanation.as_object()["message_truncated"] = true;
                truncated = true;
            }
        }
    }
    if (issues.size() > doctor_max_checks) { issues.resize(doctor_max_checks); truncated = true; }
    for (auto& issue : issues) {
        if (issue.as_string().size() > doctor_max_error_bytes) {
            issue = bounded_message(issue.as_string());
            truncated = true;
        }
    }
    const auto mark_truncated = [&] {
        if (!budget_counted) { ++not_checked; budget_counted = true; }
        object["complete"] = false;
        object["strong_prerequisites"] = false;
        object["report_truncated"] = true;
        object["omitted_check_count"] = omitted;
        object["not_checked_count"] = not_checked;
        object["report_budget_reason"] = reason("report_budget_exceeded",
            "doctor report exceeded its bounded diagnostic representation; omitted observations were not accepted");
    };
    if (truncated) mark_truncated();
    // Reserve room for the explicit truncation record before removing bounded
    // tail entries. Keep existing successful/failed observations in order.
    while (canonical_json(report).size() > doctor_max_report_bytes) {
        truncated = true;
        if (!checks.empty()) remove_check();
        auto& inputs = object.at("inputs").as_object();
        if (auto* files = inputs.if_contains("runtime_files"); files != nullptr && files->is_object()) {
            auto* items = files->as_object().if_contains("items");
            if (items != nullptr && items->is_array() && !items->as_array().empty()) {
                items->as_array().pop_back();
                files->as_object()["report_items_omitted"] = true;
            }
        }
        auto& evidence = object.at("evidence").as_object();
        if (auto* files = evidence.if_contains("runtime_files"); files != nullptr && files->is_array() && !files->as_array().empty()) {
            files->as_array().pop_back();
            evidence["runtime_file_observations_omitted"] = true;
        }
        mark_truncated();
        if (checks.empty() && canonical_json(report).size() > doctor_max_report_bytes) {
            // A caller may add oversized provenance after to_json. Retain the
            // stable old booleans and explicit absence of execution evidence.
            object["inputs"] = boost::json::object{{"omitted_due_to_report_budget", true}};
            object["evidence"] = boost::json::object{{"probe", "read-only-prerequisites"},
                {"programs_executed", 0}, {"namespace_execution_tested", false},
                {"weak_fallback_available", false}, {"network", "none"}, {"observations_omitted", true}};
            if (canonical_json(report).size() > doctor_max_report_bytes)
                throw Error(ErrorCode::InvalidInput, "doctor report contains unsupported oversized extension fields");
        }
    }
}

Json to_json(const SandboxDoctor& result) {
    boost::json::array issues;
    for (const auto& issue : result.issues) issues.emplace_back(issue);
    Json report = boost::json::object{
        {"linux_supported", result.linux_supported}, {"bwrap_available", result.bwrap_available},
        {"cgroup_v2", result.cgroup_v2}, {"delegated_root_ready", result.delegated_root_ready},
        {"runtime_files_ready", result.runtime_files_ready}, {"strong_prerequisites", result.strong_prerequisites},
        {"issues", std::move(issues)}, {"evidence", result.evidence},
        {"report_format", result.report_format}, {"inputs", result.inputs}, {"checks", result.checks},
        {"complete", result.complete}, {"not_checked_count", result.not_checked_count}, {"limitations", result.limitations},
        {"report_truncated", result.report_truncated}, {"omitted_check_count", result.omitted_check_count},
        {"report_budget_reason", result.report_budget_reason}
    };
    bound_sandbox_doctor_report(report);
    return report;
}

SandboxDoctor sandbox_doctor(const SandboxPolicy& policy) {
    using namespace sandbox_detail;
    SandboxDoctor result;
    result.linux_supported = true;
    auto& evidence = result.evidence.as_object();
    evidence["probe"] = "read-only-prerequisites";
    evidence["programs_executed"] = 0;
    evidence["namespace_execution_tested"] = false;
    evidence["weak_fallback_available"] = false;
    evidence["network"] = "none";
    result.limitations = boost::json::array{
        "No program, launcher version, template environment check or network request was executed.",
        "Namespace, mount, network isolation, resource enforcement and descendant exit require a separate real execution proof.",
        "Runtime dependency completeness and future cgroup creation cannot be established by these read-only observations.",
        "No Store, writer lock, request takeover, allocation or cleanup was opened or performed. Ordinary reads may update filesystem atime."
    };
    auto& inputs = result.inputs.as_object();
    const auto launcher_path = doctor_display(policy.bwrap_path.native());
    inputs["launcher"] = boost::json::object{{"origin", "policy"}, {"path", launcher_path.value},
        {"path_truncated", launcher_path.truncated}, {"path_display_transformed", launcher_path.transformed},
        {"default_path", "/usr/bin/bwrap"}, {"expected_sha256", doctor_text(policy.expected_bwrap_sha256, 64)}};
    const auto cgroup_path = doctor_display(policy.cgroup_root.native());
    inputs["cgroup"] = boost::json::object{{"origin", policy.cgroup_root.empty() ? "not_configured" : "policy"},
        {"path", policy.cgroup_root.empty() ? Json(nullptr) : Json(cgroup_path.value)},
        {"path_truncated", cgroup_path.truncated}, {"path_display_transformed", cgroup_path.transformed},
        {"hierarchy_observation_path", policy.cgroup_root.empty() ? Json("/sys/fs/cgroup") : Json(nullptr)}};

    auto policy_check = doctor_check("policy.limits", "policy", "", true);
    bool policy_valid = true;
    try { validate_policy_limits(policy); }
    catch (const std::exception& error) { policy_valid = false; doctor_failure(policy_check, error); }
    doctor_append(result, std::move(policy_check));

    auto launcher_check = doctor_check("launcher.file", "launcher", policy.bwrap_path.native(), true,
        boost::json::object{{"sha256", doctor_text(policy.expected_bwrap_sha256, 64)}, {"root_owned", true},
                            {"executable", true}, {"elf", true}, {"max_bytes", runtime_max_file_bytes}});
    RuntimeReadBudget launcher_budget(runtime_max_file_bytes);
    try {
        auto& observed = launcher_check.at("observed").as_object();
        const auto launcher = pin_file(policy.bwrap_path, policy.expected_bwrap_sha256, true, false, &launcher_budget, &observed);
        result.bwrap_available = true;
        evidence["bwrap_sha256"] = launcher.sha256;
        evidence["bwrap_version"] = "not-executed";
        if (!hash_string(policy.expected_bwrap_sha256))
            doctor_reason(launcher_check, "expected_hash_missing", "a frozen bubblewrap SHA-256 is required for strong execution", "expected_hash");
    } catch (const std::exception& error) { doctor_failure(launcher_check, error); }
    doctor_append(result, std::move(launcher_check));

    auto manifest_check = doctor_check("runtime.manifest", "runtime_manifest", "", true,
        boost::json::object{{"max_files", 128}, {"max_file_bytes", runtime_max_file_bytes}, {"max_total_bytes", runtime_max_total_bytes}});
    manifest_check["observed"] = boost::json::object{{"file_count", policy.runtime_files.size()}};
    if (policy.runtime_files.empty())
        doctor_reason(manifest_check, "runtime_manifest_empty", "no trusted runtime file manifest configured", "manifest");
    else if (policy.runtime_files.size() > 128)
        doctor_reason(manifest_check, "policy_unsupported", "runtime file count exceeds 128", "manifest");
    doctor_append(result, std::move(manifest_check));

    boost::json::array runtime_inputs;
    boost::json::array runtime_evidence;
    RuntimeReadBudget runtime_budget(runtime_max_total_bytes);
    bool all_files_read = !policy.runtime_files.empty() && policy.runtime_files.size() <= 128;
    std::set<std::string> destinations;
    const auto count = std::min<std::size_t>(policy.runtime_files.size(), 128);
    for (std::size_t index = 0; index < count; ++index) {
        const auto& file = policy.runtime_files[index];
        const auto source_path = doctor_display(file.source.native());
        const auto destination_path = doctor_display(file.destination);
        runtime_inputs.push_back(boost::json::object{{"index", index}, {"source", source_path.value},
            {"destination", destination_path.value}, {"expected_sha256", doctor_text(file.expected_sha256, 64)},
            {"source_truncated", source_path.truncated}, {"source_display_transformed", source_path.transformed},
            {"destination_truncated", destination_path.truncated}, {"destination_display_transformed", destination_path.transformed}});
        auto check = doctor_check("runtime." + std::to_string(index), "runtime_file", file.source.native(), true,
            boost::json::object{{"sha256", doctor_text(file.expected_sha256, 64)}, {"max_bytes", runtime_max_file_bytes}}, file.destination);
        try {
            (void)path_parts(file.source.native(), true);
            validate_destination(file.destination);
            if (!destinations.insert(file.destination).second)
                diagnostic_error(ErrorCode::InvalidInput, "destination_duplicate", "destination", "duplicate runtime mount destination");
            auto& observed = check.at("observed").as_object();
            const auto pinned = pin_file(file.source, file.expected_sha256, false, false, &runtime_budget, &observed);
            runtime_evidence.push_back(boost::json::object{{"destination", file.destination}, {"sha256", pinned.sha256}, {"bytes", pinned.size}});
            if (!hash_string(file.expected_sha256))
                doctor_reason(check, "expected_hash_missing", "a frozen SHA-256 is missing for runtime destination " + doctor_text(file.destination), "expected_hash");
        } catch (const std::exception& error) { all_files_read = false; doctor_failure(check, error); }
        doctor_append(result, std::move(check));
    }
    if (policy.runtime_files.size() > count) {
        result.not_checked_count += policy.runtime_files.size() - count;
        result.complete = false;
    }
    inputs["runtime_files"] = boost::json::object{{"origin", "policy"}, {"provided_count", policy.runtime_files.size()},
        {"omitted_count", policy.runtime_files.size() - count}, {"items", std::move(runtime_inputs)}};
    evidence["runtime_files"] = std::move(runtime_evidence);
    evidence["runtime_bytes_read"] = runtime_budget.consumed();
    evidence["runtime_read_budget_remaining"] = runtime_budget.remaining();
    result.runtime_files_ready = all_files_read;

    auto root_check = doctor_check("cgroup.root", "cgroup_root", policy.cgroup_root.native(), true,
        boost::json::object{{"filesystem", "cgroup2"}, {"dedicated_subtree", true}});
    Fd cgroup_root;
    if (policy.cgroup_root.empty()) {
        doctor_reason(root_check, "cgroup_not_configured", "no dedicated delegated cgroup v2 root configured", "configuration");
        auto hierarchy = doctor_check("cgroup.hierarchy", "cgroup_hierarchy", "/sys/fs/cgroup", false,
            boost::json::object{{"filesystem", "cgroup2"}}, {}, "fixed_auxiliary_path");
        try {
            const auto root = open_absolute("/sys/fs/cgroup", true);
            struct statfs filesystem{};
            if (::fstatfs(root.get(), &filesystem) != 0) diagnostic_system_error("stat cgroup filesystem", "filesystem");
            result.cgroup_v2 = filesystem.f_type == CGROUP2_SUPER_MAGIC;
            hierarchy["observed"] = boost::json::object{{"filesystem_type", static_cast<std::int64_t>(filesystem.f_type)},
                {"cgroup_v2", result.cgroup_v2}, {"delegation_observed", false}};
            if (!result.cgroup_v2) doctor_reason(hierarchy, "not_cgroup_v2", "observed hierarchy is not cgroup v2", "filesystem");
        } catch (const std::exception& error) { doctor_failure(hierarchy, error); }
        doctor_append(result, std::move(hierarchy));
    } else {
        try {
            (void)path_parts(policy.cgroup_root.native(), true);
            if (policy.cgroup_root == "/" || policy.cgroup_root == "/sys/fs/cgroup")
                diagnostic_error(ErrorCode::Unauthorized, "path_refused", "path", "supply a dedicated delegated cgroup directory, not the hierarchy root");
            cgroup_root = open_absolute(policy.cgroup_root, true);
            struct statfs filesystem{};
            if (::fstatfs(cgroup_root.get(), &filesystem) != 0) diagnostic_system_error("stat cgroup filesystem", "filesystem");
            result.cgroup_v2 = filesystem.f_type == CGROUP2_SUPER_MAGIC;
            root_check["observed"] = boost::json::object{{"filesystem_type", static_cast<std::int64_t>(filesystem.f_type)}, {"cgroup_v2", result.cgroup_v2}};
            require_cgroup_v2(result.cgroup_v2);
        } catch (const std::exception& error) { doctor_failure(root_check, error); cgroup_root = Fd{}; }
    }
    doctor_append(result, std::move(root_check));

    bool delegated = static_cast<bool>(cgroup_root);
    for (const auto* part : {"mount", "access", "cgroup.procs", "cgroup.subtree_control", "cgroup.type"}) {
        const std::string leaf(part);
        const auto path = policy.cgroup_root.empty() ? std::string{} :
            (leaf == "mount" || leaf == "access" ? cgroup_path.value : cgroup_path.value + "/" + leaf);
        auto check = doctor_check("cgroup." + leaf, "cgroup_control", path, true);
        if (!cgroup_root) {
            doctor_reason(check, "dependency_not_checked", "cgroup.root did not establish a configured cgroup v2 directory", "dependency");
            check["depends_on"] = "cgroup.root";
        } else {
            try {
                if (leaf == "mount") {
                    struct statvfs mount{};
                    if (::fstatvfs(cgroup_root.get(), &mount) != 0) diagnostic_system_error("stat cgroup mount", "mount");
                    const bool read_only = (mount.f_flag & ST_RDONLY) != 0;
                    check["observed"] = boost::json::object{{"read_only", read_only}};
                    require_cgroup_writable_mount(read_only);
                } else if (leaf == "access" || leaf == "cgroup.procs") {
                    const int access = leaf == "access" ? W_OK | X_OK : W_OK;
                    const char* target = leaf == "access" ? "." : "cgroup.procs";
                    const int access_result = ::faccessat(cgroup_root.get(), target, access, AT_EACCESS);
                    const int access_errno = access_result == 0 ? 0 : errno;
                    require_cgroup_access(access_result, access_errno);
                    check["observed"] = boost::json::object{{"access_permitted_now", true}, {"write_attempted", false}};
                } else if (leaf == "cgroup.subtree_control") {
                    const auto enabled = doctor_read_at(cgroup_root.get(), part);
                    evidence["enabled_controllers"] = enabled;
                    boost::json::array missing;
                    for (const auto& controller : missing_controllers(enabled)) missing.emplace_back(controller);
                    check["observed"] = boost::json::object{{"enabled_controllers", enabled}, {"missing_controllers", missing}};
                    if (!missing.empty()) doctor_reason(check, "controller_missing", "cgroup root lacks one or more required cpu/memory/pids controllers", "controllers");
                } else {
                    const auto type = doctor_read_at(cgroup_root.get(), part);
                    check["observed"] = boost::json::object{{"type", type}};
                    if (!cgroup_is_domain(type)) doctor_reason(check, "not_domain", "cgroup root is not a domain", "type");
                }
            } catch (const std::exception& error) { doctor_failure(check, error); }
        }
        if (check.at("status") != "observed_ok") delegated = false;
        doctor_append(result, std::move(check));
    }
    result.delegated_root_ready = delegated;
    const auto operation_root = policy.cgroup_root.empty() ? std::string("<configured-cgroup-root>") : cgroup_path.value;
    for (const auto* name : {"memory.max", "memory.swap.max", "memory.oom.group", "pids.max", "cpu.max", "cgroup.kill", "cgroup.procs", "cgroup.events"}) {
        auto check = doctor_check(std::string("operation.") + name, "operation_control",
            operation_root + "/wls-op-<sha256(operation_id)[0:32]>/" + name, false,
            boost::json::object{}, {}, "operation_child_pattern");
        doctor_reason(check, "operation_not_created", "no operation cgroup was created or inspected by this command", "not_executed");
        doctor_append(result, std::move(check));
    }
    for (const auto* path : {"/proc/sys/user/max_user_namespaces", "/proc/sys/kernel/unprivileged_userns_clone",
                              "/proc/sys/kernel/apparmor_restrict_unprivileged_userns"}) {
        auto check = doctor_check(std::string("auxiliary.") + path, "kernel_auxiliary", path, false,
            boost::json::object{{"max_bytes", 128}}, {}, "fixed_auxiliary_path");
        try {
            const auto file = open_absolute(path, false);
            const auto value = doctor_read(file.get(), 128);
            evidence[path] = value;
            check["observed"] = boost::json::object{{"text", doctor_text(value, 128)}, {"execution_verified", false}};
            const auto first = value.find_first_not_of(" \n\r\t");
            const auto last = value.find_last_not_of(" \n\r\t");
            const auto token = first == std::string::npos ? std::string_view(value).substr(0, 0) : std::string_view(value).substr(first, last - first + 1);
            std::uint64_t number = 0;
            const auto parsed = std::from_chars(token.data(), token.data() + token.size(), number);
            const bool boolean_setting = std::string_view(path) != "/proc/sys/user/max_user_namespaces";
            if (token.empty() || parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() || (boolean_setting && number > 1))
                doctor_reason(check, "auxiliary_value_unknown", "kernel auxiliary value is outside the known bounded integer form", "interpretation");
            else check.at("observed").as_object()["integer_value"] = number;
        } catch (const std::exception& error) { evidence[path] = "unavailable"; doctor_failure(check, error); }
        doctor_append(result, std::move(check));
    }
    bool required_ready = true;
    for (const auto& check : result.checks.as_array())
        if (check.at("required_for_prerequisites").as_bool() && check.at("status") != "observed_ok") required_ready = false;
    result.strong_prerequisites = policy_valid && result.complete && required_ready && result.bwrap_available &&
        result.runtime_files_ready && result.cgroup_v2 && result.delegated_root_ready && result.issues.empty();
    // Apply the serialization bound to the library result as well as JSON
    // callers; no public boolean may claim readiness after omitted evidence.
    const auto bounded = to_json(result);
    result.complete = bounded.at("complete").as_bool();
    result.strong_prerequisites = bounded.at("strong_prerequisites").as_bool();
    result.not_checked_count = static_cast<std::size_t>(doctor_count(bounded.as_object(), "not_checked_count"));
    result.report_truncated = bounded.at("report_truncated").as_bool();
    result.omitted_check_count = static_cast<std::size_t>(doctor_count(bounded.as_object(), "omitted_check_count"));
    result.report_budget_reason = bounded.at("report_budget_reason");
    result.inputs = bounded.at("inputs");
    result.checks = bounded.at("checks");
    result.evidence = bounded.at("evidence");
    result.issues.clear();
    for (const auto& issue : bounded.at("issues").as_array()) result.issues.emplace_back(issue.as_string());
    return result;
}

ProcessRunner::ProcessRunner(int workspace_root_fd, SandboxPolicy policy)
    : policy_(std::move(policy)) {
    validate_policy(policy_);
    Fd owned(::openat(workspace_root_fd, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!owned) system_error("pin workspace for process runner");
    check_workspace(owned.get());
    workspace_fd_ = owned.release();
}

ProcessRunner::ProcessRunner(const std::filesystem::path& workspace_root, SandboxPolicy policy)
    : policy_(std::move(policy)) {
    validate_policy(policy_);
    auto path = open_absolute(workspace_root, true);
    Fd owned(::openat(path.get(), ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    if (!owned) system_error("pin workspace for process runner");
    check_workspace(owned.get());
    workspace_fd_ = owned.release();
}

ProcessRunner::~ProcessRunner() { if (workspace_fd_ >= 0) ::close(workspace_fd_); }
ProcessRunner::ProcessRunner(ProcessRunner&& other) noexcept
    : workspace_fd_(std::exchange(other.workspace_fd_, -1)), policy_(std::move(other.policy_)),
      cleanup_unknown_(other.cleanup_unknown_) {}
ProcessRunner& ProcessRunner::operator=(ProcessRunner&& other) noexcept {
    if (this != &other) {
        if (workspace_fd_ >= 0) ::close(workspace_fd_);
        workspace_fd_ = std::exchange(other.workspace_fd_, -1);
        policy_ = std::move(other.policy_);
        cleanup_unknown_ = other.cleanup_unknown_;
    }
    return *this;
}

SandboxDoctor ProcessRunner::doctor() const { return sandbox_doctor(policy_); }

ProcessResult ProcessRunner::run(const ProcessRequest& request, std::stop_token stop) const {
    ProcessResult result;
    bool locked = false;
    std::unique_lock instance_lock(run_mutex_, std::try_to_lock);
    result.evidence = boost::json::object{{"network", "none"}, {"weak_fallback_available", false}, {"side_effects_undone", false}};
    try {
        if (!instance_lock.owns_lock())
            throw Error(ErrorCode::NotReady, "process runner is busy");
        if (cleanup_unknown_)
            throw Error(ErrorCode::UnknownOutcome, "process runner has unresolved descendant cleanup");
        validate_request(request, policy_);
        check_workspace(workspace_fd_);
        if (::flock(workspace_fd_, LOCK_EX | LOCK_NB) != 0)
            throw Error(ErrorCode::Conflict, "workspace is busy with another file/process operation");
        locked = true;
        auto cwd = open_relative_directory(workspace_fd_, request.cwd);
        if (stop.stop_requested()) {
            result.status = ProcessStatus::Cancelled;
            result.all_descendants_exited = true;
            (void)::flock(workspace_fd_, LOCK_UN);
            return result;
        }
        const auto workspace_entries = verify_workspace_tree(workspace_fd_);
        result.evidence.as_object()["workspace_audited_entries"] = workspace_entries;
        if (policy_.cgroup_root.empty() || policy_.runtime_files.empty())
            throw Error(ErrorCode::Unsupported, "strong execution requires configured runtime files and a dedicated delegated cgroup root");
        if (!hash_string(policy_.expected_bwrap_sha256) ||
            std::any_of(policy_.runtime_files.begin(), policy_.runtime_files.end(), [](const auto& file) {
                return !hash_string(file.expected_sha256);
            }))
            throw Error(ErrorCode::InvalidInput, "strong execution requires frozen SHA-256 for bubblewrap and every runtime file");
        const auto workspace_before = observe_process_workspace(workspace_fd_);
        auto launcher = pin_file(policy_.bwrap_path, policy_.expected_bwrap_sha256, true, false);
        std::vector<PinnedFile> runtime_files;
        std::uint64_t total = 0;
        for (const auto& file : policy_.runtime_files) {
            auto pinned = pin_file(file.source, file.expected_sha256, false, true);
            total += pinned.size;
            if (total > 268435456) throw Error(ErrorCode::InvalidInput, "combined runtime files exceed 256 MiB bound");
            runtime_files.push_back(std::move(pinned));
        }
        const auto operation_id = request.operation_id.empty() ? new_id("op") : request.operation_id;
        result.cgroup_name = "wls-op-" + sha256_hex(operation_id).substr(0, 32);
        result.evidence.as_object()["operation_id"] = operation_id;
        result.evidence.as_object()["cgroup_name"] = result.cgroup_name;
        Cgroup group(policy_, operation_id);
        auto status_pipe = make_pipe();
        auto gate_pipe = make_start_pipe();
        ChildSpec child;
        child.executable_fd = launcher.fd.get();
        child.working_directory_fd = workspace_fd_;
        child.cgroup_procs_fd = group.procs();
        child.timeout = request.timeout;
        child.cleanup_timeout = policy_.cleanup_timeout;
        child.output_limit = request.output_limit_bytes;
        child.env = {"PATH=/usr/bin:/bin", "LANG=C"};
        child.argv = {"bwrap", "--unshare-user", "--unshare-pid", "--unshare-ipc", "--unshare-uts",
                      "--unshare-net", "--unshare-cgroup", "--disable-userns", "--assert-userns-disabled",
                      "--new-session", "--die-with-parent", "--cap-drop", "ALL", "--clearenv",
                      "--json-status-fd", "4", "--block-fd", "5", "--proc", "/proc", "--dev", "/dev",
                      "--tmpfs", "/tmp", "--dir", "/tmp/home", "--dir", "/input",
                      "--bind", "/proc/" + std::to_string(::getpid()) + "/fd/" + std::to_string(workspace_fd_), "/work"};
        child.mappings.push_back({status_pipe[1].get(), 4});
        // Never pass a host directory fd into bwrap's descendants. bwrap
        // closes --block-fd and --ro-bind-data fds before the command exec.
        child.mappings.push_back({gate_pipe[0].get(), 5});
        boost::json::array hashes;
        for (std::size_t i = 0; i < runtime_files.size(); ++i) {
            const auto number = static_cast<int>(10 + i);
            child.mappings.push_back({runtime_files[i].fd.get(), number});
            child.argv.insert(child.argv.end(), {"--perms", (runtime_files[i].mode & 0111) != 0 ? "0555" : "0444",
                "--ro-bind-data", std::to_string(number), policy_.runtime_files[i].destination});
            hashes.emplace_back(boost::json::object{{"destination", policy_.runtime_files[i].destination},
                {"sha256", runtime_files[i].sha256}, {"bytes", runtime_files[i].size}});
        }
        // Only workspace and private tmp are writable. The empty root and
        // /input directory are read-only, and no host /etc/home/sys is mounted.
        child.argv.insert(child.argv.end(), {"--remount-ro", "/", "--setenv", "HOME", "/tmp/home",
            "--setenv", "TMPDIR", "/tmp", "--chdir", request.cwd.empty() || request.cwd == "." ? "/work" : "/work/" + request.cwd});
        for (const auto& [key, value] : request.env) child.argv.insert(child.argv.end(), {"--setenv", key, value});
        child.argv.emplace_back("--");
        child.argv.insert(child.argv.end(), request.argv.begin(), request.argv.end());
        bool workspace_verified = false;
        bool namespaces_verified = false;
        bool released = false;
        struct stat workspace_identity{};
        struct stat host_root{};
        if (::fstat(workspace_fd_, &workspace_identity) != 0 || ::stat("/", &host_root) != 0)
            system_error("capture workspace mount identity");
        const auto start_gate = [&](std::string_view status, Clock::time_point deadline) {
            if (released) return;
            const auto newline = status.find('\n');
            if (newline == std::string_view::npos) return;
            const auto record = parse_json_strict(status.substr(0, newline), 16384);
            if (!record.is_object()) throw Error(ErrorCode::Corrupt, "invalid bwrap startup record");
            const auto* child_id = record.as_object().if_contains("child-pid");
            if (child_id == nullptr || !child_id->is_int64() || child_id->as_int64() <= 0 || child_id->as_int64() > INT_MAX)
                throw Error(ErrorCode::Corrupt, "missing bwrap child identity");
            const auto prefix = "/proc/" + std::to_string(child_id->as_int64());
            struct stat sandbox_root{};
            struct stat mounted_workspace{};
            // bwrap's normal bind source handling uses realpath(). Validate
            // the actual mounted inode after pivot_root and before releasing
            // --block-fd, so a source rename/replacement can only cause refusal.
            if (::stat((prefix + "/root").c_str(), &sandbox_root) != 0) {
                if (errno == ENOENT) return;
                system_error("cannot attest blocked sandbox root");
            }
            if (sandbox_root.st_dev == host_root.st_dev && sandbox_root.st_ino == host_root.st_ino) return;
            if (::stat((prefix + "/root/work").c_str(), &mounted_workspace) != 0) {
                if (errno == ENOENT) return;
                system_error("cannot attest blocked workspace mount");
            }
            if (!S_ISDIR(mounted_workspace.st_mode) || mounted_workspace.st_dev != workspace_identity.st_dev ||
                mounted_workspace.st_ino != workspace_identity.st_ino)
                throw Error(ErrorCode::Unauthorized, "actual sandbox workspace mount differs from its pinned root");
            const auto blocked_process = open_absolute(prefix, true);
            verify_workspace_mounts(read_at(blocked_process.get(), "mountinfo", 262144));
            for (const auto* ns : {"mnt", "user", "pid", "ipc", "uts", "net", "cgroup"}) {
                struct stat host_ns{};
                struct stat child_ns{};
                if (::stat((std::string("/proc/self/ns/") + ns).c_str(), &host_ns) != 0 ||
                    ::stat((prefix + "/ns/" + ns).c_str(), &child_ns) != 0)
                    system_error("cannot attest blocked namespace");
                if (host_ns.st_dev == child_ns.st_dev && host_ns.st_ino == child_ns.st_ino)
                    throw Error(ErrorCode::Unauthorized, "sandbox did not create every required namespace");
            }
            workspace_verified = true;
            namespaces_verified = true;
            if (stop.stop_requested() || Clock::now() >= deadline) return;
            if (::write(gate_pipe[1].get(), "x", 1) != 1)
                system_error("release attested sandbox start gate");
            released = true;
        };
        // supervise duplicates all inherited fds before fork. Its parent copy
        // of status_pipe[1] must close after fork for EOF to carry meaning.
        // The supervisor records an observed fork before any later allocation
        // can throw, preserving the distinction between pre-fork refusal and
        // an exception after a real child existed.
        auto supervised = supervise(child, { [&](pid_t) { group.kill_all(); }, [&] { return group.empty(); }, true },
                                     stop, status_pipe[0].get(), status_pipe[1].release(), start_gate, &result.spawned);
        result = std::move(supervised.process);
        result.cgroup_name = group.name();
        auto& evidence = result.evidence.as_object();
        evidence["network"] = "none";
        evidence["weak_fallback_available"] = false;
        evidence["operation_id"] = operation_id;
        evidence["exit_code_semantics"] = "launcher wait status; bwrap command exit uses shell encoding in bwrap_status";
        evidence["bwrap_sha256"] = launcher.sha256;
        evidence["runtime_files"] = std::move(hashes);
        evidence["workspace_audited_entries"] = workspace_entries;
        evidence["workspace_mount_policy"] = "one private rw/nosuid/nodev mount without submounts";
        evidence["cgroup_name"] = group.name();
        evidence["limits"] = boost::json::object{{"memory_max_bytes", policy_.memory_max_bytes},
            {"memory_swap_max", 0}, {"pids_max", policy_.pids_max}, {"cpu_quota_us", policy_.cpu_quota_us},
            {"cpu_period_us", policy_.cpu_period_us}, {"timeout_ms", request.timeout.count()},
            {"output_limit_bytes", request.output_limit_bytes}};
        boost::json::array status_records;
        bool reported_startup = false;
        bool reported_exit = false;
        try {
            std::istringstream lines(supervised.status_stream);
            for (std::string line; std::getline(lines, line); ) {
                if (line.empty()) continue;
                auto record = parse_json_strict(line, 16384);
                if (!record.is_object()) throw Error(ErrorCode::Corrupt, "bubblewrap status is not an object");
                if (const auto* child_id = record.as_object().if_contains("child-pid")) {
                    if (!child_id->is_int64() || child_id->as_int64() <= 0 || child_id->as_int64() > INT_MAX)
                        throw Error(ErrorCode::Corrupt, "invalid bubblewrap child identity");
                    reported_startup = true;
                }
                if (const auto* exit = record.as_object().if_contains("exit-code")) {
                    reported_exit = exit->is_int64() || exit->is_uint64();
                }
                status_records.emplace_back(std::move(record));
            }
        } catch (const std::exception& error) {
            add_error(result.error, error.what());
            result.status = ProcessStatus::CleanupUnknown;
        }
        // bwrap emits exit-code only after its own setup-finished handshake.
        // Launcher exec alone or a child-pid record is never sufficient proof.
        // Empty exec-error EOF alone is ambiguous if SIGKILL preceded exec;
        // production confirms launcher execution with bwrap's own record.
        result.launcher_exec_confirmed = result.launcher_exec_confirmed && reported_startup;
        result.strong_isolation = result.launcher_exec_confirmed && workspace_verified && namespaces_verified &&
                                  released && !supervised.status_overflow;
        evidence["workspace_mount_verified"] = workspace_verified;
        evidence["namespaces_verified"] = namespaces_verified;
        evidence["command_start_released"] = released;
        evidence["sandbox_setup_and_command_exit_reported"] = reported_exit;
        evidence["bwrap_status"] = std::move(status_records);
        if ((!result.strong_isolation || !reported_exit) && result.status == ProcessStatus::Exited) {
            result.status = ProcessStatus::SpawnFailed;
            add_error(result.error, "bubblewrap did not confirm sandbox setup and command exit");
        }
        if (result.strong_isolation && result.all_descendants_exited) {
            const auto after = observe_process_workspace(workspace_fd_);
            evidence["workspace_observation"] = process_workspace_changes(workspace_before, after);
        }
    } catch (const Error& error) {
        add_error(result.error, error.what());
        result.evidence.as_object()["error_code"] = error_name(error.code());
        result.status = result.spawned || error.code() == ErrorCode::UnknownOutcome ?
            ProcessStatus::CleanupUnknown : ProcessStatus::Refused;
    } catch (const std::exception& error) {
        add_error(result.error, error.what());
        result.status = result.spawned ? ProcessStatus::CleanupUnknown : ProcessStatus::Refused;
    }
    // On uncertain cleanup the descriptor keeps its cooperative lock until
    // the owning ProcessRunner is destroyed. Persistent UNKNOWN additionally
    // fences subsequent runtime instances; flock is not crash recovery proof.
    const bool unresolved_cleanup = !result.all_descendants_exited &&
        (result.spawned || result.status == ProcessStatus::CleanupUnknown);
    if (locked && !unresolved_cleanup)
        (void)::flock(workspace_fd_, LOCK_UN);
    if (instance_lock.owns_lock() && unresolved_cleanup)
        cleanup_unknown_ = true;
    return result;
}

#endif
} // namespace wls::s1
