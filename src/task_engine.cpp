#include "wls/task_engine.hpp"
#include "wls/journal.hpp"

#include <algorithm>
#include <functional>
#include <limits>
#include <mutex>
#include <sstream>
#include <utility>

namespace wls {
namespace {
constexpr std::size_t max_text = 16384;
constexpr std::size_t max_id = 128;
constexpr std::size_t max_reference = 512;
constexpr std::uint64_t max_items = 1000000;

[[noreturn]] void fail(ErrorCode code, const std::string& text) { throw KernelError(code, text); }
void require(bool value, ErrorCode code, const std::string& text) { if (!value) fail(code, text); }
void text_field(const std::string& value, const std::string& name, std::size_t maximum = max_text) {
    require(!value.empty() && value.size() <= maximum && value.find('\0') == std::string::npos,
            ErrorCode::InvalidInput, "invalid " + name);
}
void identifier(const std::string& value, const std::string& name) {
    text_field(value, name, max_id);
    require(std::all_of(value.begin(), value.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == ':';
    }), ErrorCode::InvalidInput, "invalid identifier " + name);
}
Revision increment(Revision value) {
    require(value < std::numeric_limits<Revision>::max(), ErrorCode::InvalidState, "revision exhausted");
    return value + 1;
}
bool terminal(RequestState state) {
    return state == RequestState::Completed || state == RequestState::Cancelled ||
           state == RequestState::Superseded || state == RequestState::Failed;
}
bool unsettled(OperationState state) {
    return state == OperationState::Prepared || state == OperationState::Running || state == OperationState::Unknown;
}
bool control_event(const std::string& kind) {
    return kind == "RuntimeRestarted" || kind == "DispatcherRecovered" ||
        kind == "OperationFinished" || kind == "OperationReconciled" ||
        kind == "RequestCancelled" || kind == "ResourceStateChanged" ||
        kind == "AcceptanceRevoked" ||
        kind == "CandidateAccepted" || kind == "HandoffPrepared" || kind == "DeliveryConfirmed";
}
bool hash_valid(const std::string& hash) {
    return hash.size() == 64 && std::all_of(hash.begin(), hash.end(), [](unsigned char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

// Length-prefixed byte strings avoid delimiter ambiguity in command fingerprints
// and durable records. These are exact content comparisons, not security hashes.
struct Writer {
    std::string bytes;
    void number(std::uint64_t n) {
        for (int shift = 56; shift >= 0; shift -= 8) bytes.push_back(static_cast<char>((n >> shift) & 255));
    }
    void string(const std::string& text) { number(text.size()); bytes += text; }
    template<class Enum> void enumeration(Enum value) { number(static_cast<std::uint64_t>(value)); }
    void boolean(bool value) { number(value ? 1 : 0); }
};
struct Reader {
    const std::string& bytes;
    std::size_t offset{};
    std::uint64_t number() {
        require(bytes.size() - offset >= 8, ErrorCode::Persistence, "truncated task record");
        std::uint64_t result = 0;
        for (int i = 0; i < 8; ++i) result = (result << 8) | static_cast<unsigned char>(bytes[offset++]);
        return result;
    }
    std::string string() {
        auto size = number();
        require(size <= bytes.size() - offset, ErrorCode::Persistence, "invalid string length");
        auto result = bytes.substr(offset, static_cast<std::size_t>(size));
        offset += static_cast<std::size_t>(size);
        return result;
    }
    std::uint64_t count() {
        auto n = number();
        require(n <= max_items && n <= (bytes.size() - offset) / 8,
                ErrorCode::Persistence, "invalid collection length");
        return n;
    }
    template<class Enum> Enum enumeration(Enum maximum) {
        auto n = number();
        require(n <= static_cast<std::uint64_t>(maximum), ErrorCode::Persistence, "unknown enum value");
        return static_cast<Enum>(n);
    }
    bool boolean() { auto n = number(); require(n <= 1, ErrorCode::Persistence, "invalid boolean"); return n == 1; }
};

void write_input(Writer& w, const RequestInput& input) {
    for (const auto* value : {&input.tenant_id, &input.project_id, &input.root_id,
                              &input.idempotency_key, &input.goal, &input.root_defaults.model}) w.string(*value);
    w.enumeration(input.root_defaults.reasoning.mode); w.string(input.root_defaults.reasoning.effort);
    w.number(input.allowed_tools.size());
    for (const auto& tool : input.allowed_tools) w.string(tool);
}
RequestInput read_input(Reader& r) {
    RequestInput input;
    for (auto* value : {&input.tenant_id, &input.project_id, &input.root_id,
                        &input.idempotency_key, &input.goal, &input.root_defaults.model}) *value = r.string();
    input.root_defaults.reasoning.mode = r.enumeration(ReasoningMode::Explicit);
    input.root_defaults.reasoning.effort = r.string();
    auto n = r.count();
    for (std::uint64_t i = 0; i < n; ++i) {
        require(input.allowed_tools.insert(r.string()).second, ErrorCode::Persistence, "duplicate tool");
    }
    return input;
}
std::string admission_key(const RequestInput& input) {
    Writer w;
    for (const auto* value : {&input.tenant_id, &input.project_id, &input.root_id, &input.idempotency_key}) w.string(*value);
    return w.bytes;
}
void write_candidate(Writer& w, const Candidate& c) {
    w.string(c.artifact_id); w.number(c.artifact_version); w.string(c.content_hash);
    w.string(c.producer_team_id); w.number(c.requirements_revision);
}
Candidate read_candidate(Reader& r) {
    Candidate c;
    c.artifact_id = r.string(); c.artifact_version = r.number(); c.content_hash = r.string();
    c.producer_team_id = r.string(); c.requirements_revision = r.number();
    return c;
}
void write_request(Writer& w, const RequestSnapshot& q) {
    w.string(q.request_id); w.string(q.task_id); w.string(q.dispatcher_id); write_input(w, q.original_input);
    w.string(q.goal); w.number(q.requirements_revision); w.number(q.plan_revision); w.number(q.plan_requirements_revision);
    w.number(q.state_version); w.number(q.lease_epoch); w.enumeration(q.state); w.enumeration(q.resources);
    w.string(q.plan); w.string(q.replaces); w.string(q.replaced_by);
    w.number(q.teams.size());
    for (const auto& [id, t] : q.teams) {
        w.string(id); w.string(t.task_id); w.string(t.lead_id); w.string(t.worker_id);
        w.boolean(t.review_team); w.number(t.lease_epoch); w.number(t.applied_revision);
        w.number(t.applied_plan_revision); w.number(t.instructions_revision);
        w.number(t.instructions_plan_revision); w.number(t.instructions_version);
        w.number(t.applied_instructions_version); w.string(t.instructions);
    }
    w.number(q.operations.size());
    for (const auto& [id, o] : q.operations) {
        w.string(id); w.string(o.team_id); w.string(o.idempotency_key); w.string(o.tool_id); w.string(o.payload);
        w.number(o.requirements_revision); w.number(o.plan_revision); w.number(o.lease_epoch);
        w.enumeration(o.state); w.string(o.evidence_ref);
    }
    w.boolean(q.candidate.has_value()); if (q.candidate) write_candidate(w, *q.candidate);
    w.number(q.artifact_versions.size());
    for (const auto& [id, version] : q.artifact_versions) { w.string(id); w.number(version); }
    w.number(q.reviews.size());
    for (const auto& review : q.reviews) {
        write_candidate(w, review.candidate); w.string(review.reviewer_team_id);
        w.boolean(review.passed); w.string(review.evidence_ref); w.number(review.instructions_version);
    }
    w.boolean(q.accepted); w.string(q.package_ref); w.string(q.receipt_ref);
    w.number(q.command_receipts.size());
    for (const auto& [key, value] : q.command_receipts) { w.string(key); w.string(value); }
}
RequestSnapshot read_request(Reader& r) {
    RequestSnapshot q;
    q.request_id = r.string(); q.task_id = r.string(); q.dispatcher_id = r.string(); q.original_input = read_input(r);
    q.goal = r.string(); q.requirements_revision = r.number(); q.plan_revision = r.number(); q.plan_requirements_revision = r.number();
    q.state_version = r.number(); q.lease_epoch = r.number(); q.state = r.enumeration(RequestState::Failed);
    q.resources = r.enumeration(ResourceState::CleanupFailed);
    q.plan = r.string(); q.replaces = r.string(); q.replaced_by = r.string();
    auto teams = r.count();
    for (std::uint64_t i = 0; i < teams; ++i) {
        TeamSnapshot t;
        t.team_id = r.string(); t.task_id = r.string(); t.lead_id = r.string(); t.worker_id = r.string();
        t.review_team = r.boolean(); t.lease_epoch = r.number(); t.applied_revision = r.number();
        t.applied_plan_revision = r.number(); t.instructions_revision = r.number();
        t.instructions_plan_revision = r.number(); t.instructions_version = r.number();
        t.applied_instructions_version = r.number(); t.instructions = r.string();
        require(q.teams.emplace(t.team_id, t).second, ErrorCode::Persistence, "duplicate team");
    }
    auto operations = r.count();
    for (std::uint64_t i = 0; i < operations; ++i) {
        OperationSnapshot o;
        o.operation_id = r.string(); o.team_id = r.string(); o.idempotency_key = r.string();
        o.tool_id = r.string(); o.payload = r.string(); o.requirements_revision = r.number();
        o.plan_revision = r.number(); o.lease_epoch = r.number(); o.state = r.enumeration(OperationState::Cancelled);
        o.evidence_ref = r.string();
        require(q.operations.emplace(o.operation_id, o).second, ErrorCode::Persistence, "duplicate operation");
    }
    if (r.boolean()) q.candidate = read_candidate(r);
    auto artifacts = r.count();
    for (std::uint64_t i = 0; i < artifacts; ++i) {
        auto id = r.string(); auto version = r.number();
        require(version > 0 && q.artifact_versions.emplace(std::move(id), version).second,
                ErrorCode::Persistence, "invalid artifact version history");
    }
    auto reviews = r.count();
    for (std::uint64_t i = 0; i < reviews; ++i) {
        ReviewRecord review;
        review.candidate = read_candidate(r); review.reviewer_team_id = r.string();
        review.passed = r.boolean(); review.evidence_ref = r.string(); review.instructions_version = r.number();
        q.reviews.push_back(std::move(review));
    }
    q.accepted = r.boolean(); q.package_ref = r.string(); q.receipt_ref = r.string();
    auto commands = r.count();
    for (std::uint64_t i = 0; i < commands; ++i) {
        auto key = r.string(); auto value = r.string();
        require(q.command_receipts.emplace(std::move(key), std::move(value)).second,
                ErrorCode::Persistence, "duplicate command receipt");
    }
    return q;
}
void write_event(Writer& w, const EventEnvelope& e) {
    w.number(e.sequence);
    for (const auto* v : {&e.event_id, &e.request_id, &e.task_id, &e.team_id,
                          &e.command_id, &e.actor_id, &e.kind}) w.string(*v);
    w.number(e.requirements_revision); w.number(e.plan_revision); w.number(e.state_version); w.number(e.lease_epoch);
    w.string(e.detail);
}
EventEnvelope read_event(Reader& r) {
    EventEnvelope e; e.sequence = r.number();
    for (auto* v : {&e.event_id, &e.request_id, &e.task_id, &e.team_id,
                    &e.command_id, &e.actor_id, &e.kind}) *v = r.string();
    e.requirements_revision = r.number(); e.plan_revision = r.number();
    e.state_version = r.number(); e.lease_epoch = r.number(); e.detail = r.string(); return e;
}
void validate_input(const RequestInput& input) {
    for (const auto* id : {&input.tenant_id, &input.project_id, &input.root_id, &input.idempotency_key}) identifier(*id, "request scope/key");
    text_field(input.goal, "goal"); text_field(input.root_defaults.model, "Root model", max_id);
    auto mode = input.root_defaults.reasoning.mode;
    require(mode == ReasoningMode::Disabled || mode == ReasoningMode::ProviderDefault || mode == ReasoningMode::Explicit,
            ErrorCode::InvalidInput, "Root reasoning must be explicit, disabled, or provider default");
    if (mode == ReasoningMode::Explicit) text_field(input.root_defaults.reasoning.effort, "reasoning effort", max_id);
    else require(input.root_defaults.reasoning.effort.empty(), ErrorCode::InvalidInput, "unexpected reasoning effort");
    require(input.allowed_tools.size() <= 128, ErrorCode::InvalidInput, "too many tool grants");
    for (const auto& tool : input.allowed_tools) identifier(tool, "tool grant");
}
void invalidate_acceptance(RequestSnapshot& q) { q.accepted = false; q.package_ref.clear(); q.receipt_ref.clear(); }
void fence_operations(RequestSnapshot& q) {
    for (auto& [id, operation] : q.operations) {
        (void)id;
        if (operation.state == OperationState::Prepared) operation.state = OperationState::Cancelled;
        else if (operation.state == OperationState::Running) operation.state = OperationState::Unknown;
    }
}
} // namespace

std::string to_string(RequestState state) {
    switch (state) {
    case RequestState::Queued: return "QUEUED"; case RequestState::Running: return "RUNNING";
    case RequestState::Adjusting: return "ADJUSTING"; case RequestState::Submitted: return "SUBMITTED";
    case RequestState::Verifying: return "VERIFYING"; case RequestState::Accepted: return "ACCEPTED";
    case RequestState::HandoffPending: return "HANDOFF_PENDING"; case RequestState::Completed: return "COMPLETED";
    case RequestState::Cancelled: return "CANCELLED"; case RequestState::Superseded: return "SUPERSEDED";
    case RequestState::Failed: return "FAILED";
    }
    return "INVALID";
}
std::string to_string(ResourceState state) {
    switch (state) {
    case ResourceState::Starting: return "STARTING"; case ResourceState::Active: return "ACTIVE";
    case ResourceState::Draining: return "DRAINING"; case ResourceState::Stopped: return "STOPPED";
    case ResourceState::Cleaning: return "CLEANING"; case ResourceState::Closed: return "CLOSED";
    case ResourceState::CleanupFailed: return "CLEANUP_FAILED";
    }
    return "INVALID";
}
std::string to_string(OperationState state) {
    switch (state) {
    case OperationState::Prepared: return "PREPARED"; case OperationState::Running: return "RUNNING";
    case OperationState::Succeeded: return "SUCCEEDED"; case OperationState::Failed: return "FAILED";
    case OperationState::Unknown: return "UNKNOWN"; case OperationState::Cancelled: return "CANCELLED";
    }
    return "INVALID";
}

struct TaskEngine::Impl {
    mutable std::mutex mutex;
    DurableJournal journal;
    EngineLimits limits;
    ToolSet system_tools;
    std::map<std::string, RequestSnapshot> records;
    std::map<std::string, std::string> admissions;
    std::vector<EventEnvelope> log;
    std::size_t ordinary_events{};
    std::size_t diagnostic_events{};
    bool poisoned{false};

    Impl(const std::filesystem::path& path, EngineLimits configured, ToolSet tools)
        : journal(path), limits(configured), system_tools(std::move(tools)) {
        require(limits.max_requests > 0 && limits.max_requests <= 10000 &&
                limits.max_teams_per_request > 0 && limits.max_teams_per_request <= 1024 &&
                limits.max_operations_per_request > 0 && limits.max_operations_per_request <= 1024 &&
                limits.max_events > 0 && limits.max_events <= max_items &&
                limits.max_transaction_bytes >= 128U * 1024U &&
                limits.max_transaction_bytes <= DurableJournal::max_transaction_bytes,
                ErrorCode::InvalidInput, "limits must be finite positive M1 bounds");
        for (const auto& tool : system_tools) identifier(tool, "system tool");
        for (const auto& frame : journal.replay()) {
            Reader r{frame};
            require(r.string() == "WLS-TASK-TX-1", ErrorCode::Persistence, "unsupported task record schema");
            auto count = r.count();
            require(count <= 2, ErrorCode::Persistence, "too many request updates");
            for (std::uint64_t i = 0; i < count; ++i) {
                auto q = read_request(r);
                validate_input(q.original_input);
                auto old = records.find(q.request_id);
                require(q.state_version == (old == records.end() ? 1 : increment(old->second.state_version)),
                        ErrorCode::Persistence, "non-monotonic request version");
                if (old == records.end()) {
                    require(q.request_id == "request-" + std::to_string(records.size() + 1),
                            ErrorCode::Persistence, "unexpected request identity");
                } else require(q.original_input == old->second.original_input,
                               ErrorCode::Persistence, "immutable request input changed");
                require(q.requirements_revision > 0 && q.lease_epoch > 0 &&
                        q.teams.size() <= limits.max_teams_per_request &&
                        q.operations.size() <= limits.max_operations_per_request,
                        ErrorCode::Persistence, "request violates configured limits");
                records.insert_or_assign(q.request_id, std::move(q));
            }
            auto event_count = r.count();
            require(event_count > 0 && event_count <= 2, ErrorCode::Persistence, "invalid event count");
            for (std::uint64_t i = 0; i < event_count; ++i) {
                auto event = read_event(r);
                require(event.sequence == log.size() + 1 && event.event_id == "event-" + std::to_string(event.sequence),
                        ErrorCode::Persistence, "event sequence gap");
                auto found = records.find(event.request_id);
                require(found != records.end() && found->second.state_version == event.state_version &&
                        found->second.lease_epoch == event.lease_epoch &&
                        found->second.requirements_revision == event.requirements_revision &&
                        found->second.plan_revision == event.plan_revision,
                        ErrorCode::Persistence, "event/state mismatch");
                if (event.kind == "CommandRejected") ++diagnostic_events;
                else if (!control_event(event.kind)) ++ordinary_events;
                log.push_back(std::move(event));
            }
            require(r.offset == frame.size(), ErrorCode::Persistence, "trailing task record bytes");
            require(records.size() <= limits.max_requests && ordinary_events <= limits.max_events,
                    ErrorCode::Persistence, "restored state exceeds configured limits");
        }
        for (const auto& [id, q] : records)
            require(admissions.emplace(admission_key(q.original_input), id).second,
                    ErrorCode::Persistence, "duplicate admission identity");
        // Reopening a journal is a new runtime incarnation. Fence before this
        // object is exposed; callers cannot accidentally reuse old model output
        // just because they forgot to invoke an optional recovery method.
        std::vector<std::string> active_ids;
        for (const auto& [id, q] : records) if (!terminal(q.state)) active_ids.push_back(id);
        for (const auto& id : active_ids) {
            auto next = records.at(id);
            auto cmd = host_command(next, next.state_version,
                                    "runtime-restart-" + std::to_string(next.lease_epoch));
            next.lease_epoch = increment(next.lease_epoch); next.state_version = increment(next.state_version);
            fence_operations(next);
            for (auto& [team_id, team] : next.teams) { (void)team_id; team.applied_instructions_version = 0; }
            if (next.state != RequestState::Accepted && next.state != RequestState::HandoffPending)
                next.state = next.plan_revision ? RequestState::Adjusting : RequestState::Queued;
            commit({next}, {event(next, cmd, "RuntimeRestarted")});
        }
    }

    void healthy() const { require(!poisoned, ErrorCode::Persistence, "runtime requires restart after persistence failure"); }
    const RequestSnapshot& find(const std::string& id) const {
        healthy(); auto entry = records.find(id);
        require(entry != records.end(), ErrorCode::NotFound, "request not found"); return entry->second;
    }
    void authorize(const RequestSnapshot& q, const Actor& actor, Role role) const {
        require(actor.role == role && actor.root_id == q.original_input.root_id && actor.request_id == q.request_id,
                ErrorCode::Unauthorized, "actor role/request/Root mismatch");
        if (role == Role::Root) require(actor.principal_id == q.original_input.root_id && actor.team_id.empty(),
                                       ErrorCode::Unauthorized, "invalid Root identity");
        else if (role == Role::Dispatcher) require(actor.principal_id == q.dispatcher_id && actor.team_id.empty(),
                                                 ErrorCode::Unauthorized, "invalid Dispatcher identity");
        else {
            auto team = q.teams.find(actor.team_id);
            require(team != q.teams.end(), ErrorCode::Unauthorized, "actor team not owned by request");
            require(actor.principal_id == (role == Role::TeamLead ? team->second.lead_id : team->second.worker_id),
                    ErrorCode::Unauthorized, "actor principal mismatch");
        }
    }
    std::string signature(const Command& cmd, const std::string& kind, const std::string& body) const {
        Writer w; w.string(kind); w.enumeration(cmd.actor.role);
        for (const auto* value : {&cmd.actor.root_id, &cmd.actor.request_id, &cmd.actor.team_id,
                                  &cmd.actor.principal_id}) w.string(*value);
        w.number(cmd.expected_state_version); w.number(cmd.requirements_revision); w.number(cmd.lease_epoch);
        w.string(body); return w.bytes;
    }
    EventEnvelope event(const RequestSnapshot& q, const Command& cmd,
                        const std::string& kind, const std::string& detail = "") const {
        EventEnvelope e; e.request_id = q.request_id; e.task_id = q.task_id; e.team_id = cmd.actor.team_id;
        if (!e.team_id.empty() && q.teams.contains(e.team_id)) e.task_id = q.teams.at(e.team_id).task_id;
        e.command_id = cmd.command_id; e.actor_id = cmd.actor.principal_id; e.kind = kind;
        e.requirements_revision = q.requirements_revision; e.plan_revision = q.plan_revision;
        e.state_version = q.state_version; e.lease_epoch = q.lease_epoch; e.detail = detail; return e;
    }
    void commit(std::vector<RequestSnapshot> updates, std::vector<EventEnvelope> events) {
        healthy();
        const auto ordinary = static_cast<std::size_t>(std::count_if(events.begin(), events.end(), [](const auto& e) {
            return e.kind != "CommandRejected" && !control_event(e.kind);
        }));
        require(ordinary_events + ordinary <= limits.max_events, ErrorCode::QuotaExceeded, "ordinary event budget reached");
        Writer w; w.string("WLS-TASK-TX-1"); w.number(updates.size());
        for (const auto& q : updates) write_request(w, q);
        w.number(events.size());
        for (std::size_t i = 0; i < events.size(); ++i) {
            events[i].sequence = log.size() + i + 1;
            events[i].event_id = "event-" + std::to_string(events[i].sequence); write_event(w, events[i]);
        }
        require(w.bytes.size() <= limits.max_transaction_bytes, ErrorCode::QuotaExceeded,
                "transaction exceeds M1 journal frame capacity");
        require(ordinary == 0 || w.bytes.size() <= limits.max_transaction_bytes / 2,
                ErrorCode::QuotaExceeded, "ordinary transaction exceeds M1 settlement reserve");
        if (ordinary != 0) {
            std::size_t settlement_reserve = 0;
            for (const auto& q : updates) {
                const auto pending = static_cast<std::size_t>(std::count_if(q.operations.begin(), q.operations.end(), [](const auto& item) {
                    return unsettled(item.second.state);
                }));
                // A bounded 512-byte evidence reference plus command receipt
                // fits comfortably within 8 KiB per in-flight operation for
                // finish(UNKNOWN) + reconciliation. Keep 64 KiB per request
                // for acceptance/receipt/cancellation and normal close steps.
                settlement_reserve += 64U * 1024U + pending * 8192U;
            }
            require(settlement_reserve <= limits.max_transaction_bytes &&
                    w.bytes.size() <= limits.max_transaction_bytes - settlement_reserve,
                    ErrorCode::QuotaExceeded, "insufficient frame space to settle admitted operations");
        }
        try {
            journal.append(w.bytes);
            for (auto& q : updates) {
                admissions.insert_or_assign(admission_key(q.original_input), q.request_id);
                records.insert_or_assign(q.request_id, std::move(q));
            }
            for (auto& e : events) {
                if (e.kind == "CommandRejected") ++diagnostic_events;
                else if (!control_event(e.kind)) ++ordinary_events;
                log.push_back(std::move(e));
            }
        } catch (const std::exception& error) {
            poisoned = true; fail(ErrorCode::Persistence, std::string("durable commit failed: ") + error.what());
        }
    }
    bool precheck(const RequestSnapshot& q, const Command& cmd, const std::string& fingerprint,
                  bool epoch_required = true) {
        identifier(cmd.command_id, "command id");
        auto receipt = q.command_receipts.find(cmd.command_id);
        if (receipt != q.command_receipts.end()) {
            require(receipt->second == fingerprint, ErrorCode::Conflict, "command id reused with different content");
            return false;
        }
        auto reject = [&](ErrorCode code, const std::string& message) {
            if (diagnostic_events < limits.max_events) commit({}, {event(q, cmd, "CommandRejected", message)});
            fail(code, message);
        };
        if (epoch_required && cmd.lease_epoch != q.lease_epoch) reject(ErrorCode::StaleEpoch, "stale lease epoch");
        if (cmd.requirements_revision != q.requirements_revision) reject(ErrorCode::StaleRequirements, "stale requirements revision");
        if (cmd.expected_state_version != q.state_version) reject(ErrorCode::Conflict, "state version conflict");
        return true;
    }
    RequestSnapshot mutate(const std::string& id, const Command& cmd, Role role,
            const std::string& kind, const std::string& body,
            const std::function<void(RequestSnapshot&)>& change) {
        std::lock_guard lock(mutex);
        const auto& current = find(id); authorize(current, cmd.actor, role);
        auto fingerprint = signature(cmd, kind, body);
        if (!precheck(current, cmd, fingerprint)) return current;
        auto next = current;
        require(!terminal(next.state), ErrorCode::InvalidState, "request is terminal");
        change(next);
        next.state_version = increment(next.state_version);
        next.command_receipts.emplace(cmd.command_id, fingerprint);
        auto e = event(next, cmd, kind);
        if (kind == "ReviewRecorded" && current.accepted && !next.reviews.empty() && !next.reviews.back().passed)
            e.kind = "AcceptanceRevoked";
        commit({next}, {e}); return next;
    }
    RequestSnapshot fresh(const RequestInput& input) const {
        RequestSnapshot q; q.request_id = "request-" + std::to_string(records.size() + 1);
        q.task_id = q.request_id + ":task"; q.dispatcher_id = q.request_id + ":dispatcher";
        q.original_input = input; q.goal = input.goal; return q;
    }
    void active(const RequestSnapshot& q) const {
        require(q.resources == ResourceState::Active, ErrorCode::InvalidState, "resources are not active");
        require(q.state == RequestState::Running || q.state == RequestState::Adjusting ||
                q.state == RequestState::Submitted || q.state == RequestState::Verifying,
                ErrorCode::InvalidState, "request is not executable");
    }
    TeamSnapshot& ready_team(RequestSnapshot& q, const Command& cmd) const {
        active(q); auto& team = q.teams.at(cmd.actor.team_id);
        require(team.lease_epoch == q.lease_epoch, ErrorCode::StaleEpoch, "team requires new instructions after recovery");
        require(team.applied_revision == q.requirements_revision && team.applied_plan_revision == q.plan_revision &&
                q.plan_revision > 0 && q.plan_requirements_revision == q.requirements_revision &&
                team.applied_instructions_version == team.instructions_version && team.instructions_version > 0,
                ErrorCode::StaleRequirements, "Worker has not applied current requirements/plan");
        return team;
    }
    void no_unknown(const RequestSnapshot& q, const std::string& team) const {
        require(std::none_of(q.operations.begin(), q.operations.end(), [&](const auto& entry) {
            return entry.second.team_id == team && entry.second.state == OperationState::Unknown;
        }), ErrorCode::UnknownOutcome, "unknown operation outcome requires runtime reconciliation");
    }
    void no_pending(const RequestSnapshot& q) const {
        require(std::none_of(q.operations.begin(), q.operations.end(), [](const auto& entry) {
            return unsettled(entry.second.state);
        }), ErrorCode::UnknownOutcome, "pending/unknown operations require reconciliation");
    }
    Command host_command(const RequestSnapshot& q, Revision expected, const std::string& id) const {
        return {{Role::Root, q.original_input.root_id, q.request_id, "", "runtime"}, id,
                expected, q.requirements_revision, q.lease_epoch};
    }
};

TaskEngine::TaskEngine(const std::filesystem::path& path, EngineLimits limits, ToolSet tools)
    : impl_(std::make_unique<Impl>(path, limits, std::move(tools))) {}
TaskEngine::~TaskEngine() = default;

RequestSnapshot TaskEngine::submit(const Actor& root, const RequestInput& input) {
    validate_input(input);
    require(root.role == Role::Root && root.root_id == input.root_id && root.principal_id == input.root_id &&
            root.request_id.empty() && root.team_id.empty(), ErrorCode::Unauthorized, "invalid admission Root");
    std::lock_guard lock(impl_->mutex); impl_->healthy();
    auto found = impl_->admissions.find(admission_key(input));
    if (found != impl_->admissions.end()) {
        const auto& prior = impl_->records.at(found->second);
        require(prior.original_input == input, ErrorCode::Conflict, "admission key reused with different content"); return prior;
    }
    require(impl_->records.size() < impl_->limits.max_requests, ErrorCode::QuotaExceeded, "request limit reached");
    auto next = impl_->fresh(input);
    Command cmd{root, input.idempotency_key, 0, 1, 1};
    impl_->commit({next}, {impl_->event(next, cmd, "RequestReceived")}); return next;
}

RequestSnapshot TaskEngine::amend(const std::string& id, const Command& cmd, const std::string& goal) {
    text_field(goal, "goal");
    return impl_->mutate(id, cmd, Role::Root, "RequirementsAmended", goal, [&](auto& q) {
        q.goal = goal; q.requirements_revision = increment(q.requirements_revision);
        q.state = RequestState::Adjusting; fence_operations(q); invalidate_acceptance(q); q.candidate.reset();
    });
}
RequestSnapshot TaskEngine::replace(const std::string& id, const Command& cmd, const RequestInput& input) {
    validate_input(input); Writer body; write_input(body, input);
    std::lock_guard lock(impl_->mutex);
    const auto& old = impl_->find(id); impl_->authorize(old, cmd.actor, Role::Root);
    auto fingerprint = impl_->signature(cmd, "RequestReplaced", body.bytes);
    if (!impl_->precheck(old, cmd, fingerprint)) return impl_->find(old.replaced_by);
    require(!terminal(old.state), ErrorCode::InvalidState, "cannot replace terminal request");
    require(input.tenant_id == old.original_input.tenant_id && input.project_id == old.original_input.project_id &&
            input.root_id == old.original_input.root_id, ErrorCode::Unauthorized, "replacement changes ownership");
    require(!impl_->admissions.contains(admission_key(input)), ErrorCode::Conflict, "replacement admission key exists");
    require(impl_->records.size() < impl_->limits.max_requests, ErrorCode::QuotaExceeded, "request limit reached");
    auto previous = old; auto next = impl_->fresh(input);
    previous.state = RequestState::Superseded; previous.resources = ResourceState::Draining;
    previous.lease_epoch = increment(previous.lease_epoch); previous.state_version = increment(previous.state_version);
    previous.replaced_by = next.request_id; next.replaces = previous.request_id;
    fence_operations(previous); invalidate_acceptance(previous);
    previous.command_receipts.emplace(cmd.command_id, fingerprint);
    auto received = cmd; received.command_id = input.idempotency_key;
    received.actor.request_id = next.request_id;
    impl_->commit({previous, next}, {impl_->event(previous, cmd, "RequestReplaced"), impl_->event(next, received, "RequestReceived")});
    return next;
}
RequestSnapshot TaskEngine::cancel(const std::string& id, const Command& cmd) {
    return impl_->mutate(id, cmd, Role::Root, "RequestCancelled", "", [](auto& q) {
        q.state = RequestState::Cancelled; q.resources = ResourceState::Draining;
        q.lease_epoch = increment(q.lease_epoch); fence_operations(q); invalidate_acceptance(q);
    });
}
RequestSnapshot TaskEngine::plan(const std::string& id, const Command& cmd, const std::string& plan_text) {
    text_field(plan_text, "plan");
    return impl_->mutate(id, cmd, Role::Dispatcher, "PlanCommitted", plan_text, [&](auto& q) {
        require(q.state == RequestState::Queued || q.state == RequestState::Running || q.state == RequestState::Adjusting ||
                q.state == RequestState::Submitted || q.state == RequestState::Verifying,
                ErrorCode::InvalidState, "cannot plan in current state");
        q.plan = plan_text; q.plan_revision = increment(q.plan_revision);
        q.plan_requirements_revision = q.requirements_revision; q.resources = ResourceState::Active;
        q.state = RequestState::Running; fence_operations(q); invalidate_acceptance(q); q.candidate.reset();
    });
}
RequestSnapshot TaskEngine::create_team(const std::string& id, const Command& cmd, const std::string& team_id, bool review_team) {
    identifier(team_id, "team id"); Writer body; body.string(team_id); body.boolean(review_team);
    return impl_->mutate(id, cmd, Role::Dispatcher, "TeamCreated", body.bytes, [&](auto& q) {
        impl_->active(q); require(q.plan_revision > 0 && q.plan_requirements_revision == q.requirements_revision,
                                 ErrorCode::InvalidState, "current plan required");
        require(!q.teams.contains(team_id), ErrorCode::Conflict, "team id exists");
        require(q.teams.size() < impl_->limits.max_teams_per_request, ErrorCode::QuotaExceeded, "team limit reached");
        TeamSnapshot team; team.team_id = team_id; team.task_id = q.request_id + ":team:" + team_id;
        team.lead_id = team.task_id + ":lead"; team.worker_id = team.task_id + ":worker";
        team.review_team = review_team; team.lease_epoch = q.lease_epoch; q.teams.emplace(team_id, std::move(team));
    });
}
RequestSnapshot TaskEngine::instruct_team(const std::string& id, const Command& cmd, const std::string& instructions) {
    text_field(instructions, "team instructions");
    return impl_->mutate(id, cmd, Role::TeamLead, "TeamInstructionsCommitted", instructions, [&](auto& q) {
        if (q.teams.at(cmd.actor.team_id).review_team &&
                (q.state == RequestState::Accepted || q.state == RequestState::HandoffPending)) {
            q.state = RequestState::Verifying; invalidate_acceptance(q);
        }
        impl_->active(q); require(q.plan_revision > 0 && q.plan_requirements_revision == q.requirements_revision,
                                 ErrorCode::InvalidState, "current plan required");
        auto& team = q.teams.at(cmd.actor.team_id); team.instructions = instructions;
        team.instructions_revision = q.requirements_revision; team.instructions_plan_revision = q.plan_revision;
        team.lease_epoch = q.lease_epoch; team.instructions_version = increment(team.instructions_version);
        for (auto& [operation_id, operation] : q.operations) {
            (void)operation_id;
            if (operation.team_id != team.team_id) continue;
            if (operation.state == OperationState::Prepared) operation.state = OperationState::Cancelled;
            else if (operation.state == OperationState::Running) operation.state = OperationState::Unknown;
        }
    });
}
RequestSnapshot TaskEngine::acknowledge_applied(const std::string& id, const Command& cmd) {
    return impl_->mutate(id, cmd, Role::Worker, "RequirementsApplied", "", [&](auto& q) {
        impl_->active(q); auto& t = q.teams.at(cmd.actor.team_id); impl_->no_unknown(q, t.team_id);
        require(t.lease_epoch == q.lease_epoch && t.instructions_revision == q.requirements_revision &&
                t.instructions_plan_revision == q.plan_revision && !t.instructions.empty(),
                ErrorCode::StaleRequirements, "TeamLead must explain current change first");
        t.applied_revision = q.requirements_revision; t.applied_plan_revision = q.plan_revision;
        t.applied_instructions_version = t.instructions_version;
        if (q.state == RequestState::Adjusting) q.state = RequestState::Running;
    });
}

RequestSnapshot TaskEngine::prepare_operation(const std::string& id, const Command& cmd,
        const std::string& operation_key, const std::string& tool, const std::string& payload) {
    identifier(operation_key, "operation key"); identifier(tool, "tool"); text_field(payload, "operation payload");
    Writer body; body.string(operation_key); body.string(tool); body.string(payload);
    return impl_->mutate(id, cmd, Role::Worker, "OperationPrepared", body.bytes, [&](auto& q) {
        auto& team = impl_->ready_team(q, cmd);
        require(tool_allowed(cmd.actor.role, tool, impl_->system_tools, impl_->system_tools, q.original_input.allowed_tools),
                ErrorCode::Unauthorized, "tool is not in role/system/request grants");
        for (const auto& [operation_id, operation] : q.operations) {
            (void)operation_id;
            if (operation.team_id == team.team_id && operation.idempotency_key == operation_key) {
                require(operation.tool_id == tool && operation.payload == payload &&
                        operation.requirements_revision == q.requirements_revision && operation.plan_revision == q.plan_revision,
                        ErrorCode::Conflict, "operation key reused with different content/version");
                return; // Exact operation retry returns its existing status, never executes again.
            }
        }
        impl_->no_unknown(q, team.team_id);
        require(q.operations.size() < impl_->limits.max_operations_per_request,
                ErrorCode::QuotaExceeded, "operation limit reached");
        OperationSnapshot operation; operation.operation_id = q.request_id + ":op:" + std::to_string(q.operations.size() + 1);
        operation.team_id = team.team_id; operation.idempotency_key = operation_key;
        operation.tool_id = tool; operation.payload = payload; operation.requirements_revision = q.requirements_revision;
        operation.plan_revision = q.plan_revision; operation.lease_epoch = q.lease_epoch;
        q.operations.emplace(operation.operation_id, std::move(operation));
    });
}
RequestSnapshot TaskEngine::start_operation(const std::string& id, const Command& cmd, const std::string& operation_id) {
    identifier(operation_id, "operation id");
    return impl_->mutate(id, cmd, Role::Worker, "OperationStarted", operation_id, [&](auto& q) {
        impl_->ready_team(q, cmd); impl_->no_unknown(q, cmd.actor.team_id);
        auto it = q.operations.find(operation_id);
        require(it != q.operations.end(), ErrorCode::NotFound, "operation not found"); auto& operation = it->second;
        require(operation.team_id == cmd.actor.team_id, ErrorCode::Unauthorized, "operation belongs to another team");
        require(operation.state == OperationState::Prepared, ErrorCode::InvalidState, "operation is not prepared");
        require(operation.lease_epoch == q.lease_epoch && operation.requirements_revision == q.requirements_revision &&
                operation.plan_revision == q.plan_revision, ErrorCode::StaleRequirements, "operation is obsolete");
        require(tool_allowed(cmd.actor.role, operation.tool_id, impl_->system_tools, impl_->system_tools,
                             q.original_input.allowed_tools), ErrorCode::Unauthorized, "dispatch permission denied");
        operation.state = OperationState::Running;
    });
}
RequestSnapshot TaskEngine::finish_operation(const std::string& id, const Command& cmd,
        const std::string& operation_id, OperationState outcome, const std::string& evidence) {
    identifier(operation_id, "operation id"); text_field(evidence, "operation evidence", max_reference);
    require(outcome == OperationState::Succeeded || outcome == OperationState::Failed || outcome == OperationState::Unknown,
            ErrorCode::InvalidInput, "unsupported finish outcome");
    Writer body; body.string(operation_id); body.enumeration(outcome); body.string(evidence);
    return impl_->mutate(id, cmd, Role::Worker, "OperationFinished", body.bytes, [&](auto& q) {
        impl_->ready_team(q, cmd);
        auto it = q.operations.find(operation_id);
        require(it != q.operations.end(), ErrorCode::NotFound, "operation not found"); auto& operation = it->second;
        require(operation.team_id == cmd.actor.team_id, ErrorCode::Unauthorized, "operation belongs to another team");
        require(operation.state == OperationState::Running, ErrorCode::InvalidState, "operation is not running");
        require(operation.lease_epoch == q.lease_epoch && operation.requirements_revision == q.requirements_revision &&
                operation.plan_revision == q.plan_revision, ErrorCode::StaleRequirements, "obsolete operation result");
        operation.state = outcome; operation.evidence_ref = evidence;
    });
}

RequestSnapshot TaskEngine::submit_candidate(const std::string& id, const Command& cmd, Candidate candidate) {
    identifier(candidate.artifact_id, "artifact id");
    require(candidate.artifact_version > 0 && hash_valid(candidate.content_hash), ErrorCode::InvalidInput, "invalid candidate version/hash");
    Writer body; write_candidate(body, candidate);
    return impl_->mutate(id, cmd, Role::Worker, "CandidateSubmitted", body.bytes, [&](auto& q) {
        // A newer candidate may invalidate acceptance while the Root is awaiting
        // a receipt. CAS prevents an old receipt and this update both succeeding.
        if (q.state == RequestState::Accepted || q.state == RequestState::HandoffPending)
            q.state = RequestState::Submitted;
        auto& team = impl_->ready_team(q, cmd); impl_->no_pending(q);
        require(!team.review_team && candidate.producer_team_id == team.team_id &&
                candidate.requirements_revision == q.requirements_revision,
                ErrorCode::Unauthorized, "candidate producer/revision mismatch");
        auto previous = q.artifact_versions.find(candidate.artifact_id);
        if (previous != q.artifact_versions.end())
            require(candidate.artifact_version > previous->second, ErrorCode::Conflict, "artifact version must advance across all candidates");
        q.artifact_versions.insert_or_assign(candidate.artifact_id, candidate.artifact_version);
        q.candidate = candidate; q.state = RequestState::Submitted; invalidate_acceptance(q);
    });
}
RequestSnapshot TaskEngine::record_review(const std::string& id, const Command& cmd, const Candidate& candidate,
                                        bool passed, const std::string& evidence) {
    text_field(evidence, "review evidence", max_reference); Writer body; write_candidate(body, candidate); body.boolean(passed); body.string(evidence);
    return impl_->mutate(id, cmd, Role::Worker, "ReviewRecorded", body.bytes, [&](auto& q) {
        if (q.state == RequestState::Accepted || q.state == RequestState::HandoffPending) q.state = RequestState::Verifying;
        auto& team = impl_->ready_team(q, cmd);
        require(team.review_team && team.team_id != candidate.producer_team_id,
                ErrorCode::Unauthorized, "review must use a separate assigned review team");
        require(q.candidate && *q.candidate == candidate && candidate.requirements_revision == q.requirements_revision,
                ErrorCode::Conflict, "review candidate is stale");
        q.reviews.push_back({candidate, team.team_id, passed, evidence, team.instructions_version});
        q.state = passed ? RequestState::Verifying : RequestState::Running;
        invalidate_acceptance(q);
    });
}
RequestSnapshot TaskEngine::accept(const std::string& id, const Command& cmd) {
    return impl_->mutate(id, cmd, Role::Dispatcher, "CandidateAccepted", "", [&](auto& q) {
        require(q.state == RequestState::Verifying && q.candidate &&
                q.candidate->requirements_revision == q.requirements_revision,
                ErrorCode::InvalidState, "current candidate must have a passing review");
        impl_->no_pending(q);
        std::size_t reviewers = 0;
        for (const auto& [team_id, team] : q.teams) {
            if (!team.review_team) continue;
            ++reviewers;
            const ReviewRecord* latest = nullptr;
            for (const auto& review : q.reviews)
                if (review.candidate == *q.candidate && review.reviewer_team_id == team_id) latest = &review;
            require(latest && latest->passed && !latest->evidence_ref.empty() &&
                    latest->instructions_version == team.instructions_version, ErrorCode::InvalidState,
                    "each assigned review team must approve the current candidate");
        }
        require(reviewers > 0, ErrorCode::InvalidState, "independent review required");
        q.accepted = true; q.state = RequestState::Accepted;
    });
}
RequestSnapshot TaskEngine::prepare_handoff(const std::string& id, const Command& cmd, const std::string& package) {
    text_field(package, "synthetic package reference", max_reference);
    return impl_->mutate(id, cmd, Role::Dispatcher, "HandoffPrepared", package, [&](auto& q) {
        require(q.state == RequestState::Accepted && q.accepted && q.candidate,
                ErrorCode::InvalidState, "candidate not accepted");
        q.package_ref = package; q.state = RequestState::HandoffPending;
    });
}
RequestSnapshot TaskEngine::confirm_delivery(const std::string& id, const Command& cmd, const Candidate& candidate,
                                            const std::string& package, const std::string& receipt) {
    text_field(package, "package reference", max_reference); text_field(receipt, "synthetic receipt", max_reference);
    Writer body; write_candidate(body, candidate); body.string(package); body.string(receipt);
    return impl_->mutate(id, cmd, Role::Root, "DeliveryConfirmed", body.bytes, [&](auto& q) {
        require(q.state == RequestState::HandoffPending && q.accepted && q.candidate && *q.candidate == candidate &&
                candidate.requirements_revision == q.requirements_revision && q.package_ref == package,
                ErrorCode::Conflict, "receipt does not match accepted current package/candidate");
        impl_->no_pending(q); q.receipt_ref = receipt; q.state = RequestState::Completed;
        q.resources = ResourceState::Draining; q.lease_epoch = increment(q.lease_epoch);
    });
}

RequestSnapshot TaskEngine::recover(const std::string& id, Revision expected, const std::string& command_id) {
    std::lock_guard lock(impl_->mutex); const auto& current = impl_->find(id);
    auto cmd = impl_->host_command(current, expected, command_id);
    // Host retries have stable content independent of the newly incremented epoch.
    Writer body; body.string("Recover"); body.number(expected);
    auto prior = current.command_receipts.find(command_id);
    if (prior != current.command_receipts.end()) {
        require(prior->second == body.bytes, ErrorCode::Conflict, "host command key conflict"); return current;
    }
    if (!impl_->precheck(current, cmd, body.bytes)) return current;
    require(!terminal(current.state), ErrorCode::InvalidState, "cannot recover terminal business request");
    auto next = current; next.lease_epoch = increment(next.lease_epoch);
    next.state_version = increment(next.state_version); fence_operations(next);
    for (auto& [team_id, team] : next.teams) {
        (void)team_id; team.applied_instructions_version = 0;
    }
    // Acceptance/delivery references are durable facts. An in-progress request
    // returns to ADJUSTING; approved handoff state stays available for a receipt.
    if (next.state != RequestState::Accepted && next.state != RequestState::HandoffPending)
        next.state = next.plan_revision ? RequestState::Adjusting : RequestState::Queued;
    next.command_receipts.emplace(command_id, body.bytes);
    impl_->commit({next}, {impl_->event(next, cmd, "DispatcherRecovered")}); return next;
}
RequestSnapshot TaskEngine::reconcile_operation(const std::string& id, Revision expected,
        const std::string& command_id, const std::string& operation_id, OperationState outcome, const std::string& evidence) {
    identifier(operation_id, "operation id"); text_field(evidence, "reconciliation evidence", max_reference);
    require(outcome == OperationState::Succeeded || outcome == OperationState::Failed || outcome == OperationState::Cancelled,
            ErrorCode::InvalidInput, "reconciliation must establish a final outcome");
    std::lock_guard lock(impl_->mutex); const auto& current = impl_->find(id);
    auto cmd = impl_->host_command(current, expected, command_id);
    Writer body; body.string("Reconcile"); body.number(expected); body.string(operation_id); body.enumeration(outcome); body.string(evidence);
    if (!impl_->precheck(current, cmd, body.bytes)) return current;
    auto next = current; auto found = next.operations.find(operation_id);
    require(found != next.operations.end(), ErrorCode::NotFound, "operation not found");
    require(found->second.state == OperationState::Unknown, ErrorCode::InvalidState, "only unknown operations need reconciliation");
    found->second.state = outcome; found->second.evidence_ref = evidence;
    next.state_version = increment(next.state_version); next.command_receipts.emplace(command_id, body.bytes);
    impl_->commit({next}, {impl_->event(next, cmd, "OperationReconciled")}); return next;
}
RequestSnapshot TaskEngine::advance_resources(const std::string& id, Revision expected,
        const std::string& command_id, ResourceState target) {
    std::lock_guard lock(impl_->mutex); const auto& current = impl_->find(id);
    auto cmd = impl_->host_command(current, expected, command_id);
    Writer body; body.string("Resources"); body.number(expected); body.enumeration(target);
    if (!impl_->precheck(current, cmd, body.bytes)) return current;
    require(terminal(current.state), ErrorCode::InvalidState, "resource cleanup waits for delivery or exception terminal state");
    auto next = current; bool valid =
        (next.resources == ResourceState::Draining && target == ResourceState::Stopped) ||
        (next.resources == ResourceState::Stopped && target == ResourceState::Cleaning) ||
        (next.resources == ResourceState::Cleaning && (target == ResourceState::Closed || target == ResourceState::CleanupFailed)) ||
        (next.resources == ResourceState::CleanupFailed && target == ResourceState::Cleaning);
    require(valid, ErrorCode::InvalidState, "invalid resource transition");
    if (target == ResourceState::Stopped) impl_->no_pending(next);
    next.resources = target; next.state_version = increment(next.state_version);
    next.command_receipts.emplace(command_id, body.bytes);
    impl_->commit({next}, {impl_->event(next, cmd, "ResourceStateChanged", to_string(target))}); return next;
}
RequestSnapshot TaskEngine::get(const std::string& id) const {
    std::lock_guard lock(impl_->mutex); return impl_->find(id);
}
std::vector<RequestSnapshot> TaskEngine::requests() const {
    std::lock_guard lock(impl_->mutex); impl_->healthy(); std::vector<RequestSnapshot> result;
    for (const auto& [id, request] : impl_->records) { (void)id; result.push_back(request); } return result;
}
std::vector<EventEnvelope> TaskEngine::events(const std::string& id, Revision after) const {
    std::lock_guard lock(impl_->mutex); impl_->healthy(); std::vector<EventEnvelope> result;
    for (const auto& e : impl_->log) if ((id.empty() || e.request_id == id) && e.sequence > after) result.push_back(e);
    return result;
}
} // namespace wls
