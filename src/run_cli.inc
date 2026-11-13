// One trusted local command owns admission, bounded live continuation and
// durable local delivery. This is not a model tool or a persistent scheduler.
struct RunFd {
    int value{-1};
    explicit RunFd(int descriptor) : value(descriptor) {}
    ~RunFd() { if (value >= 0) ::close(value); }
    RunFd(const RunFd&) = delete;
    RunFd& operator=(const RunFd&) = delete;
};

std::filesystem::path run_absolute(std::string value, bool expand_home = false) {
    if (expand_home && (value == "~" || value.starts_with("~/"))) {
        const auto* account = ::getpwuid(::geteuid());
        if (!account || !account->pw_dir || account->pw_dir[0] != '/')
            throw Error(ErrorCode::InvalidInput, "cannot resolve the local account home directory");
        value.replace(0, 1, account->pw_dir);
    }
    if (value.empty() || value.size() > 4096 || value.find('\0') != std::string::npos || value.front() == '~')
        throw Error(ErrorCode::InvalidInput, "run requires a bounded local path");
    const std::filesystem::path path(value);
    for (const auto& part : path) if (part == "..")
        throw Error(ErrorCode::Unauthorized, "run paths must not contain '..'");
    return std::filesystem::absolute(path).lexically_normal();
}

std::unique_ptr<Workspace> run_directory(const std::filesystem::path& path, bool create, bool* made = nullptr) {
    RunFd current(::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC));
    if (current.value < 0) throw Error(ErrorCode::Storage, "cannot pin run filesystem root");
    bool final_created = false;
    for (const auto& part : path.relative_path()) {
        int next = ::openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next < 0 && errno == ENOENT) {
            if (!create) return {};
            if (::mkdirat(current.value, part.c_str(), 0700) != 0 || ::fsync(current.value) != 0)
                throw Error(ErrorCode::Storage, "cannot create a private run directory exclusively");
            next = ::openat(current.value, part.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
            final_created = true;
        } else final_created = false;
        if (next < 0) throw Error(ErrorCode::Unauthorized, "run directory is unsafe or unavailable");
        ::close(current.value); current.value = next;
    }
    struct stat metadata{};
    if (::fstat(current.value, &metadata) != 0 || metadata.st_uid != ::geteuid() ||
            (metadata.st_mode & 07777) != 0700)
        throw Error(ErrorCode::Unauthorized, "run directories must be owned by the local user with mode 0700");
    if (made) *made = final_created;
    WorkspaceLimits limits; limits.max_read_bytes = limits.max_write_bytes = 16U * 1024U * 1024U;
    return std::make_unique<Workspace>(path, WorkspaceAccess::ReadWrite, limits);
}

bool run_private_file(const Workspace& root, const char* name) {
    RunFd descriptor(root.dup_root_fd());
    struct stat metadata{};
    if (::fstatat(descriptor.value, name, &metadata, AT_SYMLINK_NOFOLLOW) != 0) {
        if (errno == ENOENT) return false;
        throw Error(ErrorCode::Storage, "cannot inspect private run file");
    }
    if (!S_ISREG(metadata.st_mode) || metadata.st_uid != ::geteuid() ||
            metadata.st_nlink != 1 || (metadata.st_mode & 07777) != 0600)
        throw Error(ErrorCode::Unauthorized, "run files must be private owned regular files");
    return true;
}

std::unique_ptr<Workspace> run_child(Workspace& parent, const char* leaf, bool create, bool* made = nullptr) {
    const auto path = std::filesystem::path(text(parent.root_identity().at("path"))) / leaf;
    auto child = run_directory(path, false);
    if (made) *made = false;
    if (!child && create) {
        const auto result = parent.mkdir(leaf);
        if (result.status != WorkspaceMutationStatus::Succeeded)
            throw Error(ErrorCode::UnknownOutcome, "private run directory creation incomplete; retain original intent");
        child = run_directory(path, false);
        if (made) *made = true;
    }
    static_cast<void>(parent.root_identity());
    return child;
}

struct RunLayout {
    std::vector<std::unique_ptr<Workspace>> roots;
    RunLayout(const std::filesystem::path& data, const std::string& id, const Json& admission, bool create) {
        bool fresh = false;
        auto root = run_directory(data, create);
        if (!root) return;
        roots.push_back(std::move(root));
        root = run_child(*roots.back(), "requests", create);
        if (!root) return;
        roots.push_back(std::move(root));
        root = run_child(*roots.back(), id.c_str(), create, &fresh);
        if (!root) return;
        roots.push_back(std::move(root));
        Json saved = nullptr;
        if (!fresh) {
            if (!run_private_file(*roots[2], "intent.json"))
                throw Error(ErrorCode::Conflict, "existing run directory has no durable admission intent");
            const auto original = roots[2]->read("intent.json", 0, 16U * 1024U * 1024U);
            if (!original.at("eof").as_bool() || original.at("content").is_null())
                throw Error(ErrorCode::Corrupt, "run intent is not complete bounded UTF-8");
            saved = parse_json_strict(text(original.at("content")), 16U * 1024U * 1024U);
            if (saved.at("admission") != admission)
                throw Error(ErrorCode::Conflict, "run identity collision or changed admission configuration/budget");
        }
        for (const auto* leaf : {"workspaces", "delivery"}) {
            auto pinned = run_child(*roots[2], leaf, create && fresh);
            if (!pinned) throw Error(ErrorCode::Conflict, "original run directory is missing");
            roots.push_back(std::move(pinned));
        }
        const Json intent = Object{{"format", "wls-run-intent-v1"}, {"admission", admission}, {"roots", identities()}};
        if (!saved.is_null()) {
            if (saved != intent) throw Error(ErrorCode::Conflict, "run directory identities differ from original intent");
        } else {
            const auto written = roots[2]->write_atomic("intent.json", canonical_json(intent));
            if (written.status != WorkspaceMutationStatus::Succeeded)
                throw Error(ErrorCode::UnknownOutcome, "run intent publication incomplete; retain directory for inspection");
        }
    }
    Json identities() const {
        Array result;
        for (const auto& root : roots) result.push_back(root->root_identity());
        return result;
    }
};

std::string run_request_id(std::string_view digest) {
    constexpr std::string_view alphabet = "0123456789ABCDEFGHJKMNPQRSTVWXYZ";
    std::string result = "req_";
    for (int i = 0; i < 26; ++i) {
        unsigned value = 0;
        for (int j = 0; j < 5; ++j) {
            const int bit = i * 5 + j - 2;
            value <<= 1;
            if (bit >= 0) {
                const auto digit = digest[static_cast<std::size_t>(bit / 4)];
                const unsigned nibble = static_cast<unsigned>(digit <= '9' ? digit - '0' : digit - 'a' + 10);
                value |= (nibble >> (3 - bit % 4)) & 1U;
            }
        }
        result += alphabet[value];
    }
    return result;
}

std::atomic<bool> run_interrupted{false};
static_assert(decltype(run_interrupted)::is_always_lock_free);
void run_signal(int) { run_interrupted.store(true, std::memory_order_relaxed); }
struct RunCancellation {
    std::stop_source source;
    decltype(std::signal(SIGINT, run_signal)) previous{SIG_DFL};
    std::jthread watcher;
    RunCancellation() {
        run_interrupted = 0;
        previous = std::signal(SIGINT, run_signal);
        if (previous == SIG_ERR) throw Error(ErrorCode::Storage, "cannot install bounded interruption handler");
        watcher = std::jthread([this](std::stop_token done) {
            std::mutex mutex; std::unique_lock lock(mutex); std::condition_variable_any wake;
            while (!done.stop_requested() && !run_interrupted)
                static_cast<void>(wake.wait_for(lock, done, std::chrono::milliseconds(1), [] { return run_interrupted != 0; }));
            if (run_interrupted) source.request_stop();
        });
    }
    ~RunCancellation() { watcher.request_stop(); watcher.join(); std::signal(SIGINT, previous); }
};

Json run_delivery(Store& store, const std::string& id, const std::filesystem::path& database,
                  const std::filesystem::path& path) {
    DeliveryDirectory directory(path);
    check_delivery_root(store, id, database, directory.root_identity());
    auto retained = store.deliveries(id);
    Json delivery = nullptr;
    for (const auto& candidate : retained) if (text(candidate.at("status")) != "SUPERSEDED") {
        if (!delivery.is_null()) throw Error(ErrorCode::Conflict, "ambiguous retained delivery intents");
        delivery = candidate;
    }
    if (!delivery.is_null() && text(delivery.at("status")) == "CONFIRMED") {
        check_delivery_scope(delivery, id, directory.root_identity());
        return directory.inspect(delivery.at("frozen_plan"));
    }
    const auto root = store.host_actor(Role::Root, id);
    const auto binding = store.request_binding(id);
    if (delivery.is_null()) delivery = store.prepare_delivery(root, binding,
        integer(store.request(id).at("state_version")), directory.root_identity(), "run-delivery-v1");
    check_delivery_scope(delivery, id, directory.root_identity());
    delivery = store.authorize_delivery_publication(root, binding, text(delivery.at("delivery_id")));
    const auto publication = text(delivery.at("status")) == "STORED"
        ? directory.verify_and_sync_existing(delivery.at("frozen_plan")) : publish_bundle(directory, delivery);
    if (text(delivery.at("status")) != "STORED") delivery = store.record_delivery_publication(root, binding,
        text(delivery.at("delivery_id")), integer(delivery.at("state_version")), publication, "run-publish-v1");
    static_cast<void>(store.acknowledge_delivery(root, binding, text(delivery.at("delivery_id")),
        text(delivery.at("manifest_sha256")), integer(delivery.at("requirements_revision")),
        integer(delivery.at("plan_revision")), "local-owner", "durable_local", "run-local-sync-v1", publication));
    return publication;
}

Json run_summary(Store& store, const std::string& id, const Json& report, const Json& delivery,
                 std::chrono::steady_clock::time_point started) {
    std::set<std::string> attempts;
    std::int64_t after = 0;
    while (true) {
        const auto page = store.events(id, after, 100);
        if (page.empty()) break;
        for (const auto& event : page) {
            after = integer(event.at("seq"));
            const auto& result = event.at("result");
            if (result.is_object()) if (const auto* attempt = result.as_object().if_contains("attempt_id"))
                if (attempt->is_string()) attempts.insert(text(*attempt));
        }
    }
    Object roles{{"dispatcher", 0}, {"team_lead", 0}, {"worker", 0}};
    Object tokens{{"input_tokens", 0}, {"output_tokens", 0}, {"cache_read_tokens", 0}, {"cache_write_tokens", 0}};
    for (const auto& attempt : attempts) {
        const auto original = store.attempt(attempt);
        const auto role = text(original.at("role"));
        roles[role] = integer(roles.at(role)) + 1;
        for (auto& dimension : tokens) {
            const auto& value = original.at(dimension.key());
            if (value.is_null()) dimension.value() = nullptr;
            else if (!dimension.value().is_null()) dimension.value() = integer(dimension.value()) + integer(value);
        }
    }
    if (attempts.empty()) for (auto& dimension : tokens) dimension.value() = nullptr;
    Json ratio = nullptr;
    if (!tokens.at("input_tokens").is_null() && !tokens.at("cache_read_tokens").is_null() && integer(tokens.at("input_tokens")) > 0)
        ratio = static_cast<double>(integer(tokens.at("cache_read_tokens"))) / static_cast<double>(integer(tokens.at("input_tokens")));
    const auto record = store.request(id), budget = store.budget(id);
    Array blockers;
    for (const auto& blocked : report.at("blocked").as_array()) {
        Object safe;
        for (const auto* key : {"code", "role", "node_id", "task_id"}) if (blocked.as_object().contains(key)) safe[key] = blocked.at(key);
        blockers.push_back(safe);
    }
    return Object{{"kind", "wls-run-summary-v1"}, {"request_id", id}, {"status", record.at("status")},
        {"stop_reason", report.at("stop_reason")}, {"role_calls", roles}, {"original_tokens", tokens}, {"cache_read_ratio", ratio},
        {"budget", budget}, {"elapsed_ms", std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started).count()},
        {"created_at", record.at("created_at")}, {"accounting", store.live_accounting_status(id)}, {"blockers", blockers},
        {"delivery_path", delivery.is_null() ? Json(nullptr) : delivery.at("path")},
        {"technical_acknowledgement", record.at("delivery_status") == "CONFIRMED"}, {"user_review", "not_recorded"}};
}

int run_request(Options& options, const RuntimeResources& resources) {
    const auto started = std::chrono::steady_clock::now();
    if (!options.has("--config")) throw Error(ErrorCode::InvalidInput, "run requires --config");
    Json provenance;
    const auto frozen = live_config(options, resources, &provenance);
    const auto locator = run_absolute(text(provenance.at("system").at("absolute_lexical_path")));
    AdmissionInput input;
    input.tenant_id = input.project_id = "local"; input.root_id = "local-root";
    input.function_id = frozen.function_id(); input.config_snapshot = frozen.json();
    input.original_text = read_runtime_file(run_absolute(options.required("--text-file")), 65536);
    if (input.original_text.empty() || input.original_text.find('\0') != std::string::npos ||
            input.original_text.find_first_not_of(" \t\r\n") == std::string::npos)
        throw Error(ErrorCode::InvalidInput, "run requires nonblank original request text");
    const auto constraints = options.take("--constraints-file");
    if (!constraints.empty()) input.constraint_catalog = parse_json_strict(read_runtime_file(run_absolute(constraints)));
    validate_constraint_catalog_shape(input.constraint_catalog);
    const auto digest = sha256_hex(canonical_json(Object{{"format", "run-key-v1"}, {"text", input.original_text},
        {"constraints", input.constraint_catalog}, {"config_locator", locator.string()}}));
    input.request_id = run_request_id(digest); input.idempotency_key = "run-key-v1:" + digest;
    const auto grants = frozen.worker_tools(); input.tool_allowlist.assign(grants.begin(), grants.end());
    const auto& defaults = frozen.json().at("system").at("budget_defaults");
    input.budget.max_attempts = frozen.max_total_calls();
    input.budget.input_tokens = defaults.at("max_input_tokens").is_null() ? 1000000 : integer(defaults.at("max_input_tokens"));
    input.budget.output_tokens = defaults.at("max_output_tokens").is_null() ? 100000 : integer(defaults.at("max_output_tokens"));
    const auto dollars = options.take("--budget-usd");
    if (dollars.empty() && defaults.at("max_cost_usd").is_null())
        throw Error(ErrorCode::InvalidInput, "run requires a configured cost cap or --budget-usd");
    input.budget.cost_micro_usd = cost_limit_micro(dollars.empty() ? defaults.at("max_cost_usd") : parse_json_strict(dollars, 128));
    if (!defaults.at("max_cost_usd").is_null() && input.budget.cost_micro_usd > cost_limit_micro(defaults.at("max_cost_usd")))
        throw Error(ErrorCode::Quota, "run cost cap exceeds frozen configuration");
    const auto data = run_absolute(options.take("--data", text(frozen.json().at("system").at("paths").at("data"))), true);
    const bool yes = options.take("--yes") == "true"; options.finish();
    if (!frozen.bounded_runtime()) throw Error(ErrorCode::Unsupported, "run requires frozen bounded-v1 continuation limits");
    if (!frozen.logical_agent_policy().is_null()) throw Error(ErrorCode::Unsupported, "run requires separate workflow role scopes");
    Object models; Array selected; std::string kind; std::int64_t reservation = 0, duration = 0;
    for (const auto role : {Role::Dispatcher, Role::TeamLead, Role::Worker}) {
        const auto model = frozen.resolve(role).model; const auto profile = frozen.model_profile(role);
        const auto settings = live_model_settings_from_json(profile, model);
        if (!kind.empty() && kind != settings.execution_kind) throw Error(ErrorCode::Unsupported, "run cannot mix execution kinds");
        kind = settings.execution_kind; models[model] = sha256_hex(canonical_json(profile));
        if (!settings.provider.pricing || !settings.provider.billing_challenge.empty())
            throw Error(ErrorCode::Unsupported, "run requires frozen token pricing without an external billing challenge");
        // Exact input bytes are captured after admission and checked before credentials/send.
        const auto bound = minimum_cost_reservation(*settings.provider.pricing, 0, settings.provider.max_output_tokens);
        if (!bound.available) throw Error(ErrorCode::Quota, "frozen pricing bound exceeds capacity");
        reservation = std::max(reservation, bound.cost_micro_usd);
        duration = std::max(duration, settings.provider.limits.max_timeout_ms);
        auto host = settings.provider.base_url.substr(8);
        if (const auto slash = host.find('/'); slash != std::string::npos) host.resize(slash);
        const auto end = host.starts_with('[') ? host.find(']') + 1 : host.find(':');
        if (end != std::string::npos) host.resize(end);
        selected.push_back(Object{{"role", role_name(role)}, {"model_ref", model}, {"model", settings.provider.model}, {"endpoint_host", host}});
    }
    reservation = std::max<std::int64_t>(1, reservation);
    if (reservation > input.budget.cost_micro_usd || input.budget.input_tokens <= 0 || input.budget.output_tokens <= 0)
        throw Error(ErrorCode::Quota, "run budget cannot cover one conservative priced attempt");
    Json authorization = Object{{"schema_version", 3}, {"kind", "wls-live-workflow-authorization-v3"},
        {"request_id", input.request_id}, {"config_snapshot_sha256", frozen.sha256()}, {"execution_kind", kind},
        {"network_enabled", true}, {"provider_enabled", kind == "provider"}, {"models", models},
        {"max_calls", 64}, {"max_total_calls", frozen.max_total_calls()}, {"max_worker_calls_per_task", 16},
        {"max_batch_duration_ms", std::min<std::int64_t>(3600000, duration * frozen.max_total_calls())}, {"reserved_cost_micro_usd", reservation}};
    auto authorized = live_cli_authorization(authorization);
    validate_live_workflow_config(frozen, authorized.workflow);
    const Json admission = Object{{"run_key_sha256", digest}, {"request_id", input.request_id}, {"config_locator", locator.string()},
        {"original_text", input.original_text}, {"constraints", input.constraint_catalog}, {"config_snapshot", input.config_snapshot},
        {"budget", Object{{"max_attempts", input.budget.max_attempts}, {"input_tokens", input.budget.input_tokens},
            {"output_tokens", input.budget.output_tokens}, {"cost_micro_usd", input.budget.cost_micro_usd}}}};
    RunLayout preflight(data, input.request_id, admission, false);
    const MigrationRegistry migrations(resources);
    const auto database = data / "requests" / input.request_id / "control.sqlite";
    bool completed = false;
    if (preflight.roots.size() == 5 && run_private_file(*preflight.roots[2], "control.sqlite")) {
        Store inspect(database, migrations, StoreOpenMode::ReadOnly);
        Json record = nullptr;
        try { record = inspect.request(input.request_id); }
        catch (const Error& error) {
            if (error.code() != ErrorCode::InvalidInput || std::string_view(error.what()) != "authoritative record not found") throw;
        }
        if (!record.is_null()) {
            validate_live_cli_authorization(inspect, input.request_id, authorized);
            completed = text(record.at("status")) == "DELIVERED";
        }
    }
    std::cerr << canonical_json(Object{{"kind", "wls-run-confirmation-v1"}, {"request_id", input.request_id}, {"models", selected},
        {"budget_micro_usd", input.budget.cost_micro_usd}, {"max_attempts", input.budget.max_attempts},
        {"input_token_budget", input.budget.input_tokens}, {"output_token_budget", input.budget.output_tokens},
        {"max_total_calls", frozen.max_total_calls()}, {"command_deadline_ms", authorization.at("max_batch_duration_ms")},
        {"minimum_output_reservation_micro_usd", reservation}, {"reservation_basis", "encoded_body_bytes_plus_max_output"},
        {"local_acknowledgement", "durable_local"}, {"completed", completed}}) << '\n';
    if (!yes && !completed) {
        std::cerr << "Authorize this bounded run? [y/N] " << std::flush;
        std::string answer; std::getline(std::cin, answer);
        if (answer != "y" && answer != "yes") throw Error(ErrorCode::Unauthorized, "run declined before admission or model calls");
    }
    RunLayout layout(data, input.request_id, admission, true);
    if (!completed) {
        Store admission_store(database, migrations, StoreOpenMode::Takeover, StoreRecoveryScope::admission());
        static_cast<void>(admission_store.admit(input));
    }
    Store store(database, migrations, completed ? StoreOpenMode::ReadOnly : StoreOpenMode::Takeover,
        completed ? StoreRecoveryScope{} : StoreRecoveryScope::request(input.request_id));
    static_cast<void>(layout.identities());
    validate_live_cli_authorization(store, input.request_id, authorized);
    Json report = Object{{"stop_reason", completed ? "completed_replay" : "quiescent"}, {"blocked", Array{}}};
    Json delivery = nullptr;
    RunCancellation cancellation;
    if (!completed) {
        std::map<std::string, std::unique_ptr<WorkerTools>> owners;
        EventRuntime::WorkspaceResolver resolver = [&](const Json& task) {
            static_cast<void>(layout.identities());
            const auto task_id = text(task.at("task_id"));
            const auto resource = allocate_owned_workspace(store, input.request_id, task_id,
                data / "requests" / input.request_id / "workspaces", "run-workspace-v1:" + text(task.at("team_id")));
            const auto path = text(resource.at("identity").at("workspace_identity").at("path"));
            auto& owner = owners[text(task.at("team_id"))];
            if (!owner) owner = std::make_unique<WorkerTools>(store, path);
            return owner.get();
        };
        report = EventRuntime(store, SchemaRegistry(resources.schema_documents()), {}, resolver).pump_live(
            input.request_id, authorized.workflow, cancellation.source.get_token());
    }
    const auto tasks = store.tasks(input.request_id);
    const auto jobs = store.experience_jobs(input.request_id);
    if (completed || (!run_interrupted && !tasks.empty() && store.live_accounting_status(input.request_id).at("ready").as_bool() &&
            std::all_of(jobs.begin(), jobs.end(), [](const auto& job) { return job.at("status") == "DONE"; }) &&
            std::all_of(tasks.begin(), tasks.end(), [](const auto& task) { return task.at("status") == "ACCEPTED"; }))) {
        try {
            static_cast<void>(layout.identities());
            delivery = run_delivery(store, input.request_id, database, data / "requests" / input.request_id / "delivery");
        } catch (const Error& error) {
            report.at("blocked").as_array().push_back(Object{{"code", error_name(error.code())}});
            report.at("stop_reason") = "delivery_blocked";
        }
    }
    output(run_summary(store, input.request_id, report, delivery, started));
    return run_interrupted ? 130 : 0;
}
