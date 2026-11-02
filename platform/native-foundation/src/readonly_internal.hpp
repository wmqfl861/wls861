#pragma once
#include "internal.hpp"
#include "wls/native_readonly.hpp"

namespace wls::native::system {
struct WindowsCapturedFile {
    std::string bytes;
    WindowsObjectObservation observation;
};
class WindowsPublicDirectory : public PublicDirectory {
public:
    virtual const WindowsObjectObservation& root_observation() const = 0;
    virtual const std::string& policy_digest() const = 0;
    virtual WindowsCapturedFile read_observed(std::string_view relative, std::size_t maximum) const = 0;
};
std::unique_ptr<WindowsPublicDirectory> open_windows_readonly_directory(
    const std::filesystem::path& root, const WindowsReadOnlyPolicy& policy);
void require_readonly_policy_shape(const WindowsReadOnlyPolicy& policy);
} // namespace wls::native::system
