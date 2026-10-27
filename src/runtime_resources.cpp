#include "wls/runtime_resources.hpp"

#include "wls/config_loader.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <fcntl.h>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace wls::s1 {
namespace {

using Object = boost::json::object;
using Array = boost::json::array;
constexpr std::size_t maximum_file_bytes = 1024U * 1024U;
constexpr std::size_t maximum_bundle_bytes = 8U * 1024U * 1024U;
constexpr std::size_t maximum_manifest_bytes = 64U * 1024U;
constexpr std::string_view bundle_format = "wls-runtime-resources-v1";

struct ResourceAsset {
    std::string_view path;
    std::string_view kind;
    std::string_view sha256;
    std::size_t size;
};

#include "runtime_resource_catalog.inc"

void require(bool valid, ErrorCode code, const std::string& message) {
    if (!valid) throw Error(code, message);
}

class Descriptor final {
public:
    explicit Descriptor(int descriptor = -1) : descriptor_(descriptor) {}
    ~Descriptor() { if (descriptor_ >= 0) ::close(descriptor_); }
    Descriptor(Descriptor&& other) noexcept : descriptor_(std::exchange(other.descriptor_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) {
            if (descriptor_ >= 0) ::close(descriptor_);
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    int get() const noexcept { return descriptor_; }
private:
    int descriptor_;
};

std::filesystem::path absolute_path(const std::filesystem::path& path) {
    const auto original = path.string();
    require(!original.empty() && original.size() <= 4096 && original.find('\0') == std::string::npos,
            ErrorCode::InvalidInput, "runtime resource path must be bounded nonempty text");
    std::filesystem::path result;
    try { result = std::filesystem::absolute(path).lexically_normal(); }
    catch (const std::filesystem::filesystem_error&) {
        throw Error(ErrorCode::InvalidInput, "cannot interpret runtime resource path");
    }
    require(result.string().size() <= 4096, ErrorCode::InvalidInput, "runtime resource path exceeds bound");
    (void)canonical_json(Object{{"path", result.string()}});
    return result;
}

Descriptor child_directory(int parent, const std::string& name) {
    Descriptor child(::openat(parent, name.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
    require(child.get() >= 0, ErrorCode::InvalidInput, "runtime resource directory is unavailable");
    struct stat status{};
    require(::fstat(child.get(), &status) == 0, ErrorCode::Storage, "cannot inspect runtime resource directory");
    require(S_ISDIR(status.st_mode), ErrorCode::Unauthorized,
            "runtime resource directories must not be symbolic links or special files");
    return child;
}

Descriptor directory_at(const std::filesystem::path& absolute) {
    Descriptor current(::open("/", O_PATH | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW));
    require(current.get() >= 0, ErrorCode::Storage, "cannot inspect runtime resource path root");
    for (const auto& component : absolute.relative_path()) {
        const auto name = component.string();
        require(!name.empty() && name != "." && name != "..", ErrorCode::InvalidInput,
                "runtime resource path must be normalized");
        current = child_directory(current.get(), name);
    }
    return current;
}

bool unchanged(const struct stat& before, const struct stat& after) {
    return before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_mode == after.st_mode && before.st_size == after.st_size &&
        before.st_mtim.tv_sec == after.st_mtim.tv_sec && before.st_mtim.tv_nsec == after.st_mtim.tv_nsec &&
        before.st_ctim.tv_sec == after.st_ctim.tv_sec && before.st_ctim.tv_nsec == after.st_ctim.tv_nsec;
}

std::string read_relative(int root, std::string_view relative, std::size_t maximum) {
    const std::filesystem::path path{std::string(relative)};
    require(!path.empty() && path.is_relative() && path.string().size() <= 512,
            ErrorCode::InvalidInput, "invalid registered runtime resource path");
    Descriptor parent(::fcntl(root, F_DUPFD_CLOEXEC, 0));
    require(parent.get() >= 0, ErrorCode::Storage, "cannot retain runtime resource root");
    for (const auto& component : path.parent_path()) {
        const auto name = component.string();
        require(!name.empty() && name != "." && name != "..", ErrorCode::InvalidInput,
                "runtime resource path traversal is forbidden");
        parent = child_directory(parent.get(), name);
    }
    const auto filename = path.filename().string();
    require(!filename.empty() && filename != "." && filename != "..", ErrorCode::InvalidInput,
            "invalid runtime resource filename");
    // Inspect with O_PATH before any data-reading open. A FIFO/device cannot
    // trigger blocking or device effects during a rejected resource load.
    Descriptor inspected(::openat(parent.get(), filename.c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
    require(inspected.get() >= 0, ErrorCode::Corrupt, "required runtime resource is missing: " + std::string(relative));
    struct stat before{};
    require(::fstat(inspected.get(), &before) == 0, ErrorCode::Storage, "cannot inspect runtime resource");
    require(S_ISREG(before.st_mode), ErrorCode::Unauthorized,
            "runtime resource must be a regular file without a symbolic link: " + std::string(relative));
    require(before.st_size >= 0 && static_cast<std::uintmax_t>(before.st_size) <= maximum,
            ErrorCode::Quota, "runtime resource exceeds byte bound: " + std::string(relative));
    const auto descriptor_path = "/proc/self/fd/" + std::to_string(inspected.get());
    Descriptor input(::open(descriptor_path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK));
    require(input.get() >= 0, ErrorCode::Storage, "cannot read inspected runtime resource");
    struct stat opened{};
    require(::fstat(input.get(), &opened) == 0 && unchanged(before, opened),
            ErrorCode::Corrupt, "runtime resource identity changed before reading");
    std::string result;
    std::array<char, 16384> chunk{};
    bool eof = false;
    while (result.size() <= maximum) {
        const auto available = std::min(chunk.size(), maximum + 1U - result.size());
        if (available == 0) break;
        const auto count = ::read(input.get(), chunk.data(), available);
        if (count < 0 && errno == EINTR) continue;
        require(count >= 0, ErrorCode::Storage, "runtime resource read failed");
        if (count == 0) { eof = true; break; }
        result.append(chunk.data(), static_cast<std::size_t>(count));
    }
    struct stat after{};
    require(::fstat(input.get(), &after) == 0 && unchanged(before, after), ErrorCode::Corrupt,
            "runtime resource changed during reading");
    require(eof && result.size() <= maximum && result.size() == static_cast<std::uintmax_t>(before.st_size),
            ErrorCode::Quota, "runtime resource EOF or size exceeds verified bounds");
    return result;
}

Json expected_manifest() {
    Array assets;
    for (const auto& asset : resource_assets)
        assets.push_back(Object{{"path", asset.path}, {"kind", asset.kind}, {"sha256", asset.sha256},
            {"size_bytes", static_cast<std::int64_t>(asset.size)}});
    return Object{{"format", bundle_format}, {"store_version", 410}, {"assets", std::move(assets)}};
}

bool manifest_present(const std::filesystem::path& path) {
    std::error_code failure;
    const auto status = std::filesystem::symlink_status(path, failure);
    if (failure == std::errc::no_such_file_or_directory || failure == std::errc::not_a_directory) return false;
    require(!failure, ErrorCode::Unauthorized, "cannot inspect executable-relative runtime resource manifest");
    return status.type() != std::filesystem::file_type::not_found;
}

std::filesystem::path actual_executable() {
    std::array<char, 4097> path{};
    const auto count = ::readlink("/proc/self/exe", path.data(), path.size());
    require(count > 0 && static_cast<std::size_t>(count) < path.size(), ErrorCode::Unsupported,
            "cannot determine a bounded actual executable path; supply an explicit resource root");
    const std::string value(path.data(), static_cast<std::size_t>(count));
    require(!value.ends_with(" (deleted)"), ErrorCode::Unsupported,
            "deleted executable requires an explicit runtime resource root");
    return absolute_path(value);
}

} // namespace

RuntimeResources::RuntimeResources(const std::filesystem::path& root, bool source_tree)
    : root_(absolute_path(root)), source_tree_(source_tree) {
    static_assert(resource_assets.size() <= 128);
    auto directory = directory_at(root_);
    const auto manifest_bytes = read_relative(directory.get(),
        source_tree_ ? "resources/runtime-manifest.json" : "runtime-manifest.json", maximum_manifest_bytes);
    const auto manifest = parse_json_strict(manifest_bytes, maximum_manifest_bytes, 8);
    require(manifest.is_object(), ErrorCode::InvalidInput, "runtime resource manifest must be an object");
    const auto* format = manifest.as_object().if_contains("format");
    require(format != nullptr && format->is_string() && format->as_string() == bundle_format,
            ErrorCode::Unsupported, "unsupported runtime resource bundle format");
    const auto expected = expected_manifest();
    require(manifest == expected, ErrorCode::Corrupt,
            "runtime resource manifest differs from compiled registrations");
    manifest_file_sha256_ = sha256_hex(manifest_bytes);
    bundle_sha256_ = sha256_hex(canonical_json(expected));
    std::size_t total = 0;
    for (const auto& asset : resource_assets) {
        require(asset.size <= maximum_file_bytes && asset.size <= maximum_bundle_bytes - total,
                ErrorCode::Quota, "compiled runtime resources exceed bundle limits");
        auto content = read_relative(directory.get(), asset.path, maximum_file_bytes);
        require(content.size() == asset.size && sha256_hex(content) == asset.sha256,
                ErrorCode::Corrupt, "runtime resource differs from compiled identity: " + std::string(asset.path));
        total += content.size();
        require(assets_.emplace(std::string(asset.path), std::move(content)).second,
                ErrorCode::Corrupt, "duplicate compiled runtime resource path");
    }
}

RuntimeResources RuntimeResources::from_source_tree(const std::filesystem::path& root) {
    return RuntimeResources(root, true);
}

RuntimeResources RuntimeResources::from_bundle(const std::filesystem::path& root) {
    return RuntimeResources(root, false);
}

RuntimeResources RuntimeResources::discover(const std::filesystem::path& supplied_executable) {
    require(supplied_executable.empty() || supplied_executable.is_absolute(), ErrorCode::InvalidInput,
            "explicit executable resource discovery needs an absolute path");
    const auto executable = supplied_executable.empty() ? actual_executable() : absolute_path(supplied_executable);
    auto parent = directory_at(executable.parent_path());
    Descriptor inspected(::openat(parent.get(), executable.filename().c_str(), O_PATH | O_CLOEXEC | O_NOFOLLOW));
    struct stat status{};
    require(inspected.get() >= 0 && ::fstat(inspected.get(), &status) == 0 && S_ISREG(status.st_mode),
            ErrorCode::Unauthorized, "resource discovery requires a regular actual executable");
    const auto directory = executable.parent_path();
    const std::vector<std::pair<std::filesystem::path, bool>> candidates{
        {(directory / "../share/wls861").lexically_normal(), false},
        {directory / "share/wls861", false},
        {directory, true},
        {directory.parent_path(), true}};
    std::set<std::pair<std::string, bool>> seen;
    for (const auto& [root, source_tree] : candidates) {
        if (!seen.emplace(root.string(), source_tree).second) continue;
        const auto manifest = root / (source_tree ? "resources/runtime-manifest.json" : "runtime-manifest.json");
        if (manifest_present(manifest)) return RuntimeResources(root, source_tree);
    }
    throw Error(ErrorCode::Unsupported,
        "no compiled-compatible executable-relative runtime bundle; supply --resource-root or --source-root");
}

const std::string& RuntimeResources::bytes(std::string_view relative_path) const {
    const auto found = assets_.find(std::string(relative_path));
    require(found != assets_.end(), ErrorCode::InvalidInput, "runtime resource path is not registered");
    return found->second;
}

std::map<std::string, std::string> RuntimeResources::schema_documents() const {
    std::map<std::string, std::string> result;
    for (const auto& [path, content] : assets_)
        if (path.starts_with("schemas/")) result.emplace(path.substr(8), content);
    return result;
}

std::map<std::string, std::string> RuntimeResources::prompt_documents() const {
    std::map<std::string, std::string> result;
    for (const auto& [path, content] : assets_)
        if (path.starts_with("prompts/")) result.emplace(path.substr(8), content);
    return result;
}

void RuntimeResources::validate_frozen_prompts(const FrozenRequestConfig& config) const {
    for (const auto& [filename, content] : prompt_documents()) {
        const auto path = "prompts/" + filename;
        std::size_t count = 0;
        for (const auto& dependency : config.dependencies()) {
            if (dependency.path != path) continue;
            ++count;
            require(dependency.content == content && dependency.sha256 == sha256_hex(content),
                    ErrorCode::Corrupt, "frozen default prompt differs from captured program resource");
        }
        require(count == 1, ErrorCode::Corrupt, "frozen configuration lacks one exact distributed default prompt");
    }
}

Json RuntimeResources::diagnostics() const {
    std::size_t total = 0;
    for (const auto& [path, content] : assets_) { (void)path; total += content.size(); }
    return Object{{"format", bundle_format}, {"layout", source_tree_ ? "source-tree" : "installed-bundle"},
        {"root", root_.string()}, {"store_version", 410}, {"bundle_sha256", bundle_sha256_},
        {"manifest_file_sha256", manifest_file_sha256_}, {"asset_count", assets_.size()},
        {"total_bytes", total}, {"complete", true}};
}

} // namespace wls::s1
