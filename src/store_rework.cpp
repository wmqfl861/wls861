#include "store_impl.hpp"

#include <algorithm>
#include <set>

namespace wls::s1 {
using namespace store_detail;

Json Store::Impl::rejected_info(const std::string& task_id) const {
    require_acceptance_schema();
    const auto task = task_row(task_id);
    const auto saved = optional("SELECT submission_id FROM workflow_submissions WHERE task_id=? AND status='BLOCKED'", {task_id});
    if (str(task, "status") != "BLOCKED" || !saved) return nullptr;
    auto result = submission_info(str(*saved, "submission_id"));
    const auto id = str(result.as_object(), "submission_id");
    const auto request = request_row(str(task, "request_id"));
    const auto config = parse_json_strict(str(request, "config_snapshot"), 8 * 1024 * 1024);
    const auto limit = number(config.at("system").at("limits").as_object(), "team_internal_rework_max");
    const auto used = number(result.as_object(), "team_rework_count");
    require(limit >= 0 && limit <= 10000 && used >= 0, ErrorCode::Corrupt, "invalid frozen shared rework allowance");
    result.as_object()["rework_limit"] = limit;
    result.as_object()["rework_remaining"] = std::max<std::int64_t>(0, limit - used);
    Json cause = nullptr;
    if (const auto rejected = optional("SELECT * FROM candidate_acceptances WHERE submission_id=? AND status='REJECTED' ORDER BY rowid DESC LIMIT 1", {id})) {
        require(next(number(*rejected, "submission_version")) == number(result.as_object(), "state_version"),
                ErrorCode::Corrupt, "rejection source does not match blocked candidate version");
        cause = Row{{"kind", "business_rejection"}, {"record", acceptance_info(*rejected)}};
    } else {
        const auto& checks = result.at("checks").as_array();
        require(!checks.empty() && checks.back().at("status") == "BLOCKED" &&
            !checks.back().at("check_body").at("passed").as_bool() &&
            checks.back().at("submission_version") == result.at("state_version"),
            ErrorCode::Corrupt, "blocked submission lacks its original failed check");
        cause = Row{{"kind", "team_rework_exhausted"}, {"record", checks.back()}};
    }
    result.as_object()["rejection_cause"] = std::move(cause);
    result.as_object()["escalation"] = nullptr;
    if (const auto prior = optional("SELECT result FROM events WHERE request_id=? AND type='escalate_task' AND json_extract(payload,'$.submission_id')=? ORDER BY seq DESC LIMIT 1", {str(request, "request_id"), id})) {
        const auto fact = parse_json_strict(str(*prior, "result"));
        auto question = question_info(str(fact.as_object(), "question_id"));
        require(fact.at("submission_id") == result.at("submission_id") &&
            fact.at("source_record_sha256") == result.at("record_sha256"), ErrorCode::Corrupt, "escalation source fingerprint changed");
        require(question.at("task_id") == result.at("task_id") && question.at("team_id") == result.at("team_id"),
                ErrorCode::Corrupt, "escalation source scope changed");
        result.as_object()["escalation"] = std::move(question);
    }
    return result;
}

Json Store::Impl::rejection_subject(const StoreActor& actor, const ExecutionBinding& captured,
                       std::int64_t task_version, const std::string& submission_id,
                       std::int64_t submission_version) const {
    const auto source = submission_row(submission_id);
    workflow_subject(actor, captured, source, true);
    const auto task_id = str(source, "task_id");
    const auto task = task_row(task_id); const auto request = request_row(actor.request_id);
    require(number(task, "state_version") == task_version && number(source, "state_version") == submission_version,
            ErrorCode::Conflict, "rejected task or submission CAS changed");
    require(str(task, "status") == "BLOCKED" && str(source, "status") == "BLOCKED" && str(task, "kind") == "team_task",
            ErrorCode::NotReady, "only a current blocked team submission has a rejection successor");
    require(number(task, "based_on_requirements_revision") == number(request, "requirements_revision") &&
            number(task, "plan_revision") == number(request, "plan_revision"), ErrorCode::Stale, "rejection successor requires current AMEND disposition");
    const auto team = team_row(str(source, "team_id"));
    require(str(team, "status") == "ACTIVE", ErrorCode::NotReady, "a stopped producer cannot be revived by corrective rework");
    return rejected_info(task_id);
}

// R2.1 corrective revisions and user escalation. Original submission/check/
// acceptance bytes stay immutable. Reuse existing tables, no asset migration.
Json Store::rejected_submission(const std::string& task_id) const {
    const std::lock_guard lock(impl_->mutex);
    return impl_->rejected_info(task_id);
}

Json Store::change_task(const StoreActor& actor, const ExecutionBinding& captured,
        std::int64_t expected_task, const std::string& id, std::int64_t expected_submission,
        const Json& correction, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    require(expected_task > 0 && expected_submission > 0, ErrorCode::InvalidInput, "correction requires captured object versions");
    object_fields(correction, {"rationale", "correction"}, {"rationale", "correction"});
    (void)workflow_text(correction, "rationale", 8000);
    (void)workflow_text(correction, "correction", 8000);
    const auto command = payload(actor, "change_task", Row{{"binding", to_json(captured)},
        {"expected_task_version", expected_task}, {"submission_id", id},
        {"expected_submission_version", expected_submission}, {"correction", correction}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false); impl_->actor_binding(actor, captured);
    require(str(impl_->submission_row(id), "request_id") == actor.request_id, ErrorCode::Unauthorized,
            "rejection belongs to another Dispatcher");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto rejected = impl_->rejection_subject(actor, captured, expected_task, id, expected_submission);
    const auto task_id = str(rejected.as_object(), "task_id"), team_id = str(rejected.as_object(), "team_id");
    require(number(rejected.as_object(), "rework_remaining") > 0, ErrorCode::NotReady,
            "frozen shared rework allowance exhausted; escalate to user, do not reset the counter");
    require(!impl_->has_blocking_question(task_id), ErrorCode::NotReady, "answer the owning question before corrective revision");
    const auto wait = impl_->wait_info(task_id);
    require(wait.is_null() || wait.at("status") != "WAITING", ErrorCode::NotReady,
            "corrective revision cannot supersede an independent wait");
    impl_->require_settled(actor.request_id, team_id);
    const auto task = impl_->task_row(task_id);
    (void)next(number(task, "task_revision"));
    (void)next(expected_task); (void)next(expected_submission);
    impl_->cas("UPDATE tasks SET task_revision=task_revision+1,status='READY',accepted_with_issues=0,applied_task_revision=0,applied_plan_revision=0,applied_instruction_revision=0,state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?",
        {stamp(), task_id, expected_task});
    impl_->execute("UPDATE teams SET paused_for_change=1,state_version=state_version+1,updated_at=? WHERE team_id=?", {stamp(), team_id});
    impl_->execute("INSERT OR IGNORE INTO team_workflow_state(team_id,rework_count) VALUES (?,0)", {team_id});
    impl_->execute("UPDATE team_workflow_state SET rework_count=rework_count+1 WHERE team_id=?", {team_id});
    impl_->cas("UPDATE workflow_submissions SET status='RETURNED_FOR_REWORK',state_version=state_version+1,updated_at=? WHERE submission_id=? AND state_version=?",
        {stamp(), id, expected_submission});
    impl_->supersede_waits(actor.request_id, task_id);
    const Row result{{"task_id", task_id}, {"team_id", team_id}, {"submission_id", id},
        {"status", "RETURNED_FOR_REWORK"}, {"source_record_sha256", rejected.at("record_sha256")},
        {"rejection_cause", rejected.at("rejection_cause")}, {"correction", correction},
        {"original_binding", rejected.at("original_binding")}, {"target_binding", to_json(impl_->binding(task_id))},
        {"task_version", expected_task + 1}, {"team_rework_count", number(rejected.as_object(), "team_rework_count") + 1},
        {"rework_limit", rejected.at("rework_limit")}, {"instruction_pending", true}};
    // The original TaskBrief and old instruction stay unchanged. Only a new
    // TeamLead order will reset per-order tool allowance and enable a fresh ack.
    require(impl_->task_row(task_id).at("brief") == task.at("brief"), ErrorCode::Corrupt, "corrective revision changed task obligations");
    impl_->event(actor.request_id, command_id, command, result, "dispatcher"); tx.commit(); return result;
}

Json Store::escalate_task(const StoreActor& actor, const ExecutionBinding& captured,
        std::int64_t expected_task, const std::string& id, std::int64_t expected_submission,
        const Json& body, const std::string& command_id) {
    const std::lock_guard lock(impl_->mutex); impl_->require_acceptance_schema();
    require(expected_task > 0 && expected_submission > 0, ErrorCode::InvalidInput, "escalation requires captured object versions");
    object_fields(body, {"text", "context", "options"}, {"text"});
    (void)workflow_text(body, "text", 16000); (void)workflow_text(body, "context", 16000, true);
    if (const auto* options = body.as_object().if_contains("options")) {
        require(options->is_array() && options->as_array().size() <= 8, ErrorCode::InvalidInput, "question options exceed capacity");
        std::set<std::string> option_ids;
        for (const auto& option : options->as_array()) {
            object_fields(option, {"id", "label"}, {"id", "label"});
            const auto option_id = workflow_text(option, "id", 16); (void)workflow_text(option, "label", 800);
            require(std::all_of(option_id.begin(), option_id.end(), [](char c) {
                return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_';
            }) && option_ids.insert(option_id).second, ErrorCode::InvalidInput, "question option identifiers must be unique");
        }
    }
    const auto command = payload(actor, "escalate_task", Row{{"binding", to_json(captured)},
        {"expected_task_version", expected_task}, {"submission_id", id},
        {"expected_submission_version", expected_submission}, {"body", body}});
    Impl::Transaction tx(*impl_);
    (void)impl_->authorize(actor, {Role::Dispatcher}, false); impl_->actor_binding(actor, captured);
    require(str(impl_->submission_row(id), "request_id") == actor.request_id, ErrorCode::Unauthorized,
            "rejection belongs to another Dispatcher");
    if (auto old = impl_->replay(actor.request_id, command_id, command)) { tx.commit(); return *old; }
    const auto rejected = impl_->rejection_subject(actor, captured, expected_task, id, expected_submission);
    const auto task_id = str(rejected.as_object(), "task_id"), team_id = str(rejected.as_object(), "team_id");
    require(rejected.at("escalation").is_null(), ErrorCode::NotReady, "this rejected submission already has its user escalation");
    const auto wait = impl_->wait_info(task_id);
    require((wait.is_null() || wait.at("status") != "WAITING") && !impl_->has_blocking_question(task_id),
            ErrorCode::NotReady, "another wait already owns this rejected task");
    impl_->capacity_available(actor.request_id, "questions");
    impl_->capacity_available(actor.request_id, "open_questions");
    const auto generation = wait.is_null() ? 1 : next(number(wait.as_object(), "generation"));
    (void)next(expected_task);
    const auto binding = impl_->binding(task_id); const auto question_id = new_id("q");
    auto complete = body.as_object(); complete["question_id"] = question_id;
    complete["task_id"] = task_id; complete["asked_by"] = "dispatcher"; complete["blocking"] = true;
    const Row original{{"body", complete}, {"original_binding", to_json(binding)}};
    impl_->cas("UPDATE tasks SET state_version=state_version+1,updated_at=? WHERE task_id=? AND state_version=?", {stamp(), task_id, expected_task});
    impl_->execute("INSERT INTO questions(question_id,request_id,task_id,team_id,asked_by,holder,body,created_at) VALUES (?,?,?,?,'dispatcher','user',?,?)",
        {question_id, actor.request_id, task_id, team_id, json_text(complete, 65536), stamp()});
    impl_->execute("INSERT INTO question_runtime VALUES (?,?,?,'OPEN',1,?)", {question_id, canonical_json(to_json(binding)), sha256_hex(canonical_json(original)), generation});
    impl_->execute("INSERT INTO worker_waits(task_id,request_id,team_id,generation,status,original_binding,reason,created_at,updated_at) VALUES (?,?,?,?,'WAITING',?,?,?,?) ON CONFLICT(task_id) DO UPDATE SET request_id=excluded.request_id,team_id=excluded.team_id,generation=excluded.generation,status=excluded.status,original_binding=excluded.original_binding,reason=excluded.reason,created_at=excluded.created_at,updated_at=excluded.updated_at",
        {task_id, actor.request_id, team_id, generation, canonical_json(to_json(binding)), "question:" + question_id, stamp(), stamp()});
    auto result = impl_->question_info(question_id);
    result.as_object()["submission_id"] = id;
    result.as_object()["source_record_sha256"] = rejected.at("record_sha256");
    result.as_object()["rejection_cause"] = rejected.at("rejection_cause");
    result.as_object()["team_rework_count"] = rejected.at("team_rework_count");
    result.as_object()["rework_limit"] = rejected.at("rework_limit");
    impl_->event(actor.request_id, command_id, command, result, "dispatcher"); tx.commit(); return result;
}

} // namespace wls::s1
