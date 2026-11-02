#include "internal.hpp"
#include "build_identity.hpp"

#include <array>
#include <utility>

namespace wls::native {
namespace {
struct ResourceAsset {
    std::string_view path;
    std::string_view kind;
    std::string_view sha256;
    std::size_t size;
};
// The same unmodified registration is compiled by the Linux runtime.
#include "runtime_resource_catalog.inc"

void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
} // namespace

system::CapturedPublicBundle system::capture_registered_public_bundle(
    const std::function<std::string(std::string_view, std::size_t)>& read) {
    const auto manifest = read("runtime-manifest.json", 64U * 1024U);
    CapturedPublicBundle result;
    result.manifest_digest = sha256(manifest);
    require(result.manifest_digest == manifest_sha256, ErrorCode::Mismatch,
        "public resource manifest differs from the compiled exact-byte identity");
    for (const auto& asset : resource_assets) {
        require(asset.size <= 1024U * 1024U, ErrorCode::Mismatch, "compiled public resource is oversized");
        auto content = read(asset.path, asset.size);
        require(content.size() == asset.size && sha256(content) == asset.sha256,
            ErrorCode::Mismatch, "public resource differs from the compiled registration");
        result.total_bytes += content.size();
        require(result.total_bytes <= 8U * 1024U * 1024U, ErrorCode::Mismatch,
            "public resource total exceeds bound");
        require(result.assets.emplace(asset.path, std::move(content)).second,
            ErrorCode::Mismatch, "duplicate compiled public resource");
    }
    return result;
}

PublicResources PublicResources::from_bundle(const std::filesystem::path& root) {
    const auto spelling = path_to_utf8(root);
    require(root.is_absolute() && !spelling.empty() && spelling.size() <= 4096 &&
        spelling.find('\0') == std::string::npos, ErrorCode::InvalidInput,
        "public resource root must be a bounded absolute path");
    require(root == root.lexically_normal(), ErrorCode::InvalidInput,
        "public resource root must be normalized");
    auto directory = system::open_public_directory(root);
    auto captured = system::capture_registered_public_bundle([&](std::string_view name, std::size_t maximum) {
        return directory->read(name, maximum);
    });
    PublicResources result;
    result.assets_ = std::move(captured.assets);
    result.total_bytes_ = captured.total_bytes;
    result.manifest_digest_ = std::move(captured.manifest_digest);
    return result;
}

PublicResources PublicResources::discover() {
    const auto executable_directory = actual_executable().parent_path();
    const std::array candidates{
        (executable_directory / "../share/wls861").lexically_normal(),
        executable_directory / "share/wls861"};
    for (const auto& candidate : candidates) {
        std::error_code error;
        const auto status = std::filesystem::symlink_status(candidate / "runtime-manifest.json", error);
        if (error == std::errc::no_such_file_or_directory) continue;
        if (error) throw Error(ErrorCode::Denied, "cannot inspect executable-relative public manifest");
        if (std::filesystem::exists(status)) return from_bundle(candidate);
    }
    throw Error(ErrorCode::Missing, "executable-relative public resource manifest is missing");
}

const std::string& PublicResources::bytes(std::string_view relative) const {
    const auto found = assets_.find(relative);
    if (found == assets_.end()) throw Error(ErrorCode::InvalidInput, "resource is not registered");
    return found->second;
}
} // namespace wls::native
