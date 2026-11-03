#include "support.hpp"
#include "wls/native_readonly.hpp"

#include <type_traits>

#ifdef _WIN32
#include "readonly_windows_internal.hpp"
#include <aclapi.h>
#include <sddl.h>
#include <barrier>
#include <chrono>
#include <thread>
#endif

namespace {
using namespace wls::native;

template<class Function>
void rejects_code(ErrorCode code, Function&& function) {
    bool rejected = false;
    try { function(); }
    catch (const Error& error) { CHECK(error.code() == code); rejected = true; }
    CHECK(rejected);
}

WindowsReadOnlyPolicy synthetic_policy() {
    // Synthetic descriptor identity; this is not an adopted deployment policy.
    return {"synthetic-unit-fixture-only", {26100}, {"S-1-5-21-111-222-333-1001"}};
}
void missing_policy_rejects_before_path(const std::filesystem::path& root) {
    for (int missing = 0; missing < 3; ++missing) {
        auto policy = synthetic_policy();
        if (missing == 0) policy.approval_reference.clear();
        if (missing == 1) policy.approved_builds.clear();
        if (missing == 2) policy.trusted_owner_and_writer_sids.clear();
        rejects_code(ErrorCode::Denied, [&] { (void)ReadOnlyRoot::open(root / "not-created", policy); });
    }
    CHECK(!std::filesystem::exists(root / "not-created"));
    static_assert(!std::is_copy_constructible_v<ReadOnlyRoot>);
    static_assert(std::is_nothrow_move_constructible_v<ReadOnlyRoot>);
    static_assert(sizeof(WindowsObjectIdentity::file_id) == 16);
    static_assert(sizeof(WindowsObjectIdentity::volume_serial) == 8);
}
void duplicate_policy_rejects(const std::filesystem::path& root) {
    auto policy = synthetic_policy();
    policy.approved_builds.push_back(26100);
    rejects_code(ErrorCode::Denied, [&] { (void)ReadOnlyRoot::open(root, policy); });
    policy = synthetic_policy();
    policy.trusted_owner_and_writer_sids.push_back(policy.trusted_owner_and_writer_sids.front());
    rejects_code(ErrorCode::Denied, [&] { (void)ReadOnlyRoot::open(root, policy); });
    policy = synthetic_policy();
    policy.approved_builds = {20348};
    rejects_code(ErrorCode::Denied, [&] { (void)ReadOnlyRoot::open(root, policy); });
    // Resource trust cannot be inferred from a non-elevated caller. Refuse
    // Administrators as either the only or an additional trusted identity,
    // before OS support or any attempt to open the nonexistent path.
    for (bool administrator_only : {false, true}) {
        policy = synthetic_policy();
        if (administrator_only) policy.trusted_owner_and_writer_sids.clear();
        policy.trusted_owner_and_writer_sids.push_back("S-1-5-32-544");
        rejects_code(ErrorCode::Denied, [&] { (void)ReadOnlyRoot::open(root / "not-created", policy); });
    }
    CHECK(!std::filesystem::exists(root / "not-created"));
}
void historical_capture_compatible(const std::filesystem::path& root) {
    const auto captured = PublicResources::from_bundle(root);
    // Exact approved Store 411 bundle, including migration 013.
    CHECK(captured.count() == 36);
    CHECK(captured.total_bytes() == 166684);
    CHECK(captured.manifest_digest() == "f03bce7bb55b02f7740c81473c12301ecc437b35d980d629fb2d896196d4dc30");
#ifndef _WIN32
    rejects_code(ErrorCode::Unsupported, [&] { (void)ReadOnlyRoot::open(root, synthetic_policy()); });
#endif
}

#ifdef _WIN32
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    ~Handle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) ::CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE handle) : value(handle) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};
struct Security {
    PSECURITY_DESCRIPTOR value{};
    explicit Security(const std::wstring& sddl) {
        CHECK(::ConvertStringSecurityDescriptorToSecurityDescriptorW(sddl.c_str(), SDDL_REVISION_1, &value, nullptr));
        CHECK(::IsValidSecurityDescriptor(value));
    }
    ~Security() { if (value != nullptr) ::LocalFree(value); }
    Security(const Security&) = delete;
    Security& operator=(const Security&) = delete;
};
std::wstring wide(std::string_view value) {
    const auto converted = utf8_to_utf16(value);
    return {converted.begin(), converted.end()};
}
std::string current_user() {
    Handle token;
    CHECK(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY, &token.value));
    DWORD length = 0;
    CHECK(!::GetTokenInformation(token.value, TokenUser, nullptr, 0, &length));
    CHECK(::GetLastError() == ERROR_INSUFFICIENT_BUFFER);
    std::vector<std::byte> bytes(length);
    CHECK(::GetTokenInformation(token.value, TokenUser, bytes.data(), length, &length));
    const auto* user = reinterpret_cast<const TOKEN_USER*>(bytes.data());
    LPWSTR text = nullptr;
    CHECK(::ConvertSidToStringSidW(user->User.Sid, &text));
    struct Free { LPWSTR value; ~Free() { ::LocalFree(value); } } free{text};
    const std::wstring_view spelling(text);
    return utf16_to_utf8(std::u16string(spelling.begin(), spelling.end()));
}
void complete_acl_policy() {
    const auto policy = synthetic_policy();
    const auto owner = wide(policy.trusted_owner_and_writer_sids.front());
    Security safe(L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;FR;;;WD)");
    const auto observed = system::inspect_windows_acl(safe.value, policy);
    CHECK(observed.owner_sid == policy.trusted_owner_and_writer_sids.front());
    CHECK(observed.descriptor_sha256.size() == 64);
    for (const auto& unsafe : {
            L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;FW;;;WD)",
            L"O:" + owner + L"D:P(D;;FA;;;WD)(A;;FA;;;WD)",
            L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;DC;;;AU)",
            L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;WD;;;BU)",
            L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;WO;;;WD)",
            L"O:BAD:P(A;;FA;;;" + owner + L")",
            L"O:" + owner + L"D:NO_ACCESS_CONTROL",
            L"O:" + owner + L"D:P"}) {
        Security descriptor(unsafe);
        rejects_code(ErrorCode::Denied, [&] { (void)system::inspect_windows_acl(descriptor.value, policy); });
    }
    auto broad = policy;
    broad.trusted_owner_and_writer_sids.push_back("S-1-1-0");
    rejects_code(ErrorCode::Denied, [&] { (void)system::inspect_windows_acl(safe.value, broad); });

    // This profile rejects explicit Administrators trust. Merely adding the
    // group cannot authorize either an administrative owner or a writer ACE.
    auto administrator = policy;
    administrator.trusted_owner_and_writer_sids.push_back("S-1-5-32-544");
    Security administrator_writer(L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;FW;;;BA)");
    Security administrator_owner(L"O:BAD:P(A;;FA;;;" + owner + L")");
    for (auto* descriptor : {safe.value, administrator_writer.value, administrator_owner.value})
        rejects_code(ErrorCode::Denied, [&] { (void)system::inspect_windows_acl(descriptor, administrator); });
    for (const auto& unsafe : {
            L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;FA;;;BA)",
            L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;WD;;;BA)",
            L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;WO;;;BA)",
            L"O:" + owner + L"D:P(D;;FA;;;BA)(A;;FA;;;BA)(A;;FA;;;" + owner + L")"}) {
        Security descriptor(unsafe);
        rejects_code(ErrorCode::Denied, [&] { (void)system::inspect_windows_acl(descriptor.value, policy); });
    }
    // Refusing the group as a trusted writer does not turn a read-only or
    // deny-only ACE into a write grant. Denies still cannot erase unsafe allows.
    Security administrator_read(L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")(A;;FR;;;BA)");
    Security administrator_deny(L"O:" + owner + L"D:P(D;;FA;;;BA)(A;;FA;;;" + owner + L")");
    for (auto* descriptor : {administrator_read.value, administrator_deny.value})
        CHECK(system::inspect_windows_acl(descriptor, policy).owner_sid == policy.trusted_owner_and_writer_sids.front());
}
void unsupported_ace_and_inheritance() {
    const auto policy = synthetic_policy();
    const auto owner = wide(policy.trusted_owner_and_writer_sids.front());
    Security inherited(L"O:" + owner + L"D:P(A;OICIIO;FA;;;CO)(A;;FA;;;" + owner + L")(A;ID;FR;;;WD)");
    CHECK(system::inspect_windows_acl(inherited.value, policy).owner_sid == policy.trusted_owner_and_writer_sids.front());
    Security object_ace(L"O:" + owner +
        L"D:P(OA;;FR;00112233-4455-6677-8899-aabbccddeeff;;WD)(A;;FA;;;" + owner + L")");
    BOOL present = FALSE, defaulted = FALSE;
    PACL dacl = nullptr;
    CHECK(::GetSecurityDescriptorDacl(object_ace.value, &present, &dacl, &defaulted));
    void* raw = nullptr;
    CHECK(present && ::GetAce(dacl, 0, &raw));
    CHECK(static_cast<ACE_HEADER*>(raw)->AceType == ACCESS_ALLOWED_OBJECT_ACE_TYPE);
    rejects_code(ErrorCode::Denied, [&] { (void)system::inspect_windows_acl(object_ace.value, policy); });
    Security owner_rights(L"O:" + owner + L"D:P(A;;FR;;;OW)(A;;FA;;;" + owner + L")");
    rejects_code(ErrorCode::Denied, [&] { (void)system::inspect_windows_acl(owner_rights.value, policy); });
}
struct Fixture {
    std::filesystem::path path;
    Fixture() {
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 100; ++attempt) {
            auto candidate = std::filesystem::temp_directory_path() /
                ("wls-readonly-fixture-" + std::to_string(stamp) + "-" + std::to_string(attempt));
            if (std::filesystem::create_directory(candidate)) { path = std::move(candidate); break; }
        }
        CHECK(!path.empty());
    }
    ~Fixture() { std::error_code error; std::filesystem::remove_all(path, error); }
};
void live_handle_identity_acl_and_replacement() {
    Fixture fixture;
    auto policy = synthetic_policy();
    policy.trusted_owner_and_writer_sids = {current_user()};
    const auto owner = wide(policy.trusted_owner_and_writer_sids.front());
    Security security(L"O:" + owner + L"D:P(A;;FA;;;" + owner + L")");
    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), security.value, FALSE};
    const auto filename = fixture.path / L"original.txt";
    Handle input(::CreateFileW(filename.c_str(), GENERIC_READ | GENERIC_WRITE | READ_CONTROL,
        FILE_SHARE_READ, &attributes, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
    CHECK(input.value != INVALID_HANDLE_VALUE);
    DWORD written = 0;
    CHECK(::WriteFile(input.value, "same", 4, &written, nullptr) && written == 4);
    CHECK(::FlushFileBuffers(input.value));
    const auto first = system::inspect_windows_identity(input.value);
    FILE_ID_INFO expected{};
    CHECK(::GetFileInformationByHandleEx(input.value, FileIdInfo, &expected, sizeof(expected)));
    CHECK(first.volume_serial == expected.VolumeSerialNumber);
    for (std::size_t index = 0; index < first.file_id.size(); ++index)
        CHECK(first.file_id[index] == expected.FileId.Identifier[index]);
    PSECURITY_DESCRIPTOR observed = nullptr;
    CHECK(::GetSecurityInfo(input.value, SE_FILE_OBJECT, OWNER_SECURITY_INFORMATION | DACL_SECURITY_INFORMATION,
        nullptr, nullptr, nullptr, nullptr, &observed) == ERROR_SUCCESS);
    struct Free { PSECURITY_DESCRIPTOR value; ~Free() { ::LocalFree(value); } } free{observed};
    CHECK(system::inspect_windows_acl(observed, policy).owner_sid == policy.trusted_owner_and_writer_sids.front());
    DWORD flags = 0;
    CHECK(::GetHandleInformation(input.value, &flags) && (flags & HANDLE_FLAG_INHERIT) == 0);
    std::barrier rendezvous(2);
    BOOL renamed = TRUE;
    DWORD error = ERROR_SUCCESS;
    std::thread replace([&] {
        rendezvous.arrive_and_wait();
        renamed = ::MoveFileExW(filename.c_str(), (fixture.path / L"replacement.txt").c_str(), 0);
        error = ::GetLastError();
    });
    rendezvous.arrive_and_wait();
    replace.join();
    CHECK(!renamed && (error == ERROR_SHARING_VIOLATION || error == ERROR_ACCESS_DENIED));
    CHECK(system::inspect_windows_identity(input.value) == first);
    CHECK(!std::filesystem::exists(fixture.path / L"replacement.txt"));
    rejects_code(ErrorCode::Unsupported, [] { (void)system::inspect_windows_identity(INVALID_HANDLE_VALUE); });
}
void impersonation_refuses_before_root(const std::filesystem::path& root) {
    auto policy = synthetic_policy();
    policy.trusted_owner_and_writer_sids = {current_user()};
    Handle process, impersonation;
    CHECK(::OpenProcessToken(::GetCurrentProcess(), TOKEN_QUERY | TOKEN_DUPLICATE, &process.value));
    CHECK(::DuplicateToken(process.value, SecurityImpersonation, &impersonation.value));
    CHECK(::SetThreadToken(nullptr, impersonation.value));
    struct Revert { ~Revert() { if (!::RevertToSelf()) std::terminate(); } } revert;
    bool refused = false;
    try { (void)ReadOnlyRoot::open(root / "not-created", policy); }
    catch (const Error& error) {
        CHECK(error.code() == ErrorCode::Denied);
        CHECK(std::string(error.what()).find("impersonation") != std::string::npos);
        refused = true;
    }
    CHECK(refused);
}
#endif
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    const auto root = std::filesystem::path(argv[1]);
    return native_test::run({
        {"missing explicit policy refuses before root open", [&] { missing_policy_rejects_before_path(root); }},
        {"duplicate or unsupported policy entries reject", [&] { duplicate_policy_rejects(root); }},
        {"historical compiled public capture stays compatible", [&] { historical_capture_compatible(root); }},
#ifdef _WIN32
        {"native complete owner and DACL allow deny policy", complete_acl_policy},
        {"native unknown ACE and inheritance boundaries", unsupported_ace_and_inheritance},
        {"native same handle identity ACL and replacement barrier", live_handle_identity_acl_and_replacement},
        {"native thread impersonation refuses before root open", [&] { impersonation_refuses_before_root(root); }},
#endif
    });
}
