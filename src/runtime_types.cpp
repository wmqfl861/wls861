#include "wls/runtime_types.hpp"

#include <boost/json/basic_parser_impl.hpp>
#include <openssl/evp.h>
#include <sys/random.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <limits>
#include <memory>
#include <mutex>
#include <set>
#include <vector>

namespace wls::s1 {
namespace {
using ParseError = boost::system::error_code;
using View = boost::json::string_view;

// Use the library's UTF-8-aware SAX decoder to catch duplicate *decoded* keys
// before DOM construction (whose normal policy is last-key-wins).
struct UniqueKeys {
    static constexpr std::size_t max_array_size = 1048576;
    static constexpr std::size_t max_object_size = 1048576;
    static constexpr std::size_t max_string_size = 16777216;
    static constexpr std::size_t max_key_size = 1048576;
    std::vector<std::set<std::string>> objects;
    std::string key;
    bool duplicate{false};
    bool nonfinite{false};
    bool on_document_begin(ParseError&) { return true; }
    bool on_document_end(ParseError&) { return true; }
    bool on_array_begin(ParseError&) { return true; }
    bool on_array_end(std::size_t, ParseError&) { return true; }
    bool on_object_begin(ParseError&) { objects.emplace_back(); return true; }
    bool on_object_end(std::size_t, ParseError&) { objects.pop_back(); return true; }
    bool on_string_part(View, std::size_t, ParseError&) { return true; }
    bool on_string(View, std::size_t, ParseError&) { return true; }
    bool on_key_part(View part, std::size_t, ParseError&) {
        key.append(part.data(), part.size()); return true;
    }
    bool on_key(View part, std::size_t, ParseError& ec) {
        key.append(part.data(), part.size());
        if (!objects.back().insert(key).second) {
            duplicate = true; ec = boost::json::error::syntax; return false;
        }
        key.clear(); return true;
    }
    bool on_number_part(View, ParseError&) { return true; }
    bool on_int64(std::int64_t, View, ParseError&) { return true; }
    bool on_uint64(std::uint64_t, View, ParseError&) { return true; }
    bool on_double(double value, View, ParseError& ec) {
        if (!std::isfinite(value)) {
            nonfinite = true; ec = boost::json::error::syntax; return false;
        }
        return true;
    }
    bool on_bool(bool, ParseError&) { return true; }
    bool on_null(ParseError&) { return true; }
    bool on_comment_part(View, ParseError&) { return true; }
    bool on_comment(View, ParseError&) { return true; }
};

void append_canonical(const Json& value, std::string& out, std::size_t depth) {
    if (depth > 128) throw Error(ErrorCode::InvalidInput, "JSON exceeds canonical depth limit");
    if (value.is_object()) {
        std::vector<const boost::json::key_value_pair*> entries;
        for (const auto& member : value.as_object()) entries.push_back(&member);
        std::sort(entries.begin(), entries.end(), [](const auto* left, const auto* right) {
            return left->key() < right->key();
        });
        out.push_back('{');
        bool first = true;
        for (const auto* member : entries) {
            if (!first) out.push_back(',');
            first = false;
            out += boost::json::serialize(member->key());
            out.push_back(':');
            append_canonical(member->value(), out, depth + 1);
        }
        out.push_back('}');
    } else if (value.is_array()) {
        out.push_back('[');
        bool first = true;
        for (const auto& element : value.as_array()) {
            if (!first) out.push_back(',');
            first = false;
            append_canonical(element, out, depth + 1);
        }
        out.push_back(']');
    } else {
        if (value.is_double() && !std::isfinite(value.as_double()))
            throw Error(ErrorCode::InvalidInput, "non-finite JSON number");
        out += boost::json::serialize(value);
    }
}

bool valid_prefix(std::string_view prefix) noexcept {
    if (prefix.empty() || prefix.size() > 32 || prefix.front() < 'a' || prefix.front() > 'z') return false;
    for (char value : prefix)
        if (!((value >= 'a' && value <= 'z') || (value >= '0' && value <= '9'))) return false;
    return true;
}
constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";

std::int64_t integer(const boost::json::object& object, View name, std::int64_t minimum) {
    const auto* value = object.if_contains(name);
    if (!value || !(value->is_int64() || value->is_uint64()))
        throw Error(ErrorCode::InvalidInput, "binding integer required: " + std::string(name));
    if (value->is_uint64() && value->as_uint64() > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
        throw Error(ErrorCode::InvalidInput, "binding integer overflow");
    const auto result = value->is_int64() ? value->as_int64() : static_cast<std::int64_t>(value->as_uint64());
    if (result < minimum) throw Error(ErrorCode::InvalidInput, "binding integer below minimum: " + std::string(name));
    return result;
}
std::string string_field(const boost::json::object& object, View name) {
    const auto* value = object.if_contains(name);
    if (!value || !value->is_string()) throw Error(ErrorCode::InvalidInput, "binding string required: " + std::string(name));
    return std::string(value->as_string());
}
} // namespace

std::string_view error_name(ErrorCode code) noexcept {
    switch (code) {
    case ErrorCode::InvalidInput: return "invalid_input";
    case ErrorCode::Unauthorized: return "unauthorized";
    case ErrorCode::Conflict: return "conflict";
    case ErrorCode::Stale: return "stale";
    case ErrorCode::NotReady: return "not_ready";
    case ErrorCode::UnknownOutcome: return "unknown_outcome";
    case ErrorCode::Quota: return "quota";
    case ErrorCode::Storage: return "storage";
    case ErrorCode::Unsupported: return "unsupported";
    case ErrorCode::Corrupt: return "corrupt";
    }
    return "invalid_error_code";
}

std::string_view role_name(Role role) noexcept {
    switch (role) {
    case Role::Root: return "root";
    case Role::Dispatcher: return "dispatcher";
    case Role::TeamLead: return "team_lead";
    case Role::Worker: return "worker";
    }
    return "invalid_role";
}
Role role_from_string(std::string_view name) {
    if (name == "root") return Role::Root;
    if (name == "dispatcher") return Role::Dispatcher;
    if (name == "team_lead") return Role::TeamLead;
    if (name == "worker") return Role::Worker;
    throw Error(ErrorCode::InvalidInput, "unknown role: " + std::string(name));
}

Json parse_json_strict(std::string_view text, std::size_t max_bytes, std::size_t max_depth) {
    if (max_depth == 0 || max_depth > 256 || text.size() > max_bytes)
        throw Error(ErrorCode::InvalidInput, "JSON input/depth limit exceeded");
    boost::json::parse_options options;
    options.max_depth = max_depth;
    options.numbers = boost::json::number_precision::precise;
    boost::json::basic_parser<UniqueKeys> check(options);
    ParseError ec;
    const auto consumed = check.write_some(false, text.data(), text.size(), ec);
    if (ec || consumed != text.size()) {
        const std::string message = check.handler().duplicate ? "duplicate JSON object key" :
            check.handler().nonfinite ? "non-finite JSON number" :
            ec ? "invalid JSON: " + ec.message() : "trailing content after JSON";
        throw Error(ErrorCode::InvalidInput, message);
    }
    auto result = boost::json::parse(text, ec, {}, options);
    if (ec) throw Error(ErrorCode::InvalidInput, "invalid JSON: " + ec.message());
    return result;
}

std::string canonical_json(const Json& value) {
    std::string result;
    append_canonical(value, result, 0);
    // Program-created strings also need UTF-8 validation before hashing/storing.
    boost::json::parse_options options;
    options.max_depth = 128;
    ParseError ec;
    static_cast<void>(boost::json::parse(result, ec, {}, options));
    if (ec) throw Error(ErrorCode::InvalidInput, "canonical JSON contains invalid UTF-8 or structure");
    return result;
}

std::string sha256_hex(std::string_view bytes) {
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    std::array<unsigned char, EVP_MAX_MD_SIZE> digest{};
    unsigned int size{};
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), bytes.data(), bytes.size()) != 1 ||
        EVP_DigestFinal_ex(context.get(), digest.data(), &size) != 1 || size != 32)
        throw Error(ErrorCode::Storage, "SHA-256 computation failed");
    constexpr char hex[] = "0123456789abcdef";
    std::string result;
    result.reserve(64);
    for (unsigned int i = 0; i < size; ++i) {
        result.push_back(hex[digest[i] >> 4]);
        result.push_back(hex[digest[i] & 15]);
    }
    return result;
}

std::string new_id(std::string_view prefix) {
    if (!valid_prefix(prefix)) throw Error(ErrorCode::InvalidInput, "invalid typed ID prefix");
    static std::mutex mutex;
    static std::uint64_t last_millis{};
    static std::array<unsigned char, 10> entropy{};
    std::array<unsigned char, 16> bytes{};
    {
        std::lock_guard lock(mutex);
        const auto count = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        if (count < 0 || static_cast<std::uint64_t>(count) >= (std::uint64_t{1} << 48))
            throw Error(ErrorCode::Storage, "clock outside ULID timestamp range");
        const auto now = static_cast<std::uint64_t>(count);
        if (now > last_millis) {
            std::size_t filled{};
            while (filled < entropy.size()) {
                const auto read = ::getrandom(entropy.data() + filled, entropy.size() - filled, 0);
                if (read < 0 && errno == EINTR) continue;
                if (read <= 0) throw Error(ErrorCode::Storage, "getrandom failed for typed ID");
                filled += static_cast<std::size_t>(read);
            }
            last_millis = now;
        } else {
            bool carry = true;
            for (std::size_t i = entropy.size(); i > 0 && carry; --i) {
                carry = entropy[i - 1] == 255;
                entropy[i - 1] = static_cast<unsigned char>(entropy[i - 1] + 1U);
            }
            if (carry) throw Error(ErrorCode::Quota, "ULID entropy range exhausted within timestamp");
        }
        for (std::size_t i = 0; i < 6; ++i)
            bytes[i] = static_cast<unsigned char>((last_millis >> ((5U - i) * 8U)) & 255U);
        std::copy(entropy.begin(), entropy.end(), bytes.begin() + 6);
    }
    std::string result(prefix);
    result.push_back('_');
    for (int i = 0; i < 26; ++i) {
        unsigned int value{};
        for (int j = 0; j < 5; ++j) {
            const int bit = i * 5 + j - 2;
            value <<= 1;
            if (bit >= 0) value |= (bytes[static_cast<std::size_t>(bit / 8)] >> (7 - bit % 8)) & 1U;
        }
        result.push_back(alphabet[value]);
    }
    return result;
}

bool is_typed_id(std::string_view id, std::string_view prefix) noexcept {
    if (!valid_prefix(prefix) || id.size() != prefix.size() + 27 ||
        id.substr(0, prefix.size()) != prefix || id[prefix.size()] != '_') return false;
    const auto ulid = id.substr(prefix.size() + 1);
    if (ulid.front() > '7') return false;
    return std::all_of(ulid.begin(), ulid.end(), [](char c) { return alphabet.find(c) != std::string_view::npos; });
}

Json to_json(const ExecutionBinding& binding) {
    return boost::json::object{
        {"request_id", binding.request_id}, {"task_id", binding.task_id}, {"team_id", binding.team_id},
        {"requirements_revision", binding.requirements_revision}, {"plan_revision", binding.plan_revision},
        {"task_revision", binding.task_revision}, {"instruction_revision", binding.instruction_revision},
        {"dispatcher_epoch", binding.dispatcher_epoch}, {"execution_epoch", binding.execution_epoch},
        {"policy_epoch", binding.policy_epoch}};
}

ExecutionBinding execution_binding_from_json(const Json& value) {
    if (!value.is_object() || value.as_object().size() != 10)
        throw Error(ErrorCode::InvalidInput, "binding must contain exactly its ten fields");
    const auto& object = value.as_object();
    ExecutionBinding result;
    result.request_id = string_field(object, "request_id");
    result.task_id = string_field(object, "task_id");
    result.team_id = string_field(object, "team_id");
    result.requirements_revision = integer(object, "requirements_revision", 1);
    result.plan_revision = integer(object, "plan_revision", 0);
    result.task_revision = integer(object, "task_revision", 0);
    result.instruction_revision = integer(object, "instruction_revision", 0);
    result.dispatcher_epoch = integer(object, "dispatcher_epoch", 1);
    result.execution_epoch = integer(object, "execution_epoch", 0);
    result.policy_epoch = integer(object, "policy_epoch", 1);
    if (result.request_id.empty() || result.task_id.empty() != result.team_id.empty())
        throw Error(ErrorCode::InvalidInput, "binding identity incomplete");
    if (!is_typed_id(result.request_id, "req") ||
        (!result.task_id.empty() && (!is_typed_id(result.task_id, "tsk") || !is_typed_id(result.team_id, "team"))))
        throw Error(ErrorCode::InvalidInput, "binding identity must use its valid typed ULID prefix");
    if (!result.task_id.empty() && (result.task_revision < 1 || result.execution_epoch < 1 || result.plan_revision < 1))
        throw Error(ErrorCode::InvalidInput, "task binding revisions incomplete");
    if (result.task_id.empty() && (result.task_revision != 0 || result.instruction_revision != 0 || result.execution_epoch != 0))
        throw Error(ErrorCode::InvalidInput, "request binding contains task versions");
    return result;
}

} // namespace wls::s1
