// Real Store lifecycle coverage for calls retained outside both M1 pools.

void check_undispatched_observation(AsyncCoordinator& runtime, const CapturedModelCall& call) {
    const auto attempt = runtime.store().attempt(call.request.attempt_id);
    CHECK(attempt.at("request_envelope") == to_json(call.request));
    CHECK(attempt.at("accepted") == 0);
    CHECK(attempt.at("response").at("error_code") == "host_not_dispatched");
    CHECK(attempt.at("response").at("binding") == to_json(call.request.binding));
    CHECK(attempt.at("usage").at("input_tokens").is_null());
    CHECK(attempt.at("usage").at("output_tokens").is_null());
    CHECK(attempt.at("usage").at("cost_micro_usd").is_null());
    CHECK(!attempt.at("settlement_sha256").is_null());
    CHECK(runtime.store().tool_feedback(call.request.attempt_id).at("response") == attempt.at("response"));
    CHECK(runtime.store().budget(call.request.binding.request_id).at("reserved_input").as_int64() > 0);
}

void resource_waiter_cancel_preserves_active_predecessor() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    const auto first_id = f.admit(runtime.store()), second_id = f.admit(runtime.store());
    const auto task = f.ready(runtime.store(), second_id);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    auto first_model = f.model(gate), second_model = f.model();
    auto first = runtime.submit(runtime.capture(first_id, Role::Dispatcher), first_model,
        WorkClass::Normal, "cancel-resource");
    const auto second_call = runtime.capture(second_id, Role::Worker, task);
    auto second = runtime.submit(second_call, second_model, WorkClass::Normal, "cancel-resource");
    until(runtime, [&] { return gate->entered == 1; });
    CHECK(runtime.snapshot().workers.outstanding == 0 && runtime.snapshot().queued == 1);
    auto cancelled = runtime.cancel(runtime.store().host_actor(Role::Root, second_id),
        number(runtime.store().request(second_id), "state_version"), new_id("cancel"));
    until(runtime, [&] { return ready(cancelled) && ready(second); });
    (void)cancelled.get(); test::throws<Error>([&] { (void)second.get(); });
    CHECK(!ready(first) && runtime.snapshot().executing == 1);
    CHECK(runtime.snapshot().managers.running == 1 && runtime.snapshot().workers.running == 0);
    CHECK(first_model->calls == 1 && second_model->calls == 0);
    CHECK(runtime.store().request(first_id).at("plan_revision") == 0);
    CHECK(runtime.store().request(second_id).at("status") == "CANCELLED");
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 1);
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 1);
    check_undispatched_observation(runtime, second_call);
    // A new waiter cannot steal the same key merely because the cancelled
    // non-head left. The original, still-running head remains authoritative.
    const auto third_id = f.admit(runtime.store());
    auto third_model = f.model();
    auto third = runtime.submit(runtime.capture(third_id, Role::Dispatcher), third_model,
        WorkClass::Review, "cancel-resource");
    (void)runtime.poll();
    CHECK(third_model->calls == 0 && runtime.snapshot().resource_blocked == 1);
    CHECK(runtime.snapshot().managers.outstanding == 1);
    gate->release(); runtime.drain(); (void)first.get(); (void)third.get();
    CHECK(runtime.store().request(first_id).at("plan_revision") == 1);
    CHECK(runtime.store().request(third_id).at("plan_revision") == 1 && third_model->calls == 1);
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 2);
    CHECK(runtime.snapshot().outstanding == 0);
}

void resource_waiter_close_records_original_unknown() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    const auto first_id = f.admit(runtime.store()), second_id = f.admit(runtime.store());
    const auto task = f.ready(runtime.store(), second_id);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    auto first_model = f.model(gate, true), second_model = f.model();
    const auto first_call = runtime.capture(first_id, Role::Dispatcher);
    const auto second_call = runtime.capture(second_id, Role::Worker, task);
    auto first = runtime.submit(first_call, first_model, WorkClass::Normal, "close-resource");
    auto second = runtime.submit(second_call, second_model, WorkClass::Normal, "close-resource");
    until(runtime, [&] { return gate->entered == 1; });
    CHECK(runtime.snapshot().workers.outstanding == 0 && runtime.snapshot().queued == 1);
    runtime.close();
    test::throws<Error>([&] { (void)first.get(); });
    test::throws<Error>([&] { (void)second.get(); });
    CHECK(first_model->calls == 1 && second_model->calls == 0);
    CHECK(runtime.snapshot().outstanding == 0 && runtime.snapshot().retained_results == 0);
    CHECK(runtime.snapshot().managers.running == 0 && runtime.snapshot().workers.running == 0);
    CHECK(!runtime.snapshot().accepting);
    CHECK(runtime.store().tasks(first_id).empty());
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 1);
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 1);
    CHECK(runtime.store().attempt(first_call.request.attempt_id).at("request_envelope") == to_json(first_call.request));
    CHECK(runtime.store().attempt(first_call.request.attempt_id).at("response").at("status") == "completed");
    CHECK(runtime.store().tool_feedback(first_call.request.attempt_id).at("result").at("observation_complete") == false);
    check_undispatched_observation(runtime, second_call);
}

void resource_waiter_foreign_destruction_preserves_pending_attempts() {
    Fixture f;
    auto runtime = std::make_unique<AsyncCoordinator>(f.open(), schemas);
    const auto first_id = f.admit(runtime->store()), second_id = f.admit(runtime->store());
    const auto task = f.ready(runtime->store(), second_id);
    const auto first_call = runtime->capture(first_id, Role::Dispatcher);
    const auto second_call = runtime->capture(second_id, Role::Worker, task);
    auto first_model = f.model(), second_model = f.model();
    auto first = runtime->submit(first_call, first_model, WorkClass::Normal, "destroy-resource");
    auto second = runtime->submit(second_call, second_model, WorkClass::Normal, "destroy-resource");
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (runtime->snapshot().awaiting_claim != 1) {
        CHECK(std::chrono::steady_clock::now() < deadline); std::this_thread::yield();
    }
    CHECK(runtime->snapshot().workers.outstanding == 0 && runtime->snapshot().queued == 1);
    auto destroyed = std::async(std::launch::async, [owned = std::move(runtime)]() mutable { owned.reset(); });
    CHECK(destroyed.wait_for(std::chrono::seconds(5)) == std::future_status::ready);
    destroyed.get();
    test::throws<std::future_error>([&] { (void)first.get(); });
    test::throws<std::future_error>([&] { (void)second.get(); });
    CHECK(first_model->calls == 0 && second_model->calls == 0);
    CHECK(count(f.path, "SELECT count(*) FROM call_attempts WHERE status='PENDING'") == 2);
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 0);
    auto recovered = f.open();
    for (const auto& call : {first_call, second_call}) {
        const auto attempt = recovered->attempt(call.request.attempt_id);
        CHECK(attempt.at("status") == "UNKNOWN" && attempt.at("response").is_null());
        CHECK(attempt.at("request_envelope") == to_json(call.request));
        CHECK(recovered->tool_feedback(call.request.attempt_id).is_null());
        CHECK(recovered->budget(call.request.binding.request_id).at("reserved_input").as_int64() > 0);
        test::throws<Error>([&] { (void)recovered->claim_offline_call(call.actor, call.request.attempt_id); });
    }
    CHECK(recovered->tasks(first_id).empty());
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 1);
}

void resource_waiter_cannot_bypass_persistence_failure() {
    // These are two fault points in one lifecycle regression, not independent
    // suites: before original settlement, and after the effect before feedback.
    for (const bool feedback_fault : {false, true}) {
        Fixture f; AsyncCoordinator runtime(f.open(), schemas);
        const auto first_id = f.admit(runtime.store()), second_id = f.admit(runtime.store());
        const auto task = f.ready(runtime.store(), first_id);
        auto gate = std::make_shared<Gate>(); Release release{gate};
        const auto first_call = runtime.capture(first_id, Role::Worker, task);
        const auto second_call = runtime.capture(second_id, Role::Dispatcher);
        auto first_model = f.model(gate), second_model = f.model();
        auto first = runtime.submit(first_call, first_model, WorkClass::Normal, "retained-resource");
        auto second = runtime.submit(second_call, second_model, WorkClass::Review, "retained-resource");
        until(runtime, [&] { return gate->entered == 1; });
        CHECK(runtime.snapshot().managers.outstanding == 0 && runtime.snapshot().queued == 1);
        sql(f.path, feedback_fault
            ? "CREATE TRIGGER resource_fault BEFORE INSERT ON model_tool_feedback BEGIN SELECT RAISE(ABORT,'original feedback fault'); END;"
            : "CREATE TRIGGER resource_fault BEFORE UPDATE OF settlement_sha256 ON call_attempts BEGIN SELECT RAISE(ABORT,'original settlement fault'); END;");
        gate->release(); test::throws<Error>([&] { runtime.drain(); });
        CHECK(runtime.snapshot().settlement_halted && !runtime.snapshot().accepting);
        CHECK(runtime.snapshot().retained_results == 2 && runtime.snapshot().outstanding == 2);
        CHECK(first_model->calls == 1 && second_model->calls == 0);
        CHECK(!ready(first) && !ready(second));
        const auto original = runtime.retained_results();
        CHECK(original.at(0).at("response").at("binding") == to_json(first_call.request.binding));
        CHECK(original.at(0).at("response").at("status") == "completed");
        CHECK(original.at(1).at("response").at("error_code") == "host_not_dispatched");
        const auto operation_count = count(f.path, "SELECT count(*) FROM ops");
        CHECK(operation_count == (feedback_fault ? 2 : 1));
        CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 1);
        CHECK(runtime.store().tasks(second_id).empty());
        (void)runtime.poll();
        CHECK(runtime.retained_results() == original && second_model->calls == 0);
        sql(f.path, "DROP TRIGGER resource_fault");
        runtime.flush_retained_evidence();
        test::throws<Error>([&] { (void)first.get(); });
        test::throws<Error>([&] { (void)second.get(); });
        CHECK(runtime.store().attempt(first_call.request.attempt_id).at("response") == original.at(0).at("response"));
        CHECK(runtime.store().attempt(first_call.request.attempt_id).at("request_envelope") == to_json(first_call.request));
        CHECK(runtime.store().tool_feedback(first_call.request.attempt_id).at("result").at("observation_complete") == false);
        check_undispatched_observation(runtime, second_call);
        CHECK(count(f.path, "SELECT count(*) FROM ops") == operation_count);
        CHECK(first_model->calls == 1 && second_model->calls == 0);
        CHECK(runtime.snapshot().outstanding == 0 && runtime.snapshot().settlement_halted);
        CHECK(runtime.snapshot().managers.outstanding == 0 && runtime.snapshot().workers.outstanding == 0);
        test::throws<Error>([&] { (void)runtime.capture(second_id, Role::Dispatcher); });
    }
}

void resource_fifo_spans_normal_review_handoff_and_allows_siblings() {
    Fixture f;
    AsyncCoordinatorLimits limits;
    limits.management_workers = 2; limits.worker_workers = 2;
    AsyncCoordinator runtime(f.open(), schemas, limits);
    const auto normal_id = f.admit(runtime.store()), review_id = f.admit(runtime.store());
    const auto handoff_id = f.admit(runtime.store()), sibling_id = f.admit(runtime.store());
    const auto normal_task = f.ready(runtime.store(), normal_id);
    const auto handoff_task = f.ready(runtime.store(), handoff_id);
    auto normal_gate = std::make_shared<Gate>(), review_gate = std::make_shared<Gate>();
    auto handoff_gate = std::make_shared<Gate>();
    Release normal_release{normal_gate}, review_release{review_gate}, handoff_release{handoff_gate};
    auto normal_model = f.model(normal_gate), review_model = f.model(review_gate);
    auto handoff_model = f.model(handoff_gate), sibling_model = f.model();
    const auto normal_call = runtime.capture(normal_id, Role::Worker, normal_task);
    const auto review_call = runtime.capture(review_id, Role::Dispatcher);
    const auto handoff_call = runtime.capture(handoff_id, Role::Worker, handoff_task);
    auto normal = runtime.submit(normal_call, normal_model, WorkClass::Normal, "class-shared-resource");
    auto review = runtime.submit(review_call, review_model, WorkClass::Review, "class-shared-resource");
    auto handoff = runtime.submit(handoff_call, handoff_model, WorkClass::Handoff, "class-shared-resource");
    auto sibling = runtime.submit(runtime.capture(sibling_id, Role::Dispatcher), sibling_model,
        WorkClass::Normal, "independent-sibling-resource");
    until(runtime, [&] { return normal_gate->entered == 1 && ready(sibling); });
    (void)sibling.get();
    CHECK(runtime.store().request(sibling_id).at("plan_revision") == 1 && sibling_model->calls == 1);
    CHECK(review_model->calls == 0 && handoff_model->calls == 0);
    CHECK(runtime.snapshot().queued == 2 && runtime.snapshot().resource_blocked == 2);
    CHECK(runtime.snapshot().managers.outstanding == 0 && runtime.snapshot().workers.outstanding == 1);
    CHECK(runtime.snapshot().workers.normal_running == 1);
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 2);
    normal_gate->release();
    until(runtime, [&] { return review_gate->entered == 1; });
    CHECK(ready(normal)); CHECK(normal.get().at("status") == "SUCCEEDED");
    CHECK(!runtime.store().tool_feedback(normal_call.request.attempt_id).is_null());
    CHECK(handoff_model->calls == 0 && runtime.snapshot().resource_blocked == 1);
    CHECK(runtime.snapshot().workers.outstanding == 0 && runtime.snapshot().managers.running == 1);
    CHECK(runtime.snapshot().managers.normal_running == 0);
    review_gate->release();
    until(runtime, [&] { return handoff_gate->entered == 1; });
    CHECK(ready(review)); (void)review.get();
    CHECK(runtime.store().request(review_id).at("plan_revision") == 1);
    CHECK(!runtime.store().tool_feedback(review_call.request.attempt_id).is_null());
    CHECK(runtime.snapshot().workers.running == 1 && runtime.snapshot().workers.normal_running == 0);
    handoff_gate->release(); runtime.drain(); CHECK(handoff.get().at("status") == "SUCCEEDED");
    CHECK(normal_model->calls == 1 && review_model->calls == 1 && handoff_model->calls == 1);
    for (const auto& call : {normal_call, review_call, handoff_call}) {
        const auto attempt = runtime.store().attempt(call.request.attempt_id);
        CHECK(attempt.at("status") == "COMPLETED" && attempt.at("accepted") == 1);
        CHECK(attempt.at("request_envelope") == to_json(call.request));
        CHECK(runtime.store().tool_feedback(call.request.attempt_id).at("response") == attempt.at("response"));
    }
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 4);
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 4);
    CHECK(runtime.snapshot().outstanding == 0);
}

void resource_waiter_amend_preserves_original_capture_and_predecessor() {
    Fixture f; AsyncCoordinator runtime(f.open(), schemas);
    const auto first_id = f.admit(runtime.store()), second_id = f.admit(runtime.store());
    const auto task = f.ready(runtime.store(), second_id);
    auto gate = std::make_shared<Gate>(); Release release{gate};
    auto first_model = f.model(gate), second_model = f.model();
    const auto first_call = runtime.capture(first_id, Role::Dispatcher);
    auto first = runtime.submit(first_call, first_model, WorkClass::Normal, "amend-resource");
    const auto second_call = runtime.capture(second_id, Role::Worker, task);
    auto second = runtime.submit(second_call, second_model, WorkClass::Handoff, "amend-resource");
    until(runtime, [&] { return gate->entered == 1; });
    CHECK(runtime.snapshot().workers.outstanding == 0 && runtime.snapshot().queued == 1);
    const auto predecessor_request = runtime.store().request(first_id);
    const auto predecessor_budget = runtime.store().budget(first_id);
    const auto predecessor_attempt = runtime.store().attempt(first_call.request.attempt_id);
    auto amended = runtime.amend(runtime.store().host_actor(Role::Root, second_id),
        number(runtime.store().request(second_id), "state_version"),
        read_runtime_file(source / "tests/fixtures/s1_amendment.txt"),
        fixture("s1_amended_constraints.json"), new_id("amend"));
    until(runtime, [&] { return ready(amended) && ready(second); });
    CHECK(amended.get().at("requirements_revision") == 2);
    test::throws<Error>([&] { (void)second.get(); });
    CHECK(first_model->calls == 1 && second_model->calls == 0 && !ready(first));
    CHECK(runtime.snapshot().executing == 1 && runtime.snapshot().managers.running == 1);
    CHECK(runtime.snapshot().workers.outstanding == 0);
    CHECK(runtime.store().request(first_id) == predecessor_request);
    CHECK(runtime.store().budget(first_id) == predecessor_budget);
    CHECK(runtime.store().attempt(first_call.request.attempt_id) == predecessor_attempt);
    CHECK(second_call.request.binding.requirements_revision == 1);
    CHECK(runtime.store().request(second_id).at("requirements_revision") == 2);
    check_undispatched_observation(runtime, second_call);
    CHECK(count(f.path, "SELECT count(*) FROM events WHERE type='claim_offline_call'") == 1);
    CHECK(count(f.path, "SELECT count(*) FROM ops") == 1);
    gate->release(); runtime.drain(); (void)first.get();
    CHECK(runtime.store().request(first_id).at("plan_revision") == 1);
    CHECK(runtime.snapshot().outstanding == 0 && second_model->calls == 0);
}
