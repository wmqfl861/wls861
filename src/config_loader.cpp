#include "wls/config_loader.hpp"

#include <yaml.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <optional>
#include <set>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

namespace wls::s1 {
namespace {

using Object = boost::json::object;
using Array = boost::json::array;
using Fields = std::initializer_list<std::pair<std::string_view, char>>;

[[noreturn]] void invalid(std::string_view where, std::string_view reason) {
    throw Error(ErrorCode::InvalidInput, std::string(where) + ": " + std::string(reason));
}

[[noreturn]] void unsupported(std::string_view where, std::string_view reason) {
    throw Error(ErrorCode::Unsupported, std::string(where) + ": " + std::string(reason));
}

std::string child(std::string_view parent, std::string_view key) {
    return std::string(parent) + "." + std::string(key);
}

std::string string_value(const Json& value, std::string_view where,
                         bool allow_empty = false, std::size_t maximum = 65536) {
    if (!value.is_string()) invalid(where, "expected a string");
    const auto& text = value.as_string();
    if ((!allow_empty && text.empty()) || text.size() > maximum ||
        text.find('\0') != boost::json::string::npos) {
        invalid(where, "string is empty, oversized, or contains NUL");
    }
    return std::string(text.data(), text.size());
}

std::uint64_t integer_value(const Json& value, std::string_view where,
                            std::uint64_t minimum = 1,
                            std::uint64_t maximum = 1000000000000ULL) {
    std::uint64_t number = 0;
    if (value.is_uint64()) number = value.as_uint64();
    else if (value.is_int64() && value.as_int64() >= 0) {
        number = static_cast<std::uint64_t>(value.as_int64());
    } else invalid(where, "expected a nonnegative integer");
    if (number < minimum || number > maximum) invalid(where, "integer is outside its allowed range");
    return number;
}

double number_value(const Json& value, std::string_view where,
                    double minimum = 0, double maximum = 1000000000000.0) {
    double number = 0;
    if (value.is_double()) number = value.as_double();
    else if (value.is_int64()) number = static_cast<double>(value.as_int64());
    else if (value.is_uint64()) number = static_cast<double>(value.as_uint64());
    else invalid(where, "expected a number");
    if (!std::isfinite(number) || number < minimum || number > maximum) {
        invalid(where, "number is outside its allowed finite range");
    }
    return number;
}

bool bool_value(const Json& value, std::string_view where) {
    if (!value.is_bool()) invalid(where, "expected true or false");
    return value.as_bool();
}

const Json& required(const Json& value, std::string_view key, std::string_view where) {
    if (!value.is_object()) invalid(where, "expected a mapping");
    const auto found = value.as_object().find(key);
    if (found == value.as_object().end()) invalid(child(where, key), "required field is missing");
    return found->value();
}

const Json* optional(const Json& value, std::string_view key) {
    if (!value.is_object()) return nullptr;
    const auto found = value.as_object().find(key);
    return found == value.as_object().end() ? nullptr : &found->value();
}

std::set<std::string> strings(const Json& value, std::string_view where,
                             std::size_t maximum = 256) {
    if (!value.is_array() || value.as_array().size() > maximum) invalid(where, "expected a bounded sequence");
    std::set<std::string> result;
    for (const auto& item : value.as_array()) {
        if (!result.insert(string_value(item, where, false, 256)).second) invalid(where, "duplicate sequence member");
    }
    return result;
}

void fields(const Json& value, std::string_view where, Fields allowed,
            std::initializer_list<std::string_view> optional_keys = {}) {
    if (!value.is_object()) invalid(where, "expected a mapping");
    for (const auto& entry : value.as_object()) {
        const std::string_view key(entry.key().data(), entry.key().size());
        if (std::none_of(allowed.begin(), allowed.end(), [&](const auto& field) { return field.first == key; })) {
            invalid(child(where, key), "unknown field");
        }
    }
    for (const auto& [key, type] : allowed) {
        const auto* item = optional(value, key);
        if (!item) {
            if (std::find(optional_keys.begin(), optional_keys.end(), key) != optional_keys.end()) continue;
            invalid(child(where, key), "required field is missing");
        }
        const auto location = child(where, key);
        switch (type) {
        case 's': (void)string_value(*item, location); break;
        case 'b': (void)bool_value(*item, location); break;
        case 'i': (void)integer_value(*item, location); break;
        case 'z': (void)integer_value(*item, location, 0); break;
        case 'n': (void)number_value(*item, location); break;
        case 'a': (void)strings(*item, location); break;
        case '-': break;
        default: invalid(location, "invalid host field descriptor");
        }
    }
}

void literal(const Json& object, std::string_view key, std::string_view expected,
             std::string_view where) {
    if (string_value(required(object, key, where), child(where, key)) != expected) {
        invalid(child(where, key), "unsupported policy value");
    }
}

void flag(const Json& object, std::string_view key, bool expected, std::string_view where) {
    if (bool_value(required(object, key, where), child(where, key)) != expected) {
        invalid(child(where, key), "required safety policy is not enabled");
    }
}

bool safe_identifier(std::string_view text) {
    if (text.empty() || text.size() > 64 || text.front() < 'a' || text.front() > 'z') return false;
    return std::all_of(text.begin(), text.end(), [](char byte) {
        return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_' || byte == '-';
    });
}

std::string identifier(const Json& value, std::string_view where) {
    const auto text = string_value(value, where, false, 64);
    if (!safe_identifier(text)) invalid(where, "expected a lowercase symbolic identifier");
    return text;
}

void validate_options(const ConfigLoadOptions& options) {
    if (options.max_file_bytes == 0 || options.max_file_bytes > 16 * 1024 * 1024 ||
        options.max_total_bytes < options.max_file_bytes || options.max_total_bytes > 64 * 1024 * 1024 ||
        options.max_yaml_depth == 0 || options.max_yaml_depth > 128 ||
        options.max_yaml_nodes == 0 || options.max_yaml_nodes > 1000000) {
        invalid("configuration limits", "invalid host limits");
    }
}

enum class NumericScalar { String, Integer, Real };

NumericScalar numeric_scalar(std::string_view text) {
    std::size_t index = !text.empty() && text.front() == '-' ? 1 : 0;
    const auto digits = [&](std::size_t& position) {
        const auto begin = position;
        while (position < text.size() && text[position] >= '0' && text[position] <= '9') ++position;
        return position != begin;
    };
    if (index == text.size()) return NumericScalar::String;
    const auto integer_begin = index;
    if (!digits(index) || (text[integer_begin] == '0' && index - integer_begin != 1)) return NumericScalar::String;
    if (index == text.size()) return NumericScalar::Integer;
    if (text[index] == '.') {
        ++index;
        if (!digits(index)) return NumericScalar::String;
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) ++index;
        if (!digits(index)) return NumericScalar::String;
    }
    return index == text.size() ? NumericScalar::Real : NumericScalar::String;
}

class YamlReader {
public:
    YamlReader(std::string_view bytes, std::string label, const ConfigLoadOptions& options)
        : label_(std::move(label)), options_(options) {
        if (bytes.size() > options.max_file_bytes) invalid(label_, "file byte limit exceeded");
        if (!yaml_parser_initialize(&parser_)) throw Error(ErrorCode::Storage, "cannot initialize YAML parser");
        yaml_parser_set_input_string(&parser_, reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size());
    }
    ~YamlReader() {
        if (has_event_) yaml_event_delete(&event_);
        yaml_parser_delete(&parser_);
    }
    YamlReader(const YamlReader&) = delete;
    YamlReader& operator=(const YamlReader&) = delete;

    Json parse() {
        next();
        expect(YAML_STREAM_START_EVENT);
        next();
        expect(YAML_DOCUMENT_START_EVENT);
        // Explicit YAML versions/tags may alter scalar meanings. This format
        // deliberately uses a single schema with quoted ambiguous strings.
        if (event_.data.document_start.version_directive ||
            event_.data.document_start.tag_directives.start != event_.data.document_start.tag_directives.end) {
            fail("YAML directives are not supported");
        }
        next();
        auto result = node(0);
        expect(YAML_DOCUMENT_END_EVENT);
        next();
        if (event_.type != YAML_STREAM_END_EVENT) fail("only one YAML document is allowed");
        return result;
    }

private:
    [[noreturn]] void fail(std::string_view reason) const {
        invalid(label_ + ":" + std::to_string(event_.start_mark.line + 1) + ":" +
                std::to_string(event_.start_mark.column + 1), reason);
    }
    void expect(yaml_event_type_t type) const {
        if (event_.type != type) fail("unexpected YAML structure");
    }
    void next() {
        if (has_event_) yaml_event_delete(&event_);
        has_event_ = false;
        if (!yaml_parser_parse(&parser_, &event_)) {
            invalid(label_ + ":" + std::to_string(parser_.problem_mark.line + 1) + ":" +
                    std::to_string(parser_.problem_mark.column + 1), "invalid YAML syntax");
        }
        has_event_ = true;
    }
    Json scalar() {
        if (event_.data.scalar.anchor || event_.data.scalar.tag) fail("YAML anchors and explicit tags are not allowed");
        const std::string text(reinterpret_cast<const char*>(event_.data.scalar.value), event_.data.scalar.length);
        const bool plain = event_.data.scalar.style == YAML_PLAIN_SCALAR_STYLE;
        Json result(text);
        if (plain) {
            if (text == "null" || text == "Null" || text == "NULL" || text == "~" || text.empty()) result = nullptr;
            else if (text == "true" || text == "True" || text == "TRUE") result = true;
            else if (text == "false" || text == "False" || text == "FALSE") result = false;
            else if (text == "off" || text == "Off" || text == "OFF" || text == "on" || text == "On" || text == "ON" ||
                     text == "yes" || text == "Yes" || text == "YES" || text == "no" || text == "No" || text == "NO") {
                fail("quote ambiguous YAML strings such as off; use true/false for booleans");
            } else {
                const auto numeric = numeric_scalar(text);
                if (numeric == NumericScalar::Integer) {
                    std::int64_t integer = 0;
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), integer);
                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size()) fail("YAML integer is outside int64 range");
                    result = integer;
                } else if (numeric == NumericScalar::Real) {
                    double number = 0;
                    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), number);
                    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !std::isfinite(number)) {
                        fail("YAML number is outside finite range");
                    }
                    result = number;
                } else if (text == ".nan" || text == ".NaN" || text == ".NAN" || text == ".inf" || text == "-.inf" || text == "+.inf") {
                    fail("nonfinite YAML numbers are not allowed");
                }
            }
        }
        next();
        return result;
    }
    Json node(std::size_t depth) {
        if (depth > options_.max_yaml_depth || ++nodes_ > options_.max_yaml_nodes) fail("YAML depth or node limit exceeded");
        if (event_.type == YAML_SCALAR_EVENT) return scalar();
        if (event_.type == YAML_ALIAS_EVENT) fail("YAML aliases are not allowed");
        if (event_.type == YAML_SEQUENCE_START_EVENT) {
            if (event_.data.sequence_start.anchor || event_.data.sequence_start.tag) fail("YAML anchors and explicit tags are not allowed");
            Array result;
            next();
            while (event_.type != YAML_SEQUENCE_END_EVENT) result.push_back(node(depth + 1));
            next();
            return result;
        }
        if (event_.type == YAML_MAPPING_START_EVENT) {
            if (event_.data.mapping_start.anchor || event_.data.mapping_start.tag) fail("YAML anchors and explicit tags are not allowed");
            Object result;
            next();
            while (event_.type != YAML_MAPPING_END_EVENT) {
                const auto key = node(depth + 1);
                if (!key.is_string()) fail("mapping keys must be strings");
                const auto name = string_value(key, label_, false, 128);
                if (name == "<<") fail("YAML merge keys are not allowed");
                if (result.contains(name)) fail("duplicate YAML mapping key");
                result.emplace(name, node(depth + 1));
            }
            next();
            return result;
        }
        fail("expected YAML scalar, sequence or mapping");
    }

    yaml_parser_t parser_{};
    yaml_event_t event_{};
    bool has_event_{false};
    std::string label_;
    const ConfigLoadOptions& options_;
    std::size_t nodes_{0};
};

Json yaml_value(std::string_view bytes, std::string_view label, const ConfigLoadOptions& options) {
    return YamlReader(bytes, std::string(label), options).parse();
}

std::string relative_path(const Json& value, std::string_view where) {
    const auto text = string_value(value, where, false, 512);
    const std::filesystem::path path(text);
    if (path.is_absolute() || text.find('\\') != std::string::npos || path.empty()) invalid(where, "expected a confined relative path");
    for (const auto& component : path) {
        if (component == "." || component == ".." || component.empty()) invalid(where, "path traversal or empty component is not allowed");
    }
    if (path.generic_string() != text) invalid(where, "path must be normalized");
    return text;
}

class Descriptor {
public:
    explicit Descriptor(int value) : value_(value) {}
    ~Descriptor() { if (value_ >= 0) (void)::close(value_); }
    Descriptor(const Descriptor&) = delete;
    Descriptor& operator=(const Descriptor&) = delete;
    Descriptor(Descriptor&& other) noexcept : value_(std::exchange(other.value_, -1)) {}
    Descriptor& operator=(Descriptor&& other) noexcept {
        if (this != &other) { if (value_ >= 0) (void)::close(value_); value_ = std::exchange(other.value_, -1); }
        return *this;
    }
    int get() const noexcept { return value_; }
private:
    int value_;
};

Descriptor open_directory(const std::filesystem::path& path, std::string_view label) {
    std::filesystem::path absolute;
    try { absolute = path.empty() ? std::filesystem::current_path() : std::filesystem::absolute(path); }
    catch (const std::filesystem::filesystem_error&) { invalid(label, "cannot resolve directory path"); }
    Descriptor current(::open("/", O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
    if (current.get() < 0) invalid(label, "cannot open configuration root");
    // Preserve parent steps until after checking their preceding components:
    // lexically collapsing link/../file could otherwise hide a symbolic link.
    for (const auto& component : absolute.relative_path()) {
        const auto name = component.string();
        Descriptor next(::openat(current.get(), name.c_str(), O_RDONLY | O_CLOEXEC | O_DIRECTORY | O_NOFOLLOW));
        if (next.get() < 0) invalid(label, "directory is missing, inaccessible, or a symbolic link");
        current = std::move(next);
    }
    return current;
}

std::string read_file(const std::filesystem::path& path, std::string_view label,
                      const ConfigLoadOptions& options, std::size_t& total) {
    auto directory = open_directory(path.parent_path(), label);
    Descriptor file(::openat(directory.get(), path.filename().c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK));
    struct stat metadata{};
    if (file.get() < 0 || ::fstat(file.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
        invalid(label, "expected an accessible regular file without symbolic links");
    }
    if (static_cast<std::uintmax_t>(metadata.st_size) > options.max_file_bytes) invalid(label, "file byte limit exceeded");
    std::string bytes;
    std::array<char, 8192> buffer{};
    for (;;) {
        const auto received = ::read(file.get(), buffer.data(), buffer.size());
        if (received < 0) { if (errno == EINTR) continue; invalid(label, "file read failed"); }
        if (received == 0) break;
        const auto count = static_cast<std::size_t>(received);
        if (count > options.max_file_bytes - bytes.size() || count > options.max_total_bytes - total) {
            invalid(label, "configuration byte limit exceeded");
        }
        bytes.append(buffer.data(), count);
        total += count;
    }
    if (bytes.find('\0') != std::string::npos) invalid(label, "configuration text contains NUL");
    return bytes;
}

ReasoningSetting reasoning(const Json* value, std::string_view where, bool allow_inherit) {
    if (!value || value->is_null()) {
        if (!allow_inherit) invalid(where, "Root defaults must specify reasoning");
        return {};
    }
    fields(*value, where, {{"mode", 's'}, {"effort", 's'}}, {"effort"});
    const auto mode = string_value(required(*value, "mode", where), child(where, "mode"));
    const auto* effort = optional(*value, "effort");
    if (mode == "effort") {
        if (!effort) invalid(where, "effort reasoning requires its explicit effort");
        return {ReasoningMode::Explicit, string_value(*effort, child(where, "effort"), false, 64)};
    }
    if (effort) invalid(where, "only effort mode accepts an effort field");
    if (mode == "off") return {ReasoningMode::Disabled, ""};
    if (mode == "provider_default") return {ReasoningMode::ProviderDefault, ""};
    unsupported(where, "reasoning mode is not implemented; use null, provider_default, quoted off, or effort");
}

ModelSelection selection(const Json& value, std::string_view where, bool allow_inherit,
                         bool allow_prompt = false) {
    if (allow_prompt) fields(value, where, {{"model_ref", '-'}, {"reasoning", '-'}, {"prompt_append", 's'}},
                            {"model_ref", "reasoning", "prompt_append"});
    else fields(value, where, {{"model_ref", '-'}, {"reasoning", '-'}},
                allow_inherit ? std::initializer_list<std::string_view>{"model_ref", "reasoning"}
                              : std::initializer_list<std::string_view>{});
    std::string model;
    const auto* reference = optional(value, "model_ref");
    if (reference && !reference->is_null()) model = identifier(*reference, child(where, "model_ref"));
    if (model.empty() && !allow_inherit) invalid(where, "Root defaults must specify model_ref");
    return {std::move(model), reasoning(optional(value, "reasoning"), child(where, "reasoning"), allow_inherit)};
}

Json reasoning_json(const ReasoningSetting& value) {
    switch (value.mode) {
    case ReasoningMode::ProviderDefault: return Object{{"mode", "provider_default"}};
    case ReasoningMode::Disabled: return Object{{"mode", "off"}};
    case ReasoningMode::Explicit: return Object{{"mode", "effort"}, {"effort", value.effort}};
    case ReasoningMode::Inherit: return nullptr;
    }
    invalid("reasoning", "invalid host reasoning enum");
}

std::string checked_role_name(Role role) {
    switch (role) {
    case Role::Root: return "root";
    case Role::Dispatcher: return "dispatcher";
    case Role::TeamLead: return "team_lead";
    case Role::Worker: return "worker";
    }
    invalid("role", "unknown role");
}

CapabilityRegistry validate_models(const Json& value) {
    if (!value.is_object() || value.as_object().empty() || value.as_object().size() > 64) invalid("system.models", "expected 1 to 64 model profiles");
    CapabilityRegistry registry;
    for (const auto& item : value.as_object()) {
        const std::string name(item.key().data(), item.key().size());
        if (!safe_identifier(name)) invalid("system.models", "invalid model_ref");
        const auto where = child("system.models", name);
        const auto& profile = item.value();
        fields(profile, where, {{"adapter", 's'}, {"model", 's'}, {"capabilities", '-'},
                               {"adapter_version", 's'}, {"protocol", 's'}});
        if (string_value(required(profile, "adapter", where), where) != "fake") unsupported(where, "only the fake adapter is implemented in this increment");
        if (string_value(required(profile, "adapter_version", where), where) != "1") {
            unsupported(where, "unknown fake adapter version");
        }
        if (string_value(required(profile, "protocol", where), where) != "fake-v1") {
            unsupported(where, "unknown fake protocol");
        }
        const auto& capabilities = required(profile, "capabilities", where);
        const auto cap_where = child(where, "capabilities");
        fields(capabilities, cap_where, {{"streaming", 's'}, {"tool_calling", 's'}, {"parallel_tool_calls", 's'},
                                         {"reasoning_modes", 'a'}, {"reasoning_efforts", 'a'}, {"context_window", 'i'},
                                         {"max_output_tokens", 'i'}, {"cache_mode", 's'}}, {"reasoning_efforts"});
        for (const auto key : {"streaming", "tool_calling", "parallel_tool_calls"}) {
            const auto status = string_value(required(capabilities, key, cap_where), child(cap_where, key));
            if (status != "supported" && status != "unsupported" && status != "unknown") invalid(child(cap_where, key), "expected supported, unsupported, or unknown");
        }
        if (string_value(required(capabilities, "streaming", cap_where), child(cap_where, "streaming")) != "unsupported") {
            unsupported(child(cap_where, "streaming"), "the fake adapter has no streaming transport");
        }
        const auto modes = strings(required(capabilities, "reasoning_modes", cap_where), child(cap_where, "reasoning_modes"), 16);
        if (modes.empty()) invalid(cap_where, "at least one reasoning mode must be declared");
        for (const auto& mode : modes) if (mode != "provider_default" && mode != "off" && mode != "effort") unsupported(cap_where, "unknown reasoning mode capability");
        ModelCapabilities model_caps;
        model_caps.supports_disabled = modes.contains("off");
        model_caps.supports_provider_default = modes.contains("provider_default");
        if (const auto* efforts = optional(capabilities, "reasoning_efforts")) model_caps.reasoning_efforts = strings(*efforts, child(cap_where, "reasoning_efforts"), 32);
        if (modes.contains("effort") == model_caps.reasoning_efforts.empty()) invalid(cap_where, "effort mode and nonempty reasoning_efforts must be declared together");
        const auto context_window = integer_value(required(capabilities, "context_window", cap_where), cap_where, 1, 100000000);
        (void)integer_value(required(capabilities, "max_output_tokens", cap_where), cap_where, 1, context_window);
        literal(capabilities, "cache_mode", "none", cap_where);
        registry.add(name, std::move(model_caps));
    }
    return registry;
}

CapabilityRegistry validate_system(const Json& system) {
    fields(system, "system", {{"schema_version", 'i'}, {"spec_version", 's'}, {"models", '-'}, {"defaults", '-'},
        {"model_calls", '-'}, {"request_config", '-'}, {"limits", '-'}, {"amendment", '-'}, {"control", '-'},
        {"budget_defaults", '-'}, {"storage", '-'}, {"context", '-'}, {"sandbox", '-'}, {"cleanup", '-'},
        {"memory", '-'}, {"paths", '-'}});
    (void)integer_value(required(system, "schema_version", "system"), "system.schema_version", 1, 1);
    literal(system, "spec_version", "0.4.1", "system");
    auto registry = validate_models(required(system, "models", "system"));
    try { validate_root_defaults(selection(required(system, "defaults", "system"), "system.defaults", false), registry); }
    catch (const ConfigurationError& error) { invalid("system.defaults", error.what()); }

    const auto& calls = required(system, "model_calls", "system");
    fields(calls, "system.model_calls", {{"real_calls_enabled", 'b'}, {"max_paid_retries", 'z'},
        {"charge_all_attempts", 'b'}, {"require_fresh_reservation_for_retry", 'b'}, {"unknown_cost_policy", 's'}});
    if (bool_value(required(calls, "real_calls_enabled", "system.model_calls"), "system.model_calls.real_calls_enabled")) unsupported("system.model_calls", "real model calls are not implemented or authorized by configuration loading");
    if (integer_value(required(calls, "max_paid_retries", "system.model_calls"), "system.model_calls.max_paid_retries", 0) != 0) unsupported("system.model_calls", "paid retries are not implemented; max_paid_retries must remain zero");
    flag(calls, "charge_all_attempts", true, "system.model_calls");
    flag(calls, "require_fresh_reservation_for_retry", true, "system.model_calls");
    literal(calls, "unknown_cost_policy", "hold_reservation", "system.model_calls");

    const auto& frozen = required(system, "request_config", "system");
    fields(frozen, "system.request_config", {{"freeze_at_request_creation", 'b'}, {"inherit_from_frozen_root", 'b'},
        {"restore_from_request_snapshot", 'b'}, {"implicit_active_request_migration", 'b'}});
    for (const auto key : {"freeze_at_request_creation", "inherit_from_frozen_root", "restore_from_request_snapshot"}) flag(frozen, key, true, "system.request_config");
    flag(frozen, "implicit_active_request_migration", false, "system.request_config");

    const auto& limits = required(system, "limits", "system");
    fields(limits, "system.limits", {{"max_active_dispatchers", 'i'}, {"max_active_workers", 'i'},
        {"reserve_dispatchers_for_review", 'i'}, {"reserve_workers_for_review", 'i'}, {"review_reservation_policy", 's'},
        {"release_slots_while_waiting", 'b'}, {"max_teams_per_request", 'i'}, {"max_concurrent_model_calls_per_endpoint", 'i'},
        {"max_concurrent_ops", 'i'}, {"worker_max_tool_calls_per_order", 'i'}, {"manager_max_tool_calls_per_wake", 'i'},
        {"team_internal_rework_max", 'z'}, {"stall_minutes", 'i'}, {"stall_same_error_repeats", 'i'}});
    for (const auto& [total_key, reserve_key] : {std::pair{"max_active_dispatchers", "reserve_dispatchers_for_review"},
                                               std::pair{"max_active_workers", "reserve_workers_for_review"}}) {
        const auto total = integer_value(required(limits, total_key, "system.limits"), child("system.limits", total_key), 2, 4096);
        (void)integer_value(required(limits, reserve_key, "system.limits"), child("system.limits", reserve_key), 1, total - 1);
    }
    (void)integer_value(required(limits, "manager_max_tool_calls_per_wake", "system.limits"), "system.limits.manager_max_tool_calls_per_wake", 1, 12);
    literal(limits, "review_reservation_policy", "always", "system.limits");
    flag(limits, "release_slots_while_waiting", true, "system.limits");

    const auto& amendment = required(system, "amendment", "system");
    fields(amendment, "system.amendment", {{"require_explicit_task_disposition", 'b'}, {"dispositions", 'a'},
        {"unhandled_task_policy", 's'}, {"keep_requires_new_instruction_and_ack", 'b'}});
    flag(amendment, "require_explicit_task_disposition", true, "system.amendment");
    flag(amendment, "keep_requires_new_instruction_and_ack", true, "system.amendment");
    literal(amendment, "unhandled_task_policy", "remain_paused", "system.amendment");
    if (strings(required(amendment, "dispositions", "system.amendment"), "system.amendment.dispositions") != std::set<std::string>{"update", "keep", "cancel"}) invalid("system.amendment.dispositions", "update, keep and cancel are all required");

    const auto& control = required(system, "control", "system");
    fields(control, "system.control", {{"ordinary_budget_blocks_control", 'b'}, {"acknowledge_only_pending_instruction", 'b'},
        {"acknowledge_allowed_with_unknown_operations", 'b'}, {"unknown_blocks_new_business", 'b'},
        {"diagnostic_tools", 'a'}, {"max_queries_per_minute", 'i'}});
    flag(control, "ordinary_budget_blocks_control", false, "system.control");
    for (const auto key : {"acknowledge_only_pending_instruction", "acknowledge_allowed_with_unknown_operations", "unknown_blocks_new_business"}) flag(control, key, true, "system.control");
    const std::set<std::string> diagnostic{"fs_list", "fs_read", "fs_search", "op_status", "op_output"};
    for (const auto& tool : strings(required(control, "diagnostic_tools", "system.control"), "system.control.diagnostic_tools")) if (!diagnostic.contains(tool)) invalid("system.control.diagnostic_tools", "tool is not a registered read-only diagnostic");

    const auto& budget = required(system, "budget_defaults", "system");
    fields(budget, "system.budget_defaults", {{"max_input_tokens", '-'}, {"max_output_tokens", '-'}, {"max_cost_usd", '-'},
        {"max_wall_minutes", 'i'}, {"warn_at_fraction", 'n'}});
    for (const auto key : {"max_input_tokens", "max_output_tokens"}) {
        const auto& value = required(budget, key, "system.budget_defaults");
        if (!value.is_null()) (void)integer_value(value, child("system.budget_defaults", key));
    }
    const auto& cost = required(budget, "max_cost_usd", "system.budget_defaults");
    if (!cost.is_null()) (void)number_value(cost, "system.budget_defaults.max_cost_usd", 0.000001);
    (void)number_value(required(budget, "warn_at_fraction", "system.budget_defaults"), "system.budget_defaults.warn_at_fraction", 0.01, 0.99);

    const auto& storage = required(system, "storage", "system");
    fields(storage, "system.storage", {{"engine", 's'}, {"journal_mode", 's'}, {"synchronous", 's'},
        {"foreign_keys", 'b'}, {"allow_normal_durability", 'b'}});
    literal(storage, "engine", "sqlite", "system.storage");
    literal(storage, "journal_mode", "WAL", "system.storage");
    literal(storage, "synchronous", "FULL", "system.storage");
    flag(storage, "foreign_keys", true, "system.storage");
    flag(storage, "allow_normal_durability", false, "system.storage");

    const auto& context = required(system, "context", "system");
    fields(context, "system.context", {{"compaction_at_fraction", 'n'}, {"snapshot_decision_log_entries", 'i'},
        {"schema_retry_max", 'z'}, {"cache_dependencies", 'a'}, {"policy_updates_use_runtime_gate", 'b'}});
    (void)number_value(required(context, "compaction_at_fraction", "system.context"), "system.context.compaction_at_fraction", 0.1, 0.95);
    flag(context, "policy_updates_use_runtime_gate", true, "system.context");
    const std::set<std::string> required_dependencies{"adapter_version", "effective_model_and_reasoning", "tool_definitions_hash",
        "system_role_function_prompt_hash", "function_template_version", "environment_card_hash", "pinned_memory_revisions",
        "rule_scope_snapshot_hash", "project_snapshot_hash"};
    if (strings(required(context, "cache_dependencies", "system.context"), "system.context.cache_dependencies") != required_dependencies) invalid("system.context.cache_dependencies", "the complete defined dependency set is required");

    const auto& sandbox = required(system, "sandbox", "system");
    fields(sandbox, "system.sandbox", {{"engine", 's'}, {"network_default", 's'}, {"allow_weak_isolation", 'b'},
        {"require_cgroup_for_strong_isolation", 'b'}, {"extra_ro_binds", 'a'}, {"run_default_timeout_s", 'i'},
        {"run_max_timeout_s", 'i'}, {"team_memory_max", 's'}, {"team_pids_max", 'i'}});
    literal(sandbox, "engine", "bwrap", "system.sandbox");
    const auto network = string_value(required(sandbox, "network_default", "system.sandbox"), "system.sandbox.network_default");
    if (network != "none" && network != "host") invalid("system.sandbox.network_default", "expected none or explicit host network");
    flag(sandbox, "require_cgroup_for_strong_isolation", true, "system.sandbox");
    const auto maximum_timeout = integer_value(required(sandbox, "run_max_timeout_s", "system.sandbox"), "system.sandbox.run_max_timeout_s", 1, 86400);
    (void)integer_value(required(sandbox, "run_default_timeout_s", "system.sandbox"), "system.sandbox.run_default_timeout_s", 1, maximum_timeout);
    const auto memory_size = string_value(required(sandbox, "team_memory_max", "system.sandbox"), "system.sandbox.team_memory_max", false, 64);
    auto memory_digits = std::string_view(memory_size);
    if (!memory_digits.empty() && (memory_digits.back() == 'K' || memory_digits.back() == 'M' || memory_digits.back() == 'G')) memory_digits.remove_suffix(1);
    if (memory_digits.empty() || memory_digits.front() < '1' || memory_digits.front() > '9' ||
        !std::all_of(memory_digits.begin(), memory_digits.end(), [](char c) { return c >= '0' && c <= '9'; }))
        invalid("system.sandbox.team_memory_max", "expected positive bytes or K/M/G size");
    if (!strings(required(sandbox, "extra_ro_binds", "system.sandbox"), "system.sandbox.extra_ro_binds").empty()) unsupported("system.sandbox.extra_ro_binds", "additional host mounts need the later sandbox integration");

    const auto& cleanup = required(system, "cleanup", "system");
    fields(cleanup, "system.cleanup", {{"enabled", 'b'}, {"policy", 's'}, {"cli_receipt_mode", 's'}, {"api_receipt_mode", 's'},
        {"receipt_ttl_days", 'i'}, {"require_all_team_experience_jobs", 'b'}, {"require_unknown_outcomes_reconciled", 'b'},
        {"preserve_job_manifest_until_complete", 'b'}, {"resume_terminal_jobs", 'b'}, {"vacuum_weekly", 'b'}});
    (void)bool_value(required(cleanup, "enabled", "system.cleanup"), "system.cleanup.enabled");
    // Opt-in only; Store 407 and explicit host cleanup authorize deletion.
    literal(cleanup, "policy", "clean_runtime", "system.cleanup");
    literal(cleanup, "cli_receipt_mode", "durable_local", "system.cleanup");
    literal(cleanup, "api_receipt_mode", "explicit_ack", "system.cleanup");
    for (const auto key : {"require_all_team_experience_jobs", "require_unknown_outcomes_reconciled", "preserve_job_manifest_until_complete", "resume_terminal_jobs"}) flag(cleanup, key, true, "system.cleanup");

    const auto& memory = required(system, "memory", "system");
    fields(memory, "system.memory", {{"enabled", 'b'}, {"governance", 's'}, {"require_scoped_auto_publish_policy", 'b'},
        {"auto_merge", 'b'}, {"require_versioned_load_bindings", 'b'}, {"require_expected_base_revision", 'b'},
        {"card_exposure_counts_as_use", 'b'}, {"score_policy_version", 's'}, {"source_grouping_policy", 's'},
        {"filter_before_limit", 'b'}, {"rules_overflow", 's'}, {"revocation_gate", 's'}, {"reindex_grants_approval", 'b'},
        {"digest_time", 's'}, {"card_max_per_trigger", 'i'}, {"card_max_tokens", 'i'}, {"recall_max", 'i'},
        {"error_trigger_max", 'i'}, {"rules_max_tokens", 'i'}, {"score_z", 'n'}, {"rank_alpha", 'n'},
        {"warn_min_failures", 'i'}, {"warn_confidence_below", 'n'}, {"merge_min_successes", 'i'},
        {"merge_min_failures", 'i'}, {"embedding_model_ref", '-'}});
    if (bool_value(required(memory, "enabled", "system.memory"), "system.memory.enabled")) unsupported("system.memory.enabled", "the memory runtime is not implemented in this increment");
    for (const auto key : {"require_scoped_auto_publish_policy", "require_versioned_load_bindings", "require_expected_base_revision", "filter_before_limit"}) flag(memory, key, true, "system.memory");
    for (const auto key : {"auto_merge", "card_exposure_counts_as_use", "reindex_grants_approval"}) flag(memory, key, false, "system.memory");
    literal(memory, "governance", "personal", "system.memory");
    literal(memory, "score_policy_version", "posterior_lower_heuristic_v1", "system.memory");
    literal(memory, "source_grouping_policy", "provenance_group_v1", "system.memory");
    literal(memory, "rules_overflow", "block_execution", "system.memory");
    literal(memory, "revocation_gate", "policy_epoch", "system.memory");
    if (!required(memory, "embedding_model_ref", "system.memory").is_null()) unsupported("system.memory.embedding_model_ref", "embedding models are not implemented in this increment");
    const auto time = string_value(required(memory, "digest_time", "system.memory"), "system.memory.digest_time", false, 5);
    const auto digit = [](char c) { return c >= '0' && c <= '9'; };
    if (time.size() != 5 || !digit(time[0]) || !digit(time[1]) || time[2] != ':' ||
        !digit(time[3]) || !digit(time[4]) || time[0] > '2' ||
        (time[0] == '2' && time[1] > '3') || time[3] > '5')
        invalid("system.memory.digest_time", "expected HH:MM");
    for (const auto key : {"rank_alpha", "warn_confidence_below"}) (void)number_value(required(memory, key, "system.memory"), child("system.memory", key), 0, 1);

    fields(required(system, "paths", "system"), "system.paths", {{"data", 's'}, {"socket", 's'}});
    return registry;
}

std::string default_capability(const Json& function) {
    for (const auto& capability : required(function, "capabilities", "function").as_array()) {
        const auto* is_default = optional(capability, "default");
        if (is_default && is_default->as_bool()) return string_value(required(capability, "id", "capability"), "capability.id");
    }
    invalid("function.capabilities", "one default capability is required");
}

Json resolutions(const Json& system, const Json& function, const CapabilityRegistry& registry) {
    try {
        const auto defaults = selection(required(system, "defaults", "system"), "system.defaults", false);
        const auto root_override = selection(required(function, "root", "function"), "function.root", true);
        const auto root = resolve_model(defaults, root_override, registry);
        const auto root_model_source = root_override.model.empty() ? "system.defaults.model_ref" : "function.root.model_ref";
        const auto root_reasoning_source = root_override.reasoning.mode == ReasoningMode::Inherit ? "system.defaults.reasoning" : "function.root.reasoning";
        Object result;
        for (const auto role : {Role::Root, Role::Dispatcher, Role::TeamLead, Role::Worker}) {
            const auto name = checked_role_name(role);
            const auto override = role == Role::Root ? ModelSelection{} : selection(required(required(function, "roles", "function"), name, "function.roles"), child("function.roles", name), true, true);
            const auto effective = role == Role::Root ? root : resolve_model(root, override, registry);
            result.emplace(name, Object{{"model_ref", effective.model}, {"reasoning", reasoning_json(effective.reasoning)},
                {"model_source", override.model.empty() ? std::string(root_model_source) : "function.roles." + name + ".model_ref"},
                {"reasoning_source", override.reasoning.mode == ReasoningMode::Inherit ? std::string(root_reasoning_source) : "function.roles." + name + ".reasoning"},
                {"inheritance", role == Role::Root ? "system_then_function_root" : "direct_frozen_root"}});
        }
        return result;
    } catch (const ConfigurationError& error) { invalid("function role resolution", error.what()); }
}

void validate_function(const Json& function, const Json& system, const CapabilityRegistry& registry,
                       const ConfigLoadOptions& options) {
    fields(function, "function", {{"schema_version", 'i'}, {"spec_version", 's'}, {"id", 's'}, {"version", 'i'},
        {"description", 's'}, {"root", '-'}, {"roles", '-'}, {"team_templates", '-'}, {"capabilities", '-'}, {"pinned_skills", 'a'}});
    (void)integer_value(required(function, "schema_version", "function"), "function.schema_version", 1, 1);
    literal(function, "spec_version", "0.4.1", "function");
    (void)identifier(required(function, "id", "function"), "function.id");
    (void)selection(required(function, "root", "function"), "function.root", true);
    const auto& roles = required(function, "roles", "function");
    fields(roles, "function.roles", {{"dispatcher", '-'}, {"team_lead", '-'}, {"worker", '-'}});
    for (const auto role : {"dispatcher", "team_lead", "worker"}) {
        const auto& value = required(roles, role, "function.roles");
        (void)selection(value, child("function.roles", role), true, true);
        if (const auto* prompt = optional(value, "prompt_append")) (void)relative_path(*prompt, child(child("function.roles", role), "prompt_append"));
    }
    if (!strings(required(function, "pinned_skills", "function"), "function.pinned_skills").empty()) unsupported("function.pinned_skills", "pinned memory/Skill loading requires the later memory integration");
    const auto& templates = required(function, "team_templates", "function");
    if (!templates.is_object() || templates.as_object().empty() || templates.as_object().size() > 64) invalid("function.team_templates", "expected 1 to 64 templates");
    const std::set<std::string> tools{"fs_list", "fs_read", "fs_search", "fs_write", "fs_patch", "run", "op_status", "op_output",
        "op_cancel", "ask", "progress", "ack_change", "submit", "note", "recall", "load_memory"};
    for (const auto& entry : templates.as_object()) {
        const std::string name(entry.key().data(), entry.key().size());
        if (!safe_identifier(name)) invalid("function.team_templates", "invalid template identifier");
        const auto where = child("function.team_templates", name);
        const auto& item = entry.value();
        fields(item, where, {{"worker_tools", 'a'}, {"network", 's'}, {"workspace", 's'}, {"env", 's'}, {"isolation", 's'}});
        for (const auto& tool : strings(required(item, "worker_tools", where), child(where, "worker_tools"))) if (!tools.contains(tool)) invalid(child(where, "worker_tools"), "unregistered worker tool name");
        const auto network = string_value(required(item, "network", where), child(where, "network"));
        if (network != "none" && network != "host") invalid(child(where, "network"), "expected none or host network");
        literal(item, "workspace", "plain", where);
        (void)relative_path(required(item, "env", where), child(where, "env"));
        const auto isolation = string_value(required(item, "isolation", where), child(where, "isolation"));
        if (isolation != "strong" && isolation != "weak") invalid(child(where, "isolation"), "expected strong or weak isolation");
        if (isolation == "weak" && !required(required(system, "sandbox", "system"), "allow_weak_isolation", "system.sandbox").as_bool()) invalid(child(where, "isolation"), "weak isolation must be explicitly allowed by system policy");
    }
    const auto& capabilities = required(function, "capabilities", "function");
    if (!capabilities.is_array() || capabilities.as_array().empty() || capabilities.as_array().size() > 64) invalid("function.capabilities", "expected 1 to 64 capabilities");
    std::set<std::string> ids;
    std::size_t defaults = 0;
    for (const auto& item : capabilities.as_array()) {
        fields(item, "function.capability", {{"id", 's'}, {"input", 's'}, {"dispatch", 's'}, {"default", 'b'},
            {"input_schema", 's'}, {"output_schema", 's'}, {"acceptance_validator", 's'}, {"required_checks_source", 's'},
            {"fail_on_required_not_run", 'b'}, {"team_template", 's'}, {"brief_template", 's'}},
            {"input", "default", "input_schema", "output_schema", "acceptance_validator", "required_checks_source", "fail_on_required_not_run", "team_template", "brief_template"});
        const auto id = identifier(required(item, "id", "function.capability"), "function.capability.id");
        if (!ids.insert(id).second) invalid("function.capabilities", "duplicate capability id");
        if (const auto* selected = optional(item, "default"); selected && selected->as_bool()) ++defaults;
        const auto dispatch = string_value(required(item, "dispatch", "function.capability"), "function.capability.dispatch");
        if (dispatch == "plan") {
            literal(item, "input", "free_text", "function.capability");
            for (const auto key : {"input_schema", "output_schema", "acceptance_validator", "required_checks_source", "fail_on_required_not_run", "brief_template"}) {
                if (optional(item, key)) invalid(child("function.capability", key), "field belongs only to template dispatch");
            }
        } else if (dispatch == "template") {
            if (optional(item, "input")) invalid("function.capability.input", "template dispatch uses input_schema");
            for (const auto key : {"input_schema", "output_schema", "acceptance_validator", "team_template"}) (void)identifier(required(item, key, "function.capability"), child("function.capability", key));
            (void)string_value(required(item, "required_checks_source", "function.capability"), "function.capability.required_checks_source");
            flag(item, "fail_on_required_not_run", true, "function.capability");
            (void)relative_path(required(item, "brief_template", "function.capability"), "function.capability.brief_template");
            const auto validator = string_value(required(item, "acceptance_validator", "function.capability"), "function.capability.acceptance_validator");
            if (!options.acceptance_validators.contains(validator)) unsupported("function.capability.acceptance_validator", "deterministic validator is not registered by the trusted host");
        } else invalid("function.capability.dispatch", "expected plan or template");
        const auto* template_id = optional(item, "team_template");
        const auto selected_template = template_id ? identifier(*template_id, "function.capability.team_template") : "default";
        if (!templates.as_object().contains(selected_template)) invalid("function.capability.team_template", "unknown team template");
    }
    if (defaults != 1) invalid("function.capabilities", "exactly one capability must be marked default");
    (void)resolutions(system, function, registry);
}

void validate_environment(const Json& environment, std::string_view where) {
    fields(environment, where, {{"schema_version", 'i'}, {"tools", '-'}, {"notes", 's'}}, {"notes"});
    (void)integer_value(required(environment, "schema_version", where), child(where, "schema_version"), 1, 1);
    const auto& tools = required(environment, "tools", where);
    if (!tools.is_array() || tools.as_array().size() > 128) invalid(where, "environment tools must be a bounded sequence");
    std::set<std::string> names;
    for (const auto& tool : tools.as_array()) {
        fields(tool, child(where, "tools"), {{"name", 's'}, {"check", 's'}, {"usage", 's'}, {"example", 's'}, {"optional", 'b'}}, {"example", "optional"});
        const auto name = string_value(required(tool, "name", where), child(where, "name"), false, 128);
        if (!names.insert(name).second) invalid(where, "duplicate environment tool name");
        (void)string_value(required(tool, "check", where), child(where, "check"), false, 4096);
    }
}

using DependencyMap = std::map<std::string, ConfigDependency>;

std::map<std::string, std::string> dependency_kinds(const Json& function) {
    const auto id = string_value(required(function, "id", "function"), "function.id");
    const auto prefix = "functions/" + id + "/";
    std::map<std::string, std::string> result{{"system.yaml", "system_config"}, {prefix + "function.yaml", "function_config"},
        {"prompts/common.md", "common_prompt"}, {"prompts/dispatcher.md", "role_prompt"},
        {"prompts/team_lead.md", "role_prompt"}, {"prompts/worker.md", "role_prompt"}};
    const auto add = [&](std::string path, std::string kind) {
        const auto [found, inserted] = result.emplace(std::move(path), kind);
        if (!inserted && found->second != kind) invalid("function dependencies", "one dependency path has incompatible purposes");
    };
    for (const auto& role : required(function, "roles", "function").as_object()) {
        if (const auto* prompt = optional(role.value(), "prompt_append")) add(prefix + relative_path(*prompt, "function.prompt_append"), "function_prompt");
    }
    for (const auto& item : required(function, "team_templates", "function").as_object()) add(prefix + relative_path(required(item.value(), "env", "team_template"), "team_template.env"), "environment_template");
    for (const auto& item : required(function, "capabilities", "function").as_array()) {
        if (const auto* brief = optional(item, "brief_template")) add(prefix + relative_path(*brief, "capability.brief_template"), "capability_brief");
    }
    return result;
}

Json dependency_json(const DependencyMap& dependencies) {
    Array array;
    for (const auto& [path, value] : dependencies) array.emplace_back(Object{{"path", path}, {"kind", value.kind}, {"sha256", value.sha256}, {"content", value.content}});
    return array;
}

void validate_document(const Json& document, const ConfigLoadOptions& options) {
    fields(document, "snapshot", {{"snapshot_format", 'i'}, {"function_id", 's'}, {"capability_id", 's'},
        {"system", '-'}, {"function", '-'}, {"dependencies", '-'}, {"resolved_root", '-'}, {"role_resolutions", '-'}});
    (void)integer_value(required(document, "snapshot_format", "snapshot"), "snapshot.snapshot_format", 1, 1);
    const auto& system = required(document, "system", "snapshot");
    const auto& function = required(document, "function", "snapshot");
    const auto registry = validate_system(system);
    validate_function(function, system, registry, options);
    const auto id = identifier(required(document, "function_id", "snapshot"), "snapshot.function_id");
    if (id != string_value(required(function, "id", "function"), "function.id")) invalid("snapshot.function_id", "function identity mismatch");
    const auto capability_id = identifier(required(document, "capability_id", "snapshot"), "snapshot.capability_id");
    bool found_capability = false;
    for (const auto& capability : required(function, "capabilities", "function").as_array()) {
        found_capability = found_capability || string_value(required(capability, "id", "capability"), "capability.id") == capability_id;
    }
    if (!found_capability) invalid("snapshot.capability_id", "capability is not part of the frozen function");
    const auto resolved = resolutions(system, function, registry);
    if (canonical_json(required(document, "role_resolutions", "snapshot")) != canonical_json(resolved) ||
        canonical_json(required(document, "resolved_root", "snapshot")) != canonical_json(required(resolved, "root", "resolutions"))) {
        invalid("snapshot.role_resolutions", "derived role configuration does not match frozen inputs");
    }
    const auto expected = dependency_kinds(function);
    const auto& dependencies = required(document, "dependencies", "snapshot");
    if (!dependencies.is_array() || dependencies.as_array().size() != expected.size()) invalid("snapshot.dependencies", "complete dependency closure is required");
    std::set<std::string> seen;
    std::string previous;
    std::size_t total = 0;
    for (const auto& dependency : dependencies.as_array()) {
        fields(dependency, "snapshot.dependency", {{"path", 's'}, {"kind", 's'}, {"sha256", 's'}, {"content", '-'}});
        const auto path = relative_path(required(dependency, "path", "dependency"), "snapshot.dependency.path");
        const auto kind = string_value(required(dependency, "kind", "dependency"), "snapshot.dependency.kind");
        const auto found = expected.find(path);
        if (found == expected.end() || found->second != kind || !seen.insert(path).second || (!previous.empty() && previous >= path)) invalid("snapshot.dependencies", "unexpected, duplicate, or unsorted dependency");
        previous = path;
        const auto content = string_value(required(dependency, "content", "dependency"), path, true, options.max_file_bytes);
        if (content.size() > options.max_total_bytes - total) invalid("snapshot.dependencies", "total dependency byte limit exceeded");
        total += content.size();
        if (sha256_hex(content) != string_value(required(dependency, "sha256", "dependency"), "snapshot.dependency.sha256")) invalid(path, "dependency hash mismatch");
        if (kind == "system_config" || kind == "function_config") {
            const auto parsed = yaml_value(content, path, options);
            if (canonical_json(parsed) != canonical_json(kind == "system_config" ? system : function)) invalid(path, "dependency bytes do not match frozen parsed configuration");
        } else if (kind == "environment_template") validate_environment(yaml_value(content, path, options), path);
        else if (content.empty()) invalid(path, "configured prompt or capability brief cannot be empty");
    }
}

} // namespace

FrozenRequestConfig::FrozenRequestConfig(Json document, const ConfigLoadOptions& options)
    : document_(std::move(document)) {
    validate_options(options);
    validate_document(document_, options);
    serialized_ = canonical_json(document_);
    if (serialized_.size() > options.max_total_bytes) invalid("snapshot", "serialized snapshot byte limit exceeded");
    sha256_ = sha256_hex(serialized_);
    function_id_ = string_value(required(document_, "function_id", "snapshot"), "snapshot.function_id");
    capability_id_ = string_value(required(document_, "capability_id", "snapshot"), "snapshot.capability_id");
    for (const auto role : {Role::Root, Role::Dispatcher, Role::TeamLead, Role::Worker}) {
        const auto& entry = required(required(document_, "role_resolutions", "snapshot"), checked_role_name(role), "snapshot.role_resolutions");
        resolved_.emplace(role, ModelSelection{string_value(required(entry, "model_ref", "role_resolution"), "role_resolution.model_ref"),
            reasoning(&required(entry, "reasoning", "role_resolution"), "role_resolution.reasoning", false)});
    }
    for (const auto& item : required(document_, "dependencies", "snapshot").as_array()) {
        dependencies_.push_back({string_value(required(item, "path", "dependency"), "dependency.path"),
            string_value(required(item, "kind", "dependency"), "dependency.kind"),
            string_value(required(item, "sha256", "dependency"), "dependency.sha256"),
            string_value(required(item, "content", "dependency"), "dependency.content", true, options.max_file_bytes)});
    }
}

FrozenRequestConfig FrozenRequestConfig::restore(std::string_view serialized,
                                                std::string_view expected_sha256,
                                                const ConfigLoadOptions& options) {
    validate_options(options);
    if (serialized.size() > options.max_total_bytes) invalid("snapshot", "serialized snapshot byte limit exceeded");
    if (expected_sha256.size() != 64 || sha256_hex(serialized) != expected_sha256) throw Error(ErrorCode::Corrupt, "request configuration snapshot hash mismatch");
    auto parsed = parse_json_strict(serialized, options.max_total_bytes, options.max_yaml_depth);
    if (canonical_json(parsed) != serialized) throw Error(ErrorCode::Corrupt, "request configuration snapshot is not canonical JSON");
    return FrozenRequestConfig(std::move(parsed), options);
}

ModelSelection FrozenRequestConfig::resolve(Role role) const {
    const auto found = resolved_.find(role);
    if (found == resolved_.end()) invalid("role", "unknown role");
    return found->second;
}

Json FrozenRequestConfig::explain(Role role) const {
    const auto name = checked_role_name(role);
    return Object{{"snapshot_format", 1}, {"snapshot_sha256", sha256_}, {"function_id", function_id_},
        {"capability_id", capability_id_}, {"role", name},
        {"resolution", required(required(document_, "role_resolutions", "snapshot"), name, "snapshot.role_resolutions")},
        {"profile", model_profile(role)}, {"environment_checks_executed", false}, {"configuration_migration", "explicit_only"},
        {"runtime_support", runtime_support()}};
}

Json FrozenRequestConfig::runtime_support() const {
    Array deferred;
    const auto add = [&](std::string_view section, std::string_view key, std::string_view reason) {
        const auto& system = required(document_, "system", "snapshot");
        const auto& value = required(required(system, section, "system"), key, section);
        deferred.emplace_back(Object{{"path", "system." + std::string(section) + "." + std::string(key)},
            {"configured_value", value}, {"status", "validated_only"}, {"reason", reason}});
    };
    for (const auto key : {"max_wall_minutes", "warn_at_fraction"})
        add("budget_defaults", key, "requires the paid-call deadline and warning runtime");
    for (const auto key : {"compaction_at_fraction", "schema_retry_max", "snapshot_decision_log_entries"})
        add("context", key, "requires bounded structured history/compaction; no automatic retry");
    for (const auto key : {"max_active_dispatchers", "max_active_workers", "reserve_dispatchers_for_review",
                          "reserve_workers_for_review", "review_reservation_policy", "release_slots_while_waiting",
                          "max_concurrent_model_calls_per_endpoint", "max_concurrent_ops"})
        add("limits", key, "the current S1 pump is serial, not the global concurrent scheduler");
    for (const auto key : {"stall_minutes", "stall_same_error_repeats"})
        add("limits", key, "requires persistent stall detection and escalation");
    add("cleanup", "vacuum_weekly", "no periodic private-data purge or maintenance scheduler");
    return Object{{"validation_scope", "configuration_shape_references_and_policies"},
        {"execution_mode", "offline_fake"}, {"real_calls_ready", false},
        {"all_configured_controls_enforced", false}, {"unenforced_settings", deferred}};
}

Json FrozenRequestConfig::model_profile(Role role) const {
    const auto selected = resolve(role);
    return required(required(required(document_, "system", "snapshot"), "models", "system"), selected.model, "system.models");
}

ToolSet FrozenRequestConfig::worker_tools(std::string_view template_id) const {
    const auto& templates = required(required(document_, "function", "snapshot"), "team_templates", "function");
    const auto& selected = required(templates, template_id, "function.team_templates");
    return strings(required(selected, "worker_tools", "team_template"), "team_template.worker_tools");
}

std::vector<std::string> ConfigCatalog::functions() const {
    std::vector<std::string> result;
    result.reserve(functions_.size());
    for (const auto& [name, document] : functions_) { (void)document; result.push_back(name); }
    return result;
}

FrozenRequestConfig ConfigCatalog::freeze(std::string_view function_id,
                                         std::string_view capability_id) const {
    const auto found = functions_.find(std::string(function_id));
    if (found == functions_.end()) invalid("function_id", "function is not loaded");
    Json document = found->second;
    document.as_object()["capability_id"] = capability_id.empty()
        ? default_capability(required(document, "function", "snapshot")) : std::string(capability_id);
    return FrozenRequestConfig(std::move(document), options_);
}

ConfigCatalog load_config_catalog(const std::filesystem::path& system_file,
                                  const std::filesystem::path& functions_directory,
                                  const std::filesystem::path& prompts_directory,
                                  const ConfigLoadOptions& options) {
    validate_options(options);
    ConfigCatalog result;
    result.options_ = options;
    std::size_t total = 0;
    const auto system_bytes = read_file(system_file, "system.yaml", options, total);
    const auto system = yaml_value(system_bytes, "system.yaml", options);
    const auto registry = validate_system(system);
    DependencyMap common;
    common.emplace("system.yaml", ConfigDependency{"system.yaml", "system_config", sha256_hex(system_bytes), system_bytes});
    for (const auto name : {"common", "dispatcher", "team_lead", "worker"}) {
        const auto filename = std::string(name) + ".md";
        const auto path = "prompts/" + filename;
        const auto content = read_file(prompts_directory / filename, path, options, total);
        if (content.empty()) invalid(path, "role prompt cannot be empty");
        common.emplace(path, ConfigDependency{path, std::string_view(name) == "common" ? "common_prompt" : "role_prompt", sha256_hex(content), content});
    }
    const auto directory = open_directory(functions_directory, "functions");
    (void)directory;
    std::vector<std::filesystem::path> function_directories;
    try {
        for (const auto& entry : std::filesystem::directory_iterator(functions_directory)) {
            if (!entry.is_directory() || entry.is_symlink()) invalid("functions", "every direct entry must be a real function directory");
            if (!safe_identifier(entry.path().filename().string())) invalid("functions", "function directory has an invalid identifier");
            function_directories.push_back(entry.path());
            if (function_directories.size() > 64) invalid("functions", "function count limit exceeded");
        }
    } catch (const std::filesystem::filesystem_error&) { invalid("functions", "function directory enumeration failed"); }
    if (function_directories.empty()) invalid("functions", "at least one function is required");
    std::sort(function_directories.begin(), function_directories.end());
    for (const auto& function_directory : function_directories) {
        const auto id = function_directory.filename().string();
        const auto prefix = "functions/" + id + "/";
        const auto function_bytes = read_file(function_directory / "function.yaml", prefix + "function.yaml", options, total);
        const auto function = yaml_value(function_bytes, prefix + "function.yaml", options);
        validate_function(function, system, registry, options);
        if (string_value(required(function, "id", "function"), "function.id") != id) invalid(prefix + "function.yaml", "function id must match its directory");
        auto dependencies = common;
        dependencies.emplace(prefix + "function.yaml", ConfigDependency{prefix + "function.yaml", "function_config", sha256_hex(function_bytes), function_bytes});
        for (const auto& [path, kind] : dependency_kinds(function)) {
            if (dependencies.contains(path)) continue;
            if (!path.starts_with(prefix)) invalid(path, "dependency does not belong to this function");
            const auto content = read_file(function_directory / path.substr(prefix.size()), path, options, total);
            if (kind == "environment_template") validate_environment(yaml_value(content, path, options), path);
            else if (content.empty()) invalid(path, "configured prompt or capability brief cannot be empty");
            dependencies.emplace(path, ConfigDependency{path, kind, sha256_hex(content), content});
        }
        const auto resolved = resolutions(system, function, registry);
        Json document = Object{{"snapshot_format", 1}, {"function_id", id}, {"capability_id", default_capability(function)},
            {"system", system}, {"function", function}, {"dependencies", dependency_json(dependencies)},
            {"resolved_root", required(resolved, "root", "role_resolutions")}, {"role_resolutions", resolved}};
        validate_document(document, options);
        if (!result.functions_.emplace(id, std::move(document)).second) invalid("functions", "duplicate function id");
    }
    return result;
}

} // namespace wls::s1
