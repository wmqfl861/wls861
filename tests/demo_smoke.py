#!/usr/bin/env python3
"""Verify the compiled synthetic CLI's JSON and exit behavior, using only stdlib."""

import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def check(condition, message):
    if not condition:
        raise AssertionError(message)


def run(binary, arguments, environment):
    return subprocess.run(
        [str(binary), *arguments],
        capture_output=True,
        text=True,
        timeout=30,
        env=environment,
        check=False,
    )


def verify_summary(summary):
    check(summary["execution_mode"] == "synthetic/no_model", "execution must be synthetic/no_model")
    check(summary["model_calls"] == 0, "demo must not call a model")
    check(summary["requests"] == 2, "demo must admit two independent requests")
    check(summary["peak_parallel"] >= 2, "independent Normal callbacks must overlap")
    for key in (
        "all_completed",
        "all_resources_closed",
        "worker_inherits_frozen_root",
        "dispatcher_override_is_independent",
        "temporary_workspace_removed",
    ):
        check(summary[key] is True, f"{key} must be true")

    requests = summary["request_states"]
    check(len(requests) == 2, "summary must contain both actual request snapshots")
    check(len({request["request_id"] for request in requests}) == 2, "request identities must differ")
    check(len({request["dispatcher_id"] for request in requests}) == 2, "Dispatcher identities must differ")
    check({request["root_id"] for request in requests} == {"demo-root"}, "both requests must share Root")
    check(summary["event_count"] == sum(len(request["event_kinds"]) for request in requests),
          "global event count must match the actual per-request events")

    worker_ids = set()
    expected_events = [
        "RequestReceived", "PlanCommitted", "TeamCreated", "TeamCreated",
        "TeamInstructionsCommitted", "RequirementsApplied",
        "TeamInstructionsCommitted", "RequirementsApplied",
        "OperationPrepared", "OperationStarted", "OperationFinished",
        "CandidateSubmitted", "ReviewRecorded", "CandidateAccepted",
        "HandoffPrepared", "DeliveryConfirmed",
        "ResourceStateChanged", "ResourceStateChanged", "ResourceStateChanged",
    ]
    for request in requests:
        check(request["state"] == "COMPLETED", "business state must actually be COMPLETED")
        check(request["resources"] == "CLOSED", "resource state must actually be CLOSED")
        check(request["accepted"] is True, "candidate must have been accepted")
        check(request["requirements_revision"] == 1 and request["plan_revision"] == 1,
              "baseline requirements and plan must remain distinct recorded revisions")
        check(request["state_version"] > request["plan_revision"], "state mutations must advance state version")
        check(request["lease_epoch"] >= 2, "completion must fence the earlier execution epoch")
        check(request["event_kinds"] == expected_events, "recorded events must show the complete workflow in order")

        models = request["model_configuration"]
        check(models["frozen_root"] == {
            "model": "mock-root", "reasoning": {"mode": "explicit", "effort": "medium"}
        }, "request must retain the specified frozen Root configuration")
        check(models["worker"] == models["frozen_root"], "Worker must inherit frozen Root fields")
        check(models["dispatcher"]["model"] == "mock-dispatcher", "Dispatcher override must resolve")
        check(models["dispatcher"]["reasoning"]["effort"] == "high", "Dispatcher reasoning override must remain independent")

        teams = {team["team_id"]: team for team in request["teams"]}
        check(set(teams) == {"build", "review"}, "each request must have separate build and review teams")
        check(teams["build"]["review_team"] is False and teams["review"]["review_team"] is True,
              "review must use a separately assigned review team")
        for team in teams.values():
            check(team["worker_id"] != team["lead_id"], "TeamLead and Worker identities must differ")
            check(team["worker_id"] not in worker_ids, "Workers cannot be shared across request/team bindings")
            worker_ids.add(team["worker_id"])
            check(team["applied_revision"] == request["requirements_revision"], "Worker must apply current requirements")
            check(team["applied_plan_revision"] == request["plan_revision"], "Worker must apply current plan")
            check(team["applied_instructions_version"] == team["instructions_version"] > 0,
                  "Worker must acknowledge TeamLead instructions")

        operations = request["operations"]
        check(len(operations) == 1, "each request must simulate exactly one operation")
        operation = operations[0]
        check(operation["tool_id"] == "test.echo" and operation["team_id"] == "build",
              "build Worker must own the synthetic echo operation")
        check(operation["state"] == "SUCCEEDED" and operation["payload"] == "hello",
              "synthetic operation must have an actual succeeded record")
        check(operation["evidence_ref"].startswith("synthetic:"), "operation evidence must be explicitly synthetic")
        candidate = request["candidate"]
        check(candidate["content_hash"] == hashlib.sha256(operation["payload"].encode()).hexdigest(),
              "candidate hash must match the synthetic fixture bytes")
        check(candidate["artifact_version"] == 1 and candidate["producer_team_id"] == "build",
              "candidate must bind artifact version and producer")
        check(candidate["requirements_revision"] == request["requirements_revision"],
              "candidate must bind current requirements")
        reviews = request["reviews"]
        check(len(reviews) == 1 and reviews[0]["passed"] is True, "current candidate needs one passing review")
        check(reviews[0]["reviewer_team_id"] == "review" and reviews[0]["candidate"] == candidate,
              "independent review must bind the exact current candidate")
        check(reviews[0]["evidence_ref"].startswith("synthetic:"), "review evidence must be explicitly synthetic")
        check(request["package_ref"] == "synthetic:package:" + request["request_id"],
              "package must bind the current request")
        check(request["receipt_ref"] == "synthetic:receipt:" + request["request_id"],
              "completion must have an explicit request-bound synthetic receipt")


def main():
    check(len(sys.argv) == 2, "usage: demo_smoke.py <compiled-cli-path>")
    binary = Path(sys.argv[1]).resolve()
    check(binary.is_file(), "compiled CLI does not exist")
    with tempfile.TemporaryDirectory(prefix="wls861-cli-smoke-") as fixture:
        temporary_root = Path(fixture) / "private-demo-parent"
        temporary_root.mkdir()
        environment = os.environ.copy()
        environment["TMPDIR"] = str(temporary_root)

        result = run(binary, ["demo"], environment)
        check(result.returncode == 0, f"demo failed: {result.stderr}")
        check(result.stderr == "", "successful demo must not write errors")
        verify_summary(json.loads(result.stdout))
        check(not list(temporary_root.iterdir()), "demo must remove only its own temporary workspace on completion")
        print("PASS compiled demo completes two parallel synthetic request lifecycles")

        help_result = run(binary, ["--help"], environment)
        check(help_result.returncode == 0 and help_result.stderr == "", "--help must succeed")
        for text in ("demo", "--help", "--version", "synthetic"):
            check(text in help_result.stdout, f"help must describe {text}")
        version_result = run(binary, ["--version"], environment)
        check(version_result.returncode == 0 and version_result.stderr == "", "--version must succeed")
        check(version_result.stdout.startswith("wls861 ") and "m1" in version_result.stdout.lower(),
              "version must identify the M1 CLI")
        print("PASS CLI help and version")

        forbidden_path = Path(fixture) / "unrequested.journal"
        for arguments in (["unknown-command"], ["demo", "unexpected"],
                          ["demo", "--journal", str(forbidden_path)]):
            invalid = run(binary, arguments, environment)
            check(invalid.returncode != 0 and invalid.stderr, "unsupported command/options must fail explicitly")
            check(not invalid.stdout, "unsupported commands must not report successful demo JSON")
        check(not forbidden_path.exists(), "unsupported persistent journal option must not create a file")
        failure_environment = environment.copy()
        failure_environment["TMPDIR"] = str(Path(fixture) / "missing-directory")
        failed_demo = run(binary, ["demo"], failure_environment)
        check(failed_demo.returncode != 0 and failed_demo.stderr, "runtime setup failure must return nonzero")
        check(not failed_demo.stdout, "failed runtime must not emit a success summary")
        print("PASS CLI invalid input and runtime setup failure return nonzero")
    print("RESULT 3 passed, 0 failed")


if __name__ == "__main__":
    try:
        main()
    except (AssertionError, KeyError, TypeError, ValueError, OSError, subprocess.TimeoutExpired) as error:
        print(f"FAIL demo smoke: {error}", file=sys.stderr)
        sys.exit(1)
