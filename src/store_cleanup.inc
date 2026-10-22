// Included by store.cpp after Store::Impl is defined. Shares the same writer
// lease, mutex, transaction helpers and current identity/authorization gates.
Json Store::Impl::owned_info(const Row& row) const {
    Json result = row;
    for (const auto* key : {"intent", "identity", "inventory", "proof"}) {
        if (!row.at(key).is_null()) result.as_object()[key] = parse_json_strict(str(row, key), 8 * 1024 * 1024);
    }
    return result;
}
Json Store::Impl::cleanup_process_facts(const std::string& id) const {
    Array facts;
    for (const auto& op : all("SELECT o.op_id,o.status,r.result,r.record_sha256 FROM ops o LEFT JOIN operation_results r USING(op_id) WHERE o.request_id=? AND o.tool_id='run' ORDER BY o.op_id", {id})) {
        if (str(op, "status") == "CANCELLED" && op.at("result").is_null()) {
            // A pre-dispatch cancellation has no process. A cancellation after
            // dispatch is UNKNOWN and cannot reach this state without evidence.
            const auto intent = one("SELECT * FROM ops WHERE op_id=?", {op.at("op_id")});
            require(intent.at("started_at").is_null(), ErrorCode::UnknownOutcome, "run cancellation lacks original exit proof");
            facts.emplace_back(Row{{"op_id", op.at("op_id")}, {"proof", "cancelled_before_dispatch"}});
            continue;
        }
        require(!op.at("result").is_null(), ErrorCode::UnknownOutcome, "run has no original process observation");
        const auto record = one("SELECT * FROM operation_results WHERE op_id=?", {op.at("op_id")});
        const auto result = parse_json_strict(str(op, "result"), 8 * 1024 * 1024);
        const Row fact{{"op_id", op.at("op_id")}, {"original_binding", parse_json_strict(str(record, "original_binding"))},
            {"outcome", record.at("observed_outcome")}, {"result", result}, {"evidence", parse_json_strict(str(record, "evidence"))},
            {"stdout_bytes", record.at("stdout_bytes")}, {"stderr_bytes", record.at("stderr_bytes")},
            {"stdout_sha256", record.at("stdout_sha256")}, {"stderr_sha256", record.at("stderr_sha256")},
            {"stdout_truncated", number(record, "stdout_truncated") != 0}, {"stderr_truncated", number(record, "stderr_truncated") != 0}};
        require(sha256_hex(canonical_json(fact)) == str(record, "record_sha256"), ErrorCode::Corrupt, "process exit record fingerprint changed");
        require(result.is_object() && result.as_object().contains("spawned") && result.at("spawned").is_bool() &&
            result.as_object().contains("all_descendants_exited") && result.at("all_descendants_exited").is_bool(),
            ErrorCode::UnknownOutcome, "run observation lacks process exit fields");
        require(result.at("all_descendants_exited").as_bool() ||
            (!result.at("spawned").as_bool() && result.as_object().contains("status") && result.at("status") == "REFUSED"),
            ErrorCode::UnknownOutcome, "original run descendants have not been proved exited");
        facts.emplace_back(Row{{"op_id", op.at("op_id")}, {"record_sha256", op.at("record_sha256")},
            {"spawned", result.at("spawned")}, {"all_descendants_exited", result.at("all_descendants_exited")}});
    }
    return facts;
}
Array Store::Impl::cleanup_blockers(const std::string& id) const {
    require(cleanup_schema, ErrorCode::Unsupported, "owned cleanup requires Store 407");
    const auto request = request_row(id); Array blockers;
    const auto config = parse_json_strict(str(request, "config_snapshot"), 8 * 1024 * 1024);
    if (!config.at("system").at("cleanup").at("enabled").as_bool()) blockers.emplace_back("cleanup_disabled_in_frozen_config");
    // Cancelled/failed work may have unique unpublished artifacts. Retain it
    // until its own explicit retention package policy is implemented.
    if (str(request, "status") != "DELIVERED") blockers.emplace_back("durable_success_receipt_required");
    if (!optional("SELECT request_id FROM receipts WHERE request_id=? AND final_status='DELIVERED'", {id}) ||
        !optional("SELECT d.delivery_id FROM delivery_runtime d JOIN delivery_receipts r USING(delivery_id) WHERE d.request_id=? AND d.status='CONFIRMED'", {id}))
        blockers.emplace_back("exact_delivery_receipt_missing");
    if (optional("SELECT experience_id FROM experience_jobs WHERE request_id=? AND status<>'DONE' LIMIT 1", {id})) blockers.emplace_back("experience_pending");
    if (optional("SELECT op_id FROM ops WHERE request_id=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN') LIMIT 1", {id})) blockers.emplace_back("unsettled_operations");
    if (optional("SELECT attempt_id FROM call_attempts WHERE request_id=? AND (status IN ('PENDING','UNKNOWN') OR unknown_input=1 OR unknown_output=1 OR unknown_cost=1) LIMIT 1", {id})) blockers.emplace_back("unsettled_model_accounting");
    if (optional("SELECT resource_id FROM owned_workspaces WHERE request_id=? AND status='PREPARED' LIMIT 1", {id})) blockers.emplace_back("workspace_creation_incomplete");
    for (const auto& bound : all("SELECT w.* FROM workspace_bindings w JOIN teams t USING(team_id) WHERE t.request_id=?", {id})) {
        const auto resource = optional("SELECT * FROM owned_workspaces WHERE request_id=? AND team_id=?", {id, bound.at("team_id")});
        if (!resource || resource->at("identity").is_null()) { blockers.emplace_back("unowned_workspace:" + str(bound, "team_id")); continue; }
        const auto identity = owned_info(*resource).at("identity").at("workspace_identity").as_object();
        if (str(identity, "path") != str(bound, "canonical_path") || number(identity, "device") != number(bound, "device") || number(identity, "inode") != number(bound, "inode"))
            blockers.emplace_back("workspace_binding_mismatch:" + str(bound, "team_id"));
    }
    try { (void)cleanup_process_facts(id); }
    catch (const Error& error) {
        if (error.code() != ErrorCode::UnknownOutcome) throw;
        blockers.emplace_back("original_process_exit_proof_missing");
    }
    return blockers;
}
Json Store::Impl::cleanup_status(const std::string& id) const {
    const auto eligibility = cleanup_blockers(id);
    const auto run = optional("SELECT * FROM workspace_cleanup_runs WHERE request_id=?", {id});
    const bool done = run && str(*run, "status") == "DONE";
    const auto job = optional("SELECT * FROM cleanup_jobs WHERE request_id=?", {id});
    const auto receipt = optional("SELECT * FROM receipts WHERE request_id=?", {id});
    Array blockers = eligibility;
    blockers.emplace_back("private_row_purge_not_implemented");
    // A workspace phase is not a secure erase or a private-data purge. Keep
    // global cleanup pending and distinguish previously recorded completion
    // from a fresh filesystem observation.
    return Row{{"request_id", id}, {"receipt", receipt ? exposed(*receipt) : Json(nullptr)},
        {"cleanup_job", job ? exposed(*job) : Json(nullptr)}, {"workspace_cleanup_recorded", done},
        {"workspace_eligible", eligibility.empty()}, {"workspace_blockers", eligibility}, {"blockers", blockers},
        {"workspace_run", run ? Json(*run) : Json(nullptr)}, {"cleanup_complete", false},
        {"private_data_retained", true}, {"deletion_performed", done ? Json(true) : run ? Json(nullptr) : Json(false)},
        {"deletion_may_have_started", run.has_value()},
        {"checkpoint", done ? "WORKSPACES_REMOVED_PRIVATE_RETAINED" : run ? "WORKSPACE_DELETION_IN_PROGRESS" : "RETAINED"}};
}
void Store::Impl::cleanup_publication(const std::string& id, const Json& publication) const {
    const auto delivery = one("SELECT * FROM delivery_runtime WHERE request_id=? AND status='CONFIRMED'", {id});
    verify_publication(delivery, publication);
    const auto receipt = one("SELECT * FROM receipts WHERE request_id=?", {id});
    require(receipt.at("manifest_sha256") == delivery.at("manifest_sha256") &&
        receipt.at("delivery_path") == publication.at("path"), ErrorCode::Conflict, "cleanup package differs from retained receipt");
}
void Store::Impl::cleanup_authorize(const StoreActor& actor, const Json& claim) const {
    require(cleanup_schema, ErrorCode::Unsupported, "owned cleanup requires Store 407");
    require(mode == StoreOpenMode::Takeover, ErrorCode::Unauthorized, "cleanup requires the Store writer lease");
    (void)authorize(actor, {Role::Root});
    require(claim.is_object() && claim.at("request_id") == Json(actor.request_id), ErrorCode::Unauthorized, "cleanup claim belongs to another request");
    require(cleanup_blockers(actor.request_id).empty(), ErrorCode::NotReady, "cleanup eligibility changed; inspect workspace blockers");
    const auto job = one("SELECT * FROM cleanup_jobs WHERE request_id=?", {actor.request_id});
    const auto run = one("SELECT * FROM workspace_cleanup_runs WHERE request_id=?", {actor.request_id});
    require(job.at("cleanup_id") == claim.at("cleanup_id") && job.at("lease_epoch") == claim.at("lease_epoch") &&
        job.at("state_version") == claim.at("job_version") && job.at("claimed_by") == claim.at("claim_id") &&
        str(job, "status") == "RUNNING" && run.at("manifest_sha256") == claim.at("manifest_sha256"),
        ErrorCode::Stale, "cleanup claim was superseded or recovered");
    require(sha256_hex(str(run, "manifest")) == str(run, "manifest_sha256"), ErrorCode::Corrupt, "cleanup manifest fingerprint mismatch");
}
std::vector<Json> Store::owned_workspaces(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->cleanup_schema, ErrorCode::Unsupported, "owned cleanup requires Store 407");
    (void)impl_->request_row(id); std::vector<Json> result;
    for (const auto& row : impl_->all("SELECT * FROM owned_workspaces WHERE request_id=? ORDER BY resource_id", {id})) result.push_back(impl_->owned_info(row));
    return result;
}
Json Store::workspace_cleanup_status(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); return impl_->cleanup_status(id);
}
Json Store::prepare_owned_workspace(const StoreActor& actor, const std::string& task_id, const Json& parent, const std::string& key) {
    const std::lock_guard lock(impl_->mutex);
    require(impl_->cleanup_schema, ErrorCode::Unsupported, "owned cleanup requires Store 407");
    text_field(key, "owned allocation key", 128); require(parent.is_object(), ErrorCode::InvalidInput, "parent identity required");
    const auto path = workflow_text(parent, "path", 4096);
    require(std::filesystem::path(path).is_absolute() && std::filesystem::path(path).lexically_normal() == path && path != "/" &&
        number(parent.as_object(), "device") > 0 && number(parent.as_object(), "inode") > 0,
        ErrorCode::InvalidInput, "owned parent requires canonical identity");
    Impl::Transaction tx(*impl_); const auto request = impl_->authorize(actor, {Role::Root});
    require(!terminal_request(str(request, "status")), ErrorCode::NotReady, "terminal request cannot allocate workspaces");
    const auto task = impl_->task_row(task_id); require(str(task, "request_id") == actor.request_id, ErrorCode::Unauthorized, "task belongs to another request");
    const auto team = impl_->one("SELECT * FROM teams WHERE task_id=? AND status IN ('STARTING','ACTIVE')", {task_id});
    const auto team_id = str(team, "team_id");
    if (const auto old = impl_->optional("SELECT * FROM owned_workspaces WHERE request_id=? AND command_key=?", {actor.request_id, key})) {
        const auto result = impl_->owned_info(*old);
        require(str(*old, "team_id") == team_id && result.at("intent").at("parent_identity") == parent,
                ErrorCode::Conflict, "owned allocation key was reused for another target");
        tx.commit(); return result;
    }
    require(!impl_->optional("SELECT team_id FROM workspace_bindings WHERE team_id=?", {team_id}), ErrorCode::Unauthorized, "legacy workspace cannot be adopted");
    require(!impl_->optional("SELECT resource_id FROM owned_workspaces WHERE team_id=?", {team_id}), ErrorCode::Conflict, "team already has an owned allocation");
    require(impl_->all("SELECT resource_id FROM owned_workspaces WHERE request_id=?", {actor.request_id}).size() < 64, ErrorCode::Quota, "owned workspace capacity is 64");
    for (const auto& other : impl_->all("SELECT canonical_path FROM workspace_bindings"))
        require(path != str(other, "canonical_path") && !path.starts_with(str(other, "canonical_path") + "/"), ErrorCode::Unauthorized, "ownership envelope cannot be inside a business workspace");
    for (const auto& other : impl_->all("SELECT intent FROM owned_workspaces")) {
        const auto saved = parse_json_strict(str(other, "intent"));
        const auto envelope = str(saved.at("parent_identity").as_object(), "path") + "/" + str(saved.as_object(), "leaf");
        require(path != envelope && !path.starts_with(envelope + "/"), ErrorCode::Unauthorized, "owned resources cannot be nested inside another allocation");
    }
    const auto id = new_id("res"); const auto envelope_path = path + "/" + id;
    require(!overlap(envelope_path, impl_->database_file_path), ErrorCode::Unauthorized, "owned allocation overlaps control database");
    for (const auto& old : impl_->all("SELECT frozen_plan FROM delivery_runtime"))
        require(!overlap(envelope_path, str(parse_json_strict(str(old, "frozen_plan"), 8 * 1024 * 1024).at("root_identity").as_object(), "path")),
                ErrorCode::Unauthorized, "owned allocation overlaps delivery root");
    const Row marker{{"format", "wls-owned-workspace-v1"}, {"resource_id", id}, {"request_id", actor.request_id}, {"team_id", team_id}, {"nonce", new_id("nonce")}};
    const Row intent{{"resource_id", id}, {"request_id", actor.request_id}, {"team_id", team_id}, {"leaf", id}, {"parent_identity", parent}, {"marker", marker}};
    impl_->execute("INSERT INTO owned_workspaces(resource_id,request_id,team_id,command_key,intent,status,created_at,updated_at) VALUES (?,?,?,?,?,'PREPARED',?,?)",
        {id, actor.request_id, team_id, key, json_text(intent), stamp(), stamp()});
    const auto result = impl_->owned_info(impl_->one("SELECT * FROM owned_workspaces WHERE resource_id=?", {id}));
    impl_->event(actor.request_id, new_id("own"), payload(actor, "prepare_owned_workspace", intent), Row{{"resource_id", id}, {"status", "PREPARED"}}, "root");
    tx.commit(); return result;
}
Json Store::register_owned_workspace(const StoreActor& actor, const std::string& id, const Json& identity) {
    const std::lock_guard lock(impl_->mutex); require(impl_->cleanup_schema, ErrorCode::Unsupported, "owned cleanup requires Store 407");
    Impl::Transaction tx(*impl_); const auto request = impl_->authorize(actor, {Role::Root});
    require(!terminal_request(str(request, "status")), ErrorCode::NotReady, "terminal request cannot register workspaces");
    const auto row = impl_->one("SELECT * FROM owned_workspaces WHERE resource_id=?", {id});
    require(str(row, "request_id") == actor.request_id, ErrorCode::Unauthorized, "resource belongs to another request");
    if (!row.at("identity").is_null()) {
        require(impl_->owned_info(row).at("identity") == identity, ErrorCode::Conflict, "resource original identity changed");
        tx.commit(); return impl_->owned_info(row);
    }
    const auto intent = parse_json_strict(str(row, "intent")); const auto path = str(intent.at("parent_identity").as_object(), "path") + "/" + id;
    require(identity.is_object() && identity.at("parent_identity") == intent.at("parent_identity") &&
        workflow_text(identity.at("envelope_identity"), "path", 4096) == path && workflow_text(identity.at("workspace_identity"), "path", 4096) == path + "/work" &&
        workflow_text(identity, "marker_sha256", 64) == sha256_hex(canonical_json(intent.at("marker"))), ErrorCode::Conflict, "creation observation differs from original intent");
    for (const auto* name : {"envelope_identity", "workspace_identity"}) {
        const auto& object = identity.at(name).as_object();
        require(number(object, "device") > 0 && number(object, "inode") > 0 && number(object, "mount_id") > 0 &&
            number(object, "birth_seconds") >= 0 && number(object, "birth_nanoseconds") >= 0, ErrorCode::InvalidInput, "complete filesystem generation required");
    }
    impl_->cas("UPDATE owned_workspaces SET identity=?,status='OWNED',state_version=state_version+1,updated_at=? WHERE resource_id=? AND state_version=? AND status='PREPARED'",
        {json_text(identity), stamp(), id, number(row, "state_version")});
    impl_->event(actor.request_id, new_id("own"), payload(actor, "register_owned_workspace", Row{{"resource_id", id}}), identity, "root");
    const auto result = impl_->owned_info(impl_->one("SELECT * FROM owned_workspaces WHERE resource_id=?", {id})); tx.commit(); return result;
}
Json Store::begin_workspace_cleanup(const StoreActor& actor, const Json& publication, const Json& inventories) {
    const std::lock_guard lock(impl_->mutex); require(impl_->cleanup_schema, ErrorCode::Unsupported, "owned cleanup requires Store 407");
    Impl::Transaction tx(*impl_); (void)impl_->authorize(actor, {Role::Root});
    require(impl_->cleanup_blockers(actor.request_id).empty(), ErrorCode::NotReady, "workspace cleanup is blocked; inspect closeout");
    impl_->cleanup_publication(actor.request_id, publication);
    require(inventories.is_array(), ErrorCode::InvalidInput, "complete inventory array required");
    const auto resources = impl_->all("SELECT * FROM owned_workspaces WHERE request_id=? ORDER BY resource_id", {actor.request_id});
    require(resources.size() == inventories.as_array().size(), ErrorCode::InvalidInput, "inventory must cover every owned resource");
    Array targets;
    for (const auto& row : resources) {
        const auto id = str(row, "resource_id"); const auto current = impl_->owned_info(row); const Json* inventory = nullptr;
        for (const auto& item : inventories.as_array()) if (item.at("resource_id") == Json(id)) { require(!inventory, ErrorCode::InvalidInput, "duplicate cleanup inventory"); inventory = &item; }
        require(inventory && inventory->at("identity") == current.at("identity") && inventory->at("entries").is_array(), ErrorCode::Conflict, "cleanup inventory does not bind owned identity");
        if (!current.at("inventory").is_null()) require(current.at("inventory") == *inventory, ErrorCode::Conflict, "cleanup cannot change its frozen inventory");
        targets.emplace_back(Row{{"resource_id", id}, {"identity", current.at("identity")}, {"inventory_sha256", sha256_hex(canonical_json(*inventory))}});
    }
    const Row manifest{{"version", "owned-workspace-cleanup-v1"}, {"request_id", actor.request_id}, {"publication", publication},
        {"targets", targets}, {"process_exit_facts", impl_->cleanup_process_facts(actor.request_id)}, {"private_data_retained", true}};
    // publication's optional transport hints are not part of authorization:
    // immutable package identity and inventory alone bind restart equivalence.
    Json canonical_manifest = manifest; canonical_manifest.as_object()["publication"] = Row{{"delivery_id", publication.at("delivery_id")},
        {"manifest_sha256", publication.at("manifest_sha256")}, {"root_identity", publication.at("root_identity")}, {"path", publication.at("path")}, {"files_sha256", publication.at("files_sha256")}};
    const auto encoded = json_text(canonical_manifest, 8 * 1024 * 1024); const auto hash = sha256_hex(encoded);
    if (const auto old = impl_->optional("SELECT * FROM workspace_cleanup_runs WHERE request_id=?", {actor.request_id}))
        require(str(*old, "manifest") == encoded && str(*old, "manifest_sha256") == hash, ErrorCode::Conflict, "cleanup authorization changed after deletion began");
    else {
        impl_->execute("INSERT INTO workspace_cleanup_runs(request_id,manifest,manifest_sha256,status,created_at,updated_at) VALUES (?,?,?,'RUNNING',?,?)", {actor.request_id, encoded, hash, stamp(), stamp()});
        for (const auto& item : inventories.as_array()) impl_->execute("UPDATE owned_workspaces SET inventory=?,status='DELETING',state_version=state_version+1,updated_at=? WHERE resource_id=? AND request_id=?",
            {json_text(item, 8 * 1024 * 1024), stamp(), item.at("resource_id"), actor.request_id});
    }
    const auto job = impl_->one("SELECT * FROM cleanup_jobs WHERE request_id=?", {actor.request_id}); const auto claim = new_id("clean");
    impl_->cas("UPDATE cleanup_jobs SET status='RUNNING',claimed_by=?,lease_epoch=lease_epoch+1,attempts=attempts+1,state_version=state_version+1,updated_at=? WHERE cleanup_id=? AND state_version=?",
        {claim, stamp(), job.at("cleanup_id"), job.at("state_version")});
    const Row result{{"request_id", actor.request_id}, {"cleanup_id", job.at("cleanup_id")}, {"lease_epoch", next(number(job, "lease_epoch"))},
        {"job_version", next(number(job, "state_version"))}, {"claim_id", claim}, {"manifest_sha256", hash}};
    tx.commit(); return result;
}
void Store::authorize_workspace_cleanup(const StoreActor& actor, const Json& claim) const {
    const std::lock_guard lock(impl_->mutex); impl_->cleanup_authorize(actor, claim);
}
Json Store::record_workspace_removal(const StoreActor& actor, const Json& claim, const std::string& id, const Json& proof) {
    const std::lock_guard lock(impl_->mutex); Impl::Transaction tx(*impl_); impl_->cleanup_authorize(actor, claim);
    const auto row = impl_->one("SELECT * FROM owned_workspaces WHERE resource_id=?", {id}); const auto resource = impl_->owned_info(row);
    require(str(row, "request_id") == actor.request_id, ErrorCode::Unauthorized, "resource belongs to another request");
    require(proof.is_object() && proof.at("resource_id") == Json(id) && proof.at("identity") == resource.at("identity") &&
        proof.at("inventory_sha256") == Json(sha256_hex(canonical_json(resource.at("inventory")))) &&
        proof.at("workspace_absent") == true && proof.at("durable") == true && proof.at("envelope_retained") == true,
        ErrorCode::Conflict, "cleanup proof must bind the exact durable inventory");
    if (!row.at("proof").is_null()) require(resource.at("proof") == proof, ErrorCode::Conflict, "original cleanup proof changed");
    else impl_->cas("UPDATE owned_workspaces SET proof=?,status='REMOVED',state_version=state_version+1,updated_at=? WHERE resource_id=? AND state_version=?",
        {json_text(proof), stamp(), id, row.at("state_version")});
    tx.commit(); return proof;
}
Json Store::finish_workspace_cleanup(const StoreActor& actor, const Json& claim, const Json& publication, const Json& proofs) {
    const std::lock_guard lock(impl_->mutex); Impl::Transaction tx(*impl_); impl_->cleanup_authorize(actor, claim); impl_->cleanup_publication(actor.request_id, publication);
    require(proofs.is_array(), ErrorCode::InvalidInput, "fresh absence proofs required");
    const auto resources = impl_->all("SELECT * FROM owned_workspaces WHERE request_id=? ORDER BY resource_id", {actor.request_id});
    require(resources.size() == proofs.as_array().size(), ErrorCode::InvalidInput, "every owned directory needs a fresh absence proof");
    for (const auto& row : resources) {
        require(str(row, "status") == "REMOVED", ErrorCode::NotReady, "workspace deletion is incomplete");
        const auto proof = impl_->owned_info(row).at("proof"); std::size_t matched = 0;
        for (const auto& candidate : proofs.as_array()) if (candidate == proof) ++matched;
        require(matched == 1, ErrorCode::Conflict, "fresh proof does not match original cleanup evidence");
    }
    impl_->execute("UPDATE workspace_cleanup_runs SET status='DONE',state_version=state_version+1,updated_at=? WHERE request_id=?", {stamp(), actor.request_id});
    // Keep the global job PENDING: private data is intentionally still retained.
    impl_->execute("UPDATE cleanup_jobs SET status='PENDING',claimed_by=NULL,last_error='private_row_purge_pending',state_version=state_version+1,updated_at=? WHERE request_id=?", {stamp(), actor.request_id});
    const auto result = impl_->cleanup_status(actor.request_id); tx.commit(); return result;
}
