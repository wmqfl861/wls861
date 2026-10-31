#include "support.hpp"
#include <array>
#include <chrono>

namespace {
using namespace wls::native;
using native_test::rejects;

void strict_unicode() {
    const std::string text = "\xe6\x97\xa0\xe9\x87\x8f\xe5\xb1\xb1 \xf0\x9f\x8f\x94";
    CHECK(utf16_to_utf8(utf8_to_utf16(text)) == text);
    CHECK(utf8_to_utf16(text).size() == 6);
    CHECK(path_to_utf8(path_from_utf8(text)) == text);
    for (const auto bad : {"\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe4\xb8", "\x80"})
        rejects<Error>([&] { (void)utf8_to_utf16(bad); });
    for (const auto& bad : {std::u16string(1, static_cast<char16_t>(0xd800)),
        std::u16string(1, static_cast<char16_t>(0xdc00)),
        std::u16string{static_cast<char16_t>(0xd800), u'x'}})
        rejects<Error>([&] { (void)utf16_to_utf8(bad); });
    rejects<Error>([] { (void)path_from_utf8(std::string("a\0b", 3)); });
    CHECK(json_string("\"\n\\") == "\"\\\"\\u000a\\\\\"");
}

void refuses_business() {
    for (const auto name : {"store", "fs-read", "fs-write", "run", "cleanup", "call-model"})
        rejects<Error>([&] { (void)run_command({name, "--target", "never-created"}); });
    for (const auto& capability : capabilities()) {
        if (capability.name == "store" || capability.name == "workspace" ||
            capability.name == "process" || capability.name == "sandbox" || capability.name == "cleanup")
            CHECK(capability.status == Status::Unsupported);
    }
    CHECK(actual_executable().is_absolute());
}

void original_public_resources(const std::filesystem::path& root) {
    const auto captured = PublicResources::from_bundle(root);
    CHECK(captured.count() == 35);
    CHECK(captured.total_bytes() == 160549);
    CHECK(captured.bytes("prompts/worker.md").size() == 1502);
    CHECK(captured.manifest_digest() == "b9f5468c6858ebd467fb4e6905d2ef4b8bd51a006e4fd2a636f8e4db1fde55cf");
    rejects<Error>([&] { (void)captured.bytes("../../secret"); });
    rejects<Error>([] { (void)PublicResources::from_bundle("relative"); });
}

void captured_bytes_survive_removal(const std::filesystem::path& root) {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    std::filesystem::path fixture;
    for (int attempt = 0; attempt < 100; ++attempt) {
        const auto candidate = std::filesystem::temp_directory_path() /
            ("wls-native-capture-" + std::to_string(stamp) + "-" + std::to_string(attempt));
        if (std::filesystem::create_directory(candidate)) { fixture = candidate; break; }
    }
    CHECK(!fixture.empty());
    struct Remove { std::filesystem::path path; ~Remove() { std::error_code error; std::filesystem::remove_all(path, error); } } remove{fixture};
    std::filesystem::copy(root, fixture / "bundle", std::filesystem::copy_options::recursive);
    const auto captured = PublicResources::from_bundle(fixture / "bundle");
    const auto original = captured.bytes("prompts/worker.md");
    CHECK(std::filesystem::remove_all(fixture / "bundle") > 0);
    CHECK(captured.bytes("prompts/worker.md") == original);
    CHECK(captured.count() == 35);
}
} // namespace

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    // Test fixture paths supplied by CTest are ASCII. Unicode native paths and
    // wmain argument decoding are exercised by the Python integration suite.
    const auto root = std::filesystem::path(argv[1]);
    return native_test::run({
        {"strict UTF scalar conversion and JSON escaping", strict_unicode},
        {"unsupported business operations reject", refuses_business},
        {"original full public resource catalog", [&] { original_public_resources(root); }},
        {"captured original bytes survive source removal", [&] { captured_bytes_survive_removal(root); }}
    });
}
