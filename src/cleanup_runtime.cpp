#include "wls/cleanup_runtime.hpp"
#include "wls/cleanup.hpp"
#include "wls/delivery.hpp"
#include "wls/workspace.hpp"
#include "wls/owned_capacity.hpp"
#include <optional>

namespace wls::s1 {
namespace {
std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
void need(bool value, ErrorCode code, const char* reason) { if (!value) throw Error(code, reason); }
std::string replay_parent_path(const std::filesystem::path& supplied) {
    const auto native = supplied.string();
    need(!native.empty() && native.find('\0') == std::string::npos, ErrorCode::InvalidInput,
         "allocation replay requires the original parent path");
    // Match Workspace's path policy without opening a retained parent which
    // may have disappeared or been replaced after explicit abandonment.
    for (const auto& component : supplied)
        need(component != "..", ErrorCode::Unauthorized, "allocation parent must not contain '..'");
    std::error_code error;
    const auto absolute = std::filesystem::absolute(supplied, error);
    need(!error, ErrorCode::InvalidInput, "cannot resolve allocation replay parent path");
    auto normalized = absolute.lexically_normal().string();
    while (normalized.size() > 1 && normalized.back() == '/') normalized.pop_back();
    return normalized;
}
}
Json allocate_owned_workspace(Store& store, const std::string& request_id, const std::string& task_id,
        const std::filesystem::path& parent_path, const std::string& key) {
    if (store.allocation_recovery_enabled()) {
        const auto abandoned = store.abandoned_owned_workspace(request_id, key);
        if (!abandoned.is_null()) {
            const auto& original_parent = abandoned.at("intent").at("parent_identity");
            need(replay_parent_path(parent_path) == text(original_parent, "path"), ErrorCode::Conflict,
                 "abandoned allocation key was reused for another parent path");
            const auto actor = store.host_actor(Role::Root, request_id);
            // Revalidate the current Root, request, original task and key in
            // Store's normal exact-replay path. Reusing the saved generation is
            // valid only for this irreversible metadata result, never creation.
            return store.prepare_owned_workspace(actor, task_id, original_parent, key);
        }
    }
    Json parent_identity;
    if (store.allocation_recovery_enabled()) parent_identity = allocation_parent_identity(parent_path);
    else {
        Workspace parent(parent_path, WorkspaceAccess::ReadOnly);
        parent_identity = parent.root_identity();
    }
    const auto actor = store.host_actor(Role::Root, request_id);
    auto resource = store.prepare_owned_workspace(actor, task_id, parent_identity, key);
    // A historical key cannot revive its explicitly abandoned allocation.
    if (text(resource, "status") == "ABANDONED") return resource;
    if (text(resource, "status") == "PREPARED") {
        if (store.allocation_recovery_enabled())
            resource = store.authorize_owned_allocation(actor, text(resource, "resource_id"),
                resource.at("state_version").to_number<std::int64_t>());
        Json observed;
        const auto* version = resource.at("intent").as_object().if_contains("version");
        if (version && *version == "owned-allocation-v2") {
            if (resource.at("allocation_receipt").is_null()) {
                const auto receipt = stage_owned_workspace(resource.at("intent"));
                resource = store.record_owned_allocation_receipt(actor, text(resource, "resource_id"),
                    resource.at("state_version").to_number<std::int64_t>(), receipt);
            }
            resource = store.authorize_owned_allocation(actor, text(resource, "resource_id"),
                resource.at("state_version").to_number<std::int64_t>());
            observed = publish_owned_workspace(resource);
        } else observed = create_owned_workspace(resource.at("intent"));
        auto verified = resource; verified.as_object()["identity"] = observed;
        (void)inspect_owned_workspace(verified);
        resource = store.register_owned_workspace(actor, text(resource, "resource_id"), observed,
            resource.at("state_version").to_number<std::int64_t>());
    } else {
        need(text(resource, "status") == "OWNED", ErrorCode::NotReady, "allocation is already being removed");
        (void)inspect_owned_workspace(resource);
    }
    return resource;
}
Json cleanup_owned_workspaces(Store& store, const std::string& id, std::size_t limit) {
    need(limit > 0 && limit <= 4096, ErrorCode::InvalidInput, "cleanup pass must be 1..4096 entries");
    const auto status = store.workspace_cleanup_status(id);
    need(status.at("workspace_eligible").as_bool(), ErrorCode::NotReady, "cleanup blocked; inspect closeout before deleting");
    std::optional<Json> confirmed;
    for (const auto& delivery : store.deliveries(id)) if (text(delivery, "status") == "CONFIRMED") {
        need(!confirmed, ErrorCode::Corrupt, "multiple confirmed packages for one request"); confirmed = delivery;
    }
    need(confirmed.has_value(), ErrorCode::NotReady, "no exact confirmed package");
    const auto& plan = confirmed->at("frozen_plan");
    DeliveryDirectory directory(text(plan.at("root_identity"), "path"));
    const auto verified = directory.verify_and_sync_existing(plan);
    auto resources = store.active_owned_workspaces(id); boost::json::array inventories;
    for (const auto& resource : resources) {
        try {
            inventories.push_back(resource.at("inventory").is_null() ? inventory_owned_workspace(resource) : resource.at("inventory"));
        } catch (const Error& error) {
            if (error.code() != ErrorCode::Quota) throw;
            // This is before begin_workspace_cleanup and before any unlink in
            // this pass. A legacy tree is never split, adopted or silently
            // assigned today's policy merely to make cleanup succeed.
            auto retained = status;
            retained.as_object()["workspace_eligible"] = false;
            retained.at("workspace_blockers").as_array().push_back("owned_workspace_capacity_exceeded");
            retained.at("blockers").as_array().push_back("owned_workspace_capacity_exceeded");
            retained.as_object()["capacity_exit"] = boost::json::object{
                {"status", "RETAINED_CAPACITY_EXCEEDED"}, {"resource_id", resource.at("resource_id")},
                {"original_capacity_policy", owned_workspace_intent_capacity(resource.at("intent"))},
                {"reason", error.what()}, {"inventory_committed_this_pass", false},
                {"deletion_performed_this_pass", false},
                {"host_action_required", "retain_or_explicitly_handle_outside_automated_cleanup"}};
            retained.as_object()["pass_removed_entries"] = 0;
            retained.as_object()["workspace_absence_verified_now"] = false;
            return retained;
        }
    }
    const auto actor = store.host_actor(Role::Root, id);
    const auto claim = store.begin_workspace_cleanup(actor, verified, inventories);
    std::size_t removed = 0; bool complete = true;
    resources = store.active_owned_workspaces(id);
    for (const auto& resource : resources) {
        store.authorize_workspace_cleanup(actor, claim);
        // Recheck the *existing* durable delivery at every resource boundary.
        // This cannot reconstruct a package lost since the acknowledgment.
        (void)directory.verify_and_sync_existing(plan);
        const auto& inventory = resource.at("inventory");
        if (text(resource, "status") == "REMOVED") {
            (void)verify_owned_workspace_removed(resource, inventory); continue;
        }
        if (removed == limit) { complete = false; break; }
        const auto pass = erase_owned_workspace(resource, inventory, limit - removed);
        removed += pass.at("removed_entries").to_number<std::size_t>();
        if (!pass.at("complete").as_bool()) { complete = false; break; }
        store.record_workspace_removal(actor, claim, text(resource, "resource_id"), pass.at("proof"));
    }
    Json result;
    if (complete) {
        boost::json::array proofs;
        for (const auto& resource : store.active_owned_workspaces(id)) proofs.push_back(verify_owned_workspace_removed(resource, resource.at("inventory")));
        result = store.finish_workspace_cleanup(actor, claim, directory.verify_and_sync_existing(plan), proofs);
    } else result = store.workspace_cleanup_status(id);
    result.as_object()["pass_removed_entries"] = removed;
    result.as_object()["workspace_absence_verified_now"] = complete;
    return result;
}
} // namespace wls::s1
