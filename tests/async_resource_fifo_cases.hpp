// Included after the shared real Store/FakeModel fixtures in
// async_coordinator_tests.cpp. The standalone same-source probe uses these
// exact cases against each version's own headers, libraries and Store assets.

std::vector<std::string> resource_plan(Fixture& fixture, AsyncCoordinator& runtime,
                                     const std::string& request) {
    auto script = steps();
    auto& nodes = script.front().tool_calls.front().arguments.at("nodes").as_array();
    const auto first = nodes.front();
    for (int index = 1; index != 3; ++index) {
        auto node = first;
        node.as_object()["node_id"] = "resource-task-" + std::to_string(index);
        nodes.push_back(std::move(node));
    }
    const FakeModel initial(script, fixture.config.resolve(Role::Dispatcher).model);
    OfflineRuntime offline(runtime.store(), *schemas);
    const auto call = offline.reserve_call(initial, request, Role::Dispatcher);
    (void)offline.accept_independent_response(call, initial.complete(call.request));
    std::vector<std::string> tasks;
    for (const auto& task : runtime.store().tasks(request)) tasks.push_back(text(task, "task_id"));
    CHECK(tasks.size() == 3);
    return tasks;
}

void resource_worker_ready(Fixture& fixture, AsyncCoordinator& runtime,
                           const std::string& request, const std::string& task) {
    const FakeModel initial(steps(), fixture.config.resolve(Role::TeamLead).model);
    OfflineRuntime offline(runtime.store(), *schemas);
    const auto call = offline.reserve_call(initial, request, Role::TeamLead, task);
    (void)offline.accept_response(call, initial.complete(call.request));
    const auto binding = runtime.store().binding(task);
    (void)runtime.store().acknowledge(
        runtime.store().host_actor(Role::Worker, request, binding.team_id), binding,
        number(runtime.store().task(task), "state_version"), new_id("ack"));
}

void cross_pool_fifo_resource_heads_progress_before_slot_admission() {
    Fixture fixture;
    AsyncCoordinatorLimits limits;
    limits.management_workers = 2;
    limits.worker_workers = 2;
    AsyncCoordinator runtime(fixture.open(), schemas, limits);
    const auto a = fixture.admit(runtime.store()), b = fixture.admit(runtime.store());
    const auto tasks_a = resource_plan(fixture, runtime, a);
    const auto tasks_b = resource_plan(fixture, runtime, b);
    resource_worker_ready(fixture, runtime, a, tasks_a[0]);
    resource_worker_ready(fixture, runtime, a, tasks_a[2]);
    resource_worker_ready(fixture, runtime, b, tasks_b[1]);

    auto blockers = std::make_shared<Gate>();
    Release release{blockers};
    // The first running request in each pool remains in its fair-turn ring.
    // Its later job must not occupy the pool while waiting on the other pool.
    auto held_manager = runtime.submit(runtime.capture(b, Role::TeamLead, tasks_b[0]),
        fixture.model(blockers), WorkClass::Normal, "block-manager-B");
    auto held_worker = runtime.submit(runtime.capture(a, Role::Worker, tasks_a[0]),
        fixture.model(blockers), WorkClass::Normal, "block-worker-A");
    until(runtime, [&] { return blockers->entered == 2; });
    CHECK(runtime.snapshot().managers.normal_running == 1);
    CHECK(runtime.snapshot().workers.normal_running == 1);
    const auto claims_before = count(fixture.path,
        "SELECT count(*) FROM events WHERE type='claim_offline_call'");
    const auto ops_before = count(fixture.path, "SELECT count(*) FROM ops");
    auto order = std::make_shared<Order>();
    const std::vector<std::string> labels{"M_A(X)", "W_B(Y)", "M_B(Y)", "W_A(X)"};
    const std::vector<CapturedModelCall> calls{
        runtime.capture(a, Role::TeamLead, tasks_a[1]),
        runtime.capture(b, Role::Worker, tasks_b[1]),
        runtime.capture(b, Role::TeamLead, tasks_b[2]),
        runtime.capture(a, Role::Worker, tasks_a[2])};
    const std::vector<std::string> keys{"resource-X", "resource-Y", "resource-Y", "resource-X"};
    std::vector<std::shared_ptr<GatedModel>> models;
    std::vector<std::future<Json>> results;
    for (std::size_t index = 0; index != calls.size(); ++index) {
        auto model = fixture.model();
        const auto label = labels[index];
        model->on_start = [order, label] { order->add(label); };
        results.push_back(runtime.submit(calls[index], model, WorkClass::Normal, keys[index]));
        models.push_back(std::move(model));
    }
    CHECK(runtime.snapshot().queued == 4);
    CHECK(count(fixture.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == claims_before);
    blockers->release();
    const auto cycle = [&] {
        const auto view = runtime.snapshot();
        return view.awaiting_claim == 2 && view.queued == 2 && view.executing == 0 &&
            view.ready_results == 0 && view.resource_blocked == 2 &&
            view.managers.normal_running == 1 && view.workers.normal_running == 1 &&
            std::all_of(models.begin(), models.end(), [](const auto& model) { return model->calls == 0; });
    };
    until(runtime, [&] {
        return (ready(held_manager) && ready(held_worker) && cycle()) ||
            std::all_of(results.begin(), results.end(), [](auto& result) { return ready(result); });
    });
    (void)held_manager.get();
    CHECK(held_worker.get().at("status") == "SUCCEEDED");
    const bool structural_cycle = cycle();
    if (structural_cycle) {
        // Diagnose the already-observed wait graph; no timeout is a red result.
        CHECK(runtime.poll() == 0);
        CHECK(cycle());
        CHECK(count(fixture.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == claims_before);
        for (const auto& call : calls) {
            const auto attempt = runtime.store().attempt(call.request.attempt_id);
            CHECK(attempt.at("status") == "PENDING");
            CHECK(attempt.at("request_envelope") == to_json(call.request));
            CHECK(attempt.at("response").is_null());
            CHECK(runtime.store().tool_feedback(call.request.attempt_id).is_null());
        }
        std::cout << "CROSS_POOL_FIFO cycle=1 awaiting=2 queued=2 executing=0 "
                     "resource_blocked=2 managers_normal_running=1 workers_normal_running=1 "
                     "new_model_calls=0\n";
        runtime.close();
        CHECK(runtime.snapshot().managers.running == 0 && runtime.snapshot().workers.running == 0);
        CHECK(std::all_of(results.begin(), results.end(), [](auto& result) { return ready(result); }));
    }
    CHECK(!structural_cycle);
    for (std::size_t index = 0; index != calls.size(); ++index) {
        const auto result = results[index].get();
        if (calls[index].request.role == Role::Worker) CHECK(result.at("status") == "SUCCEEDED");
        CHECK(models[index]->calls == 1);
        const auto attempt = runtime.store().attempt(calls[index].request.attempt_id);
        CHECK(attempt.at("status") == "COMPLETED" && attempt.at("accepted") == 1);
        CHECK(attempt.at("request_envelope") == to_json(calls[index].request));
        CHECK(attempt.at("response").at("binding") == to_json(calls[index].request.binding));
        CHECK(!attempt.at("settlement_sha256").is_null());
        CHECK(runtime.store().tool_feedback(calls[index].request.attempt_id).at("response") == attempt.at("response"));
    }
    const auto observed = order->read();
    CHECK(observed.size() == 4);
    CHECK(std::find(observed.begin(), observed.end(), "M_A(X)") <
        std::find(observed.begin(), observed.end(), "W_A(X)"));
    CHECK(std::find(observed.begin(), observed.end(), "W_B(Y)") <
        std::find(observed.begin(), observed.end(), "M_B(Y)"));
    CHECK(count(fixture.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == claims_before + 4);
    CHECK(count(fixture.path, "SELECT count(*) FROM ops") == ops_before + 3); // Blocker + two new notes.
    CHECK(runtime.snapshot().outstanding == 0 && runtime.snapshot().retained_results == 0);
    std::cout << "CROSS_POOL_FIFO cycle=0 new_model_calls=4 accepted=4 resource_fifo=preserved\n";
}
