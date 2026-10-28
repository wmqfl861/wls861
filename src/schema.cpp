#include "wls/schema.hpp"
#include "wls/offline_runtime.hpp"

// Valijson's default error helper aborts when this option is absent. Schema
// failures are normal rejected input, and must never terminate the runtime.
#ifndef VALIJSON_USE_EXCEPTIONS
#define VALIJSON_USE_EXCEPTIONS 1
#endif
#if !VALIJSON_USE_EXCEPTIONS
#error "WLS requires Valijson exceptions so invalid schemas cannot abort the host"
#endif

#include <valijson/adapters/boost_json_adapter.hpp>
#include <valijson/schema.hpp>
#include <valijson/schema_parser.hpp>
#include <valijson/validator.hpp>

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <utility>

namespace wls::s1 {
namespace {

using Documents = std::map<std::string, Json>;
constexpr std::size_t max_schema_bytes = 1024U * 1024U;
constexpr std::size_t max_registry_bytes = 8U * 1024U * 1024U;
constexpr std::size_t max_schema_count = 128;

void require(bool valid, const std::string& message) {
    if (!valid) throw Error(ErrorCode::InvalidInput, message);
}

bool schema_name_valid(const std::string& name) {
    return !name.empty() && name.size() <= 128 &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
        });
}

std::map<std::string, std::string> read_schema_directory(const std::filesystem::path& directory) {
    require(std::filesystem::is_directory(directory), "schema directory is missing");
    std::vector<std::filesystem::path> paths;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.path().filename().string().ends_with(".schema.json")) {
            require(paths.size() < max_schema_count, "invalid schema registry size");
            paths.push_back(entry.path());
        }
    }
    require(!paths.empty() && paths.size() <= max_schema_count, "invalid schema registry size");
    std::sort(paths.begin(), paths.end());
    std::map<std::string, std::string> result;
    std::size_t total_bytes = 0;
    for (const auto& path : paths) {
        // The compatibility directory entry point obeys the same pinned
        // regular-file, original-component and bounded EOF checks as other
        // runtime inputs. Enforce the remaining registry budget while reading.
        auto bytes = read_runtime_file(path, std::min(max_schema_bytes, max_registry_bytes - total_bytes));
        total_bytes += bytes.size();
        result.emplace(path.filename().string(), std::move(bytes));
    }
    return result;
}

std::string string_property(const boost::json::object& source, std::string_view name) {
    const auto* value = source.if_contains(name);
    require(value != nullptr && value->is_string(), "schema needs string " + std::string(name));
    return std::string(value->as_string());
}

std::string normalize_ref(const std::string& ref, const std::string& base,
                          const Documents& documents) {
    const auto separator = ref.find('#');
    const auto doc_part = ref.substr(0, separator);
    const auto fragment = separator == std::string::npos ? std::string{} : ref.substr(separator + 1);
    require(fragment.empty() || fragment.front() == '/', "schema refs must use JSON Pointer fragments");
    std::string uri;
    if (doc_part.empty()) {
        uri = base;
    } else if (doc_part.find("://") != std::string::npos) {
        uri = doc_part;
    } else {
        // The program registry is flat. Refuse path traversal, file schemes,
        // queries, hosts and percent-encoded filenames instead of resolving
        // them through a general-purpose network/filesystem URI mechanism.
        require(doc_part.find_first_of("/\\:?%") == std::string::npos &&
                doc_part.ends_with(".schema.json"), "unsupported nonlocal schema reference");
        const auto slash = base.rfind('/');
        require(slash != std::string::npos, "schema reference has no local base");
        uri = base.substr(0, slash + 1) + doc_part;
    }
    require(documents.contains(uri), "unregistered schema reference refused offline: " + uri);
    return uri + (separator == std::string::npos ? std::string{} : "#" + fragment);
}

// Only these Draft-07 keywords contain schemas. Objects in const/default/enum
// are instance data, and a properties map is not itself a schema. The bool
// identifies children evaluating the same instance rather than a nested value.
template<class Value, class Visitor>
void for_each_subschema(Value& value, const Visitor& visitor) {
    if (!value.is_object()) return;
    for (auto& entry : value.as_object()) {
        const std::string_view key = entry.key();
        auto& child = entry.value();
        const auto visit = [&](auto& item, bool same_instance) {
            require(item.is_object() || item.is_bool(), "schema child must be an object or boolean");
            visitor(item, same_instance);
        };
        if (key == "allOf" || key == "anyOf" || key == "oneOf") {
            require(child.is_array(), "schema combinator must be an array");
            for (auto& item : child.as_array()) visit(item, true);
        } else if (key == "if" || key == "then" || key == "else" || key == "not") {
            visit(child, true);
        } else if (key == "properties" || key == "patternProperties" || key == "definitions") {
            require(child.is_object(), "schema declarations must be an object");
            for (auto& item : child.as_object()) visit(item.value(), false);
        } else if (key == "items" && child.is_array()) {
            for (auto& item : child.as_array()) visit(item, false);
        } else if (key == "items" || key == "additionalProperties" || key == "additionalItems" ||
                   key == "contains" || key == "propertyNames") {
            visit(child, false);
        } else if (key == "dependencies") {
            require(child.is_object(), "schema dependencies must be an object");
            for (auto& item : child.as_object()) {
                if (!item.value().is_array()) visit(item.value(), true);
            }
        }
    }
}

void normalize_refs(Json& value, const std::string& base,
                    const Documents& documents, bool root = false) {
    if (!value.is_object()) return;
    auto& source = value.as_object();
    require(root || (!source.contains("$id") && !source.contains("id")),
            "nested schema IDs are unsupported by this flat offline registry");
    if (const auto* ref = source.if_contains("$ref")) {
        require(ref->is_string(), "schema $ref must be a string");
        const auto normalized = normalize_ref(std::string(ref->as_string()), base, documents);
        source["$ref"] = normalized;
    }
    for_each_subschema(value, [&](Json& child, bool) { normalize_refs(child, base, documents); });
}

void collect_runtime_names(const Json& value, std::set<std::string>& names) {
    if (value.is_object()) {
        const auto& source = value.as_object();
        if (const auto* properties = source.if_contains("properties")) {
            require(properties->is_object(), "schema properties must be an object");
            for (const auto& entry : properties->as_object()) {
                if (!entry.value().is_object()) continue;
                if (const auto* host = entry.value().as_object().if_contains("x-runtime-field")) {
                    require(host->is_bool(), "x-runtime-field must be boolean");
                    if (host->as_bool()) names.insert(std::string(entry.key()));
                }
            }
        }
        for_each_subschema(value, [&](const Json& child, bool same_instance) {
            if (same_instance) collect_runtime_names(child, names);
        });
    }
}

bool condition_uses_runtime(const Json& condition, const std::set<std::string>& runtime_names) {
    if (!condition.is_object()) return false;
    const auto& source = condition.as_object();
    if (const auto* properties = source.if_contains("properties")) {
        require(properties->is_object(), "schema properties must be an object");
        for (const auto& property : properties->as_object()) {
            if (runtime_names.contains(std::string(property.key()))) return true;
        }
    }
    if (const auto* required = source.if_contains("required")) {
        require(required->is_array(), "schema required must be an array");
        for (const auto& name : required->as_array()) {
            require(name.is_string(), "schema required entries must be strings");
            if (runtime_names.contains(std::string(name.as_string()))) return true;
        }
    }
    bool found = false;
    for_each_subschema(condition, [&](const Json& child, bool same_instance) {
        if (same_instance && condition_uses_runtime(child, runtime_names)) found = true;
    });
    return found;
}

Json project_schema(const Json& value, const std::set<std::string>& inherited_names = {}) {
    if (!value.is_object()) return value;
    const auto& source = value.as_object();
    auto runtime_names = inherited_names;
    // A normal nested business field may share a host field's spelling. Only
    // annotations applying to this exact instance can remove its requirements.
    collect_runtime_names(value, runtime_names);
    std::set<std::string> removed;
    if (const auto* properties = source.if_contains("properties")) {
        require(properties->is_object(), "schema properties must be an object");
        for (const auto& property : properties->as_object()) {
            if (runtime_names.contains(std::string(property.key()))) removed.insert(std::string(property.key()));
        }
    }
    const auto* condition = source.if_contains("if");
    const bool strip_condition = condition != nullptr && condition_uses_runtime(*condition, runtime_names);
    boost::json::object result;
    for (const auto& entry : source) {
        const std::string key(entry.key());
        if (strip_condition && (key == "if" || key == "then" || key == "else")) continue;
        if (key == "not" && condition_uses_runtime(entry.value(), runtime_names)) continue;
        if (key == "properties") {
            boost::json::object properties;
            for (const auto& property : entry.value().as_object()) {
                if (!removed.contains(std::string(property.key()))) {
                    properties.emplace(property.key(), property.value());
                }
            }
            result.emplace(key, std::move(properties));
        } else if (key == "required") {
            require(entry.value().is_array(), "schema required must be an array");
            boost::json::array required;
            for (const auto& name : entry.value().as_array()) {
                require(name.is_string(), "schema required entries must be strings");
                if (!runtime_names.contains(std::string(name.as_string()))) required.push_back(name);
            }
            if (!required.empty()) result.emplace(key, std::move(required));
        } else if (key == "dependencies") {
            require(entry.value().is_object(), "schema dependencies must be an object");
            boost::json::object dependencies;
            for (const auto& dependency : entry.value().as_object()) {
                if (runtime_names.contains(std::string(dependency.key()))) continue;
                if (dependency.value().is_array()) {
                    boost::json::array names;
                    for (const auto& name : dependency.value().as_array()) {
                        require(name.is_string(), "property dependencies must be strings");
                        if (!runtime_names.contains(std::string(name.as_string()))) names.push_back(name);
                    }
                    if (!names.empty()) dependencies.emplace(dependency.key(), std::move(names));
                } else {
                    dependencies.emplace(dependency.key(), dependency.value());
                }
            }
            result.emplace(key, std::move(dependencies));
        } else {
            result.emplace(key, entry.value());
        }
    }
    Json projected(std::move(result));
    for_each_subschema(projected, [&](Json& child, bool same_instance) {
        child = project_schema(child, same_instance ? runtime_names : std::set<std::string>{});
    });
    auto& projected_object = projected.as_object();
    if (!removed.empty()) {
        // additionalProperties can be true in a referenced contract. Removing
        // a host property must still forbid model input from supplying it.
        boost::json::array forbidden;
        for (const auto& name : removed) forbidden.emplace_back(name);
        const Json restriction = boost::json::object{{"propertyNames", boost::json::object{
            {"not", boost::json::object{{"enum", std::move(forbidden)}}}}}};
        if (auto* all_of = projected_object.if_contains("allOf")) {
            require(all_of->is_array(), "schema allOf must be an array");
            all_of->as_array().push_back(restriction);
        } else {
            projected_object.emplace("allOf", boost::json::array{restriction});
        }
    }
    return projected;
}

} // namespace

class SchemaRegistry::Impl {
public:
    explicit Impl(const std::filesystem::path& directory) : Impl(read_schema_directory(directory)) {}

    explicit Impl(const std::map<std::string, std::string>& files) {
        require(!files.empty() && files.size() <= max_schema_count, "invalid schema registry size");
        std::size_t total_bytes = 0;
        for (const auto& [filename, bytes] : files) {
            require(filename.ends_with(".schema.json"), "registered schema filename has an invalid suffix");
            const auto name = filename.substr(0, filename.size() - std::string(".schema.json").size());
            require(schema_name_valid(name), "invalid schema filename");
            require(bytes.size() <= max_schema_bytes, "schema file exceeds byte limit");
            require(bytes.size() <= max_registry_bytes - total_bytes, "schema registry exceeds byte limit");
            total_bytes += bytes.size();
            auto schema = parse_json_strict(bytes, max_schema_bytes, 64);
            require(schema.is_object(), "registered schema must have an object root");
            const auto& source = schema.as_object();
            require(string_property(source, "$schema") == "http://json-schema.org/draft-07/schema#",
                    "registry accepts only declared draft-07 schemas");
            const auto id = string_property(source, "$id");
            require(id.starts_with("https://") && id.find_first_of("?#") == std::string::npos,
                    "schema $id must be a stable absolute HTTPS document identity");
            require(!source.contains("id"), "draft-04 id cannot replace draft-07 $id");
            require(documents_.emplace(id, std::move(schema)).second, "duplicate schema $id");
            require(names_.emplace(name, id).second, "duplicate schema name");
        }
        // Normalize and allowlist every reference before compilation, including
        // branches not visited by a particular input. No resolver can fetch.
        for (auto& [id, schema] : documents_) normalize_refs(schema, id, documents_, true);
        for (const auto& [id, schema] : documents_) {
            projected_.emplace(id, project_schema(schema));
        }
        compile_all(documents_, compiled_);
        compile_all(projected_, model_compiled_);
    }

    void validate(std::string_view name, const Json& document, bool model_input) const {
        const auto key = std::string(name);
        const auto& registry = model_input ? model_compiled_ : compiled_;
        const auto found = registry.find(key);
        require(found != registry.end(), "unregistered schema name: " + key);
        // Keep strong C++ integer/string typing, finite values, UTF-8 and depth
        // limits consistent with all other runtime JSON ingress.
        const auto bounded = parse_json_strict(canonical_json(document), max_schema_bytes, 64);
        valijson::Validator validator(valijson::Validator::kStrongTypes);
        const valijson::adapters::BoostJsonAdapter target(bounded);
        if (!validator.validate(*found->second, target, nullptr)) {
            throw Error(ErrorCode::InvalidInput, "draft-07 validation failed for " + key +
                        (model_input ? " model input" : " complete record"));
        }
        if (key == "plan") {
            // Capability inputs intentionally have dynamic schemas. Team-task
            // briefs have a known contract and cannot hide inside opaque JSON.
            for (const auto& node : bounded.as_object().at("nodes").as_array()) {
                const auto& object = node.as_object();
                if (object.at("kind").as_string() == "team_task") {
                    validate("task_brief", object.at("brief"), model_input);
                }
            }
        }
    }

    Json schema(std::string_view name, bool model_input) const {
        const auto found = names_.find(std::string(name));
        require(found != names_.end(), "unregistered schema name: " + std::string(name));
        return (model_input ? projected_ : documents_).at(found->second);
    }

    std::vector<std::string> names() const {
        std::vector<std::string> result;
        for (const auto& [name, unused] : names_) {
            (void)unused;
            result.push_back(name);
        }
        return result;
    }

private:
    using Schemas = std::map<std::string, std::unique_ptr<valijson::Schema>>;
    Documents documents_;
    Documents projected_;
    std::map<std::string, std::string> names_;
    Schemas compiled_;
    Schemas model_compiled_;

    void compile_all(const Documents& documents, Schemas& result) {
        const auto fetch = [&documents](const std::string& uri) -> const Json* {
            const auto found = documents.find(uri);
            require(found != documents.end(), "unregistered schema reference refused offline: " + uri);
            return &found->second;
        };
        const auto release = [](const Json*) {};
        for (const auto& [name, id] : names_) {
            auto schema = std::make_unique<valijson::Schema>();
            const valijson::adapters::BoostJsonAdapter adapter(documents.at(id));
            valijson::SchemaParser parser(valijson::SchemaParser::kDraft7);
            try {
                parser.populateSchema(adapter, *schema, fetch, release);
            } catch (const Error&) {
                throw;
            } catch (const std::exception& error) {
                throw Error(ErrorCode::InvalidInput, "cannot compile draft-07 schema " + name + ": " + error.what());
            }
            result.emplace(name, std::move(schema));
        }
    }
};

SchemaRegistry::SchemaRegistry(const std::filesystem::path& directory)
    : impl_(std::make_unique<Impl>(directory)) {}
SchemaRegistry::SchemaRegistry(const std::map<std::string, std::string>& documents)
    : impl_(std::make_unique<Impl>(documents)) {}
SchemaRegistry::~SchemaRegistry() = default;
std::vector<std::string> SchemaRegistry::schema_names() const { return impl_->names(); }
void SchemaRegistry::validate(std::string_view name, const Json& document, bool model_input) const {
    impl_->validate(name, document, model_input);
}
Json SchemaRegistry::project_model_input(std::string_view name) const { return impl_->schema(name, true); }
Json SchemaRegistry::full_schema(std::string_view name) const { return impl_->schema(name, false); }

} // namespace wls::s1
