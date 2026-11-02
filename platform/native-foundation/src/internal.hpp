#pragma once
#include "wls/native_foundation.hpp"
#include <functional>
#include <memory>

namespace wls::native::system {
class PublicDirectory {
public:
    virtual ~PublicDirectory() = default;
    virtual std::string read(std::string_view registered_relative, std::size_t maximum) const = 0;
};
std::unique_ptr<PublicDirectory> open_public_directory(const std::filesystem::path& absolute_root);
std::string sha256(std::string_view bytes);
struct CapturedPublicBundle {
    std::map<std::string, std::string, std::less<>> assets;
    std::size_t total_bytes{};
    std::string manifest_digest;
};
// Both historical and explicit-policy readers use the identical registration,
// byte budgets and exact compiled hashes.
CapturedPublicBundle capture_registered_public_bundle(
    const std::function<std::string(std::string_view, std::size_t)>& read);
} // namespace wls::native::system
