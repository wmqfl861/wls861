#pragma once
#include "wls/native_foundation.hpp"
#include <memory>

namespace wls::native::system {
class PublicDirectory {
public:
    virtual ~PublicDirectory() = default;
    virtual std::string read(std::string_view registered_relative, std::size_t maximum) const = 0;
};
std::unique_ptr<PublicDirectory> open_public_directory(const std::filesystem::path& absolute_root);
std::string sha256(std::string_view bytes);
} // namespace wls::native::system
