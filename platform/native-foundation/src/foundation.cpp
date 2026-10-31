#include "wls/native_foundation.hpp"

#include <array>
#include <cstdint>
#include <set>
#include <utility>

namespace wls::native {
namespace {
[[noreturn]] void invalid(const char* message) { throw Error(ErrorCode::InvalidInput, message); }

std::uint32_t scalar(std::string_view text, std::size_t& offset) {
    const auto first = static_cast<unsigned char>(text[offset++]);
    if (first < 0x80U) return first;
    std::size_t tail = 0;
    std::uint32_t value = 0;
    std::uint32_t minimum = 0;
    if (first >= 0xc2U && first <= 0xdfU) { tail = 1; value = first & 0x1fU; minimum = 0x80U; }
    else if (first >= 0xe0U && first <= 0xefU) { tail = 2; value = first & 0x0fU; minimum = 0x800U; }
    else if (first >= 0xf0U && first <= 0xf4U) { tail = 3; value = first & 0x07U; minimum = 0x10000U; }
    else invalid("invalid UTF-8 leading byte");
    if (tail > text.size() - offset) invalid("truncated UTF-8 sequence");
    for (std::size_t index = 0; index < tail; ++index) {
        const auto next = static_cast<unsigned char>(text[offset++]);
        if ((next & 0xc0U) != 0x80U) invalid("invalid UTF-8 continuation byte");
        value = (value << 6U) | (next & 0x3fU);
    }
    if (value < minimum || value > 0x10ffffU || (value >= 0xd800U && value <= 0xdfffU))
        invalid("UTF-8 does not encode a Unicode scalar");
    return value;
}

void append_scalar(std::string& output, std::uint32_t value) {
    if (value < 0x80U) output.push_back(static_cast<char>(value));
    else if (value < 0x800U) {
        output.push_back(static_cast<char>(0xc0U | (value >> 6U)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else if (value < 0x10000U) {
        output.push_back(static_cast<char>(0xe0U | (value >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    } else {
        output.push_back(static_cast<char>(0xf0U | (value >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((value >> 12U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | ((value >> 6U) & 0x3fU)));
        output.push_back(static_cast<char>(0x80U | (value & 0x3fU)));
    }
}

ReasoningSetting reasoning(std::string_view spelling) {
    if (spelling == "inherit") return {};
    if (spelling == "off") return {ReasoningMode::Disabled, {}};
    if (spelling == "provider_default") return {ReasoningMode::ProviderDefault, {}};
    if (spelling.starts_with("effort:") && spelling.size() > 7)
        return {ReasoningMode::Explicit, std::string(spelling.substr(7))};
    invalid("reasoning must be inherit, off, provider_default or effort:VALUE");
}

std::vector<std::string> comma_values(std::string_view text) {
    std::vector<std::string> result;
    if (text.empty()) return result;
    std::set<std::string> seen;
    while (true) {
        const auto end = text.find(',');
        auto value = std::string(text.substr(0, end));
        if (value.empty() || !seen.insert(value).second) invalid("empty or duplicate comma-separated value");
        result.push_back(std::move(value));
        if (end == std::string_view::npos) return result;
        text.remove_prefix(end + 1);
    }
}

CapabilityRegistry registry(const std::vector<std::string>& specifications) {
    if (specifications.empty()) invalid("at least one explicit capability is required");
    CapabilityRegistry result;
    for (const auto& specification : specifications) {
        const auto equals = specification.find('=');
        if (equals == std::string::npos || equals == 0) invalid("capability must be MODEL=SETTING,...");
        ModelCapabilities capability{{}, false, false};
        for (const auto& setting : comma_values(std::string_view(specification).substr(equals + 1))) {
            const auto value = reasoning(setting);
            switch (value.mode) {
            case ReasoningMode::Disabled: capability.supports_disabled = true; break;
            case ReasoningMode::ProviderDefault: capability.supports_provider_default = true; break;
            case ReasoningMode::Explicit: capability.reasoning_efforts.insert(value.effort); break;
            case ReasoningMode::Inherit: invalid("inherit is not a model capability");
            }
        }
        result.add(specification.substr(0, equals), std::move(capability));
    }
    return result;
}

using Options = std::map<std::string, std::string, std::less<>>;
Options options(const std::vector<std::string>& arguments, std::vector<std::string>& capabilities_out) {
    Options result;
    for (std::size_t index = 1; index < arguments.size(); index += 2) {
        if (index + 1 >= arguments.size()) invalid("option requires a value");
        const auto& name = arguments[index];
        if (name == "--capability") capabilities_out.push_back(arguments[index + 1]);
        else if (!result.emplace(name, arguments[index + 1]).second) invalid("duplicate option");
    }
    return result;
}

void allowed_options(const Options& values, const std::set<std::string>& allowed) {
    for (const auto& [name, value] : values) {
        (void)value;
        if (!allowed.contains(name)) invalid("unknown option for this command");
    }
}

std::string required(const Options& values, std::string_view name) {
    const auto found = values.find(name);
    if (found == values.end()) invalid("required option is missing");
    return found->second;
}

std::string optional(const Options& values, std::string_view name, std::string fallback = {}) {
    const auto found = values.find(name);
    return found == values.end() ? fallback : found->second;
}

std::string capability_json() {
    const auto host = host_facts();
    std::string result = "{\"format\":\"wls-native-capabilities-v1\",\"platform\":" +
        json_string(host.platform) + ",\"token_elevation\":" + json_string(host.token_elevation) +
        ",\"filesystem\":" + json_string(host.filesystem) + ",\"capabilities\":[";
    bool first = true;
    for (const auto& capability : capabilities()) {
        if (!first) result += ',';
        first = false;
        result += "{\"name\":" + json_string(capability.name) + ",\"status\":" +
            json_string(status_name(capability.status)) + ",\"reason\":" + json_string(capability.reason) + '}';
    }
    return result + "]}";
}
} // namespace

std::string_view status_name(Status status) {
    switch (status) {
    case Status::Available: return "available";
    case Status::Unsupported: return "unsupported";
    case Status::Denied: return "denied";
    case Status::NotChecked: return "not_checked";
    }
    invalid("unknown capability status");
}

std::string_view error_name(ErrorCode code) {
    switch (code) {
    case ErrorCode::InvalidInput: return "invalid_input";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::Denied: return "denied";
    case ErrorCode::Missing: return "resource_missing";
    case ErrorCode::Mismatch: return "resource_mismatch";
    case ErrorCode::Io: return "io_error";
    }
    return "unknown_error";
}

std::vector<Capability> capabilities() {
    return {
        {"config_resolution", Status::Available, "original config.cpp frozen-Root resolver"},
        {"tool_grant_calculation", Status::Available, "original Worker-only three-allowlist intersection; calculation only"},
        {"bounded_scheduler", Status::Available, "original scheduler.cpp; in-process callbacks only"},
        {"unicode_conversion", Status::Available, "strict UTF-8 and UTF-16 scalar conversion"},
        {"public_resource_capture", Status::Available, "compiled-approved public distribution bytes; no Workspace authority"},
        {"store", Status::Unsupported, "no native Store lock or database security adapter in this slice"},
        {"workspace", Status::Unsupported, "W-FS ownership, ACL and alias acceptance remain open"},
        {"process", Status::Unsupported, "W-PROC creation, cancellation and exit proof remain open"},
        {"sandbox", Status::Unsupported, "W-SBX token, filesystem, IPC and network isolation remain open"},
        {"cleanup", Status::Unsupported, "W-CLEAN original ownership, exit, inventory and claim gates remain open"},
        {"model_calls", Status::Denied, "this offline slice has no provider transport or call authorization"},
        {"network_isolation", Status::NotChecked, "no network sandbox probe is executed"},
        {"ordinary_user_security", Status::NotChecked, "token observation is not an ACL or sandbox acceptance test"}
    };
}

void require_business_capability(std::string_view operation) {
    if (operation == "call-model") throw Error(ErrorCode::Denied, "model calls are denied in this offline slice");
    throw Error(ErrorCode::Unsupported, "native business operation is unsupported in this foundation slice");
}

std::u16string utf8_to_utf16(std::string_view text) {
    std::u16string result;
    for (std::size_t offset = 0; offset < text.size();) {
        auto value = scalar(text, offset);
        if (value <= 0xffffU) result.push_back(static_cast<char16_t>(value));
        else {
            value -= 0x10000U;
            result.push_back(static_cast<char16_t>(0xd800U | (value >> 10U)));
            result.push_back(static_cast<char16_t>(0xdc00U | (value & 0x3ffU)));
        }
    }
    return result;
}

std::string utf16_to_utf8(std::u16string_view text) {
    std::string result;
    for (std::size_t offset = 0; offset < text.size();) {
        auto value = static_cast<std::uint32_t>(text[offset++]);
        if (value >= 0xd800U && value <= 0xdbffU) {
            if (offset == text.size()) invalid("truncated UTF-16 surrogate pair");
            const auto low = static_cast<std::uint32_t>(text[offset++]);
            if (low < 0xdc00U || low > 0xdfffU) invalid("invalid UTF-16 low surrogate");
            value = 0x10000U + ((value - 0xd800U) << 10U) + low - 0xdc00U;
        } else if (value >= 0xdc00U && value <= 0xdfffU) invalid("unpaired UTF-16 low surrogate");
        append_scalar(result, value);
    }
    return result;
}

std::filesystem::path path_from_utf8(std::string_view text) {
    if (text.empty() || text.size() > 4096 || text.find('\0') != std::string_view::npos)
        invalid("path must be bounded nonempty text without NUL");
    const auto encoded = utf8_to_utf16(text);
    (void)encoded;
#ifdef _WIN32
    return std::filesystem::path(std::wstring(encoded.begin(), encoded.end()));
#else
    return std::filesystem::path(std::string(text));
#endif
}

std::string path_to_utf8(const std::filesystem::path& path) {
#ifdef _WIN32
    const auto& text = path.native();
    return utf16_to_utf8(std::u16string(text.begin(), text.end()));
#else
    (void)utf8_to_utf16(path.native());
    return path.native();
#endif
}

std::string json_string(std::string_view text) {
    (void)utf8_to_utf16(text);
    constexpr std::string_view hex = "0123456789abcdef";
    std::string result = "\"";
    for (const unsigned char byte : text) {
        if (byte == '"' || byte == '\\') { result += '\\'; result += static_cast<char>(byte); }
        else if (byte < 0x20U) { result += "\\u00"; result += hex[byte >> 4U]; result += hex[byte & 0x0fU]; }
        else result += static_cast<char>(byte);
    }
    return result + '"';
}

std::string run_command(const std::vector<std::string>& arguments) {
    if (arguments.empty() || arguments.size() > 128) invalid("one command and bounded arguments are required");
    std::size_t total = 0;
    for (const auto& argument : arguments) {
        total += argument.size();
        if (argument.size() > 8192 || total > 65536 || argument.find('\0') != std::string::npos)
            invalid("command arguments exceed text bounds");
        (void)utf8_to_utf16(argument);
    }
    const auto& command = arguments.front();
    if (command == "capabilities") {
        if (arguments.size() != 1) invalid("capabilities takes no options");
        return capability_json();
    }
    if (command == "store" || command == "fs-read" || command == "fs-write" ||
        command == "run" || command == "cleanup" || command == "call-model")
        require_business_capability(command);
    std::vector<std::string> capability_specs;
    const auto values = options(arguments, capability_specs);
    if (command == "resolve-config") {
        allowed_options(values, {"--root-model", "--root-reasoning", "--model", "--reasoning"});
        const ModelSelection root{required(values, "--root-model"), reasoning(required(values, "--root-reasoning"))};
        const ModelSelection override{optional(values, "--model"), reasoning(optional(values, "--reasoning", "inherit"))};
        const auto resolved = resolve_model(root, override, registry(capability_specs));
        const std::array mode_names{"inherit", "off", "provider_default", "explicit"};
        const auto mode = mode_names.at(static_cast<std::size_t>(resolved.reasoning.mode));
        return "{\"format\":\"wls-native-config-v1\",\"model\":" + json_string(resolved.model) +
            ",\"reasoning\":{\"mode\":" + json_string(mode) + ",\"effort\":" + json_string(resolved.reasoning.effort) + "}}";
    }
    if (!capability_specs.empty()) invalid("capability options only belong to resolve-config");
    if (command == "tool-grants") {
        allowed_options(values, {"--role", "--role-tools", "--system-tools", "--request-tools"});
        const auto role_name = required(values, "--role");
        const std::map<std::string, Role> roles{{"root", Role::Root}, {"dispatcher", Role::Dispatcher},
            {"teamlead", Role::TeamLead}, {"worker", Role::Worker}};
        const auto role = roles.find(role_name);
        if (role == roles.end()) invalid("unknown role");
        const auto tools = [&](std::string_view name) {
            const auto parsed = comma_values(required(values, name));
            return ToolSet(parsed.begin(), parsed.end());
        };
        const auto grants = effective_tool_grants(role->second, tools("--role-tools"),
            tools("--system-tools"), tools("--request-tools"));
        std::string result = "{\"format\":\"wls-native-tool-grants-v1\",\"calculation_only\":true,\"tools\":[";
        bool first = true;
        for (const auto& tool : grants) { if (!first) result += ','; first = false; result += json_string(tool); }
        return result + "]}";
    }
    if (command == "resources") {
        allowed_options(values, {"--resource-root"});
        const auto found = values.find("--resource-root");
        const auto captured = found == values.end() ? PublicResources::discover() :
            PublicResources::from_bundle(path_from_utf8(found->second));
        return "{\"format\":\"wls-native-public-resources-v1\",\"asset_count\":" +
            std::to_string(captured.count()) + ",\"asset_bytes\":" + std::to_string(captured.total_bytes()) +
            ",\"manifest_sha256\":" + json_string(captured.manifest_digest()) + '}';
    }
    invalid("unknown native foundation command");
}
} // namespace wls::native
