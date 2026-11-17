"""Fixed CSV and function FakeModel requests; native mode is hosted-only."""
import argparse
import base64
import errno
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


def exercise(binary, source, output, native, scenario="csv"):
    assert scenario in ("csv", "function")
    worker_calls = 3 if scenario == "csv" else 4
    output.mkdir(mode=0o700)
    evidence, private = output / "evidence", output / "private"
    evidence.mkdir(mode=0o700)
    private.mkdir(mode=0o700)
    fixture = source / "tests/fixtures/g4"
    sys.path.insert(0, str(source / "scripts"))
    if scenario == "csv":
        from check_g4d_csv import check_output
    else:
        from check_g4d_function import check_output

    def save(name, value):
        with (evidence / name).open("x", encoding="utf-8") as stream:
            json.dump(value, stream, ensure_ascii=False, indent=2)
        return evidence / name

    def cli(label, *arguments):
        argv = [str(binary), *(str(arg) for arg in arguments), "--source-root", str(source)]
        run = subprocess.run(argv, capture_output=True, timeout=30)
        save(label + ".json", {"argv": argv, "returncode": run.returncode})
        (evidence / (label + ".stdout")).write_bytes(run.stdout)
        (evidence / (label + ".stderr")).write_bytes(run.stderr)
        assert run.returncode == 0, (label, run.stderr.decode(errors="replace"))
        return json.loads(run.stdout)

    programs = ("csv-transform.py", "csv-expected.csv") if scenario == "csv" else ("clamp.py", "test_clamp.py")
    for name in (f"{scenario}-task.txt", f"{scenario}-constraints.json", *programs):
        shutil.copyfile(fixture / name, evidence / name)
    work, inputs = private / "work", private / "input"
    work.mkdir(mode=0o700)
    system = (source / "config/examples/system.yaml").read_text(encoding="utf-8")
    if scenario == "csv":
        inputs.mkdir(mode=0o700)
        original = (fixture / "input/orders.csv").read_bytes()
        (inputs / "orders.csv").write_bytes(original)
        (evidence / "orders.csv").write_bytes(original)
        input_stat = inputs.stat()
        system = system.replace("sandbox:\n", f"sandbox:\n  input: {{mode: read-only-v1, max_total_bytes: {len(original)}}}\n")
    (evidence / "system.yaml").write_text(system, encoding="utf-8")
    functions = private / "functions"
    shutil.copytree(source / "config/examples/functions", functions)
    function = functions / "general/function.yaml"
    selected = function.read_text(encoding="utf-8").replace("  default:\n", "  files-original:\n", 1).replace("  shell:\n", "  default:\n", 1)
    function.write_text(selected, encoding="utf-8")
    shutil.copyfile(function, evidence / "function.yaml")
    manifest = {"schema_version": 1,
                "bwrap_path": native[0] if native else str(private / "absent-bwrap"),
                "bwrap_sha256": native[1] if native else "0" * 64,
                "cgroup_root": native[2] if native else str(private / "absent-cgroup")}
    if scenario == "csv":
        manifest["input_root"] = str(inputs)
    manifest_file = save("manifest.json", manifest)
    mapping = save("workspaces.json", {scenario: str(work)})
    script = json.loads((fixture / f"{scenario}-fake.json").read_text(encoding="utf-8"))
    for step in script["nodes"][scenario]:
        call = step["tool_calls"][0]
        if call["name"] == "fs_write":
            assert call["arguments"]["path"] in programs
            call["arguments"]["content"] = (fixture / call["arguments"]["path"]).read_text(encoding="utf-8")
    script_file = save("script.json", script)
    database = private / "runtime.sqlite"
    admitted = cli("admit", "submit", "--db", database, "--key", "g4d-" + scenario, "--config", evidence / "system.yaml",
                   "--functions-dir", functions, "--text-file", evidence / f"{scenario}-task.txt",
                   "--constraints-file", evidence / f"{scenario}-constraints.json", "--max-attempts", 6)
    request = admitted["request"]["request_id"]
    scope = ["--db", database, "--request", request]
    pump = cli("pump", "pump-fake", *scope, "--script", script_file, "--workspaces-file", mapping,
               "--sandbox-manifest", manifest_file, "--max-calls", worker_calls + 2, "--worker-calls", worker_calls)
    assert pump["real_model_calls"] == 0 and not pump["business_delivered"]
    task = cli("show", "show", *scope)["tasks"][0]
    history = cli("history", "history", *scope, "--role", "worker", "--task", task["task_id"], "--limit", 8)
    events = cli("events", "events", *scope, "--limit", 200)
    assert len(events) < 200
    submissions = cli("submissions", "submissions", *scope, "--task", task["task_id"])["submissions"]
    calls = [call for call in pump["calls"] if call["role"] == "worker"]
    run_call, = [call for call in calls if call["response"]["tool_calls"][0]["name"] == "run"]
    op = run_call["result"]["operation"]
    query_scope = [*scope, "--task", task["task_id"], "--workspace", work, "--sandbox-manifest", manifest_file]

    def query(tool, **arguments):
        label = tool + "-" + arguments.get("stream", "record")
        return cli(label, "tool-query", *query_scope, "--tool", tool, "--input", save(label + "-args.json", arguments))

    status = query("op_status", op_id=op["op_id"])
    record = status["execution_result"]
    assert status["operation"] == op and record == run_call["result"]["execution_result"]
    assert run_call["result"]["executed_now"] and not run_call["result"]["replayed"]
    assert op["arguments"] == run_call["response"]["tool_calls"][0]["arguments"]
    captured, = [item for item in history if item["attempt_id"] == run_call["attempt_id"]]
    binding = captured["frozen_binding"]
    assert captured["response"] == run_call["response"] and captured["feedback"]["original_binding"] == binding
    assert captured["feedback"]["result"] == run_call["result"] and record["original_binding"] == binding
    assert all(op[key] == value for key, value in binding.items()) and op["started_at"]
    assert op["request_id"] == request and op["task_id"] == task["task_id"] and op["tool_id"] == "run"
    claim, = [event for event in events if event["type"] == "start_tool_op" and event["payload"]["op_id"] == op["op_id"]]
    assert claim["result"]["dispatch_claimed"] and claim["result"]["operation"]["status"] == "RUNNING"
    assert claim["command_id"] == run_call["attempt_id"] + f":{scenario}-run-{worker_calls - 1}:start"
    workspace, = [event["result"] for event in events if event["type"] == "bind_workspace"]
    policy = workspace["execution_policy"]
    assert policy["config_snapshot_sha256"] == run_call["config_snapshot_sha256"] and policy["network"] == "none"
    process, proof = record["result"], record["evidence"]
    assert record["operation_status"] == op["status"] and process["evidence"] == proof
    if scenario == "csv":
        input_identity = {key: value for key, value in policy["input"].items() if key != "path"}
        assert (inputs / "orders.csv").read_bytes() == original
        assert (inputs.stat().st_dev, inputs.stat().st_ino) == (input_stat.st_dev, input_stat.st_ino)
        save("input-check.json", {"before_sha256": hashlib.sha256(original).hexdigest(),
                                  "after_sha256": hashlib.sha256((inputs / "orders.csv").read_bytes()).hexdigest(), "identity": input_identity})
    for stream in ("stdout", "stderr"):
        page = query("op_output", op_id=op["op_id"], stream=stream)
        raw = page["data"].encode() if page["encoding"] == "utf-8" else base64.b64decode(page["data"], validate=True)
        assert page["eof"] and not page["truncated"] and hashlib.sha256(raw).hexdigest() == record[stream + "_sha256"]
        (evidence / ("run." + stream)).write_bytes(raw)
    if not native:
        assert len(calls) == worker_calls - 1 and op["status"] == "FAILED" and process["status"] == "REFUSED"
        assert not process["spawned"] and not submissions and not (work / "summary.csv").exists()
        save("summary.json", {"status": "PASS", "stage": "Worker run pre-spawn refusal", "native_executed": False, "real_model_calls": 0})
        return
    assert len(calls) == worker_calls and op["status"] == "SUCCEEDED" and process["exit_code"] == 0
    assert process["spawned"] and process["launcher_exec_confirmed"] and process["all_descendants_exited"]
    assert not process["strong_isolation"] and not process["output_truncated"]
    assert proof["proof_format"] == "wls-process-proof-standard-cgroup-v1" and proof["isolation_verified"]
    assert proof["network"] == "none" and proof["resource_enforcement"] == "cgroup-v2"
    assert proof["descendants_proof"] == "cgroup.events populated=0" and proof["workspace_observation"]["complete"]
    assert proof["operation_id"] == op["op_id"] and not (Path(native[2]) / process["cgroup_name"]).exists()
    if scenario == "csv":
        mount = proof["input_mount"]
        assert mount["verified"] and mount["read_only"] and mount["destination"] == "/input" and mount["audited_bytes"] == len(original)
        assert mount["identity"] == input_identity
        assert mount["identity"]["device"] == input_stat.st_dev and mount["identity"]["inode"] == input_stat.st_ino
        observed = json.loads((evidence / "run.stdout").read_bytes())
        assert observed["input_sha256"] == hashlib.sha256(original).hexdigest() and observed["input_bytes"] == len(original)
        assert observed["input_write_errno"] in (errno.EROFS, errno.EACCES, errno.EPERM)
    submission, = submissions
    assert submission["status"] == "SUBMITTED" and submission["original_binding"] == binding
    assert all(submission["body"][key] == value for key, value in binding.items())
    submitted, = [item for item in history if item["attempt_id"] == calls[-1]["attempt_id"]]
    assert calls[-1]["response"]["tool_calls"][0]["name"] == "submit" and submitted["frozen_binding"] == binding
    artifacts = submission["artifact_snapshot"]
    names = {"summary.csv"} if scenario == "csv" else {"clamp.py", "test_clamp.py"}
    assert len(artifacts) == len(names) and {item["path"] for item in artifacts} == {"/work/" + name for name in names}
    exported = private / "captured"
    exported.mkdir(mode=0o700)
    for artifact in artifacts:
        content = base64.b64decode(artifact["content_base64"], validate=True)
        assert artifact["size_bytes"] == len(content) and artifact["sha256"] == hashlib.sha256(content).hexdigest()
        if scenario == "csv":
            assert artifact["sha256"] == observed["output_sha256"]
        (exported / Path(artifact["path"]).name).write_bytes(content)
        (evidence / Path(artifact["path"]).name).write_bytes(content)
    verdict = check_output(exported) if scenario == "csv" else check_output(exported, process["exit_code"],
        (evidence / "run.stdout").read_bytes(), (evidence / "run.stderr").read_bytes())
    save("oracle.json", verdict)
    assert verdict["passed"]
    save("summary.json", {"status": "PASS", "stage": "Worker run to immutable submission", "native_executed": True,
                          "real_model_calls": 0, "request_id": request, "op_id": op["op_id"], "submission_id": submission["submission_id"]})


def main(scenario="csv"):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("source", type=Path)
    mode = parser.add_mutually_exclusive_group()
    mode.add_argument("--native", nargs=4, metavar=("BWRAP", "SHA256", "CGROUP_ROOT", "NEW_OUTPUT"))
    mode.add_argument("--evidence", type=Path, help="retain the local refusal fixture at a new path")
    args = parser.parse_args()
    assert os.geteuid() != 0
    os.umask(0o077)
    with tempfile.TemporaryDirectory(prefix="wls-g4d-csv-") as temporary:
        output = Path(args.native[3]) if args.native else args.evidence or Path(temporary) / "case"
        exercise(args.binary.resolve(), args.source.resolve(), output.resolve(), args.native, scenario)
    print("PASS G4-D " + scenario + " " + ("hosted native preparation" if args.native else "Worker pre-spawn refusal"))


if __name__ == "__main__":
    main()
