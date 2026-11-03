#!/usr/bin/env python3
"""Runner contract tests only; these are not WLS C++/ASan product results."""
import contextlib
import hashlib
import importlib.util
import io
import json
import os
from pathlib import Path
import signal
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

    def test_shipped_catalog_and_default_count_stay_in_sync(self):
        catalog = json.loads((Path(gate.__file__).resolve().parents[1] /
                              "tests/gcc_asan_expected_tests.json").read_text())
        self.assertEqual(len(catalog["tests"]), gate.DEFAULT_EXPECTED_TESTS)
        self.assertEqual(len({entry["name"] for entry in catalog["tests"]}),
                         gate.DEFAULT_EXPECTED_TESTS)
        additions = {entry["name"]: entry["timeout"] for entry in catalog["tests"]}
        self.assertEqual(additions["persistent_event_store"], 120)
        self.assertEqual(additions["persistent_event_runtime"], 180)

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


    def exercise_main(self, failing_step=None, mutate_binary=False, compiler_version="13.3.0",
                      missing_log=False, interrupt=False, launch_error=False, private_output=None,
                      identity_fault=None, ctest_junit=None):
        # This is deliberately a mocked command flow, not a C++/ASan run.
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        (self.root / ".gitignore").write_text("/build*/\n")
        (self.root / "tests").mkdir()
        (self.root / "tests/gcc_asan_expected_tests.json").write_text(json.dumps(
            {"schema_version": 1, "tests": [{"name": "one", "timeout": 90}]}))
        (self.root / "tests/fixture.cpp").write_text('return test::run({{"safe fixture method", fixture_method}});\n')
        subprocess.run(["git", "add", ".gitignore", "tests"], cwd=self.root, check=True)
        subprocess.run(["git", "-c", "user.name=Runner Test", "-c", "user.email=test@example.invalid",
                        "commit", "-qm", "fixture"], cwd=self.root, check=True)
        observed = []
        compiler = os.path.abspath(sys.executable)

        def run(recorder, name, argv, **kwargs):
            observed.append((name, argv))
            build = recorder.directory.parent
            path = recorder.directory / (name + (".json" if kwargs.get("json_output") else ".log"))
            path.write_text(compiler_version if name == "compiler-version" else "fixture")
            (recorder.directory / (name + ".stderr.log")).write_text("")
            record = {"name": name, "argv": argv, "cwd": str(recorder.source),
                      "started_at": gate.utc(), "ended_at": gate.utc(),
                      "exit_code": 7 if name == failing_step else 0}
            recorder.steps.append(record)
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
                (recorder.directory / "ctest.xml").write_text(ctest_junit if ctest_junit is not None else
                    '<testsuite><testcase name="one" status="run"/></testsuite>')
                logs = build / "Testing/Temporary"
                logs.mkdir(parents=True)
                if not missing_log:
                    (logs / "LastTest.log").write_text(private_output if private_output is not None else "original success output\n" * 200)
                if name == failing_step:
                    (logs / "LastTestsFailed.log").write_text("1:one\n")
                if interrupt or launch_error:
                    record["interrupted_or_launch_error"] = True
                    record["exit_code"] = None if launch_error else -signal.SIGTERM
                if mutate_binary:
                    (build / "wls").write_text("modified")
                if identity_fault == "source":
                    with (self.root / "tests/fixture.cpp").open("a") as fixture:
                        fixture.write("// source changed during the synthetic command\n")
                elif identity_fault == "head":
                    subprocess.run(["git", "-c", "user.name=Runner Test", "-c", "user.email=test@example.invalid",
                                    "commit", "--allow-empty", "-qm", "changed head"], cwd=self.root, check=True)
                elif identity_fault == "dirty":
                    (self.root / "late-untracked.txt").write_text("synthetic untracked input\n")
            gate.write_json(recorder.directory / (name + ".step.json"), record)
            if name == "ctest" and interrupt:
                raise KeyboardInterrupt("injected recoverable interruption")
            if name == "ctest" and launch_error:
                raise FileNotFoundError("injected launch error")
            if name == failing_step:
                raise RuntimeError("injected command failure")
            return path

        with patch.object(gate.os, "geteuid", return_value=1000), patch.object(gate.Recorder, "run", run), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            result = gate.main(["--source", str(self.root), "--python", sys.executable,
                                "--compiler", compiler, "--expected-tests", "1"])
        receipts = list(self.root.glob("build-gcc-asan-*/*/summary.json"))
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
        self.assertEqual((evidence / "ctest-logs/LastTestsFailed.log").read_text(), "1:one\n")
        self.assertEqual(json.loads((evidence / "ctest-evidence.json").read_text())["ctest_step"]["exit_code"], 7)

    def test_successful_main_keeps_complete_original_lasttest_and_index(self):
        result, summary, _, evidence = self.exercise_main()
        self.assertEqual(result, 0)
        self.assertEqual(summary["status"], "PASSED")
        original = (evidence.parent / "Testing/Temporary/LastTest.log").read_bytes()
        self.assertGreater(len(original), 1024)
        self.assertTrue((evidence / "ctest-logs/LastTest.log").is_file(), "original CTest LastTest.log was not retained")
        self.assertEqual((evidence / "ctest-logs/LastTest.log").read_bytes(), original)
        self.assertTrue(summary["ctest_evidence"]["complete"])
        self.assertEqual(summary["ctest_evidence"]["sha256"], gate.digest(evidence / "ctest-evidence.json"))

    def test_missing_lasttest_rejects_apparent_success_without_fabricating_it(self):
        result, summary, _, evidence = self.exercise_main(missing_log=True)
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertTrue(summary["identities_unchanged"])
        self.assertFalse((evidence / "ctest-logs/LastTest.log").exists())
        self.assertIn("missing original ctest-logs/LastTest.log", summary["ctest_evidence"]["warnings"])

    def test_interrupted_main_retains_original_logs_identities_and_terminal_status(self):
        result, summary, _, evidence = self.exercise_main(interrupt=True)
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertEqual(summary["error"], "injected recoverable interruption")
        self.assertTrue(summary["identities_unchanged"])
        self.assertIn("ended_at", summary)
        index = json.loads((evidence / "ctest-evidence.json").read_text())
        self.assertFalse(index["complete"])
        self.assertEqual(index["ctest_step"]["exit_code"], -signal.SIGTERM)
        for name in ("ctest-logs/LastTest.log", "ctest.xml", "source-before.json", "source-after.json",
                     "binaries-before.json", "binaries-after.json"):
            self.assertEqual(index["artifacts"][name]["sha256"], gate.digest(evidence / name))

    def test_launch_error_main_cannot_pass_with_apparently_complete_files(self):
        result, summary, _, evidence = self.exercise_main(launch_error=True)
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertTrue(summary["identities_unchanged"])
        index = json.loads((evidence / "ctest-evidence.json").read_text())
        self.assertIsNone(index["ctest_step"]["exit_code"])
        self.assertFalse(index["execution_completed"])
        self.assertFalse(index["complete"])

    def run_fixture_ctest(self, exit_code=0, interrupt=False):
        # Real CTest + Recorder, but only a synthetic Python child: no build,
        # Store, network or sanitizer product run is involved in this selfcheck.
        build, evidence = self.root / "fixture-build", self.root / "evidence"
        build.mkdir()
        evidence.mkdir()
        marker = "complete-success-method-output-" + "x" * 4096
        child = build / "child.py"
        child.write_text("import os,signal,sys\n"
                         f"print({marker!r}, flush=True)\n" +
                         (f"os.kill({os.getpid()}, signal.SIGUSR1)\nsignal.pause()\n" if interrupt else
                          f"sys.exit({exit_code})\n"))
        (build / "CTestTestfile.cmake").write_text(
            f'add_test(long-output "{sys.executable}" "{child}")\n'
            'set_tests_properties(long-output PROPERTIES TIMEOUT 10)\n')
        recorder = gate.Recorder(evidence, self.root, gate.clean_environment(dict(os.environ)))
        argv = ["ctest", "--test-dir", str(build), "--output-on-failure", "--parallel", "1",
                "--no-tests=error", "--output-junit", str(evidence / "ctest.xml")]
        def interrupted(_signum, _frame):
            raise KeyboardInterrupt("synthetic child reached its barrier")
        previous = signal.signal(signal.SIGUSR1, interrupted) if interrupt else None
        try:
            with contextlib.redirect_stdout(io.StringIO()):
                if interrupt:
                    with self.assertRaisesRegex(KeyboardInterrupt, "reached its barrier"):
                        recorder.run("ctest", argv)
                elif exit_code:
                    with self.assertRaises(RuntimeError):
                        recorder.run("ctest", argv)
                else:
                    recorder.run("ctest", argv)
        finally:
            if interrupt:
                signal.signal(signal.SIGUSR1, previous)
        with contextlib.redirect_stderr(io.StringIO()):
            index = gate.preserve_ctest_evidence(build, evidence, recorder.steps)
        return build, evidence, index, marker

    def test_real_ctest_success_preserves_more_than_junit_output_limit(self):
        build, evidence, index, marker = self.run_fixture_ctest()
        original = (build / "Testing/Temporary/LastTest.log").read_bytes()
        self.assertGreater(len(marker.encode()), 1024)
        self.assertIn(marker.encode(), original)
        self.assertEqual((evidence / "ctest-logs/LastTest.log").read_bytes(), original)
        self.assertTrue(index["complete"])
        self.assertEqual(index["ctest_step"]["exit_code"], 0)
        self.assertEqual(index["artifacts"]["ctest.xml"]["sha256"], gate.digest(evidence / "ctest.xml"))
        gate.check_junit(evidence / "ctest.xml", ["long-output"])

    def test_real_ctest_failure_keeps_lasttest_failed_list_and_junit(self):
        build, evidence, index, marker = self.run_fixture_ctest(exit_code=7)
        self.assertNotEqual(index["ctest_step"]["exit_code"], 0)
        self.assertTrue(index["execution_completed"])
        for name in ("LastTest.log", "LastTestsFailed.log"):
            self.assertEqual((evidence / "ctest-logs" / name).read_bytes(),
                             (build / "Testing/Temporary" / name).read_bytes())
        self.assertIn(marker, (evidence / "ctest-logs/LastTest.log").read_text())
        with self.assertRaises(RuntimeError):
            gate.check_junit(evidence / "ctest.xml", ["long-output"])

    def test_real_ctest_interruption_keeps_available_originals_and_never_passes(self):
        build, evidence, index, _ = self.run_fixture_ctest(interrupt=True)
        self.assertFalse(index["execution_completed"])
        self.assertFalse(index["complete"])
        self.assertTrue(index["ctest_step"]["interrupted_or_launch_error"])
        self.assertIsNotNone(index["ctest_step"]["exit_code"])
        self.assertTrue(index["warnings"])
        originals = list((build / "Testing/Temporary").glob("LastTest*"))
        self.assertTrue(originals)
        for original in originals:
            if original.name in ("LastTest.log", "LastTest.log.tmp", "LastTestsFailed.log", "LastTestsFailed.log.tmp"):
                self.assertEqual((evidence / "ctest-logs" / original.name).read_bytes(), original.read_bytes())

    def test_real_launch_failure_preserves_receipt_and_reports_missing_ctest_logs(self):
        evidence = self.root / "evidence"
        evidence.mkdir()
        recorder = gate.Recorder(evidence, self.root, gate.clean_environment(dict(os.environ)))
        with contextlib.redirect_stdout(io.StringIO()), self.assertRaises(FileNotFoundError):
            recorder.run("ctest", [str(self.root / "missing-ctest")])
        with contextlib.redirect_stderr(io.StringIO()):
            index = gate.preserve_ctest_evidence(self.root, evidence, recorder.steps)
        self.assertFalse(index["complete"])
        self.assertIsNone(index["ctest_step"]["exit_code"])
        self.assertIn("ctest.step.json", index["artifacts"])
        self.assertIn("missing original ctest-logs/LastTest.log", index["warnings"])

    def test_public_guard_refuses_known_doctor_config_body_and_header_records(self):
        # Deliberately synthetic negative samples, never actual host secrets.
        samples = [b'{"report_format":"wls-doctor-report-v1","objects":[{"path":"SYNTHETIC_PRIVATE"}]}\n',
                   b'{"config":{"credential_present":true,"value":"SYNTHETIC_PRIVATE"}}\n',
                   b'{"request_body":"SYNTHETIC_PRIVATE"}\n',
                   b'Authorization: Bearer SYNTHETIC_PRIVATE\n']
        for sample in samples:
            with self.subTest(sample=sample.split(b":", 1)[0]), self.assertRaises(RuntimeError):
                gate.check_public_log(sample, ({"safe fixture method"}, set(), set()), {"one"})

    def test_public_projection_preserves_long_static_methods_and_every_unchanged_byte(self):
        name = "static long method " + "x" * 1600
        names = ({name}, {"test_fixture"}, {"Fixture"})
        original = (f"START {name}\n" + '{"body":"SYNTHETIC_PRIVATE"}\n' +
                    f"PASS {name}\n" + "test_fixture (__main__.Fixture.test_fixture) ... ok\n" +
                    "RESULT 1 passed, 0 failed\n").encode()
        public, ranges = gate.public_log_projection(original, names, {"one"})
        self.assertGreater(len(public), 1024)
        self.assertNotIn(b"SYNTHETIC_PRIVATE", public)
        self.assertIn(f"START {name}\n".encode(), public)
        self.assertIn(f"PASS {name}\n".encode(), public)
        self.assertIn(b"test_fixture (__main__.Fixture.test_fixture) ... ok\n", public)
        for span in ranges:
            if span["kind"] == "verbatim":
                self.assertEqual(original[slice(*span["source_bytes"])], public[slice(*span["public_bytes"])])
        self.assertEqual(ranges[-1]["source_bytes"][1], len(original))
        self.assertEqual(ranges[-1]["public_bytes"][1], len(public))
        gate.check_public_log(public, names, {"one"})

    def grouped_method_registry(self):
        tests = self.root / "tests"
        tests.mkdir(exist_ok=True)
        source = tests / "grouped.cpp"
        source.write_text('const std::vector<test::Case> cases{\n'
                          '  {"static grouped method", grouped_method}\n};\n', encoding="utf-8")
        registry = {"format": "wls-gcc-asan-static-methods-v1", "sources": [
            {"path": "tests/grouped.cpp", "sha256": gate.digest(source),
             "methods": ["static grouped method"]}]}
        path = tests / "gcc_asan_public_methods.json"
        path.write_text(json.dumps(registry), encoding="utf-8")
        return source, path, registry

    def test_public_registry_preserves_grouped_names_without_dynamic_output(self):
        self.grouped_method_registry()
        names = gate.public_method_names(self.root)
        self.assertIn("static grouped method", names[0])
        raw = (b"START static grouped method\nPASS static grouped method\n"
               b"FAIL static grouped method: SYNTHETIC_PRIVATE\n"
               b"PASS unregistered runtime label\n")
        public, _ = gate.public_log_projection(raw, names, {"one"})
        self.assertIn(b"START static grouped method\nPASS static grouped method\n", public)
        self.assertIn(b"FAIL static grouped method: [private output omitted:", public)
        self.assertNotIn(b"SYNTHETIC_PRIVATE", public)
        self.assertNotIn(b"unregistered runtime label", public)
        gate.check_public_log(public, names, {"one"})

    def test_public_registry_refuses_changed_source_and_unregistered_label(self):
        source, path, registry = self.grouped_method_registry()
        source.write_text(source.read_text() + "// changed source\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "static method source"):
            gate.public_method_names(self.root)
        registry["sources"][0]["sha256"] = gate.digest(source)
        registry["sources"][0]["methods"] = ["unregistered label"]
        path.write_text(json.dumps(registry), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "static method label"):
            gate.public_method_names(self.root)

    def test_public_registry_refuses_unsafe_paths_duplicates_and_newlines(self):
        _, path, registry = self.grouped_method_registry()
        entry = registry["sources"][0]
        for key, value in (("path", "../outside.cpp"), ("path", "/absolute.cpp"),
                           ("methods", ["static grouped method", "static grouped method"]),
                           ("methods", ["static grouped method\nPASS injected"])):
            with self.subTest(key=key, value=value):
                candidate = json.loads(json.dumps(registry))
                candidate["sources"][0][key] = value
                path.write_text(json.dumps(candidate), encoding="utf-8")
                with self.assertRaises(RuntimeError):
                    gate.public_method_names(self.root)
        self.assertEqual(entry["path"], "tests/grouped.cpp")

    def test_fail_method_keeps_name_but_never_dynamic_exception_text(self):
        names = ({"safe fixture method"}, set(), set())
        public, ranges = gate.public_log_projection(
            b'FAIL safe fixture method: {"headers":{"Authorization":"SYNTHETIC_PRIVATE"}}\n', names, {"one"})
        self.assertTrue(public.startswith(b"FAIL safe fixture method: [private output omitted:"))
        self.assertNotIn(b"Authorization", public)
        self.assertEqual([span["kind"] for span in ranges], ["verbatim", "redacted"])
        gate.check_public_log(public, names, {"one"})

    def test_main_only_publishes_checked_projection_and_keeps_private_original(self):
        raw = 'START safe fixture method\n{"report_format":"wls-doctor-report-v1","objects":["SYNTHETIC_PRIVATE"]}\nPASS safe fixture method\nRESULT 1 passed, 0 failed\n'
        result, summary, _, private = self.exercise_main(private_output=raw)
        self.assertEqual(result, 0)
        self.assertEqual(summary["status"], "PASSED")
        self.assertEqual((private / "ctest-logs/LastTest.log").read_text(), raw)
        self.assertFalse((private.parent / "evidence").exists())
        public = private.parent / "public-evidence"
        self.assertTrue(public.is_dir())
        self.assertFalse((private.parent / "public-evidence-pending").exists())
        index = json.loads((public / "public-index.json").read_text())
        entry = index["artifacts"]["ctest-logs__LastTest.log.projection.txt"]
        self.assertEqual(entry["source_sha256"], gate.digest(private / "ctest-logs/LastTest.log"))
        self.assertEqual(entry["source_bytes"], len(raw.encode()))
        for path in public.iterdir():
            self.assertNotIn(b"SYNTHETIC_PRIVATE", path.read_bytes())
            self.assertNotIn(b"credential_present", path.read_bytes())
        names = gate.public_method_names(self.root)
        gate.check_public_package(public, names, {"one"})

    def test_public_package_guard_rejects_added_raw_file_and_rehashed_dynamic_log(self):
        _, _, _, private = self.exercise_main()
        public = private.parent / "public-evidence"
        names = gate.public_method_names(self.root)
        raw = public / "raw-doctor.json"
        raw.write_text('{"report_format":"wls-doctor-report-v1"}\n')
        with self.assertRaisesRegex(RuntimeError, "unexpected public artifact"):
            gate.check_public_package(public, names, {"one"})
        raw.unlink()
        path = public / "ctest-logs__LastTest.log.projection.txt"
        path.write_bytes(b'Authorization: Bearer SYNTHETIC_PRIVATE\n')
        manifest = json.loads((public / "public-index.json").read_text())
        entry = manifest["artifacts"][path.name]
        entry["public_bytes"] = path.stat().st_size
        entry["public_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
        entry["ranges"] = [{"kind": "redacted", "source_bytes": [0, entry["source_bytes"]],
                            "public_bytes": [0, entry["public_bytes"]]}]
        gate.write_json(public / "public-index.json", manifest)
        with self.assertRaisesRegex(RuntimeError, "unapproved dynamic line"):
            gate.check_public_package(public, names, {"one"})

    def test_public_package_guard_rejects_private_text_in_rehashed_junit(self):
        _, _, _, private = self.exercise_main()
        public = private.parent / "public-evidence"
        names = gate.public_method_names(self.root)
        path = public / "ctest.projection.xml"
        for sample in ('{"report_format":"wls-doctor-report-v1"}', '{"config":"SYNTHETIC_PRIVATE"}',
                       '{"request_body":"SYNTHETIC_PRIVATE"}', 'Authorization: Bearer SYNTHETIC_PRIVATE'):
            document = gate.ET.parse(path).getroot()
            document.text = sample
            path.write_bytes(gate.ET.tostring(document, encoding="utf-8", xml_declaration=True))
            manifest = json.loads((public / "public-index.json").read_text())
            entry = manifest["artifacts"][path.name]
            entry["public_bytes"] = path.stat().st_size
            entry["public_sha256"] = hashlib.sha256(path.read_bytes()).hexdigest()
            gate.write_json(public / "public-index.json", manifest)
            with self.subTest(kind=sample.split(":", 1)[0]), self.assertRaisesRegex(RuntimeError, "JUnit root text"):
                gate.check_public_package(public, names, {"one"})

    def rewrite_public_artifact(self, public, name, data):
        path = public / name
        path.write_bytes(data)
        manifest = json.loads((public / "public-index.json").read_text())
        entry = manifest["artifacts"][name]
        entry["public_bytes"] = len(data)
        entry["public_sha256"] = hashlib.sha256(data).hexdigest()
        if isinstance(entry["ranges"], list):
            entry["ranges"] = [{"kind": "verbatim", "source_bytes": [0, entry["source_bytes"]],
                                "public_bytes": [0, len(data)]}]
        gate.write_json(public / "public-index.json", manifest)

    def check_failed_junit_projection(self, category, failing_step="ctest"):
        original = ('<testsuite><testcase name="one" status="fail" time="1.25">'
                    f'<failure message="{category}">SYNTHETIC_PRIVATE_DIAGNOSTIC</failure>'
                    '</testcase></testsuite>')
        result, summary, _, private = self.exercise_main(failing_step=failing_step, ctest_junit=original)
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertEqual((private / "ctest.xml").read_bytes(), original.encode())
        public = private.parent / "public-evidence"
        path = public / "ctest.projection.xml"
        self.assertTrue(path.is_file(), "legal CTest status=fail prevented checked failure publication")
        case = gate.ET.parse(path).getroot().find("testcase")
        self.assertEqual(case.get("status"), "fail")
        failure = case.find("failure")
        self.assertIsNotNone(failure)
        self.assertEqual(failure.attrib, {})
        self.assertEqual(failure.text, "Original diagnostic is retained privately.")
        self.assertNotIn(b"SYNTHETIC_PRIVATE_DIAGNOSTIC", path.read_bytes())
        self.assertEqual(json.loads((public / "summary.projection.json").read_text())["status"], "NOT_COMPLETE")
        entry = json.loads((public / "public-index.json").read_text())["artifacts"][path.name]
        self.assertEqual(entry["source_sha256"], gate.digest(private / "ctest.xml"))
        gate.check_public_package(public, gate.public_method_names(self.root), {"one"})
        with self.assertRaises(RuntimeError):
            gate.check_junit(path, ["one"])

    def test_failed_ctest_junit_retains_legal_fail_projection(self):
        self.check_failed_junit_projection("Failed")

    def test_timed_out_ctest_junit_retains_legal_fail_projection(self):
        self.check_failed_junit_projection("Timeout")

    def test_failed_junit_cannot_pass_even_after_ctest_exit_zero(self):
        self.check_failed_junit_projection("Failed", failing_step=None)

    def test_public_guard_accepts_clean_failed_projection(self):
        _, _, _, private = self.exercise_main(failing_step="ctest")
        public = private.parent / "public-evidence"
        root = gate.ET.parse(public / "ctest.projection.xml").getroot()
        case = root.find("testcase")
        case.set("status", "fail")
        gate.ET.SubElement(case, "failure").text = "Original diagnostic is retained privately."
        self.rewrite_public_artifact(public, "ctest.projection.xml",
                                     gate.ET.tostring(root, encoding="utf-8", xml_declaration=True))
        error = None
        try:
            gate.check_public_package(public, gate.public_method_names(self.root), {"one"})
        except RuntimeError as caught:
            error = str(caught)
        self.assertIsNone(error, "a checked failure projection must accept CTest status=fail")

    def test_public_guard_rejects_passed_summary_with_nonpass_junit(self):
        _, _, _, private = self.exercise_main()
        public = private.parent / "public-evidence"
        original = (public / "ctest.projection.xml").read_bytes()
        for status, tag in (("run", "failure"), ("run", "error"), ("run", "skipped"),
                            ("notrun", None), ("fail", "failure")):
            root = gate.ET.fromstring(original)
            case = root.find("testcase")
            case.set("status", status)
            if tag:
                gate.ET.SubElement(case, tag).text = "Original diagnostic is retained privately."
            self.rewrite_public_artifact(public, "ctest.projection.xml",
                                         gate.ET.tostring(root, encoding="utf-8", xml_declaration=True))
            with self.subTest(status=status, tag=tag), self.assertRaises(RuntimeError):
                gate.check_public_package(public, gate.public_method_names(self.root), {"one"})

    def test_public_guard_rejects_failed_status_without_failure_marker(self):
        _, _, _, private = self.exercise_main(failing_step="ctest")
        public = private.parent / "public-evidence"
        root = gate.ET.parse(public / "ctest.projection.xml").getroot()
        root.find("testcase").set("status", "fail")
        self.rewrite_public_artifact(public, "ctest.projection.xml",
                                     gate.ET.tostring(root, encoding="utf-8", xml_declaration=True))
        with self.assertRaises(RuntimeError):
            gate.check_public_package(public, gate.public_method_names(self.root), {"one"})

    def test_public_package_guard_rejects_duplicate_index_key(self):
        _, _, _, private = self.exercise_main()
        public = private.parent / "public-evidence"
        path = public / "public-index.json"
        path.write_bytes(b'{"policy":{"request_body":"SYNTHETIC_PRIVATE"},' + path.read_bytes()[1:])
        with self.assertRaisesRegex(RuntimeError, "duplicate public JSON key"):
            gate.check_public_package(public, gate.public_method_names(self.root), {"one"})

    def test_public_package_guard_rejects_nested_duplicate_json_keys(self):
        _, _, _, private = self.exercise_main()
        public = private.parent / "public-evidence"
        names = gate.public_method_names(self.root)
        samples = [("summary.projection.json", b'"ASAN_OPTIONS":'),
                   ("source-before.json", b'"sha256":'),
                   ("expected-tests.json", b'"timeout":')]
        for name, key in samples:
            original = (public / name).read_bytes()
            injected = original.replace(key, key + b'{"request_body":"SYNTHETIC_PRIVATE"},' + key, 1)
            self.assertNotEqual(injected, original, "the fixture must inject an actual duplicate")
            self.rewrite_public_artifact(public, name, injected)
            with self.subTest(artifact=name), self.assertRaisesRegex(RuntimeError, "duplicate public JSON key"):
                gate.check_public_package(public, names, {"one"})
            self.rewrite_public_artifact(public, name, original)

    def test_public_package_guard_rejects_nonfinite_json_numbers(self):
        _, _, _, private = self.exercise_main()
        public = private.parent / "public-evidence"
        names = gate.public_method_names(self.root)
        samples = [("summary.projection.json", b'"name": "compiler-version",', b'"seconds":'),
                   ("expected-tests.json", b'"timeout": 90', None)]
        for name, anchor, inserted in samples:
            original = (public / name).read_bytes()
            for number in (b"NaN", b"Infinity", b"-Infinity", b"1e999"):
                replacement = anchor + inserted + number + b"," if inserted else b'"timeout": ' + number
                injected = original.replace(anchor, replacement, 1)
                self.assertNotEqual(injected, original, "the fixture must inject an actual number")
                self.rewrite_public_artifact(public, name, injected)
                with self.subTest(artifact=name, number=number), self.assertRaisesRegex(RuntimeError, "non-finite public JSON number"):
                    gate.check_public_package(public, names, {"one"})
                self.rewrite_public_artifact(public, name, original)

    def test_public_package_guard_rejects_discarded_xml_syntax(self):
        _, _, _, private = self.exercise_main()
        public = private.parent / "public-evidence"
        names = gate.public_method_names(self.root)
        original = (public / "ctest.projection.xml").read_bytes()
        declaration, body = original.split(b"\n", 1)
        comment = b"<!-- Authorization: Bearer SYNTHETIC_PRIVATE -->"
        instruction = b'<?diagnostic request_body="SYNTHETIC_PRIVATE"?>'
        doctype = b'<!DOCTYPE testsuite [<!ENTITY diagnostic "SYNTHETIC_PRIVATE">]>'
        samples = [("leading comment", declaration + b"\n" + comment + body),
                   ("nested comment", original.replace(b"<testsuite>", b"<testsuite>" + comment, 1)),
                   ("trailing comment", original + comment),
                   ("leading PI", declaration + b"\n" + instruction + body),
                   ("nested PI", original.replace(b"<testsuite>", b"<testsuite>" + instruction, 1)),
                   ("DTD", declaration + b"\n" + doctype + body)]
        for name, injected in samples:
            self.assertNotEqual(injected, original, "the fixture must inject actual XML syntax")
            self.rewrite_public_artifact(public, "ctest.projection.xml", injected)
            with self.subTest(syntax=name), self.assertRaisesRegex(RuntimeError, "noncanonical public JUnit XML"):
                gate.check_public_package(public, names, {"one"})

    def test_publication_guard_failure_keeps_originals_without_public_directory(self):
        original = gate.check_public_package

        def inject_then_check(directory, names, tests):
            path = directory / "public-index.json"
            path.write_bytes(b'{"policy":{"request_body":"SYNTHETIC_PRIVATE"},' + path.read_bytes()[1:])
            return original(directory, names, tests)

        with patch.object(gate, "check_public_package", side_effect=inject_then_check):
            result, summary, _, private = self.exercise_main()
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertIn("duplicate public JSON key", summary["public_evidence_error"])
        self.assertFalse((private.parent / "public-evidence").exists())
        self.assertTrue((private.parent / "public-evidence-pending").is_dir())
        self.assertEqual((private / "ctest-logs/LastTest.log").read_bytes(),
                         (private.parent / "Testing/Temporary/LastTest.log").read_bytes())
        self.assertTrue((private / "binaries-after.json").is_file())

    def test_publication_interrupt_cannot_leave_a_passed_private_summary(self):
        with patch.object(gate, "publish_public_evidence", side_effect=KeyboardInterrupt("publication barrier")):
            try:
                result, summary, _, private = self.exercise_main()
            except KeyboardInterrupt:
                # Read the original persisted receipt even when the old runner
                # lets the interruption escape its publication finally block.
                result = 1
                paths = list(self.root.glob("build-gcc-asan-*/private-evidence/summary.json"))
                self.assertEqual(len(paths), 1)
                private = paths[0].parent
                summary = json.loads(paths[0].read_text())
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertIn("ended_at", summary)
        self.assertTrue((private / "binaries-after.json").is_file())
        self.assertFalse((private.parent / "public-evidence").exists())

    def test_publication_interrupt_after_rename_quarantines_public_directory(self):
        original, published_bytes = gate.publish_public_evidence, None

        def publish_then_interrupt(source, build, private, summary):
            nonlocal published_bytes
            public = original(source, build, private, summary)
            published_bytes = {path.name: path.read_bytes() for path in public.iterdir()}
            raise KeyboardInterrupt("after publication rename")

        with patch.object(gate, "publish_public_evidence", side_effect=publish_then_interrupt):
            try:
                result, summary, _, private = self.exercise_main()
            except KeyboardInterrupt:
                result = 1
                paths = list(self.root.glob("build-gcc-asan-*/private-evidence/summary.json"))
                self.assertEqual(len(paths), 1)
                private = paths[0].parent
                summary = json.loads(paths[0].read_text())
        self.assertIsNotNone(published_bytes, "the injection must follow an actual completed rename")
        self.assertFalse((private.parent / "public-evidence").exists(),
                         "failed publication left an uploadable public directory")
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        quarantined = list(private.parent.glob("public-evidence-incomplete-*"))
        self.assertEqual(len(quarantined), 1)
        self.assertEqual({path.name: path.read_bytes() for path in quarantined[0].iterdir()}, published_bytes)
        self.assertTrue((private / "ctest-logs/LastTest.log").is_file())
        index = json.loads(published_bytes["public-index.json"])
        entry = index["artifacts"]["summary.projection.json"]
        self.assertEqual(entry["source"], "summary-publication-input.json")
        self.assertEqual(entry["source_sha256"], gate.digest(private / entry["source"]))
        self.assertEqual(json.loads((private / entry["source"]).read_text())["status"], "PASSED")

    def test_private_summary_stays_incomplete_until_publication_returns(self):
        original, status_at_publication = gate.publish_public_evidence, None

        def observe_then_publish(source, build, private, summary):
            nonlocal status_at_publication
            status_at_publication = json.loads((private / "summary.json").read_text())["status"]
            return original(source, build, private, summary)

        with patch.object(gate, "publish_public_evidence", side_effect=observe_then_publish):
            result, summary, _, private = self.exercise_main()
        self.assertEqual(status_at_publication, "NOT_COMPLETE")
        self.assertEqual(result, 0)
        self.assertEqual(summary["status"], "PASSED")
        self.assertEqual((private / "summary-publication-input.json").read_bytes(),
                         (private / "summary.json").read_bytes())

    def test_quarantine_failure_still_records_incomplete_terminal_receipt(self):
        original_publish, original_rename = gate.publish_public_evidence, Path.rename
        published_bytes = None

        def publish_then_interrupt(source, build, private, summary):
            nonlocal published_bytes
            public = original_publish(source, build, private, summary)
            published_bytes = {path.name: path.read_bytes() for path in public.iterdir()}
            raise KeyboardInterrupt("after publication rename")

        def refuse_quarantine(path, target):
            if path.name == "public-evidence":
                raise PermissionError("synthetic quarantine permission failure")
            return original_rename(path, target)

        with patch.object(gate, "publish_public_evidence", side_effect=publish_then_interrupt), \
                patch.object(Path, "rename", refuse_quarantine):
            try:
                result, summary, _, private = self.exercise_main()
            except PermissionError:
                # The old finalizer leaves its checkpoint without recording
                # why publication and quarantine both failed.
                result = 1
                paths = list(self.root.glob("build-gcc-asan-*/private-evidence/summary.json"))
                self.assertEqual(len(paths), 1)
                private = paths[0].parent
                summary = json.loads(paths[0].read_text())
        self.assertIsNotNone(published_bytes, "the injection must follow an actual checked publication")
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertEqual(summary.get("public_evidence_error"), "after publication rename",
                         "the final receipt lost the original publication failure")
        self.assertIn("synthetic quarantine permission failure", summary["public_quarantine_error"])
        self.assertIs(summary["failed_ci_required"], True)
        public = private.parent / "public-evidence"
        self.assertTrue(public.is_dir(), "the test must expose the unsuccessful quarantine boundary")
        self.assertEqual({path.name: path.read_bytes() for path in public.iterdir()}, published_bytes)
        self.assertEqual((private / "ctest-logs/LastTest.log").read_bytes(),
                         (private.parent / "Testing/Temporary/LastTest.log").read_bytes())
        self.assertTrue((private / "binaries-after.json").is_file())

    def check_identity_failure_keeps_binary_after(self, fault):
        result, summary, _, private = self.exercise_main(identity_fault=fault)
        self.assertEqual(result, 1)
        self.assertEqual(summary["status"], "NOT_COMPLETE")
        self.assertTrue((private / "binaries-after.json").is_file(),
                        "source/HEAD/dirty failure skipped the final binary snapshot")
        self.assertEqual((private / "binaries-before.json").read_bytes(), (private / "binaries-after.json").read_bytes())
        self.assertTrue((private / "ctest-logs/LastTest.log").is_file())

    def test_source_change_still_captures_binary_after(self):
        self.check_identity_failure_keeps_binary_after("source")

    def test_head_change_still_captures_binary_after(self):
        self.check_identity_failure_keeps_binary_after("head")

    def test_dirty_checkout_still_captures_binary_after(self):
        self.check_identity_failure_keeps_binary_after("dirty")

    def test_source_snapshot_error_still_captures_binary_after(self):
        original, calls = gate.tracked_snapshot, 0
        def snapshot(source, env):
            nonlocal calls
            calls += 1
            if calls == 2:
                raise RuntimeError("synthetic final source read failure")
            return original(source, env)
        with patch.object(gate, "tracked_snapshot", side_effect=snapshot):
            self.check_identity_failure_keeps_binary_after(None)

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
