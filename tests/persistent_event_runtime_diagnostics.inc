// Test-only observations for the unchanged first five-second concurrency wait.
// The original until helper and its assertion remain the deadline authority.
struct DeadlinePollObservation {
    PersistentEventRuntime& runtime;
    std::chrono::steady_clock::time_point started{std::chrono::steady_clock::now()};
    std::uint64_t polls{}, progressed{};
    std::chrono::nanoseconds total{}, last{}, longest{};

    void record(std::chrono::steady_clock::time_point begin) noexcept {
        last = std::chrono::steady_clock::now() - begin;
        total += last;
        longest = std::max(longest, last);
        ++polls;
    }
    std::size_t poll() {
        const auto begin = std::chrono::steady_clock::now();
        try {
            const auto result = runtime.poll();
            record(begin);
            progressed += static_cast<std::uint64_t>(result);
            return result;
        } catch (...) {
            record(begin);
            throw;
        }
    }
};

template<class Read>
void failed_wait_observation(std::string_view label, Read read) noexcept {
    try {
        const auto value = read();
        std::cerr << "WAIT-OBSERVATION " << label << ' ' << boost::json::serialize(value) << std::endl;
    } catch (...) {
        try { std::cerr << "WAIT-OBSERVATION " << label << " unavailable" << std::endl; } catch (...) {}
    }
}

void observe_failed_concurrency_wait(PersistentEventRuntime& runtime,
        const std::filesystem::path& path, const std::shared_ptr<Gate>& gate,
        const std::vector<std::shared_ptr<GatedModel>>& models,
        const std::string& first, const std::string& second,
        const DeadlinePollObservation& timing, std::exception_ptr original) noexcept {
    // Flush the original failure before any additional read or serialization.
    try {
        try { std::rethrow_exception(original); }
        catch (const std::exception& error) {
            std::cerr << "WAIT-FAILURE scenario=two_requests_two_nodes phase=before_gate_release original_exception="
                      << error.what() << std::endl;
        } catch (...) {
            std::cerr << "WAIT-FAILURE scenario=two_requests_two_nodes phase=before_gate_release original_exception=non_std"
                      << std::endl;
        }
    } catch (...) {}
    failed_wait_observation("host-polls", [&] {
        return Json(Object{{"polls", timing.polls}, {"progressed", timing.progressed},
            {"total_ns", timing.total.count()}, {"last_ns", timing.last.count()},
            {"longest_ns", timing.longest.count()}, {"elapsed_ns",
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    std::chrono::steady_clock::now() - timing.started).count()}});
    });
    failed_wait_observation("callbacks", [&] {
        std::int64_t entries = 0, responses = 0;
        for (const auto& model : models) {
            entries += model->calls.load();
            if (!model->original().is_null()) ++responses;
        }
        return Json(Object{{"gate_entered", gate->entered.load()},
            {"models_created", static_cast<std::uint64_t>(models.size())},
            {"complete_entries", entries}, {"responses_retained", responses}});
    });
    failed_wait_observation("successful-notes", [&] {
        sqlite3* raw{};
        const auto opened = sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READONLY, nullptr);
        const std::unique_ptr<sqlite3, decltype(&sqlite3_close)> database(raw, sqlite3_close);
        CHECK(opened == SQLITE_OK);
        sqlite3_stmt* statement{};
        const auto prepared = sqlite3_prepare_v2(database.get(),
            "SELECT count(*) FROM ops WHERE status='SUCCEEDED'", -1, &statement, nullptr);
        const std::unique_ptr<sqlite3_stmt, decltype(&sqlite3_finalize)> query(statement, sqlite3_finalize);
        CHECK(prepared == SQLITE_OK);
        CHECK(sqlite3_step(query.get()) == SQLITE_ROW);
        const auto count = static_cast<std::int64_t>(sqlite3_column_int64(query.get(), 0));
        CHECK(sqlite3_step(query.get()) == SQLITE_DONE);
        return Json(Object{{"count", count}});
    });
    failed_wait_observation("coordinator", [&] {
        const auto state = runtime.snapshot();
        return Json(Object{{"outstanding", static_cast<std::uint64_t>(state.outstanding)},
            {"queued", static_cast<std::uint64_t>(state.queued)},
            {"awaiting_claim", static_cast<std::uint64_t>(state.awaiting_claim)},
            {"executing", static_cast<std::uint64_t>(state.executing)},
            {"ready_results", static_cast<std::uint64_t>(state.ready_results)},
            {"retained_results", static_cast<std::uint64_t>(state.retained_results)},
            {"resource_blocked", static_cast<std::uint64_t>(state.resource_blocked)},
            {"dispatches", state.dispatches}, {"accepting", state.accepting},
            {"settlement_halted", state.settlement_halted}});
    });
    std::int64_t request_index = 0;
    for (const auto* id : {&first, &second}) {
        failed_wait_observation("request", [&] {
            const auto report = runtime.report(*id);
            Array calls, blocked;
            Object link_states;
            for (const auto& call : report.at("calls").as_array())
                calls.push_back(Object{{"role", call.at("role")}, {"node", call.at("node_id")},
                    {"completion_observed", call.at("completion_observed")}, {"error", call.at("error")}});
            for (const auto& item : report.at("blocked").as_array())
                blocked.push_back(Object{{"role", item.at("role")}, {"node", item.at("node_id")},
                    {"code", item.at("code")}});
            for (const auto& link : report.at("links").as_array()) {
                const auto state = text(link, "state");
                if (auto* count = link_states.if_contains(state)) *count = count->as_int64() + 1;
                else link_states[state] = 1;
            }
            // No Actor/session, request/call IDs, envelope, response or prompt bytes.
            return Json(Object{{"request_index", request_index}, {"calls", std::move(calls)},
                {"blocked", std::move(blocked)}, {"link_states", std::move(link_states)},
                {"pending_count", static_cast<std::uint64_t>(report.at("pending").as_array().size())}});
        });
        ++request_index;
    }
}
