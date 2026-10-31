#pragma once

#include "wls/config.hpp"

#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace wls::native {

enum class Status { Available, Unsupported, Denied, NotChecked };
enum class ErrorCode { InvalidInput, Unsupported, Denied, Missing, Mismatch, Io };

class Error final : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }
private:
    ErrorCode code_;
};

std::string_view status_name(Status status);
std::string_view error_name(ErrorCode code);
struct Capability {
    std::string_view name;
    Status status;
    std::string_view reason;
};
std::vector<Capability> capabilities();
// Diagnostic availability never issues a StoreActor or any business authority.
void require_business_capability(std::string_view operation);

// Strict Unicode scalar conversion: no replacement characters for invalid
// encodings. NUL is valid text here; path and argv boundaries reject it.
std::u16string utf8_to_utf16(std::string_view text);
std::string utf16_to_utf8(std::u16string_view text);
std::filesystem::path path_from_utf8(std::string_view text);
std::string path_to_utf8(const std::filesystem::path& path);
std::string json_string(std::string_view text);

struct HostFacts {
    std::string platform;
    std::string token_elevation;
    std::string filesystem;
};
HostFacts host_facts();
std::filesystem::path actual_executable();

// Captured, compiled-approved PUBLIC distribution bytes only. This is not a
// Workspace, Store, SQL executor, permission grant or production FS adapter.
class PublicResources final {
public:
    static PublicResources from_bundle(const std::filesystem::path& absolute_root);
    static PublicResources discover();
    const std::string& bytes(std::string_view relative) const;
    std::size_t count() const noexcept { return assets_.size(); }
    std::size_t total_bytes() const noexcept { return total_bytes_; }
    const std::string& manifest_digest() const noexcept { return manifest_digest_; }
private:
    std::map<std::string, std::string, std::less<>> assets_;
    std::size_t total_bytes_{};
    std::string manifest_digest_;
};

// A bounded local command boundary. Configuration is supplied by the trusted
// operator; no YAML/config file loading, Store, networking or model calls.
std::string run_command(const std::vector<std::string>& arguments);

} // namespace wls::native
