#include "readonly_windows_internal.hpp"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <set>
#include <utility>

namespace wls::native::system {
namespace {
void require(bool condition, ErrorCode code, const char* message) {
    if (!condition) throw Error(code, message);
}
struct LocalMemory {
    HLOCAL value{};
    ~LocalMemory() { if (value != nullptr) ::LocalFree(value); }
    LocalMemory() = default;
    LocalMemory(const LocalMemory&) = delete;
    LocalMemory& operator=(const LocalMemory&) = delete;
};
struct Token {
    HANDLE value{};
    ~Token() { if (value != nullptr) ::CloseHandle(value); }
    Token() = default;
    Token(const Token&) = delete;
    Token& operator=(const Token&) = delete;
};

std::string sid_text(PSID sid) {
    require(sid != nullptr && ::IsValidSid(sid), ErrorCode::Denied, "invalid security identifier");
    LPWSTR spelling = nullptr;
    require(::ConvertSidToStringSidW(sid, &spelling) != 0, ErrorCode::Denied,
        "security identifier cannot be represented");
    LocalMemory memory;
    memory.value = spelling;
    const std::wstring_view wide(spelling);
    return utf16_to_utf8(std::u16string(wide.begin(), wide.end()));
}

std::set<std::string> trusted_sids(const WindowsReadOnlyPolicy& policy) {
    require_readonly_policy_shape(policy);
    std::set<std::string> result;
    for (const auto& spelling : policy.trusted_owner_and_writer_sids) {
        const auto utf16 = utf8_to_utf16(spelling);
        const std::wstring wide(utf16.begin(), utf16.end());
        PSID sid = nullptr;
        require(::ConvertStringSidToSidW(wide.c_str(), &sid) != 0, ErrorCode::Denied,
            "policy contains an invalid SID");
        LocalMemory memory;
        memory.value = sid;
        require(sid_text(sid) == spelling, ErrorCode::Denied, "policy SID is not canonical");
        // These identities are broad/dynamic, never OS management exceptions.
        require(spelling != "S-1-0-0" && spelling != "S-1-1-0" &&
            spelling != "S-1-2-0" && spelling != "S-1-2-1" &&
            !spelling.starts_with("S-1-3-") && spelling != "S-1-5-2" &&
            spelling != "S-1-5-4" && spelling != "S-1-5-7" &&
            spelling != "S-1-5-11" && spelling != "S-1-5-15" &&
            spelling != "S-1-5-32-545" && spelling != "S-1-5-32-546" &&
            !spelling.starts_with("S-1-15-2-"), ErrorCode::Denied,
            "broad or dynamic SID cannot be a trusted owner/writer");
        result.insert(spelling);
    }
    return result;
}

WindowsAclObservation inspect_acl(PSECURITY_DESCRIPTOR descriptor, const std::set<std::string>& trusted) {
    require(descriptor != nullptr && ::IsValidSecurityDescriptor(descriptor), ErrorCode::Denied,
        "invalid security descriptor");
    SECURITY_DESCRIPTOR_CONTROL control{};
    DWORD revision = 0;
    require(::GetSecurityDescriptorControl(descriptor, &control, &revision) != 0 &&
        revision == SECURITY_DESCRIPTOR_REVISION && (control & SE_SELF_RELATIVE) != 0,
        ErrorCode::Denied, "security descriptor is not a complete self-relative observation");
    PSID owner = nullptr;
    BOOL defaulted = FALSE;
    require(::GetSecurityDescriptorOwner(descriptor, &owner, &defaulted) != 0 && owner != nullptr,
        ErrorCode::Denied, "owner observation is missing");
    auto owner_text = sid_text(owner);
    require(trusted.contains(owner_text), ErrorCode::Denied, "object owner is outside the explicit policy");
    PACL dacl = nullptr;
    BOOL present = FALSE;
    require(::GetSecurityDescriptorDacl(descriptor, &present, &dacl, &defaulted) != 0 &&
        present && dacl != nullptr && ::IsValidAcl(dacl), ErrorCode::Denied,
        "absent, NULL or invalid DACL refused");
    ACL_SIZE_INFORMATION acl_size{};
    require(::GetAclInformation(dacl, &acl_size, sizeof(acl_size), AclSizeInformation) != 0 &&
        acl_size.AceCount > 0 && acl_size.AceCount <= 4096 &&
        (dacl->AclRevision == ACL_REVISION || dacl->AclRevision == ACL_REVISION_DS),
        ErrorCode::Denied, "empty or unsupported DACL refused");
    constexpr BYTE known_flags = OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE |
        NO_PROPAGATE_INHERIT_ACE | INHERIT_ONLY_ACE | INHERITED_ACE;
    constexpr ACCESS_MASK dangerous = FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA |
        FILE_WRITE_ATTRIBUTES | FILE_DELETE_CHILD | DELETE | WRITE_DAC | WRITE_OWNER;
    for (DWORD index = 0; index < acl_size.AceCount; ++index) {
        void* raw = nullptr;
        require(::GetAce(dacl, index, &raw) != 0 && raw != nullptr, ErrorCode::Denied,
            "DACL ACE cannot be inspected");
        const auto* header = static_cast<const ACE_HEADER*>(raw);
        require((header->AceType == ACCESS_ALLOWED_ACE_TYPE || header->AceType == ACCESS_DENIED_ACE_TYPE) &&
            (header->AceFlags & static_cast<BYTE>(~known_flags)) == 0 &&
            header->AceSize >= offsetof(ACCESS_ALLOWED_ACE, SidStart) + 8,
            ErrorCode::Denied, "unknown, conditional, object-specific or malformed ACE refused");
        const auto* ace = static_cast<const ACCESS_ALLOWED_ACE*>(raw);
        auto* sid = const_cast<DWORD*>(&ace->SidStart);
        const auto* sid_bytes = reinterpret_cast<const BYTE*>(sid);
        require(sid_bytes[1] <= SID_MAX_SUB_AUTHORITIES &&
            offsetof(ACCESS_ALLOWED_ACE, SidStart) + 8U + static_cast<unsigned int>(sid_bytes[1]) * 4U ==
                header->AceSize && ::IsValidSid(sid) &&
            offsetof(ACCESS_ALLOWED_ACE, SidStart) + ::GetLengthSid(sid) == header->AceSize,
            ErrorCode::Denied, "ACE SID or trailing data is unsupported");
        const auto principal = sid_text(sid);
        require(!principal.starts_with("S-1-3-") || (header->AceFlags & INHERIT_ONLY_ACE) != 0,
            ErrorCode::Denied, "effective creator/owner-rights ACE is unsupported");
        if ((header->AceFlags & (INHERIT_ONLY_ACE | NO_PROPAGATE_INHERIT_ACE)) != 0)
            require((header->AceFlags & (OBJECT_INHERIT_ACE | CONTAINER_INHERIT_ACE)) != 0,
                ErrorCode::Denied, "ACE inheritance flags cannot be interpreted");
        ACCESS_MASK mask = ace->Mask;
        GENERIC_MAPPING mapping{FILE_GENERIC_READ, FILE_GENERIC_WRITE, FILE_GENERIC_EXECUTE, FILE_ALL_ACCESS};
        ::MapGenericMask(&mask, &mapping);
        require((mask & ~static_cast<ACCESS_MASK>(FILE_ALL_ACCESS)) == 0, ErrorCode::Denied,
            "ACE contains unknown or privilege-dependent access rights");
        // Each existing child is checked separately. An inherit-only ACE has no
        // effective rights on this object. Denies never erase an unsafe allow.
        if (header->AceType == ACCESS_ALLOWED_ACE_TYPE && (header->AceFlags & INHERIT_ONLY_ACE) == 0)
            require((mask & dangerous) == 0 || trusted.contains(principal), ErrorCode::Denied,
                "untrusted principal can modify, replace or take ownership of the object");
    }
    const auto length = ::GetSecurityDescriptorLength(descriptor);
    require(length > 0 && length <= 1024U * 1024U, ErrorCode::Denied,
        "security descriptor exceeds the observation bound");
    return {std::move(owner_text), sha256(std::string_view(static_cast<const char*>(descriptor), length))};
}

template<class Value>
Value token_value(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    Value value{};
    DWORD length = 0;
    require(::GetTokenInformation(token, kind, &value, sizeof(value), &length) != 0 && length == sizeof(value),
        ErrorCode::Denied, "effective token observation failed");
    return value;
}
std::vector<std::byte> token_bytes(HANDLE token, TOKEN_INFORMATION_CLASS kind) {
    DWORD length = 0;
    require(::GetTokenInformation(token, kind, nullptr, 0, &length) == 0 &&
        ::GetLastError() == ERROR_INSUFFICIENT_BUFFER && length > 0 && length <= 65536,
        ErrorCode::Denied, "effective token observation size is unsupported");
    std::vector<std::byte> bytes(length);
    DWORD returned = 0;
    require(::GetTokenInformation(token, kind, bytes.data(), length, &returned) != 0 && returned == length,
        ErrorCode::Denied, "effective token observation changed");
    return bytes;
}
TOKEN_STATISTICS ordinary_token(const std::set<std::string>& trusted) {
    Token thread;
    if (::OpenThreadToken(::GetCurrentThread(), TOKEN_QUERY, TRUE, &thread.value))
        throw Error(ErrorCode::Denied, "thread impersonation is unsupported");
    require(::GetLastError() == ERROR_NO_TOKEN, ErrorCode::Denied,
        "thread impersonation state is unknown");
    Token process;
    require(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &process.value) != 0,
        ErrorCode::Denied, "process token cannot be observed");
    const auto stats = token_value<TOKEN_STATISTICS>(process.value, TokenStatistics);
    const auto elevation = token_value<TOKEN_ELEVATION>(process.value, TokenElevation);
    const auto type = token_value<TOKEN_ELEVATION_TYPE>(process.value, TokenElevationType);
    require(stats.TokenType == TokenPrimary && elevation.TokenIsElevated == 0 && type == TokenElevationTypeDefault,
        ErrorCode::Denied, "read-only policy requires a normal non-elevated primary token");
    alignas(DWORD) std::array<std::byte, SECURITY_MAX_SID_SIZE> administrator{};
    DWORD sid_size = static_cast<DWORD>(administrator.size());
    BOOL member = FALSE;
    require(::CreateWellKnownSid(WinBuiltinAdministratorsSid, nullptr, administrator.data(), &sid_size) != 0 &&
        ::CheckTokenMembership(nullptr, administrator.data(), &member) != 0 && !member,
        ErrorCode::Denied, "administrator or unknown effective group membership refused");
    const auto user = token_bytes(process.value, TokenUser);
    const auto* user_info = reinterpret_cast<const TOKEN_USER*>(user.data());
    require(trusted.contains(sid_text(user_info->User.Sid)), ErrorCode::Denied,
        "current user SID is not explicitly trusted");
    const auto integrity = token_bytes(process.value, TokenIntegrityLevel);
    const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(integrity.data());
    require(::IsValidSid(label->Label.Sid), ErrorCode::Denied, "invalid token integrity observation");
    const auto count = *::GetSidSubAuthorityCount(label->Label.Sid);
    require(count != 0 && *::GetSidSubAuthority(label->Label.Sid, static_cast<DWORD>(count - 1)) ==
        SECURITY_MANDATORY_MEDIUM_RID, ErrorCode::Denied, "normal medium-integrity token required");
    return stats;
}
bool same_luid(const LUID& first, const LUID& second) {
    return first.LowPart == second.LowPart && first.HighPart == second.HighPart;
}
void require_host(const WindowsReadOnlyPolicy& policy) {
    USHORT process = 0, native = 0;
    require(::IsWow64Process2(::GetCurrentProcess(), &process, &native) != 0 &&
        process == IMAGE_FILE_MACHINE_UNKNOWN && native == IMAGE_FILE_MACHINE_AMD64 && sizeof(void*) == 8,
        ErrorCode::Unsupported, "read-only identity requires native Windows x64");
    const auto module = ::GetModuleHandleW(L"ntdll.dll");
    const auto address = module == nullptr ? nullptr : ::GetProcAddress(module, "RtlGetVersion");
    require(address != nullptr, ErrorCode::Unsupported, "exact native Windows version is unavailable");
    using VersionFunction = LONG(WINAPI*)(OSVERSIONINFOEXW*);
    static_assert(sizeof(VersionFunction) == sizeof(address));
    VersionFunction function = nullptr;
    std::memcpy(&function, &address, sizeof(function));
    OSVERSIONINFOEXW version{};
    version.dwOSVersionInfoSize = sizeof(version);
    require(function(&version) >= 0 && version.dwMajorVersion == 10 && version.dwMinorVersion == 0 &&
        version.wProductType == VER_NT_WORKSTATION && version.dwBuildNumber >= 22000 &&
        std::find(policy.approved_builds.begin(), policy.approved_builds.end(), version.dwBuildNumber) !=
            policy.approved_builds.end(), ErrorCode::Unsupported,
        "Windows build is outside the explicit Windows 11 workstation policy");
}
std::uint64_t filetime(const FILETIME& value) {
    return (static_cast<std::uint64_t>(value.dwHighDateTime) << 32U) | value.dwLowDateTime;
}

void require_long_component(HANDLE handle, std::wstring_view expected) {
    // This is alias rejection only. The retained ancestor handles and checked
    // ACL chain establish containment; a path-string prefix does not.
    if (expected.empty()) return; // The already validated local drive root.
    std::vector<wchar_t> storage(32768);
    const auto length = ::GetFinalPathNameByHandleW(handle, storage.data(), static_cast<DWORD>(storage.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    require(length != 0 && length < storage.size(), ErrorCode::Unsupported,
        "canonical object component is unavailable");
    const std::wstring_view full(storage.data(), length);
    const auto separator = full.find_last_of(L'\\');
    const auto leaf = separator == std::wstring_view::npos ? full : full.substr(separator + 1);
    require(leaf == expected, ErrorCode::Denied, "short-name or case-alias component refused");
}
} // namespace

WindowsAclObservation inspect_windows_acl(PSECURITY_DESCRIPTOR descriptor, const WindowsReadOnlyPolicy& policy) {
    return inspect_acl(descriptor, trusted_sids(policy));
}
WindowsObjectIdentity inspect_windows_identity(HANDLE handle) {
    FILE_ID_INFO information{};
    require(::GetFileInformationByHandleEx(handle, FileIdInfo, &information, sizeof(information)) != 0,
        ErrorCode::Unsupported, "128-bit Windows file identity is unavailable");
    WindowsObjectIdentity result;
    result.volume_serial = information.VolumeSerialNumber;
    std::copy(std::begin(information.FileId.Identifier), std::end(information.FileId.Identifier), result.file_id.begin());
    return result;
}

struct WindowsReadOnlyChecks::Impl {
    std::set<std::string> trusted;
    TOKEN_STATISTICS token{};
    std::string digest;
};
WindowsReadOnlyChecks::WindowsReadOnlyChecks(const WindowsReadOnlyPolicy& policy) : impl_(std::make_unique<Impl>()) {
    impl_->trusted = trusted_sids(policy);
    impl_->token = ordinary_token(impl_->trusted);
    require_host(policy);
    std::string encoded = "windows-readonly-policy-v1\n" + json_string(policy.approval_reference) + "\n";
    for (const auto build : policy.approved_builds) encoded += std::to_string(build) + "\n";
    encoded += "sids\n";
    for (const auto& sid : impl_->trusted) encoded += sid + "\n";
    impl_->digest = sha256(encoded);
}
WindowsReadOnlyChecks::~WindowsReadOnlyChecks() = default;
const std::string& WindowsReadOnlyChecks::policy_digest() const { return impl_->digest; }
void WindowsReadOnlyChecks::require_current_token() const {
    const auto current = ordinary_token(impl_->trusted);
    require(same_luid(current.TokenId, impl_->token.TokenId) &&
        same_luid(current.AuthenticationId, impl_->token.AuthenticationId) &&
        same_luid(current.ModifiedId, impl_->token.ModifiedId), ErrorCode::Denied,
        "read-only root token binding changed");
}
void WindowsReadOnlyChecks::require_volume_root(HANDLE handle) const {
    // A SUBST drive rooted below the real volume root would omit necessary
    // ancestors. Refuse it instead of treating its DOS drive name as authority.
    std::array<wchar_t, 128> path{};
    const auto length = ::GetFinalPathNameByHandleW(handle, path.data(), static_cast<DWORD>(path.size()),
        FILE_NAME_NORMALIZED | VOLUME_NAME_GUID);
    require(length > 0 && length < path.size(), ErrorCode::Unsupported,
        "real volume-root identity is unavailable");
    const std::wstring_view spelling(path.data(), length);
    require(spelling.starts_with(L"\\\\?\\Volume{") && spelling.ends_with(L"}\\") &&
        spelling.find(L'\\', 4) == spelling.size() - 1, ErrorCode::Denied,
        "drive alias omits real volume ancestors");
}
WindowsObjectObservation WindowsReadOnlyChecks::observe(
    HANDLE handle, bool directory, std::wstring_view expected_component) const {
    require_current_token();
    DWORD handle_flags = 0;
    require(::GetHandleInformation(handle, &handle_flags) != 0 && (handle_flags & HANDLE_FLAG_INHERIT) == 0,
        ErrorCode::Denied, "invalid or inheritable read-only handle refused");
    FILE_ATTRIBUTE_TAG_INFO tag{};
    BY_HANDLE_FILE_INFORMATION facts{};
    require(::GetFileType(handle) == FILE_TYPE_DISK &&
        ::GetFileInformationByHandleEx(handle, FileAttributeTagInfo, &tag, sizeof(tag)) != 0 &&
        ::GetFileInformationByHandle(handle, &facts) != 0, ErrorCode::Denied,
        "read-only object attributes are unavailable");
    require((tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) == 0 && tag.ReparseTag == 0 &&
        ((tag.FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) == directory &&
        (directory || facts.nNumberOfLinks == 1), ErrorCode::Denied,
        "read-only object type, reparse tag or hard-link count refused");
    std::array<wchar_t, 64> filesystem{};
    DWORD flags = 0;
    require(::GetVolumeInformationByHandleW(handle, nullptr, 0, nullptr, nullptr, &flags,
        filesystem.data(), static_cast<DWORD>(filesystem.size())) != 0 &&
        std::wstring_view(filesystem.data()) == L"NTFS" && (flags & FILE_PERSISTENT_ACLS) != 0,
        ErrorCode::Unsupported, "read-only objects require observed NTFS with persistent ACLs");
    if (directory) {
        FILE_CASE_SENSITIVE_INFO sensitive{};
        require(::GetFileInformationByHandleEx(handle, FileCaseSensitiveInfo, &sensitive, sizeof(sensitive)) != 0 &&
            sensitive.Flags == 0, ErrorCode::Unsupported, "case-sensitive or unknown directory semantics refused");
    }
    require_long_component(handle, expected_component);
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    const auto status = ::GetSecurityInfo(handle, SE_FILE_OBJECT,
        OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION, nullptr, nullptr, nullptr, nullptr, &descriptor);
    LocalMemory security;
    security.value = descriptor;
    require(status == ERROR_SUCCESS && descriptor != nullptr, ErrorCode::Denied,
        "same-handle owner/DACL observation is unavailable");
    auto acl = inspect_acl(descriptor, impl_->trusted);
    WindowsObjectObservation result;
    result.identity = inspect_windows_identity(handle);
    result.attributes = tag.FileAttributes;
    result.reparse_tag = tag.ReparseTag;
    result.links = facts.nNumberOfLinks;
    result.size = (static_cast<std::uint64_t>(facts.nFileSizeHigh) << 32U) | facts.nFileSizeLow;
    result.creation_time = filetime(facts.ftCreationTime);
    result.last_write_time = filetime(facts.ftLastWriteTime);
    result.owner_sid = std::move(acl.owner_sid);
    result.security_descriptor_sha256 = std::move(acl.descriptor_sha256);
    return result;
}
void WindowsReadOnlyChecks::require_unchanged(HANDLE handle, const WindowsObjectObservation& original) const {
    require(observe(handle, (original.attributes & FILE_ATTRIBUTE_DIRECTORY) != 0, {}) == original,
        ErrorCode::Mismatch, "read-only object identity, attributes or owner/DACL changed");
}
} // namespace wls::native::system
