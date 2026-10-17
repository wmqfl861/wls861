#include "wls/runtime_types.hpp"
#include "test_support.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <mutex>
#include <set>
#include <thread>

namespace {
using namespace wls::s1;

void strict_json() {
    CHECK(parse_json_strict("  {\"中文\": [true, null, -2, 1.5]}\n").is_object());
    for (const std::string text : {"{\"a\":1,\"a\":2}", "{\"a\":1,\"\\u0061\":2}",
         "[{\"b\":1,\"b\":2}]", "{\"a\":{\"x\":1,\"x\":2}}", "{}{}", "{} x", "[1,]", "//x\n1",
         "1e400", "NaN", "Infinity", "\"\\uD800\"", "", " ", "{\"unterminated\":1"})
        test::throws<Error>([&] { (void)parse_json_strict(text); });
    const std::string invalid_utf8 = std::string("\"") + static_cast<char>(0xc0) + static_cast<char>(0xaf) + "\"";
    test::throws<Error>([&] { (void)parse_json_strict(invalid_utf8); });
    CHECK(parse_json_strict(R"({"same":1,"nested":{"same":2},"last":{"same":3}})").is_object());
    test::throws<Error>([] { (void)parse_json_strict("[1,2]", 4); });
    test::throws<Error>([] { (void)parse_json_strict("[[[]]]", 100, 2); });
    CHECK(parse_json_strict("18446744073709551615").as_uint64() == std::numeric_limits<std::uint64_t>::max());
}

void canonical_and_digest() {
    const auto left = parse_json_strict(R"({"z":[3,2,1],"a":{"y":2,"x":1}})");
    const auto right = parse_json_strict(R"({"a":{"x":1,"y":2},"z":[3,2,1]})");
    CHECK(canonical_json(left) == canonical_json(right));
    CHECK(canonical_json(left) == R"({"a":{"x":1,"y":2},"z":[3,2,1]})");
    CHECK(sha256_hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(sha256_hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(sha256_hex(std::string("a\0b", 3)) != sha256_hex("a"));
    test::throws<Error>([] { (void)canonical_json(Json(std::numeric_limits<double>::infinity())); });
    test::throws<Error>([] { (void)canonical_json(Json(std::string(1, static_cast<char>(0xff)))); });
}

void typed_ulids() {
    auto previous = new_id("req");
    CHECK(is_typed_id(previous, "req"));
    for (int i = 0; i < 1000; ++i) {
        const auto next = new_id("req");
        CHECK(next > previous);
        previous = next;
    }
    CHECK(!is_typed_id(previous, "tsk"));
    CHECK(!is_typed_id("req_8ZZZZZZZZZZZZZZZZZZZZZZZZZ", "req"));
    CHECK(!is_typed_id("req_01ARZ3NDEKTSV4RRFFQ69G5FAI", "req"));
    CHECK(!is_typed_id("req_01arz3ndektsv4rrffq69g5fav", "req"));
    test::throws<Error>([] { (void)new_id("not/a/prefix"); });
    test::throws<Error>([] { (void)new_id(""); });
    std::mutex mutex;
    std::set<std::string> ids;
    std::array<std::jthread, 4> threads;
    for (auto& thread : threads) thread = std::jthread([&] {
        for (int i = 0; i < 500; ++i) {
            const auto id = new_id("evt");
            std::lock_guard lock(mutex);
            ids.insert(id);
        }
    });
    for (auto& thread : threads) thread.join();
    CHECK(ids.size() == 2000);
}

void binding_round_trip_and_boundaries() {
    ExecutionBinding request;
    request.request_id = new_id("req");
    request.execution_epoch = 0;
    CHECK(execution_binding_from_json(to_json(request)) == request);
    auto task = request;
    task.task_id = new_id("tsk"); task.team_id = new_id("team");
    task.plan_revision = 1; task.task_revision = 1; task.execution_epoch = 1;
    CHECK(execution_binding_from_json(to_json(task)) == task);
    task.instruction_revision = 2; task.requirements_revision = 3;
    CHECK(execution_binding_from_json(parse_json_strict(canonical_json(to_json(task)))) == task);
    auto forged = to_json(task);
    forged.as_object()["extra_authority"] = true;
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    forged = to_json(task); forged.as_object()["policy_epoch"] = std::numeric_limits<std::uint64_t>::max();
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    forged = to_json(task); forged.as_object()["task_revision"] = 1.0;
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    forged = to_json(request); forged.as_object()["instruction_revision"] = 1;
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    for (const auto name : {"request_id", "task_id", "team_id"}) {
        forged = to_json(task); forged.as_object()[name] = "nonempty-but-not-a-typed-id";
        test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    }
    forged = to_json(task); forged.as_object()["task_id"] = task.team_id;
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    forged = to_json(task); forged.as_object()["request_id"] = task.request_id + std::string(1, '\0');
    test::throws<Error>([&] { (void)execution_binding_from_json(forged); });
    CHECK(role_from_string("team_lead") == wls::Role::TeamLead);
    test::throws<Error>([] { (void)role_from_string("admin"); });
}
} // namespace

int main() {
    return test::run({
        {"strict JSON rejects duplicates, bad UTF-8 and non-finite input", strict_json},
        {"canonical bytes and SHA-256 preserve content identity", canonical_and_digest},
        {"typed ULIDs are monotonic and unique across threads", typed_ulids},
        {"execution binding round trips keep request and task scopes distinct", binding_round_trip_and_boundaries}
    });
}
