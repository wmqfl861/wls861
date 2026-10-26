#!/usr/bin/env python3
"""Read-only deployment diagnostics with real disposable files and SQLite.

No fixture changes the invoking umask. Fake launchers are never executed.
The cgroup directory below is deliberately ordinary, not deployment evidence.
"""
from contextlib import closing
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest

if len(sys.argv) != 3:
    raise SystemExit("usage: test_doctor_diagnostics_cli.py WLS SOURCE")
BINARY, SOURCE = (Path(x).resolve() for x in sys.argv[1:])
sys.argv = [sys.argv[0]]


class DoctorCli(unittest.TestCase):
    def setUp(self):
        self.assertNotEqual(os.getuid(), 0, "permission tests require an ordinary user")
        self.temporary = tempfile.TemporaryDirectory(prefix="wls-doctor-cli-")
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.directory.chmod(0o700)

    def invoke(self, *args, error=None, cwd=None, source=True):
        command = [str(BINARY), *(str(a) for a in args)]
        if source:
            command += ["--source-root", str(SOURCE)]
        result = subprocess.run(command, cwd=cwd or self.directory, capture_output=True,
                                text=True, encoding="utf-8", timeout=30, check=False)
        if error is not None:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, "")
            value = json.loads(result.stderr)
            self.assertEqual(value["error"], error, value)
            return value
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")
        return json.loads(result.stdout)

    def file(self, name, data, mode=0o600):
        path = self.directory / name
        path.write_bytes(data.encode() if isinstance(data, str) else data)
        path.chmod(mode)
        return path

    def manifest(self, value):
        return self.file("manifest.json", json.dumps(value, indent=2, ensure_ascii=False) + "\n")

    def checks(self, report):
        return {item["id"]: item for item in report["checks"]}

    def state(self):
        return {str(p.relative_to(self.directory)):
                (stat.S_IMODE(p.lstat().st_mode),
                 hashlib.sha256(p.read_bytes()).hexdigest() if p.is_file() and not p.is_symlink() else None)
                for p in self.directory.rglob("*")}

    def test_default_provenance_legacy_fields_and_no_files(self):
        before = self.state()
        report = self.invoke("doctor")
        self.assertEqual(report["report_format"], "wls-doctor-report-v1")
        for field in ("linux_supported", "bwrap_available", "cgroup_v2", "delegated_root_ready",
                      "runtime_files_ready", "strong_prerequisites"):
            self.assertIsInstance(report[field], bool)
        self.assertFalse(report["strong_prerequisites"])
        self.assertFalse(report["process_isolation_verified"])
        self.assertFalse(report["environment_checks_executed"])
        self.assertEqual(report["evidence"]["programs_executed"], 0)
        self.assertFalse(report["evidence"]["namespace_execution_tested"])
        self.assertEqual(report["inputs"]["manifest"]["origin"], "not_configured")
        self.assertIsNone(report["inputs"]["manifest"]["raw_sha256"])
        self.assertEqual(report["inputs"]["launcher"]["origin"], "default")
        self.assertEqual(report["inputs"]["launcher"]["path"], "/usr/bin/bwrap")
        config = report["inputs"]["config"]
        self.assertEqual(config["source_root"]["origin"], "supplied")
        self.assertEqual(config["system"]["origin"], "default")
        self.assertEqual(config["snapshot_sha256"], report["config_snapshot_sha256"])
        self.assertEqual(config["snapshot_scope"], "current_explicit_configuration")
        self.assertEqual(before, self.state())

    def test_relative_and_explicit_configuration_sources(self):
        shutil.copytree(SOURCE / "config/examples", self.directory / "config")
        args = ("--config", "config/system.yaml", "--functions-dir", "config/functions",
                "--prompts-dir", SOURCE / "prompts", "--function", "general",
                "--capability", "run", "--template", "default")
        checked = self.invoke("config", "check", *args[:-2])
        report = self.invoke("doctor", *args)
        config = report["inputs"]["config"]
        self.assertEqual(config["system"]["input_path"], "config/system.yaml")
        self.assertEqual(config["system"]["absolute_lexical_path"],
                         str(self.directory / "config/system.yaml"))
        self.assertEqual(config["system"]["path_kind"], "lexical_interpretation")
        for name in ("system", "functions", "prompts", "function", "capability", "template"):
            self.assertEqual(config[name]["origin"], "supplied")
        self.assertEqual(report["config_snapshot_sha256"], checked["snapshot_sha256"])

    def test_default_source_root_is_reported_without_canonical_claim(self):
        report = self.invoke("doctor", cwd=SOURCE, source=False)
        source = report["inputs"]["config"]["source_root"]
        self.assertEqual(source["origin"], "default")
        self.assertEqual(source["input_path"], ".")
        self.assertEqual(Path(source["absolute_lexical_path"]), SOURCE)
        self.assertEqual(source["path_kind"], "lexical_interpretation")

    def test_manifest_hash_uses_exact_bytes_and_explicit_default_path(self):
        path = self.manifest({"schema_version": 1, "bwrap_path": "/usr/bin/bwrap"})
        report = self.invoke("doctor", "--sandbox-manifest", path.name)
        info = report["inputs"]["manifest"]
        self.assertEqual(info["origin"], "supplied")
        self.assertEqual(info["input_path"], path.name)
        self.assertEqual(info["absolute_lexical_path"], str(path))
        self.assertEqual(info["raw_sha256"], hashlib.sha256(path.read_bytes()).hexdigest())
        self.assertEqual(info["bytes"], path.stat().st_size)
        self.assertEqual(info["schema_version"], 1)
        self.assertEqual(report["inputs"]["launcher"]["origin"], "supplied")
        self.assertNotEqual(info["raw_sha256"], report["execution_policy_sha256"])

    def test_bad_runtime_does_not_hide_next_file_or_expose_content(self):
        first = self.file("first.bin", "private fixture marker A")
        second = self.file("second.bin", "private fixture marker B")
        digest = hashlib.sha256(second.read_bytes()).hexdigest()
        manifest = self.manifest({"schema_version": 1, "runtime_files": [
            {"source": str(first), "destination": "/usr/lib/first", "sha256": "0" * 64},
            {"source": str(second), "destination": "/usr/lib/second", "sha256": digest}]})
        before = self.state()
        report = self.invoke("doctor", "--sandbox-manifest", manifest)
        checks = self.checks(report)
        self.assertIn("hash_mismatch", [x["code"] for x in checks["runtime.0"]["reasons"]])
        self.assertEqual(checks["runtime.1"]["status"], "observed_ok")
        self.assertEqual(checks["runtime.1"]["path"], str(second))
        self.assertEqual(checks["runtime.1"]["destination"], "/usr/lib/second")
        self.assertEqual(checks["runtime.1"]["observed"]["sha256"], digest)
        self.assertNotIn("private fixture marker", json.dumps(report))
        self.assertEqual(before, self.state())

    def test_plain_directory_cgroup_is_refused_without_writes(self):
        cgroup = self.directory / "ordinary-cgroup"
        cgroup.mkdir(mode=0o700)
        for name, content in (("cgroup.type", "domain\n"),
                              ("cgroup.subtree_control", "cpu memory pids\n"),
                              ("cgroup.procs", "")):
            (cgroup / name).write_text(content)
        manifest = self.manifest({"schema_version": 1, "cgroup_root": str(cgroup)})
        before = self.state()
        report = self.invoke("doctor", "--sandbox-manifest", manifest)
        checks = self.checks(report)
        self.assertFalse(report["delegated_root_ready"])
        self.assertIn("not_cgroup_v2", [x["code"] for x in checks["cgroup.root"]["reasons"]])
        operations = [x for x in report["checks"] if x["id"].startswith("operation.")]
        self.assertEqual(len(operations), 8)
        self.assertTrue(all(x["path_kind"] == "operation_child_pattern" and
                            x["status"] == "not_checked" for x in operations))
        self.assertEqual(before, self.state())

    def test_fake_launcher_is_not_executed(self):
        marker = self.directory / "executed"
        launcher = self.file("launcher", "#!/bin/sh\nprintf bad > " + str(marker) + "\n", 0o700)
        manifest = self.manifest({"schema_version": 1, "bwrap_path": str(launcher),
                                 "bwrap_sha256": hashlib.sha256(launcher.read_bytes()).hexdigest()})
        before = self.state()
        report = self.invoke("doctor", "--sandbox-manifest", manifest)
        self.assertFalse(report["bwrap_available"])
        self.assertFalse(marker.exists())
        self.assertEqual(report["evidence"]["programs_executed"], 0)
        self.assertEqual(before, self.state())

    def test_invalid_manifest_never_falls_back(self):
        for content in ('{"schema_version":', '{"schema_version":1,"unknown":true}',
                        '{"schema_version":1,"schema_version":1}', '[]', 'x' * 262145):
            path = self.file("invalid.json", content)
            self.invoke("doctor", "--sandbox-manifest", path, error="invalid_input")
        self.invoke("doctor", "--sandbox-manifest", self.directory / "missing", error="invalid_input")
        private = self.file("unreadable", '{"schema_version":1}', 0)
        try:
            self.invoke("doctor", "--sandbox-manifest", private, error="invalid_input")
        finally:
            private.chmod(0o600)

    def test_symlink_and_special_manifest_refuse_without_blocking(self):
        regular = self.file("regular.json", '{"schema_version":1}')
        link = self.directory / "link"
        link.symlink_to(regular)
        self.invoke("doctor", "--sandbox-manifest", link, error="invalid_input")
        fifo = self.directory / "fifo"
        os.mkfifo(fifo, 0o600)
        self.invoke("doctor", "--sandbox-manifest", fifo, error="invalid_input")

    def test_unknown_database_options_are_not_a_store_entry(self):
        database = self.directory / "must-not-exist.sqlite"
        self.invoke("doctor", "--db", database, error="invalid_input")
        self.assertFalse(database.exists())
        self.assertFalse(Path(str(database) + ".writer-lock").exists())
        self.invoke("doctor", "--request", "not-a-request", error="invalid_input")

    def test_invalid_path_bytes_and_oversized_paths_return_json_without_effects(self):
        invalid = os.fsdecode(os.fsencode(self.directory) + b"/missing-\xff")
        before = self.state()
        for path in (invalid, "x" * 4097):
            for option in ("--config", "--functions-dir", "--prompts-dir", "--sandbox-manifest"):
                self.invoke("doctor", option, path, error="invalid_input")
        self.assertEqual(before, self.state())

    def test_active_writer_lock_and_all_database_rows_remain_unchanged(self):
        database = self.directory / "runtime.sqlite"
        self.invoke("init", "--db", database)
        self.invoke("submit", "--db", database, "--key", "doctor-fixture",
                    "--text-file", SOURCE / "tests/fixtures/s12_request.txt",
                    "--constraints-file", SOURCE / "tests/fixtures/s12_constraints.json")
        def rows():
            with closing(sqlite3.connect(database.as_uri() + "?mode=ro", uri=True)) as connection:
                tables = connection.execute("SELECT name FROM sqlite_master WHERE type='table' ORDER BY name").fetchall()
                return {name: connection.execute('SELECT * FROM "' + name.replace('"', '""') + '"').fetchall()
                        for (name,) in tables}
        with Path(str(database) + ".writer-lock").open("r+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            self.invoke("init", "--db", database, error="conflict")
            before_rows = rows()
            before_files = self.state()
            report = self.invoke("doctor")
            self.assertEqual(report["evidence"]["programs_executed"], 0)
            self.assertEqual(rows(), before_rows)
            self.assertEqual(self.state(), before_files)


if __name__ == "__main__":
    previous = os.umask(0)
    os.umask(previous)
    print(f"doctor CLI fixture UID={os.getuid()} effective_umask={previous:04o}", flush=True)
    unittest.main(verbosity=2)
