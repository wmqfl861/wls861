#!/usr/bin/env python3
"""Local evidence-tool selfchecks; no WLS build, model or product gate is run."""
from __future__ import annotations

import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

ROOT = Path(__file__).resolve().parents[1]


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


tool = load("private_evidence", ROOT / "scripts/private_evidence.py")
gate = load("run_gcc_asan", ROOT / "scripts/run_gcc_asan.py")
SECRET = "SYNTHETIC_EVIDENCE_PRIVATE_HEADER"


def fixture_job(source: Path, scenario: str) -> int:
    """Real small CTest; Python executable identity, not simulated WLS binaries."""
    build = source / "fixture-build"
    private = build / "private-evidence"
    build.mkdir(mode=0o700)
    private.mkdir(mode=0o700)
    recorder = gate.Recorder(private, source, gate.clean_environment(dict(os.environ)))
    started = gate.utc()
    before = gate.tracked_snapshot(source, recorder.env)
    gate.write_json(private / "source-before.json", before)
    binaries = {"fixture-python": gate.digest(Path(sys.executable))}
    gate.write_json(private / "binaries-before.json", binaries)
    shutil.copyfile(source / "tests/gcc_asan_expected_tests.json", private / "expected-tests.json")
    summary = {"head": "a" * 40, "source": str(source), "started_at": started,
        "status": "NOT_COMPLETE", "steps": recorder.steps, "uid": os.geteuid(), "gid": os.getegid(),
        "build_umask": "0022", "test_umask": "0002", "historical_clang_asan_failures_closed": False,
        "sanitizer_environment": {"ASAN_OPTIONS": gate.ASAN, "UBSAN_OPTIONS": gate.UBSAN, "LSAN_OPTIONS": None},
        "registered_tests": 1}
    # ASAN_OPTIONS here is only publication metadata. No native executable is
    # instrumented and no result is represented as a product sanitizer test.
    child = source / "fixture_child.py"
    (build / "CTestTestfile.cmake").write_text(
        f'add_test(one "{sys.executable}" "{child}" "{scenario}" "{os.getppid()}")\n'
        f'set_tests_properties(one PROPERTIES TIMEOUT {0.2 if scenario == "timeout" else 10})\n')
    def interrupted(_number, _frame):
        raise KeyboardInterrupt("synthetic fixture interruption")
    signal.signal(signal.SIGTERM, interrupted)
    result = 1
    try:
        recorder.run("ctest", ["ctest", "--test-dir", str(build), "--output-on-failure",
                     "--parallel", "1", "--no-tests=error", "--output-junit", str(private / "ctest.xml")])
        gate.check_junit(private / "ctest.xml", ["one"])
        summary["status"] = "PASSED"
        result = 0
    except (Exception, KeyboardInterrupt) as error:
        summary["error"] = str(error)
    finally:
        gate.write_json(private / "source-after.json", gate.tracked_snapshot(source, recorder.env))
        gate.write_json(private / "binaries-after.json", {"fixture-python": gate.digest(Path(sys.executable))})
        summary["identities_unchanged"] = before == gate.tracked_snapshot(source, recorder.env)
        index = gate.preserve_ctest_evidence(build, private, recorder.steps)
        summary["ctest_evidence"] = {"sha256": gate.digest(private / "ctest-evidence.json"),
                                     "complete": index["complete"]}
        summary["ended_at"] = gate.utc()
        gate.write_json(private / "summary.json", summary)
        gate.publish_public_evidence(source, build, private, summary)
    return result


class EvidenceContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def prepare(self, scenario="success", name="job"):
        source = self.root / name
        source.mkdir(mode=0o700)
        (source / ".gitignore").write_text("fixture-build/\n")
        (source / "tests").mkdir()
        (source / "tests/gcc_asan_expected_tests.json").write_text(json.dumps(
            {"schema_version": 1, "tests": [{"name": "one", "timeout": 0.2 if scenario == "timeout" else 10}]}))
        (source / "tests/methods.cpp").write_text('return test::run({{"fixture method", run}});\n')
        (source / "fixture_child.py").write_text(
            "import os,signal,sys\n"
            "print('START fixture method', flush=True)\n"
            f"print('Authorization: Bearer {SECRET}' + 'x' * 4096, flush=True)\n"
            "if sys.argv[1] == 'interrupt':\n"
            "    os.kill(int(sys.argv[2]), signal.SIGTERM)\n"
            "    signal.pause()\n"
            "if sys.argv[1] == 'timeout': signal.pause()\n"
            "print('PASS fixture method', flush=True)\n"
            "sys.exit(7 if sys.argv[1] == 'failure' else 0)\n")
        subprocess.run(["git", "init", "-q", str(source)], check=True)
        subprocess.run(["git", "add", "."], cwd=source, check=True)
        return source, [sys.executable, "-B", str(Path(__file__).resolve()), "--fixture", str(source), scenario]

    def run_job(self, scenario="success", name="job"):
        source, command = self.prepare(scenario, name)
        capture = self.root / (name + "-capture")
        # Invoke the actual wrapper executable: its exit is observed separately
        # from the child exit in command.json, including catchable SIGTERM.
        completed = subprocess.run([sys.executable, "-B", str(ROOT / "scripts/private_evidence.py"),
            "capture", "--cwd", str(source), "--directory", str(capture), "--", *command],
            capture_output=True, timeout=20)
        private = source / "fixture-build/private-evidence"
        return source, capture, private, completed

    def bundle(self, scenario="success", name="job"):
        source, capture, private, completed = self.run_job(scenario, name)
        destination = self.root / (name + "-bundle")
        manifest = tool.pack(private, capture, destination)
        return source, capture, private, completed, destination, manifest

    def test_successful_job_originals_survive_ephemeral_job_deletion(self):
        source, capture, private, completed = self.run_job()
        self.assertEqual(completed.returncode, 0, completed.stderr)
        originals = {name: (private / name).read_bytes() for name in tool.REQUIRED}
        self.assertGreater(len(originals["ctest-logs/LastTest.log"]), 4096)
        self.assertIn(SECRET.encode(), originals["ctest-logs/LastTest.log"])
        public = self.root / "retained-public-artifact"
        shutil.copytree(source / "fixture-build/public-evidence", public)
        for path in public.iterdir():
            self.assertNotIn(SECRET.encode(), path.read_bytes())
        destination = self.root / "retained-private-bundle"
        # The legacy mode deliberately models the checked-in aa2 CI's actual
        # projection-only retention policy. It runs the same real fixture and
        # public publisher; it does not fail on an import or absent new API.
        if os.environ.get("WLS_EVIDENCE_LEGACY_RETENTION") != "1":
            tool.pack(private, capture, destination)
        shutil.rmtree(source)
        self.assertFalse(private.exists())
        self.assertTrue(public.is_dir())
        self.assertTrue(destination.is_dir(), "projection-only retention lost the complete private originals")
        manifest = tool.verify(destination)
        self.assertEqual(manifest["assessment"]["collection_status"], "COMPLETE")
        self.assertEqual(manifest["assessment"]["hosted_persistence"], "BLOCKED")
        for name, raw in originals.items():
            self.assertEqual((destination / "private-evidence" / name).read_bytes(), raw)

    def test_failed_ctest_preserves_originals_and_both_real_exit_codes(self):
        _, capture, private, completed, destination, manifest = self.bundle("failure")
        self.assertEqual(completed.returncode, 1)
        receipt = tool.decode((capture / "command.json").read_bytes())
        self.assertEqual(receipt["exit_code"], 1)
        self.assertTrue(receipt["reaped"])
        self.assertEqual(tool.decode((private / "ctest.step.json").read_bytes())["exit_code"], 8)
        self.assertEqual(manifest["assessment"]["collection_status"], "COMPLETE")
        self.assertEqual(manifest["assessment"]["runner_status"], "NOT_COMPLETE")
        self.assertEqual((destination / "private-evidence/ctest-logs/LastTestsFailed.log").read_bytes(),
                         (private / "ctest-logs/LastTestsFailed.log").read_bytes())

    def test_real_ctest_timeout_is_kept_without_claiming_success(self):
        _, _, private, completed, destination, manifest = self.bundle("timeout")
        self.assertEqual(completed.returncode, 1)
        self.assertIn(b'Timeout', (private / "ctest.xml").read_bytes())
        self.assertEqual(manifest["assessment"]["collection_status"], "COMPLETE")
        self.assertEqual(manifest["assessment"]["command_exit_code"], 1)
        self.assertEqual((destination / "private-evidence/ctest.xml").read_bytes(), (private / "ctest.xml").read_bytes())

    def test_catchable_sigterm_preserves_partial_bytes_and_real_outer_exit(self):
        _, capture, private, completed, destination, manifest = self.bundle("interrupt")
        self.assertEqual(completed.returncode, 1, completed.stderr)
        receipt = tool.decode((capture / "command.json").read_bytes())
        self.assertEqual(receipt["status"], "INTERRUPTED")
        self.assertEqual(receipt["signals"], [signal.SIGTERM])
        self.assertEqual(receipt["exit_code"], 1)
        self.assertTrue(receipt["reaped"])
        self.assertFalse(receipt["forced_kill"])
        self.assertEqual(manifest["assessment"]["collection_status"], "INCOMPLETE")
        self.assertIn("command_not_uninterrupted", manifest["assessment"]["problems"])
        names = list((private / "ctest-logs").iterdir())
        self.assertTrue(names)
        for original in names:
            self.assertEqual((destination / "private-evidence/ctest-logs" / original.name).read_bytes(), original.read_bytes())
        with self.assertRaises(ProcessLookupError):
            os.kill(receipt["pid"], 0)

    def test_launch_failure_has_no_fabricated_exit(self):
        output = self.root / "launch-failed"
        receipt = tool.capture_command([str(self.root / "absent-command")], self.root, output)
        self.assertEqual(receipt["status"], "LAUNCH_FAILED")
        self.assertIsNone(receipt["exit_code"])
        self.assertFalse(receipt["reaped"])
        self.assertEqual(set(path.name for path in output.iterdir()), tool.CAPTURE_FILES)

    def test_missing_original_is_retained_as_incomplete(self):
        _, capture, private, _ = self.run_job()
        (private / "ctest-logs/LastTest.log").unlink()
        manifest = tool.pack(private, capture, self.root / "bundle")
        self.assertEqual(manifest["assessment"]["collection_status"], "INCOMPLETE")
        self.assertIn("ctest-logs/LastTest.log", manifest["assessment"]["missing"])
        self.assertFalse((self.root / "bundle/private-evidence/ctest-logs/LastTest.log").exists())

    def test_wrong_command_receipt_cannot_rebind_old_evidence(self):
        _, _, private, _ = self.run_job()
        source, other_capture, _, _ = self.run_job(name="other-job")
        manifest = tool.pack(private, other_capture, self.root / "bundle")
        self.assertEqual(manifest["assessment"]["collection_status"], "INCOMPLETE")
        self.assertTrue(any("association mismatch" in issue for issue in manifest["assessment"]["problems"]))

    def test_modified_index_cannot_claim_complete_evidence(self):
        _, capture, private, _ = self.run_job()
        with (private / "ctest-evidence.json").open("ab") as stream:
            stream.write(b" ")
        manifest = tool.pack(private, capture, self.root / "bundle")
        self.assertEqual(manifest["assessment"]["collection_status"], "INCOMPLETE")
        self.assertTrue(any("index identity mismatch" in issue for issue in manifest["assessment"]["problems"]))

    def test_unsafe_files_and_directories_are_refused(self):
        for kind in ("symlink", "hardlink", "fifo", "directory-symlink"):
            with self.subTest(kind=kind):
                root = self.root / kind
                root.mkdir(mode=0o700)
                target = self.root / (kind + "-target")
                target.write_bytes(b"synthetic private content")
                if kind == "symlink":
                    (root / "unsafe").symlink_to(target)
                elif kind == "hardlink":
                    os.link(target, root / "unsafe")
                elif kind == "fifo":
                    os.mkfifo(root / "unsafe")
                else:
                    (root / "unsafe").symlink_to(self.root, target_is_directory=True)
                with self.assertRaises(tool.Refused):
                    tool.inventory(root)

    def test_shared_destination_is_refused_without_rewriting_permissions(self):
        output = self.root / "shared"
        output.mkdir(mode=0o755)
        with self.assertRaises(tool.Refused):
            tool.preflight(output)
        self.assertEqual(output.stat().st_mode & 0o777, 0o755)

    def test_existing_bundle_is_never_overwritten(self):
        _, capture, private, _, destination, _ = self.bundle()
        before = tool.inventory(destination)
        with self.assertRaises(FileExistsError):
            tool.pack(private, capture, destination)
        self.assertEqual(before, tool.inventory(destination))

    def test_input_mutation_during_pack_cannot_publish_manifest(self):
        _, capture, private, _ = self.run_job()
        original = tool.inventory
        def mutate_after_copy(root, destination=None):
            result = original(root, destination)
            if root == private and destination is not None:
                (private / "ctest.log").write_bytes(b"changed after actual copy")
            return result
        with patch.object(tool, "inventory", side_effect=mutate_after_copy), self.assertRaises(tool.Refused):
            tool.pack(private, capture, self.root / "bundle")
        self.assertFalse((self.root / "bundle/manifest.json").exists())
        self.assertTrue((self.root / "bundle/private-evidence/ctest.log").is_file())

    def test_verifier_rejects_mutation_addition_and_mode_change(self):
        _, _, _, _, destination, _ = self.bundle()
        path = destination / "private-evidence/ctest.log"
        original = path.read_bytes()
        path.write_bytes(original + b"tamper")
        with self.assertRaises(tool.Refused):
            tool.verify(destination)
        path.write_bytes(original)
        path.chmod(0o644)
        with self.assertRaises(tool.Refused):
            tool.verify(destination)
        path.chmod(0o600)
        extra = destination / "unexpected.txt"
        tool.exclusive(extra, b"unexpected")
        with self.assertRaises(tool.Refused):
            tool.verify(destination)

    def test_private_json_rejects_duplicates_and_nonfinite_numbers(self):
        for raw in (b'{"a":1,"a":2}', b'{"a":NaN}', b'{"a":1e999}'):
            with self.subTest(raw=raw), self.assertRaises(tool.Refused):
                tool.decode(raw)

    def test_preflight_never_equates_local_mode_with_hosted_persistence(self):
        result = tool.preflight(self.root)
        self.assertEqual(result["hosted_persistence"], "BLOCKED")
        self.assertFalse(result["raw_private_public_artifact_allowed"])
        self.assertFalse(result["encryption_or_upload_implemented"])
        self.assertIn("authorized_reviewer_acl", result["missing_verified_prerequisites"])
        with contextlib.redirect_stdout(io.StringIO()) as public:
            self.assertEqual(tool.main(["preflight", "--directory", str(self.root)]), 2)
        self.assertNotIn(str(self.root), public.getvalue())

    def test_cli_output_has_no_original_data_or_command_path(self):
        _, capture, private, completed = self.run_job("failure")
        self.assertNotIn(SECRET.encode(), completed.stdout + completed.stderr)
        self.assertNotIn(str(self.root).encode(), completed.stdout + completed.stderr)
        with contextlib.redirect_stdout(io.StringIO()) as public:
            result = tool.main(["pack", "--evidence", str(private), "--capture", str(capture),
                                "--directory", str(self.root / "bundle")])
        self.assertEqual(result, 0)
        self.assertNotIn(SECRET, public.getvalue())
        self.assertNotIn(str(self.root), public.getvalue())


if __name__ == "__main__":
    if sys.argv[1:2] == ["--fixture"]:
        sys.exit(fixture_job(Path(sys.argv[2]), sys.argv[3]))
    unittest.main(verbosity=2)
