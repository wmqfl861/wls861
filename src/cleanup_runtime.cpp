#include "wls/cleanup_runtime.hpp"
#include "wls/cleanup.hpp"
#include "wls/delivery.hpp"
#include "wls/workspace.hpp"
#include <optional>

namespace wls::s1 {
namespace {
std::string text(const Json& value, std::string_view key) { return std::string(value.at(key).as_string()); }
void need(bool value, ErrorCode code, const char* reason) { if (!value) throw Error(code, reason); }
}
Json allocate_owned_workspace(Store& store, const std::string& request_id, const std::string& task_id,
        const std::filesystem::path& parent_path, const std::string& key) {
    Workspace parent(parent_path, WorkspaceAccess::ReadOnly);
    const auto actor = store.host_actor(Role::Root, request_id);
    auto resource = store.prepare_owned_workspace(actor, task_id, parent.root_identity(), key);
    if (text(resource, "status") == "PREPARED") {
        const auto observed = create_owned_workspace(resource.at("intent"));
        auto verified = resource; verified.as_object()["identity"] = observed;
        (void)inspect_owned_workspace(verified);
        resource = store.register_owned_workspace(actor, text(resource, "resource_id"), observed);
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
    auto resources = store.owned_workspaces(id); boost::json::array inventories;
    for (const auto& resource : resources) {
        inventories.push_back(resource.at("inventory").is_null() ? inventory_owned_workspace(resource) : resource.at("inventory"));
    }
    const auto actor = store.host_actor(Role::Root, id);
    const auto claim = store.begin_workspace_cleanup(actor, verified, inventories);
    std::size_t removed = 0; bool complete = true;
    resources = store.owned_workspaces(id);
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
        for (const auto& resource : store.owned_workspaces(id)) proofs.push_back(verify_owned_workspace_removed(resource, resource.at("inventory")));
        result = store.finish_workspace_cleanup(actor, claim, directory.verify_and_sync_existing(plan), proofs);
    } else result = store.workspace_cleanup_status(id);
    result.as_object()["pass_removed_entries"] = removed;
    result.as_object()["workspace_absence_verified_now"] = complete;
    return result;
}
} // namespace wls::s1
