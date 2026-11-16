#pragma once

#include "wls/runtime_types.hpp"
#include <fcntl.h>
#include <linux/openat2.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <cerrno>
#include <filesystem>
#include <vector>

namespace wls::s1::input_detail {
inline void require(bool condition, const char* message) {
    if (!condition) throw Error(ErrorCode::Unauthorized, message);
}
inline int pin_path(int parent, const char* path) {
    open_how how{};
    how.flags = O_PATH | O_CLOEXEC | O_NOFOLLOW;
    how.resolve = RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS;
    return static_cast<int>(::syscall(SYS_openat2, parent, path, &how, sizeof(how)));
}
inline Json identity(int fd, const std::string& path, bool input = false) {
    struct statx info{};
    constexpr unsigned mask = STATX_TYPE | STATX_MODE | STATX_UID | STATX_NLINK | STATX_INO | STATX_MNT_ID;
    require(::syscall(SYS_statx, fd, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_BASIC_STATS | STATX_MNT_ID, &info) == 0 &&
        (info.stx_mask & mask) == mask, "input boundary requires complete inode and mount identity");
    require(S_ISDIR(info.stx_mode) || S_ISREG(info.stx_mode), "input boundary refuses symbolic links and special objects");
    if (input) require(S_ISDIR(info.stx_mode) && info.stx_uid == ::geteuid() && (info.stx_mode & 0022U) == 0 &&
        info.stx_nlink > 0 && path != "/", "input root must be an owned directory without group/other write access");
    return boost::json::object{{"path", path}, {"device", static_cast<std::uint64_t>(makedev(info.stx_dev_major, info.stx_dev_minor))},
        {"inode", info.stx_ino}, {"mount_id", info.stx_mnt_id}};
}
// O_PATH never opens a data channel. Only retained/missing protected paths may
// resolve to an existing ancestor; an input root must itself exist.
inline Json identity(std::filesystem::path path, bool input = false, bool ancestor = false) {
    require(path.is_absolute() && path.string() == path.lexically_normal().string() && (path == "/" || path.string().back() != '/') &&
        path.string().find('\0') == std::string::npos &&
        path.string().size() <= 4096, "input boundary requires a bounded canonical absolute path");
    for (unsigned depth = 0; depth < 64; ++depth) {
        const int fd = pin_path(AT_FDCWD, path.c_str());
        if (fd >= 0) {
            struct Close { int value; ~Close() { ::close(value); } } close{fd};
            return identity(fd, path.string(), input);
        }
        require(ancestor && errno == ENOENT && path != "/", "input boundary cannot pin a path without symbolic links");
        path = path.parent_path();
    }
    throw Error(ErrorCode::Quota, "input boundary ancestor depth exceeded");
}
inline void match(const Json& pinned, const Json& actual) {
    for (const auto* key : {"path", "device", "inode", "mount_id"})
        require(pinned.at(key) == actual.at(key), "input root identity changed");
}
inline void validate(const Json& input) {
    if (!input.is_object() || input.as_object().size() != 6)
        throw Error(ErrorCode::InvalidInput, "invalid input policy fields");
    for (const auto* key : {"mode", "path", "device", "inode", "mount_id", "max_total_bytes"})
        if (!input.as_object().contains(key)) throw Error(ErrorCode::InvalidInput, "missing input policy field");
    if (input.at("mode") != "read-only-v1" || !input.at("path").is_string())
        throw Error(ErrorCode::InvalidInput, "invalid input policy mode or path");
    for (const auto* key : {"device", "inode", "mount_id", "max_total_bytes"}) {
        const auto& value = input.at(key);
        if ((!value.is_int64() && !value.is_uint64()) || (value.is_int64() && value.as_int64() <= 0) ||
            (value.is_uint64() && value.as_uint64() == 0)) throw Error(ErrorCode::InvalidInput, "invalid input policy identity or limit");
    }
    if (input.at("max_total_bytes").to_number<std::uint64_t>() > 268435456)
        throw Error(ErrorCode::InvalidInput, "input byte limit exceeds maximum");
    match(input, identity(std::string(input.at("path").as_string()), true));
}
inline void distinct(const Json& root, const Json& ancestor) {
    require(root.at("device") != ancestor.at("device") ||
        (root.at("mount_id") == ancestor.at("mount_id") && root.at("inode") != ancestor.at("inode")),
        "input root aliases a protected host object or uses an unsupported mount alias");
}
inline std::vector<Json> ancestors(const Json& observed) {
    const std::filesystem::path path(std::string(observed.at("path").as_string()));
    struct Close { int fd; ~Close() { if (fd >= 0) ::close(fd); } } current{pin_path(AT_FDCWD, path.c_str())};
    require(current.fd >= 0, "input boundary cannot pin ancestor chain");
    match(observed, identity(current.fd, path.string()));
    struct stat info{};
    require(::fstat(current.fd, &info) == 0, "input boundary cannot inspect ancestor type");
    if (!S_ISDIR(info.st_mode)) {
        Close parent{pin_path(AT_FDCWD, path.parent_path().c_str())};
        require(parent.fd >= 0, "input boundary cannot pin file parent");
        Close file{pin_path(parent.fd, path.filename().c_str())};
        require(file.fd >= 0, "input boundary cannot verify file parent");
        match(observed, identity(file.fd, path.string()));
        ::close(current.fd); current.fd = parent.fd; parent.fd = -1;
    }
    std::vector<Json> result;
    for (unsigned depth = 0; depth < 64; ++depth) {
        const auto child = identity(current.fd, "");
        result.push_back(child);
        Close parent{pin_path(current.fd, "..")};
        require(parent.fd >= 0, "input boundary cannot walk pinned parent");
        const auto next = identity(parent.fd, "");
        if (child == next) return result;
        ::close(current.fd); current.fd = parent.fd; parent.fd = -1;
    }
    throw Error(ErrorCode::Quota, "input boundary ancestor depth exceeded");
}
inline void distinct_chain(const Json& root, const std::vector<Json>& chain) {
    for (const auto& parent : chain) distinct(root, parent);
}
inline void separate(const Json& input, const std::string& protected_path, const Json& observed) {
    const std::string path(input.at("path").as_string());
    require(path != protected_path && !path.starts_with(protected_path + "/") && !protected_path.starts_with(path + "/"),
        "input root overlaps a protected host path");
    distinct(input, observed);
    // Compare each root with the opposite pinned ancestor chain. A protected
    // leaf may be on another device below an alias of the input's filesystem.
    distinct_chain(input, ancestors(observed));
    if (observed.at("path") == Json(protected_path))
        distinct_chain(observed, ancestors(input));
}
} // namespace wls::s1::input_detail
