#!/usr/bin/env python3
"""Runner contract tests only; these are not WLS C++/ASan product results."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

MODULE = Path(__file__).resolve().parents[1] / "scripts" / "run_gcc_asan.py"
spec = importlib.util.spec_from_file_location("run_gcc_asan", MODULE)
gate = importlib.util.module_from_spec(spec)
spec.loader.exec_module(gate)


class GateContracts(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def json_file(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_environment_has_no_provider_secrets_or_inherited_flags(self):
        inherited = {k: "untrusted" for k in ("CXXFLAGS", "CPPFLAGS", "LDFLAGS", "LSAN_OPTIONS",
                     "LD_PRELOAD", "LD_LIBRARY_PATH", "CMAKE_TOOLCHAIN_FILE", "CMAKE_GENERATOR",
                     "CTEST_PARALLEL_LEVEL", "OPENAI_API_KEY", "ASAN_OPTIONS")}
        inherited["PATH"] = "/usr/bin:/bin"
        env = gate.clean_environment(inherited)
        self.assertEqual(env["ASAN_OPTIONS"], "halt_on_error=1:detect_leaks=1")
        self.assertEqual(env["PATH"], inherited["PATH"])
        self.assertFalse((set(inherited) - {"PATH", "ASAN_OPTIONS"}) & set(env))

    def test_configure_is_debug_without_optimization_or_cache_reuse(self):
        args = gate.configure_command(self.root, self.root / "build-new", "/usr/bin/g++-13", "/venv/bin/python")
        for token in ("-DCMAKE_BUILD_TYPE=Debug", "-DCMAKE_CXX_FLAGS=", "-DCMAKE_CXX_FLAGS_DEBUG=-g",
                      "-DWLS_ENABLE_ASAN=ON", "-DWLS_ENABLE_UBSAN=OFF", "-DBUILD_TESTING=ON",
                      "-DCMAKE_CXX_COMPILER_LAUNCHER=", "-DPython3_EXECUTABLE=/venv/bin/python"):
            self.assertIn(token, args)
        self.assertNotIn("-O2", " ".join(args))

    def test_compile_flags_pass(self):
        args = ["/usr/bin/g++", "-g", "-Werror", "-fsanitize=address", "-fno-omit-frame-pointer"]
        path = self.json_file("compile.json", [{"arguments": args}])
        gate.check_compile_commands(path, "/usr/bin/g++")

    def test_compile_flags_rejects_each_mismatch(self):
        original = ["/usr/bin/g++", "-g", "-Werror", "-fsanitize=address", "-fno-omit-frame-pointer"]
        changes = [original + ["-O2"], original + ["-DNDEBUG"], original + ["-fno-sanitize=address"],
                   ["/usr/bin/clang++"] + original[1:], original[:3], []]
        for index, args in enumerate(changes):
            with self.subTest(index=index), self.assertRaises((RuntimeError, IndexError, KeyError)):
                path = self.json_file("compile.json", [{"arguments": args}] if args else [])
                gate.check_compile_commands(path, "/usr/bin/g++")

    def test_compile_command_string_is_supported(self):
        command = "/usr/bin/g++ -g -Werror -fsanitize=address -fno-omit-frame-pointer -c 'file name.cpp'"
        gate.check_compile_commands(self.json_file("compile.json", [{"command": command}]), "/usr/bin/g++")

    def test_registration_passes_and_retains_timeout(self):
        tests = [{"name": "a", "properties": [{"name": "TIMEOUT", "value": 90}]}]
        path = self.json_file("plan.json", {"tests": tests})
        self.assertEqual(gate.registered_tests(path, 1), ["a"])
        self.assertEqual(json.loads(path.read_text())["tests"], tests)

    def test_registration_rejects_empty_duplicate_disabled_and_count_change(self):
        invalid = [([], 0), ([{"name": "a"}], 2), ([{"name": "a"}, {"name": "a"}], 2),
                   ([{"name": "a", "properties": [{"name": "DISABLED", "value": True}]}], 1)]
        for tests, count in invalid:
            with self.subTest(tests=tests), self.assertRaises(RuntimeError):
                gate.registered_tests(self.json_file("plan.json", {"tests": tests}), count)

    def test_junit_requires_every_registered_case(self):
        path = self.root / "test.xml"
        path.write_text('<testsuite><testcase name="a" status="run"/><testcase name="b" status="run"/></testsuite>')
        gate.check_junit(path, ["a", "b"])
        with self.assertRaises(RuntimeError):
            gate.check_junit(path, ["a", "b", "c"])

    def test_reviewed_names_reject_same_count_substitution(self):
        manifest = self.json_file("expected.json", {"schema_version": 1,
            "tests": [{"name": "original", "timeout": 90}]})
        plan = self.json_file("plan.json", {"tests": [{"name": "replacement",
            "properties": [{"name": "TIMEOUT", "value": 90}]}]})
        with self.assertRaisesRegex(RuntimeError, "CTest names"):
            gate.registered_tests(plan, 1, manifest)

    def test_reviewed_deadline_must_match_and_original_is_accepted(self):
        manifest = self.json_file("expected.json", {"schema_version": 1,
            "tests": [{"name": "original", "timeout": 90}]})
        for deadline in (None, 89, 91):
            plan = self.json_file("plan.json", {"tests": [{"name": "original",
                "properties": [{"name": "TIMEOUT", "value": deadline}]}]})
            with self.subTest(deadline=deadline), self.assertRaisesRegex(RuntimeError, "deadline"):
                gate.registered_tests(plan, 1, manifest)
        plan = self.json_file("plan.json", {"tests": [{"name": "original",
            "properties": [{"name": "TIMEOUT", "value": 90.0}]}]})
        self.assertEqual(gate.registered_tests(plan, 1, manifest), ["original"])

    def test_manifest_missing_duplicate_and_count_change_are_refused(self):
        plan = self.json_file("plan.json", {"tests": [{"name": "a"}]})
        with self.assertRaises(FileNotFoundError):
            gate.registered_tests(plan, 1, self.root / "absent")
        for items in ([], [{"name": "a", "timeout": 90}, {"name": "a", "timeout": 90}],
                      [{"name": "a", "timeout": 0}], [{"name": "a", "timeout": True}]):
            manifest = self.json_file("expected.json", {"schema_version": 1, "tests": items})
            with self.subTest(items=items), self.assertRaises(RuntimeError):
                gate.registered_tests(plan, 1, manifest)

    def test_junit_rejects_skips_failures_errors_and_notrun(self):
        path = self.root / "test.xml"
        for content in ('<failure/>', '<error/>', '<skipped/>'):
            path.write_text(f'<testsuite><testcase name="a" status="run">{content}</testcase></testsuite>')
            with self.subTest(content=content), self.assertRaises(RuntimeError):
                gate.check_junit(path, ["a"])
        path.write_text('<testsuite><testcase name="a" status="notrun"/></testsuite>')
        with self.assertRaises(RuntimeError):
            gate.check_junit(path, ["a"])

    def test_recorder_preserves_nonzero_exit_and_stderr(self):
        recorder = gate.Recorder(self.root, self.root, gate.clean_environment(dict(os.environ)))
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(RuntimeError):
            recorder.run("failed", [sys.executable, "-c", "import sys; print('out'); print('err',file=sys.stderr); sys.exit(7)"])
        record = json.loads((self.root / "failed.step.json").read_text())
        self.assertEqual(record["exit_code"], 7)
        self.assertEqual((self.root / "failed.stderr.log").read_text().strip(), "err")
        self.assertIn("ended_at", record)
        self.assertEqual(record["logs"]["failed.log"], gate.digest(self.root / "failed.log"))

    def test_recorder_retains_launch_failure(self):
        recorder = gate.Recorder(self.root, self.root, gate.clean_environment(dict(os.environ)))
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(FileNotFoundError):
            recorder.run("missing", [str(self.root / "missing-executable")])
        record = json.loads((self.root / "missing.step.json").read_text())
        self.assertIsNone(record["exit_code"])
        self.assertTrue(record["interrupted_or_launch_error"])

    def test_root_is_refused_before_creating_build(self):
        with patch.object(gate.os, "geteuid", return_value=0), self.assertRaises(RuntimeError):
            gate.main(["--source", str(self.root), "--python", sys.executable])
        self.assertEqual(list(self.root.iterdir()), [])

    def test_venv_symlink_is_not_resolved_to_system_python(self):
        args = gate.configure_command(self.root, self.root / "build", "/usr/bin/g++-13", "/venv/bin/python")
        self.assertIn("-DPython3_EXECUTABLE=/venv/bin/python", args)

    def test_source_snapshot_detects_byte_and_mode_changes(self):
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        path = self.root / "input.txt"
        path.write_text("original")
        subprocess.run(["git", "add", "input.txt"], cwd=self.root, check=True)
        env = gate.clean_environment(dict(os.environ))
        before = gate.tracked_snapshot(self.root, env)
        path.write_text("changed")
        self.assertNotEqual(before, gate.tracked_snapshot(self.root, env))
        path.write_text("original")
        path.chmod(0o755)
        self.assertNotEqual(before, gate.tracked_snapshot(self.root, env))

    def test_build_snapshot_detects_binary_and_cache_changes(self):
        for name in ("wls", "wls861", "libwls_s1.a", "CMakeCache.txt", "CTestTestfile.cmake"):
            (self.root / name).write_text("original")
        (self.root / "wls").chmod(0o755)
        (self.root / "wls861").chmod(0o755)
        before = gate.build_snapshot(self.root)
        (self.root / "wls").write_text("changed")
        self.assertNotEqual(before, gate.build_snapshot(self.root))
        self.assertIn("CTestTestfile.cmake", before)

    def test_atomic_receipt_replacement_leaves_no_temporary_file(self):
        path = self.root / "summary.json"
        gate.write_json(path, {"status": "NOT_COMPLETE"})
        gate.write_json(path, {"status": "PASSED"})
        self.assertEqual(json.loads(path.read_text())["status"], "PASSED")
        self.assertFalse(path.with_suffix(".json.tmp").exists())


    def exercise_main(self, failing_step=None, mutate_binary=False, compiler_version="13.3.0"):
        # This is deliberately a mocked command flow, not a C++/ASan run.
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        (self.root / ".gitignore").write_text("/build*/\n")
        (self.root / "tests").mkdir()
        (self.root / "tests/gcc_asan_expected_tests.json").write_text(json.dumps(
            {"schema_version": 1, "tests": [{"name": "one", "timeout": 90}]}))
        subprocess.run(["git", "add", ".gitignore", "tests/gcc_asan_expected_tests.json"], cwd=self.root, check=True)
        subprocess.run(["git", "-c", "user.name=Runner Test", "-c", "user.email=test@example.invalid",
                        "commit", "-qm", "fixture"], cwd=self.root, check=True)
        observed = []
        compiler = os.path.abspath(sys.executable)

        def run(recorder, name, argv, **kwargs):
            observed.append((name, argv))
            build = recorder.directory.parent
            path = recorder.directory / (name + (".json" if kwargs.get("json_output") else ".log"))
            path.write_text(compiler_version if name == "compiler-version" else "fixture")
            if name == "build":
                for binary in ("wls", "wls861"):
                    (build / binary).write_text("fixture executable")
                    (build / binary).chmod(0o755)
                for filename in ("CMakeCache.txt", "CTestTestfile.cmake"):
                    (build / filename).write_text("fixture")
                (build / "compile_commands.json").write_text(json.dumps([{"arguments": [compiler,
                    "-g", "-Werror", "-fsanitize=address", "-fno-omit-frame-pointer"]}]))
            if name == "test-plan":
                path.write_text(json.dumps({"tests": [{"name": "one",
                    "properties": [{"name": "TIMEOUT", "value": 90}]}]}))
            if name == "ctest":
                (recorder.directory / "ctest.xml").write_text('<testsuite><testcase name="one" status="run"/></testsuite>')
                if mutate_binary:
                    (build / "wls").write_text("modified")
            if name == failing_step:
                raise RuntimeError("injected command failure")
            return path

        with patch.object(gate.os, "geteuid", return_value=1000), patch.object(gate.Recorder, "run", run), \
                contextlib.redirect_stdout(io.StringIO()):
            result = gate.main(["--source", str(self.root), "--python", sys.executable,
                                "--compiler", compiler, "--expected-tests", "1"])
        receipts = list(self.root.glob("build-gcc-asan-*/evidence/summary.json"))
        self.assertEqual(len(receipts), 1)
        return result, json.loads(receipts[0].read_text()), observed, receipts[0].parent

    def test_full_command_flow_is_not_filtered_and_uses_exact_sanitizer(self):
        result, summary, observed, _ = self.exercise_main()
        self.assertEqual(result, 0)
        self.assertEqual(summary["status"], "PASSED")
        self.assertFalse(summary["historical_clang_asan_failures_closed"])
        command = dict(observed)["ctest"]
        self.assertIn("--no-tests=error", command)
        self.assertEqual(command[command.index("--parallel") + 1], "1")
        self.assertFalse({"-R", "-E", "--timeout", "--rerun-failed", "--repeat"} & set(command))

    def test_failed_ctest_keeps_before_and_after_identities_and_failure(self):
        result, summary, _, evidence = self.exercise_main(failing_step="ctest")
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertTrue(summary["identities_unchanged"])
        for name in ("source-before.json", "source-after.json", "binaries-before.json", "binaries-after.json"):
            self.assertTrue((evidence / name).is_file())

    def test_binary_change_rejects_apparent_ctest_success(self):
        result, summary, _, _ = self.exercise_main(mutate_binary=True)
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertIn("build identity", summary["identity_error"])

    def test_wrong_gcc_version_never_builds_or_tests(self):
        result, summary, observed, _ = self.exercise_main(compiler_version="14.2.0")
        self.assertEqual(result, 1)
        self.assertEqual([name for name, _ in observed], ["compiler-version"])
        self.assertIn("expected GCC 13.3.0", summary["error"])


if __name__ == "__main__":
    unittest.main(verbosity=2)
