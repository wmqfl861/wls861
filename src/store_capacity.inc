    // Additive bookkeeping in the existing immutable event log. No SQL asset
    // changes and no deletion of commands/results needed for exact replay.
    static std::string capacity_control_sql() {
        return "(type IN ('ack','cancel_op','finish_op','reconcile_op','record_op_result',"
            "'execute_tool_control','wait_worker','wake_worker','tool_feedback','ask_question',"
            "'forward_question','answer_question','escalate_task','report_experience','record_delivery_publication',"
            "'acknowledge_delivery','cancel_request','settle_attempt','attempts_unknown',"
            "'claim_live_send','record_live_transport','reconcile_usage',"
            "'allocation_receipt','allocation_registered','allocation_abandoned') OR "
            "(type='check_submission' AND json_extract(payload,'$.adverse_revoke')=1) OR "
            "(type='accept_candidate' AND json_extract(payload,'$.decision.passed')=0) OR "
            "(type='reserve_attempt' AND json_type(payload,'$.experience_job_id') IS NOT NULL))";
    }
    static bool capacity_control(const Row& command) {
        const auto type = str(command, "type");
        const std::set<std::string> types{"ack", "cancel_op", "finish_op", "reconcile_op", "record_op_result",
            "execute_tool_control", "wait_worker", "wake_worker", "tool_feedback", "ask_question",
            "forward_question", "answer_question", "escalate_task", "report_experience", "record_delivery_publication",
            "acknowledge_delivery", "cancel_request", "settle_attempt", "attempts_unknown",
            "claim_live_send", "record_live_transport", "reconcile_usage",
            "allocation_receipt", "allocation_registered", "allocation_abandoned"};
        return types.contains(type) || (type == "check_submission" && command.at("adverse_revoke").as_bool()) ||
            (type == "accept_candidate" && !command.at("decision").at("passed").as_bool()) ||
            (type == "reserve_attempt" && command.contains("experience_job_id"));
    }
    static bool capacity_maintenance(std::string_view type) {
        return type == "recover" || type == "event_bootstrap" || type == "recover_experience" || type.starts_with("capacity_");
    }
    static bool capacity_continuation(const Row& command) {
        // No new general operation/model admission here. Existing operation
        // dispatch and bounded settlement must be able to finish after a hold.
        const auto type = str(command, "type");
        return capacity_maintenance(type) || type == "start_op" || type == "start_tool_op" ||
            (capacity_control(command) && type != "wait_worker" && type != "ask_question" && type != "escalate_task");
    }
    struct CapacityEventState {
        Json saved{Row{}};
        std::int64_t seq{0};
        std::int64_t window{0};
        std::int64_t business{0};
        std::int64_t control{0};
        bool held{false};
        bool warned{false};
    };
    mutable std::map<std::string, CapacityEventState> capacity_event_cache;
    mutable std::map<std::string, Json> capacity_limit_cache;
    mutable std::int64_t capacity_data_version{-1};
    void refresh_capacity_cache() const {
        const auto version = number(one("PRAGMA data_version"), "data_version");
        if (version != capacity_data_version) {
            capacity_event_cache.clear(); capacity_limit_cache.clear(); capacity_data_version = version;
        }
    }
    Json capacity_limits(const std::string& id) const {
        refresh_capacity_cache();
        if (mode == StoreOpenMode::Takeover) {
            const auto found = capacity_limit_cache.find(id);
            if (found != capacity_limit_cache.end()) return found->second;
        }
        const auto result = store_capacity_limits(parse_json_strict(str(request_row(id), "config_snapshot"), 8 * 1024 * 1024).at("system"));
        if (mode == StoreOpenMode::Takeover) {
            if (capacity_limit_cache.size() >= 1024) capacity_limit_cache.clear();
            capacity_limit_cache.emplace(id, result);
        }
        return result;
    }
    CapacityEventState capacity_events(const std::string& id) const {
        refresh_capacity_cache();
        if (mode == StoreOpenMode::Takeover) {
            const auto found = capacity_event_cache.find(id);
            if (found != capacity_event_cache.end()) return found->second;
        }
        CapacityEventState state;
        const auto checkpoint = optional("SELECT seq,result FROM events WHERE request_id=? AND type='capacity_checkpoint' ORDER BY seq DESC LIMIT 1", {id});
        if (checkpoint) {
            state.saved = parse_json_strict(str(*checkpoint, "result"));
            state.seq = number(*checkpoint, "seq"); state.window = number(state.saved.as_object(), "window");
        }
        const auto control = capacity_control_sql();
        const auto row = one("SELECT COALESCE(SUM(CASE WHEN " + control + " THEN 0 ELSE 1 END),0) AS business_events, "
            "COALESCE(SUM(CASE WHEN " + control + " THEN 1 ELSE 0 END),0) AS control_events FROM events WHERE request_id=? AND seq>? "
            "AND type NOT IN ('recover','event_bootstrap','recover_experience','capacity_warning','capacity_hold','capacity_checkpoint')", {id, state.seq});
        state.business = number(row, "business_events"); state.control = number(row, "control_events");
        state.held = optional("SELECT seq FROM events WHERE request_id=? AND seq>? AND type='capacity_hold' LIMIT 1", {id,state.seq}).has_value();
        state.warned = optional("SELECT seq FROM events WHERE request_id=? AND seq>? AND type='capacity_warning' LIMIT 1", {id,state.seq}).has_value();
        if (mode == StoreOpenMode::Takeover) {
            if (capacity_event_cache.size() >= 1024) capacity_event_cache.clear();
            capacity_event_cache.emplace(id, state);
        }
        return state;
    }
    void capacity_event_inserted(const std::string& id, const Row& command) {
        const auto found = capacity_event_cache.find(id);
        if (found == capacity_event_cache.end()) return;
        const auto type = str(command, "type");
        if (type == "capacity_checkpoint") { capacity_event_cache.erase(found); return; }
        if (type == "capacity_warning") found->second.warned = true;
        if (type == "capacity_hold") found->second.held = true;
        if (!capacity_maintenance(type)) {
            if (capacity_control(command)) ++found->second.control;
            else ++found->second.business;
        }
    }
    Row capacity_totals(const std::string& id) const {
        const auto count = [&](std::string_view table) {
            return number(one("SELECT COUNT(*) AS n FROM " + std::string(table) + " WHERE request_id=?", {id}), "n");
        };
        Row result{{"operations", count("ops")}, {"requirement_revisions", number(one("SELECT requirements_revision FROM requests WHERE request_id=?", {id}), "requirements_revision")},
            {"questions", workflow_schema ? count("questions") : 0},
            {"submissions", workflow_schema ? count("workflow_submissions") : 0},
            {"acceptances", acceptance_schema ? count("candidate_acceptances") : 0},
            {"deliveries", acceptance_schema ? count("delivery_runtime") : 0}};
        return result;
    }
    Row capacity_state(const std::string& id) const {
        const auto history = capacity_events(id);
        const bool checkpoint = history.window != 0;
        const auto& saved = history.saved;
        const auto seq = history.seq;
        const auto window = history.window;
        auto counts = capacity_totals(id);
        const auto totals = counts;
        if (checkpoint) for (auto& item : counts) {
            const auto base = number(saved.at("totals").as_object(), item.key());
            require(number(counts, item.key()) >= base, ErrorCode::Corrupt, "capacity audit totals were removed or rewritten");
            item.value() = number(counts, item.key()) - base;
        }
        counts["business_events"] = history.business;
        counts["control_events"] = history.control;
        Row task_counts;
        std::int64_t maximum = 0;
        if (workflow_schema) for (const auto& task : all("SELECT task_id,COUNT(*) AS n FROM workflow_submissions WHERE request_id=? GROUP BY task_id", {id})) {
            const auto task_id = str(task, "task_id");
            const auto total = number(task, "n"); task_counts[task_id] = total;
            const auto* old = checkpoint ? saved.at("task_submissions").as_object().if_contains(task_id) : nullptr;
            const auto base = old ? old->as_int64() : 0;
            require(total >= base, ErrorCode::Corrupt, "submission audit history was removed");
            maximum = std::max(maximum, total - base);
        }
        counts["submissions_per_task"] = maximum;
        const auto open_questions = workflow_schema ? number(one("SELECT COUNT(*) AS n FROM questions q JOIN question_runtime x USING(question_id) WHERE q.request_id=? AND x.status='OPEN'", {id}), "n") : 0;
        const auto pending_ops = number(one("SELECT COUNT(*) AS n FROM ops WHERE request_id=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN')", {id}), "n");
        // Original nullable settlements are immutable. For reconciled live
        // calls, the current five-dimensional projection determines whether
        // settlement controls are still needed; Fake/legacy calls retain
        // their original three-dimensional accounting rule.
        const auto pending_calls = number(one(reconciliation_schema ?
            "SELECT COUNT(*) AS n FROM call_attempts c LEFT JOIN live_attempt_execution l USING(attempt_id) "
            "LEFT JOIN live_usage_projection p USING(attempt_id) WHERE c.request_id=? AND "
            "(c.status IN ('PENDING','UNKNOWN') OR "
            "(l.attempt_id IS NULL AND (c.unknown_input=1 OR c.unknown_output=1 OR c.unknown_cost=1)) OR "
            "(l.attempt_id IS NOT NULL AND (p.attempt_id IS NULL OR p.disputed=1 OR p.input_tokens IS NULL OR "
            "p.output_tokens IS NULL OR p.cache_read_tokens IS NULL OR p.cache_write_tokens IS NULL OR p.cost_micro_usd IS NULL)))" :
            "SELECT COUNT(*) AS n FROM call_attempts WHERE request_id=? AND "
            "(status IN ('PENDING','UNKNOWN') OR unknown_input=1 OR unknown_output=1 OR unknown_cost=1)", {id}), "n");
        // New allocation stages reserve their remaining finite settlement
        // tail. Old event types retain their original classification. A v1
        // PREPARED resource needs one register-or-abandon event; v2 needs a
        // receipt and then register-or-abandon, or just the latter once its
        // immutable receipt is present. This is not a checkpoint total: old
        // saved windows have no fabricated allocation counters.
        const auto reserved_owned = allocation_schema ? number(one(
            "SELECT COALESCE(SUM(CASE WHEN json_extract(intent,'$.version')='owned-allocation-v2' AND allocation_receipt IS NULL THEN 2 ELSE 1 END),0) AS n "
            "FROM owned_workspaces WHERE request_id=? AND status='PREPARED'", {id}), "n") : 0;
        const auto reserved_control = 4 * (pending_ops + pending_calls) + 16 + reserved_owned;
        const auto prepared = number(one("SELECT COUNT(*) AS n FROM ops WHERE request_id=? AND status='PREPARED'", {id}), "n");
        const auto limits = capacity_limits(id);
        Array exhausted, approaching;
        for (const auto& item : counts) {
            const auto used = number(counts, item.key());
            const auto limit = number(limits.as_object(), item.key());
            if (used >= limit) exhausted.emplace_back(item.key());
            if (used >= (limit * 4) / 5) approaching.emplace_back(item.key());
        }
        if (number(counts, "control_events") + reserved_control >= number(limits.as_object(), "control_events") * 4 / 5) approaching.emplace_back("control_reserve");
        if (number(counts, "business_events") + prepared >= number(limits.as_object(), "business_events") * 4 / 5) approaching.emplace_back("dispatch_reserve");
        if (number(counts, "control_events") + reserved_control + 8 >= number(limits.as_object(), "control_events")) exhausted.emplace_back("control_reserve");
        if (number(counts, "business_events") + prepared + 4 >= number(limits.as_object(), "business_events")) exhausted.emplace_back("dispatch_reserve");
        const bool recorded_hold = history.held;
        Row result{{"request_id", id}, {"window", window}, {"start_seq", seq}, {"limits", limits}, {"counts", counts},
            {"totals", totals}, {"task_submissions", task_counts}, {"open_questions", open_questions},
            {"reserved_control", reserved_control}, {"reserved_dispatch", prepared}, {"exhausted", exhausted}, {"approaching", approaching},
            {"recorded_hold", recorded_hold}, {"recorded_warning", history.warned}, {"status", recorded_hold || !exhausted.empty() ? "WAITING_INPUT" : "OPEN"},
            {"resume_command", "capacity-checkpoint"}, {"audit_rows_retained", true}, {"budgets_reset", false}};
        if (allocation_schema) {
            result["reserved_owned_allocation"] = reserved_owned;
            result["owned_allocation"] = Row{{"active_limit", 64}, {"history_limit", 256},
                {"active", number(one("SELECT COUNT(*) AS n FROM owned_workspaces WHERE request_id=? AND status<>'ABANDONED'", {id}), "n")},
                {"history", number(one("SELECT COUNT(*) AS n FROM owned_workspaces WHERE request_id=?", {id}), "n")},
                {"lifetime_limits", true}};
        }
        return result;
    }
    void capacity_raw_event(const std::string& id, const std::string& key, const Row& command, const Json& result) {
        text_field(key, "command_id");
        execute("INSERT INTO events(event_id,request_id,type,actor,command_id,command_sha256,payload,result,created_at) VALUES (?,?,?,'runtime',?,?,?,?,?)",
            {new_id("evt"), id, str(command, "type"), key, sha256_hex(canonical_json(command)), json_text(command), json_text(result), stamp()});
        capacity_event_inserted(id, command);
    }
    void observe_capacity(const std::string& id) {
        const auto state = capacity_state(id);
        if ((!state.at("approaching").as_array().empty() || !state.at("exhausted").as_array().empty()) &&
            !state.at("recorded_warning").as_bool())
            capacity_raw_event(id, new_id("capacity"), Row{{"type", "capacity_warning"}, {"window", state.at("window")}}, state);
        if (!state.at("exhausted").as_array().empty() && !state.at("recorded_hold").as_bool()) {
            capacity_raw_event(id, new_id("capacity"), Row{{"type", "capacity_hold"}, {"window", state.at("window")}}, state);
        }
    }
    void guard_capacity_event(const std::string& id, const Row& command) const {
        if (capacity_continuation(command)) return;
        const auto state = capacity_state(id);
        // A business hold must not consume control slots or prevent asking for
        // help. New control admissions use only their own pool and reservations;
        // existing answers/cancellations/settlements take the continuation path.
        const bool control = capacity_control(command);
        require(control || !state.at("recorded_hold").as_bool(), ErrorCode::NotReady,
            "capacity WAITING_INPUT: settle pending work then Root capacity-checkpoint");
        const auto& counts = state.at("counts").as_object();
        const auto& limits = state.at("limits").as_object();
        // PREPARED dispatches and already-reserved call settlements have space
        // before new side effects are admitted. Continuations never lose their
        // safety gates, and physical storage failure still fails closed.
        const auto ordinary = control ? "control_events" : "business_events";
        require(number(counts, ordinary) + 1 <= number(limits, ordinary) &&
            (control || number(counts, "business_events") + number(state, "reserved_dispatch") + 1 <= number(limits, "business_events")) &&
            number(counts, "control_events") + number(state, "reserved_control") + 1 <= number(limits, "control_events"),
            ErrorCode::NotReady, "capacity reserve unavailable; Root capacity-checkpoint is required");
    }
    void capacity_available(const std::string& id, const std::string& dimension, const std::string& task_id = {}) const {
        const auto state = capacity_state(id);
        auto used = dimension == "open_questions" ? number(state, "open_questions") :
            number(state.at("counts").as_object(), dimension == "adverse_acceptances" ? "acceptances" : dimension);
        if (dimension == "submissions_per_task") {
            const auto current = number(one("SELECT COUNT(*) AS n FROM workflow_submissions WHERE task_id=?", {task_id}), "n");
            const auto history = capacity_events(id);
            const auto* old = history.window ? history.saved.at("task_submissions").as_object().if_contains(task_id) : nullptr;
            used = current - (old ? old->as_int64() : 0);
        }
        require(used < number(state.at("limits").as_object(), dimension), ErrorCode::NotReady,
            "capacity WAITING_INPUT: " + dimension + "; settle/answer current work then Root capacity-checkpoint");
    }
    Row checkpoint_intent(const StoreActor& root, std::int64_t expected, const std::string& evidence) const {
        text_field(evidence, "capacity checkpoint evidence", 4096);
        require(evidence.find_first_not_of(" \t\r\n") != std::string::npos && expected >= 0,
            ErrorCode::InvalidInput, "capacity checkpoint needs nonblank evidence and a window CAS");
        const Row command{{"type", "capacity_checkpoint"}, {"root_id", root.principal_id},
            {"request_id", root.request_id}, {"expected_window", expected}, {"evidence", evidence}};
        (void)canonical_json(command); return command;
    }
    Row checkpoint_state(const std::string& id, std::int64_t expected) const {
        const auto state = capacity_state(id);
        require(number(state, "window") == expected, ErrorCode::Conflict, "capacity window CAS mismatch");
        require(state.at("status") == "WAITING_INPUT" || !state.at("approaching").as_array().empty(),
            ErrorCode::NotReady, "capacity checkpoint requires a warning or hold");
        require_settled(id); return state;
    }
