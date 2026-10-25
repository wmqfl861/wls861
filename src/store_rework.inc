// R2.1 uses existing immutable events and workflow tables. No old SQL bytes or
// frozen snapshots change; the only new authority is the captured host action.
Json Store::blocked_submission_action(const std::string& task_id) const {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    const auto task = impl_->task_row(task_id);
    const auto request = impl_->request_row(str(task, "request_id"));
    if (terminal_request(str(request, "status")) || str(task, "status") != "BLOCKED" ||
        number(task, "based_on_requirements_revision") != number(request, "requirements_revision") ||
        number(task, "plan_revision") != number(request, "plan_revision")) return nullptr;
    const auto row = impl_->optional("SELECT * FROM workflow_submissions WHERE task_id=? AND status='BLOCKED'", {task_id});
    if (!row) return nullptr;
    const auto binding = impl_->binding(task_id);
    if (!semantic_binding_equal(execution_binding_from_json(parse_json_strict(str(*row, "original_binding"))), binding, true)) return nullptr;
    auto target = impl_->submission_info(str(*row, "submission_id"));
    const auto config = parse_json_strict(str(request, "config_snapshot"), 8 * 1024 * 1024);
    const auto maximum = number(config.at("system").at("limits").as_object(), "team_internal_rework_max");
    require(maximum >= 0 && maximum <= 10000, ErrorCode::Corrupt, "invalid frozen rework limit");
    const auto used = number(target.as_object(), "team_rework_count");
    Json question = nullptr;
    const auto previous = impl_->optional("SELECT result FROM events WHERE request_id=? AND type='resolve_blocked_submission' AND json_extract(payload,'$.submission_id')=? ORDER BY seq DESC LIMIT 1",
        {str(task, "request_id"), str(*row, "submission_id")});
    if (previous) {
        const auto result = parse_json_strict(str(*previous, "result"));
        if (!result.at("question_id").is_null()) question = impl_->question_info(str(result.as_object(), "question_id"));
        if (question.is_null() || question.at("status") != "ANSWERED" || used >= maximum) return nullptr;
    }
    if (impl_->has_blocking_question(task_id)) return nullptr;
    const auto wait = impl_->wait_info(task_id);
    if (!wait.is_null() && wait.at("status") == "WAITING") return nullptr;
    Json rejected = nullptr;
    if (const auto rejection = impl_->optional("SELECT * FROM candidate_acceptances WHERE submission_id=? AND status='REJECTED' ORDER BY rowid DESC LIMIT 1", {str(*row, "submission_id")}))
        rejected = impl_->acceptance_info(*rejection);
    return Row{{"kind", "blocked_submission"}, {"target", target}, {"rejection", rejected},
        {"expected_task_version", task.at("state_version")}, {"rework_count", used}, {"rework_limit", maximum},
        {"rework_available", used < maximum}, {"followup_question", question},
        {"ask_user_available", !previous.has_value()}};
}

Json Store::submission_followup(const std::string& id) const {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    const auto submission = impl_->submission_row(id);
    const auto row = impl_->optional("SELECT result FROM events WHERE request_id=? AND type='resolve_blocked_submission' AND json_extract(payload,'$.submission_id')=? ORDER BY seq DESC LIMIT 1",
        {str(submission, "request_id"), id});
    return row ? parse_json_strict(str(*row, "result")) : Json(nullptr);
}

Json Store::rework_request(const std::string& task_id) const {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    const auto task = impl_->task_row(task_id);
    const auto row = impl_->optional("SELECT result FROM events WHERE request_id=? AND type='resolve_blocked_submission' AND json_extract(result,'$.task_id')=? AND json_extract(result,'$.action')='rework' ORDER BY seq DESC LIMIT 1",
        {str(task, "request_id"), task_id});
    if (!row) return nullptr;
    const auto result = parse_json_strict(str(*row, "result"));
    const auto target = execution_binding_from_json(result.at("next_binding"));
    const auto current = impl_->binding(task_id);
    if (target.requirements_revision != current.requirements_revision || target.plan_revision != current.plan_revision ||
        target.task_revision != current.task_revision) return nullptr;
    return result;
}

Json Store::resolve_blocked_submission(const StoreActor& actor, const ExecutionBinding& captured,
        std::int64_t expected_task, const std::string& id, std::int64_t expected_submission,
        const Json& decision, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    object_fields(decision, {"action", "reason"}, {"action", "reason"});
    const auto action = workflow_text(decision, "action", 32);
    const auto reason = workflow_text(decision, "reason", 16000);
    require((action == "rework" || action == "ask_user") && expected_task > 0 && expected_submission > 0,
            ErrorCode::InvalidInput, "follow-up needs an action and captured object versions");
    const auto command = payload(actor, "resolve_blocked_submission", Row{{"binding", to_json(captured)},
        {"expected_task_version", expected_task}, {"submission_id", id},
        {"expected_submission_version", expected_submission}, {"decision", decision}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false); impl_->actor_binding(actor, captured);
    const auto submission = impl_->submission_row(id);
    require(str(submission, "request_id") == actor.request_id, ErrorCode::Unauthorized, "rejected submission belongs to another Dispatcher");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    impl_->workflow_subject(actor, captured, submission, true);
    const auto request = impl_->request_row(actor.request_id);
    const auto task_id = str(submission, "task_id"), team_id = str(submission, "team_id");
    const auto task = impl_->task_row(task_id);
    require(str(submission, "status") == "BLOCKED" && str(task, "status") == "BLOCKED",
            ErrorCode::NotReady, "only a current blocked submission has a follow-up");
    require(number(task, "state_version") == expected_task && number(submission, "state_version") == expected_submission,
            ErrorCode::Conflict, "blocked submission or task CAS changed");
    require(number(task, "based_on_requirements_revision") == number(request, "requirements_revision") &&
            number(task, "plan_revision") == number(request, "plan_revision"), ErrorCode::Stale, "follow-up requires the current explicit disposition and plan");
    require(str(impl_->team_row(team_id), "status") == "ACTIVE", ErrorCode::NotReady, "follow-up cannot revive a stopped team");
    const auto binding = impl_->binding(task_id);
    const auto target = impl_->submission_info(id); // validates original immutable evidence
    const auto config = parse_json_strict(str(request, "config_snapshot"), 8 * 1024 * 1024);
    const auto maximum = number(config.at("system").at("limits").as_object(), "team_internal_rework_max");
    require(maximum >= 0 && maximum <= 10000, ErrorCode::Corrupt, "invalid frozen rework limit");
    const auto used = number(target.as_object(), "team_rework_count");
    const auto wait = impl_->optional("SELECT * FROM worker_waits WHERE task_id=?", {task_id});
    require(!impl_->has_blocking_question(task_id) && (!wait || str(*wait, "status") != "WAITING"),
            ErrorCode::NotReady, "an existing question or wait must be resolved first");
    Row source{{"submission_id", id}, {"record_sha256", target.at("record_sha256")},
        {"original_binding", target.at("original_binding")}, {"checks", target.at("checks")}, {"rejection", nullptr}};
    if (const auto rejection = impl_->optional("SELECT * FROM candidate_acceptances WHERE submission_id=? AND status='REJECTED' ORDER BY rowid DESC LIMIT 1", {id}))
        source["rejection"] = impl_->acceptance_info(*rejection);
    require(!source.at("checks").as_array().empty() || !source.at("rejection").is_null(), ErrorCode::Corrupt, "blocked submission lacks a rejection source");
    Row result{{"request_id", actor.request_id}, {"task_id", task_id}, {"team_id", team_id}, {"submission_id", id},
        {"action", action}, {"reason", reason}, {"source", source}, {"question_id", nullptr},
        {"rework_count", used}, {"rework_limit", maximum}, {"next_binding", to_json(binding)},
        {"business_accepted", false}};
    if (action == "rework") {
        require(used < maximum, ErrorCode::Quota, "frozen team rework limit exhausted; ask the user, do not reset the counter");
        impl_->require_settled(actor.request_id, team_id);
        auto next_binding = binding; next_binding.task_revision = next(binding.task_revision);
        impl_->cas("UPDATE tasks SET task_revision=?,status='READY',accepted_with_issues=0,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
            {next_binding.task_revision, stamp(), task_id, expected_task});
        impl_->execute("UPDATE teams SET paused_for_change=1,state_version=state_version+1,updated_at=? WHERE team_id=?", {stamp(), team_id});
        impl_->cas("UPDATE workflow_submissions SET status='RETURNED_FOR_REWORK',state_version=state_version+1,updated_at=? WHERE submission_id=? AND state_version=?",
            {stamp(), id, expected_submission});
        impl_->execute("INSERT OR IGNORE INTO team_workflow_state(team_id,rework_count) VALUES (?,0)", {team_id});
        impl_->execute("UPDATE team_workflow_state SET rework_count=rework_count+1 WHERE team_id=?", {team_id});
        impl_->supersede_waits(actor.request_id, task_id);
        result["rework_count"] = next(used); result["next_binding"] = to_json(next_binding);
        result["status"] = "AWAITING_TEAM_LEAD_ORDER";
    } else {
        require(!impl_->optional("SELECT seq FROM events WHERE request_id=? AND type='resolve_blocked_submission' AND json_extract(payload,'$.submission_id')=? LIMIT 1", {actor.request_id, id}),
                ErrorCode::NotReady, "this rejected submission already has a user escalation");
        impl_->capacity_available(actor.request_id, "questions");
        impl_->capacity_available(actor.request_id, "open_questions");
        const auto question_id = new_id("q"); const auto generation = wait ? next(number(*wait, "generation")) : 1;
        const Row body{{"question_id", question_id}, {"task_id", task_id}, {"asked_by", "dispatcher"},
            {"text", reason}, {"blocking", true},
            {"context", "Rejected submission " + id + "; original SHA-256 " + str(submission, "record_sha256") +
                ". Answering grants no extra rework budget. Exhaustion requires explicit AMEND, CANCEL or NEW."}};
        const Row original{{"body", body}, {"original_binding", to_json(binding)}};
        impl_->execute("INSERT INTO questions(question_id,request_id,task_id,team_id,asked_by,holder,body,created_at) VALUES (?,?,?,?,'dispatcher','user',?,?)",
            {question_id, actor.request_id, task_id, team_id, json_text(body, 65536), stamp()});
        impl_->execute("INSERT INTO question_runtime VALUES (?,?,?,'OPEN',1,?)",
            {question_id, canonical_json(to_json(binding)), sha256_hex(canonical_json(original)), generation});
        impl_->execute("INSERT INTO worker_waits(task_id,request_id,team_id,generation,status,original_binding,reason,created_at,updated_at) VALUES (?,?,?,?,'WAITING',?,?,?,?) ON CONFLICT(task_id) DO UPDATE SET generation=excluded.generation,status='WAITING',original_binding=excluded.original_binding,reason=excluded.reason,created_at=excluded.created_at,updated_at=excluded.updated_at",
            {task_id, actor.request_id, team_id, generation, canonical_json(to_json(binding)), "question:" + question_id, stamp(), stamp()});
        impl_->cas("UPDATE tasks SET state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), task_id, expected_task});
        impl_->cas("UPDATE workflow_submissions SET state_version=state_version+1,updated_at=? WHERE submission_id=? AND state_version=?", {stamp(), id, expected_submission});
        result["question_id"] = question_id; result["status"] = "AWAITING_USER";
    }
    impl_->event(actor.request_id, command_id, command, result, "dispatcher"); tx.commit(); return result;
}
