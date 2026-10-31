#include "internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <linux/magic.h>
#include <openssl/evp.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <utility>

namespace wls::native {
namespace {
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
class Descriptor final {
public:
    explicit Descriptor(int value = -1) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) ::close(value_); }
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) { if (value_ >= 0) ::close(value_); value_ = std::exchange(other.value_, -1); }
        return *this;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return value_; }
private:
    int value_;
};

Descriptor child_directory(int parent, const std::string& name) {
    require(!name.empty() && name != "." && name != "..", ErrorCode::InvalidInput,
        "invalid public directory component");
    Descriptor next(::openat(parent, name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
    require(next.get() >= 0, errno == ENOENT ? ErrorCode::Missing : ErrorCode::Denied,
        "public resource directory is unavailable");
    struct stat info{};
    require(::fstat(next.get(), &info) == 0, ErrorCode::Io, "cannot inspect public directory");
    require(S_ISDIR(info.st_mode), ErrorCode::Denied, "public directory link or special object refused");
    return next;
}

bool same(const struct stat& first, const struct stat& second) {
    return first.st_dev == second.st_dev && first.st_ino == second.st_ino &&
        first.st_mode == second.st_mode && first.st_size == second.st_size &&
        first.st_nlink == second.st_nlink &&
        first.st_mtim.tv_sec == second.st_mtim.tv_sec && first.st_mtim.tv_nsec == second.st_mtim.tv_nsec &&
        first.st_ctim.tv_sec == second.st_ctim.tv_sec && first.st_ctim.tv_nsec == second.st_ctim.tv_nsec;
}

class Directory final : public system::PublicDirectory {
public:
    explicit Directory(const std::filesystem::path& root) {
        Descriptor current(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
        require(current.get() >= 0, ErrorCode::Io, "cannot inspect filesystem root");
        for (const auto& component : root.relative_path())
            current = child_directory(current.get(), component.native());
        root_ = std::move(current);
    }
    std::string read(std::string_view relative, std::size_t maximum) const override {
        const std::filesystem::path path{std::string(relative)};
        require(path.is_relative() && !path.empty() && relative.size() <= 512,
            ErrorCode::InvalidInput, "invalid registered public resource path");
        Descriptor parent(::fcntl(root_.get(), F_DUPFD_CLOEXEC, 0));
        require(parent.get() >= 0, ErrorCode::Io, "cannot retain public resource directory");
        for (const auto& component : path.parent_path())
            parent = child_directory(parent.get(), component.native());
        const auto name = path.filename().native();
        require(!name.empty() && name != "." && name != "..", ErrorCode::InvalidInput,
            "invalid registered public filename");
        Descriptor inspected(::openat(parent.get(), name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
        require(inspected.get() >= 0, errno == ENOENT ? ErrorCode::Missing : ErrorCode::Denied,
            "public resource file is unavailable");
        struct stat before{};
        require(::fstat(inspected.get(), &before) == 0, ErrorCode::Io, "cannot inspect public resource file");
        require(S_ISREG(before.st_mode) && before.st_nlink == 1, ErrorCode::Denied,
            "public resource must be a regular file with one link");
        require(before.st_size >= 0 && static_cast<std::uintmax_t>(before.st_size) <= maximum,
            ErrorCode::Mismatch, "public resource file exceeds bound");
        const auto pinned = "/proc/self/fd/" + std::to_string(inspected.get());
        Descriptor input(::open(pinned.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
        require(input.get() >= 0, ErrorCode::Denied, "cannot read pinned public resource file");
        struct stat opened{};
        require(::fstat(input.get(), &opened) == 0 && same(before, opened), ErrorCode::Mismatch,
            "public resource changed before reading");
        std::string output;
        std::array<char, 16384> chunk{};
        bool eof = false;
        while (output.size() <= maximum) {
            const auto length = std::min(chunk.size(), maximum + 1 - output.size());
            if (length == 0) break;
            const auto count = ::read(input.get(), chunk.data(), length);
            if (count < 0 && errno == EINTR) continue;
            require(count >= 0, ErrorCode::Io, "public resource read failed");
            if (count == 0) { eof = true; break; }
            output.append(chunk.data(), static_cast<std::size_t>(count));
        }
        struct stat after{};
        require(::fstat(input.get(), &after) == 0 && same(before, after), ErrorCode::Mismatch,
            "public resource changed during reading");
        require(eof && output.size() <= maximum && output.size() == static_cast<std::uintmax_t>(before.st_size),
            ErrorCode::Mismatch, "public resource EOF or size differs from observation");
        return output;
    }
private:
    Descriptor root_;
};
} // namespace

std::filesystem::path actual_executable() {
    std::array<char, 4097> buffer{};
    const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    require(count > 0 && static_cast<std::size_t>(count) <= 4096, ErrorCode::Io,
        "cannot locate bounded actual executable");
    return path_from_utf8(std::string_view(buffer.data(), static_cast<std::size_t>(count)));
}

HostFacts host_facts() {
    struct statfs info{};
    const auto executable = actual_executable();
    const auto filesystem = ::statfs(executable.c_str(), &info) == 0 && info.f_type == EXT4_SUPER_MAGIC ?
        "ext4" : "not_checked";
    return {"linux-verification-only", ::geteuid() == 0 ? "root" : "ordinary_uid", filesystem};
}

namespace system {
std::unique_ptr<PublicDirectory> open_public_directory(const std::filesystem::path& root) {
    return std::make_unique<Directory>(root);
}

std::string sha256(std::string_view bytes) {
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int length = 0;
    require(EVP_Digest(bytes.data(), bytes.size(), digest.data(), &length, EVP_sha256(), nullptr) == 1 &&
        length == 32, ErrorCode::Io, "SHA-256 failed");
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    for (std::size_t index = 0; index < length; ++index) {
        result += hex[digest[index] >> 4U];
        result += hex[digest[index] & 0x0fU];
    }
    return result;
}
} // namespace system
} // namespace wls::native
