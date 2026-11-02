#include "readonly_windows_internal.hpp"

#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <utility>

namespace wls::native {
namespace {
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
class Handle final {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() { if (valid()) ::CloseHandle(value_); }
    Handle(Handle&& other) noexcept : value_(std::exchange(other.value_, INVALID_HANDLE_VALUE)) {}
    Handle& operator=(Handle&& other) noexcept {
        if (this != &other) { if (valid()) ::CloseHandle(value_); value_ = std::exchange(other.value_, INVALID_HANDLE_VALUE); }
        return *this;
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const noexcept { return value_; }
    bool valid() const noexcept { return value_ != INVALID_HANDLE_VALUE && value_ != nullptr; }
private:
    HANDLE value_;
};

ErrorCode open_error() {
    const auto code = ::GetLastError();
    return code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND ? ErrorCode::Missing : ErrorCode::Denied;
}

BY_HANDLE_FILE_INFORMATION information(HANDLE handle, bool directory) {
    BY_HANDLE_FILE_INFORMATION result{};
    FILE_ATTRIBUTE_TAG_INFO tag{};
    require(::GetFileType(handle) == FILE_TYPE_DISK &&
        ::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) &&
        ::GetFileInformationByHandle(handle, &result), ErrorCode::Denied, "cannot inspect public resource handle");
    require((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 &&
        ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory,
        ErrorCode::Denied, "public resource reparse point or wrong object type refused");
    if (!directory) require(result.nNumberOfLinks == 1, ErrorCode::Denied,
        "public resource file must have one link");
    return result;
}

Handle open_directory(const std::filesystem::path& path, bool inspect_acl = false) {
    // Hold every ancestor without write/delete sharing until capture ends.
    // This also excludes a concurrent reparse-data writer after inspection.
    Handle handle(::CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | (inspect_acl ? READ_CONTROL : 0U),
        FILE_SHARE_READ, nullptr, OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr));
    require(handle.valid(), open_error(), "public resource directory cannot be opened");
    (void)information(handle.get(), true);
    return handle;
}

void component_name(const std::filesystem::path& component) {
    const auto& name = component.native();
    require(!name.empty() && name != L"." && name != L".." &&
        name.find_first_of(L"<>:\"|?*\\/") == std::wstring::npos &&
        name.back() != L'.' && name.back() != L' ', ErrorCode::Denied,
        "unsupported Windows public resource path component");
    for (const wchar_t code : name)
        require(code >= 32, ErrorCode::Denied, "Windows public resource control character refused");
    auto basename = name.substr(0, name.find(L'.'));
    for (auto& code : basename)
        if (code >= L'a' && code <= L'z') code = static_cast<wchar_t>(code - L'a' + L'A');
    const bool numbered = basename.size() == 4 &&
        (basename.starts_with(L"COM") || basename.starts_with(L"LPT")) &&
        ((basename[3] >= L'0' && basename[3] <= L'9') || basename[3] == L'\u00b9' ||
            basename[3] == L'\u00b2' || basename[3] == L'\u00b3');
    require(basename != L"CON" && basename != L"PRN" && basename != L"AUX" && basename != L"NUL" &&
        basename != L"CONIN$" && basename != L"CONOUT$" && !numbered, ErrorCode::Denied,
        "Windows reserved device component refused");
}

std::vector<Handle> pin_directories(const std::filesystem::path& path,
    const system::WindowsReadOnlyChecks* checks, std::vector<WindowsObjectObservation>& observations) {
    auto preferred = path;
    preferred.make_preferred();
    const auto root = preferred.root_path().native();
    require(path.is_absolute() && root.size() == 3 && root[1] == L':' && root[2] == L'\\' &&
        ((root[0] >= L'A' && root[0] <= L'Z') || (root[0] >= L'a' && root[0] <= L'z')),
        ErrorCode::Denied, "public resources require an absolute local drive path; UNC/device paths refused");
    require(::GetDriveTypeW(root.c_str()) == DRIVE_FIXED, ErrorCode::Denied,
        "public resources require a fixed local drive");
    std::filesystem::path current(root);
    std::vector<Handle> held;
    held.push_back(open_directory(current, checks != nullptr));
    if (checks != nullptr) {
        checks->require_volume_root(held.back().get());
        observations.push_back(checks->observe(held.back().get(), true, {}));
    }
    for (const auto& component : preferred.relative_path()) {
        component_name(component);
        current /= component;
        held.push_back(open_directory(current, checks != nullptr));
        if (checks != nullptr) {
            auto observation = checks->observe(held.back().get(), true, component.native());
            require(observation.identity.volume_serial == observations.front().identity.volume_serial,
                ErrorCode::Denied, "read-only ancestor changed volume");
            observations.push_back(std::move(observation));
        }
    }
    return held;
}

bool same(const BY_HANDLE_FILE_INFORMATION& first, const BY_HANDLE_FILE_INFORMATION& second) {
    return first.dwVolumeSerialNumber == second.dwVolumeSerialNumber &&
        first.nFileIndexHigh == second.nFileIndexHigh && first.nFileIndexLow == second.nFileIndexLow &&
        first.dwFileAttributes == second.dwFileAttributes && first.nNumberOfLinks == second.nNumberOfLinks &&
        first.nFileSizeHigh == second.nFileSizeHigh && first.nFileSizeLow == second.nFileSizeLow &&
        first.ftCreationTime.dwHighDateTime == second.ftCreationTime.dwHighDateTime &&
        first.ftCreationTime.dwLowDateTime == second.ftCreationTime.dwLowDateTime &&
        first.ftLastWriteTime.dwHighDateTime == second.ftLastWriteTime.dwHighDateTime &&
        first.ftLastWriteTime.dwLowDateTime == second.ftLastWriteTime.dwLowDateTime;
}

class Directory final : public system::WindowsPublicDirectory {
public:
    explicit Directory(std::filesystem::path root, const WindowsReadOnlyPolicy* policy = nullptr)
        : root_(std::move(root)),
          checks_(policy == nullptr ? nullptr : std::make_unique<system::WindowsReadOnlyChecks>(*policy)),
          held_(pin_directories(root_, checks_.get(), observations_)) { require_ancestors(); }
    std::string read(std::string_view relative, std::size_t maximum) const override {
        return capture(relative, maximum).bytes;
    }
    const WindowsObjectObservation& root_observation() const override {
        require(checks_ != nullptr && !observations_.empty(), ErrorCode::Denied,
            "historical public reader has no ACL policy observation");
        return observations_.back();
    }
    const std::string& policy_digest() const override {
        require(checks_ != nullptr, ErrorCode::Denied, "explicit read-only policy is missing");
        return checks_->policy_digest();
    }
    system::WindowsCapturedFile read_observed(std::string_view relative, std::size_t maximum) const override {
        require(checks_ != nullptr, ErrorCode::Denied, "explicit read-only policy is missing");
        return capture(relative, maximum);
    }
private:
    void require_ancestors() const {
        if (checks_ == nullptr) return;
        checks_->require_current_token();
        for (std::size_t index = 0; index < held_.size(); ++index)
            checks_->require_unchanged(held_[index].get(), observations_[index]);
    }
    system::WindowsCapturedFile capture(std::string_view relative, std::size_t maximum) const {
        require_ancestors();
        const auto path = path_from_utf8(relative);
        require(path.is_relative() && !path.empty() && relative.size() <= 512 &&
            maximum < std::numeric_limits<std::size_t>::max(), ErrorCode::InvalidInput,
            "invalid registered public resource path");
        std::vector<Handle> children;
        std::vector<WindowsObjectObservation> child_observations;
        auto parent = root_;
        for (const auto& component : path.parent_path()) {
            component_name(component);
            parent /= component;
            children.push_back(open_directory(parent, checks_ != nullptr));
            if (checks_ != nullptr) {
                auto observation = checks_->observe(children.back().get(), true, component.native());
                require(observation.identity.volume_serial == root_observation().identity.volume_serial,
                    ErrorCode::Denied, "read-only resource directory changed volume");
                child_observations.push_back(std::move(observation));
            }
        }
        component_name(path.filename());
        const auto filename = parent / path.filename();
        // Denying write/delete sharing pins both the opened bytes and the
        // object through EOF/hash capture. No HANDLE is converted to an int.
        Handle input(::CreateFileW(filename.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        require(input.valid(), open_error(), "public resource file cannot be opened");
        const auto before = information(input.get(), false);
        WindowsObjectObservation observed;
        if (checks_ != nullptr) {
            observed = checks_->observe(input.get(), false, path.filename().native());
            require(observed.identity.volume_serial == root_observation().identity.volume_serial,
                ErrorCode::Denied, "read-only resource file changed volume");
        }
        const auto size = (static_cast<std::uint64_t>(before.nFileSizeHigh) << 32U) | before.nFileSizeLow;
        require(size <= maximum, ErrorCode::Mismatch, "public resource file exceeds bound");
        std::string output;
        std::array<char, 16384> chunk{};
        bool eof = false;
        while (output.size() <= maximum) {
            const auto length = std::min(chunk.size(), maximum + 1 - output.size());
            if (length == 0) break;
            DWORD count = 0;
            require(::ReadFile(input.get(), chunk.data(), static_cast<DWORD>(length), &count, nullptr) != 0,
                ErrorCode::Io, "public resource read failed");
            if (count == 0) { eof = true; break; }
            output.append(chunk.data(), static_cast<std::size_t>(count));
        }
        require(same(before, information(input.get(), false)), ErrorCode::Mismatch,
            "public resource changed during reading");
        require(eof && output.size() <= maximum && output.size() == size, ErrorCode::Mismatch,
            "public resource EOF or size differs from observation");
        if (checks_ != nullptr) {
            checks_->require_unchanged(input.get(), observed);
            for (std::size_t index = 0; index < children.size(); ++index)
                checks_->require_unchanged(children[index].get(), child_observations[index]);
            require_ancestors();
        }
        return {std::move(output), std::move(observed)};
    }
    std::filesystem::path root_;
    std::unique_ptr<system::WindowsReadOnlyChecks> checks_;
    std::vector<WindowsObjectObservation> observations_;
    std::vector<Handle> held_;
};
} // namespace

std::filesystem::path actual_executable() {
    std::vector<wchar_t> buffer(32768);
    const auto count = ::GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    require(count > 0 && count < buffer.size(), ErrorCode::Io, "cannot locate bounded actual executable");
    const std::filesystem::path path(std::wstring(buffer.data(), count));
    require(path_to_utf8(path).size() <= 4096 && path.is_absolute(), ErrorCode::Denied,
        "actual executable path is outside the supported path bounds");
    return path;
}

HostFacts host_facts() {
    std::string elevation = "not_checked";
    HANDLE raw = nullptr;
    if (::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &raw)) {
        Handle token(raw);
        TOKEN_ELEVATION observed{};
        DWORD returned = 0;
        if (::GetTokenInformation(token.get(), TokenElevation, &observed, sizeof(observed), &returned))
            elevation = observed.TokenIsElevated != 0 ? "elevated" : "not_elevated";
    }
    std::string filesystem = "not_checked";
    const auto root = actual_executable().root_path();
    std::array<wchar_t, 64> name{};
    if (::GetVolumeInformationW(root.c_str(), nullptr, 0, nullptr, nullptr, nullptr,
        name.data(), static_cast<DWORD>(name.size()))) {
        const std::wstring_view spelling(name.data());
        filesystem = utf16_to_utf8(std::u16string(spelling.begin(), spelling.end()));
    }
    return {"windows-native", std::move(elevation), std::move(filesystem)};
}

namespace system {
std::unique_ptr<PublicDirectory> open_public_directory(const std::filesystem::path& root) {
    return std::make_unique<Directory>(root);
}

std::unique_ptr<WindowsPublicDirectory> open_windows_readonly_directory(
    const std::filesystem::path& root, const WindowsReadOnlyPolicy& policy) {
    return std::make_unique<Directory>(root, &policy);
}

std::string sha256(std::string_view bytes) {
    require(bytes.size() <= std::numeric_limits<ULONG>::max(), ErrorCode::InvalidInput,
        "SHA-256 input exceeds platform bound");
    struct Algorithm {
        BCRYPT_ALG_HANDLE handle{};
        ~Algorithm() { if (handle) ::BCryptCloseAlgorithmProvider(handle, 0); }
    } algorithm;
    require(::BCryptOpenAlgorithmProvider(&algorithm.handle, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0,
        ErrorCode::Io, "cannot open Windows SHA-256 provider");
    struct Hash {
        BCRYPT_HASH_HANDLE handle{};
        ~Hash() { if (handle) ::BCryptDestroyHash(handle); }
    } hash;
    require(::BCryptCreateHash(algorithm.handle, &hash.handle, nullptr, 0, nullptr, 0, 0) >= 0,
        ErrorCode::Io, "cannot create Windows SHA-256 hash");
    auto* data = reinterpret_cast<const unsigned char*>(bytes.data());
    require(::BCryptHashData(hash.handle, const_cast<PUCHAR>(data), static_cast<ULONG>(bytes.size()), 0) >= 0,
        ErrorCode::Io, "Windows SHA-256 update failed");
    std::array<unsigned char, 32> digest{};
    require(::BCryptFinishHash(hash.handle, digest.data(), static_cast<ULONG>(digest.size()), 0) >= 0,
        ErrorCode::Io, "Windows SHA-256 finalization failed");
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result;
    for (const auto value : digest) { result += hex[value >> 4U]; result += hex[value & 0x0fU]; }
    return result;
}
} // namespace system
} // namespace wls::native
