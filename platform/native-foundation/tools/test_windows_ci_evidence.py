#!/usr/bin/env python3
"""Synthetic runner evidence checks; never build or execute the native product.

Only Windows/toolchain discovery and build/install are substituted. The CTest
process and its stdout, JUnit truncation and LastTest files are real. The timeout
case injects a partial supervised result without waiting for a product deadline.
The owned-process supervision boundary (windows_process.run_owned) is injectable
for these synthetic checks; production callers cannot disable it.
"""
import argparse
import ast
import datetime
import importlib.util
import io
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET


PARSER = argparse.ArgumentParser()
PARSER.add_argument('--runner', type=Path, default=Path(__file__).with_name('run_windows_ci.py'))
PARSER.add_argument('--source-root', type=Path, default=Path(__file__).resolve().parents[3])
PARSER.add_argument('--retain-fixtures', type=Path, help='optional disposable selfcheck evidence directory')
ARGS, UNITTEST_ARGS = PARSER.parse_known_args()
SPEC = importlib.util.spec_from_file_location('windows_ci_under_test', ARGS.runner.resolve())
RUNNER = importlib.util.module_from_spec(SPEC)
sys.path.insert(0, str(ARGS.runner.resolve().parent))
SPEC.loader.exec_module(RUNNER)
import windows_host
import windows_process
SOURCE = ARGS.source_root.resolve()
SUITES = {
    'native_modules': ('modules_tests.cpp', 30),
    'native_platform': ('platform_tests.cpp', 30),
    'native_readonly': ('readonly_tests.cpp', 30),
    'native_cli': ('test_native_cli.py', 60),
    'native_bundle_capture': ('test_bundle_capture.py', 30),
}


class WindowsEvidence(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='native-log-selfcheck-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        self.work = self.root / 'work'
        self.installation = self.root / 'visual-studio'
        self.programs = self.root / 'program-files'
        self.methods = {}
        self.original_ctest_stdout = None
        self.original_last_test = None
        self.binary_before_at_ctest_entry = None
        for suite, (name, _) in SUITES.items():
            relative = Path('platform/native-foundation/tests') / name
            destination = self.source / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / relative, destination)
            text = destination.read_text(encoding='utf-8')
            if name.endswith('.py'):
                self.methods[suite] = [method.name for cls in ast.parse(text).body
                    if isinstance(cls, ast.ClassDef) for method in cls.body
                    if isinstance(method, ast.FunctionDef) and method.name.startswith('test_')]
            else:
                self.methods[suite] = re.findall(r'\{"([^"\n]+)",', text.split('return native_test::run({')[1])
        tool = self.source / 'platform/native-foundation/tools/verify_detached.py'
        tool.parent.mkdir(parents=True)
        tool.write_text('# unused synthetic detached verifier\n', encoding='utf-8')
        for path in (self.installation / 'VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe',
                     self.programs / 'Windows Kits/10/Include/10.0.26100.0/um/Windows.h'):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic pinned-tool input, never executed\n')

    def tearDown(self):
        if ARGS.retain_fixtures is not None:
            destination = ARGS.retain_fixtures / self._testMethodName
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(self.root, destination)

    def configure_fixture(self, mode):
        build = self.work / 'build'
        (build / 'CMakeFiles/synthetic').mkdir(parents=True)
        for name in ('CMakeCache.txt', 'native-toolchain.txt', 'CMakeFiles/synthetic/CMakeCXXCompiler.cmake'):
            (build / name).write_text('synthetic metadata; no compiler executed\n', encoding='utf-8')
        script = build / 'synthetic_methods.py'
        script.write_text('''import json, sys, unittest
suite, mode, raw_names = sys.argv[1:]
names = json.loads(raw_names)
if mode == 'missing' and suite == 'native_cli':
    names = names[:-1]
if suite in ('native_modules', 'native_platform', 'native_readonly'):
    for name in names:
        print('PASS ' + name)
    if mode == 'duplicate' and suite == 'native_modules':
        print('PASS ' + names[0])
    print('RESULT %d passed, 0 failed' % len(names))
else:
    class SyntheticMethods(unittest.TestCase):
        pass
    for name in names:
        def method(self, name=name):
            if suite == 'native_cli' and name == names[-1]:
                if mode == 'failure':
                    self.fail('synthetic final method failure')
                if mode == 'skipped':
                    self.skipTest('synthetic skip must remain incomplete')
        setattr(SyntheticMethods, name, method)
    unittest.main(argv=[sys.argv[0]], verbosity=2)
''', encoding='utf-8')
        registrations = []
        for suite, (_, timeout) in SUITES.items():
            argv = [sys.executable, script, suite, mode, json.dumps(self.methods[suite])]
            command = ' '.join('[==[' + os.fspath(arg).replace('\\', '/') + ']==]' for arg in argv)
            registrations.append('add_test(' + suite + ' ' + command + ')\n'
                                 'set_tests_properties(' + suite + ' PROPERTIES TIMEOUT ' + str(timeout) + ')\n')
        (build / 'CTestTestfile.cmake').write_text(''.join(registrations), encoding='utf-8')

    def invoke(self, mode='success', *, windows11=False, changed_image=False, host=None,
               unproven_ctest=False):
        real_run = subprocess.run
        self.supervised_calls = []

        def owned_receipt(command_id, *, exit_code=0, proven=True, outcome=None,
                          status='COMPLETED', extra_env=None):
            return dict(schema='wls-owned-process-receipt-v1', command_id=command_id,
                argv=[command_id], cwd=str(self.work),
                task_level_env_overrides=dict(extra_env) if extra_env else {},
                proof_rule='root handle signaled AND actual exit code read AND successful '
                           'same-Job ActiveProcesses==0 query',
                execution_timeout_seconds=30, cleanup_timeout_seconds=5.0,
                started_at_utc='2026-11-05T00:00:00+00:00',
                ended_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                execution_elapsed_seconds=0.05,
                establishment={'job_created': True, 'kill_on_close_configured': True,
                    'process_created_suspended': True, 'assigned_to_job': True,
                    'primary_thread_resumed': True,
                    'primary_thread_resume_precedes_any_target_code': True},
                root_identity={'pid': 4242, 'pid_and_creation_time_are_diagnostic_only': True,
                    'creation_filetime': None, 'handle_is_operational_identity': True},
                exit_code_available=True, exit_code=exit_code,
                termination={'reason': None, 'requested': False, 'api': None,
                             'requested_exit_code': None, 'job_terminate_succeeded': None,
                             'pre_assign_process_terminate_succeeded': None},
                job_zero_observation={'query_ok': True, 'last_active_processes': 0,
                                      'zero_observed': True, 'query_failures': []},
                root_wait={'result': 'SIGNALED', 'signaled': True, 'signaled_at_utc':
                           datetime.datetime.now(datetime.timezone.utc).isoformat()},
                settle={'configured_seconds': 1.0, 'effective_seconds': 1.0,
                        'elapsed_seconds': 0.0, 'status': 'NATURAL_ZERO'},
                cleanup={'timeout_seconds': 5.0, 'elapsed_seconds': 0.01,
                         'status': 'PROVEN_ZERO' if proven else 'UNPROVEN',
                         'errors': [], 'handles_closed': {},
                         'second_interrupt_recorded_and_deferred': False},
                output={'stdout_bytes': None, 'stdout_sha256': None, 'stderr_bytes': None,
                        'stderr_sha256': None, 'complete': True, 'capture_errors': [],
                        'spool_directory': None, 'spool_retained': True},
                tree_exit_proven=proven,
                outcome=outcome or ('COMPLETED' if proven else 'UNPROVEN'),
                failure_reason=None)

        def owned(command, *, cwd, env, timeout, spool_root, command_id, extra_env=None, **kwargs):
            command = [os.fspath(arg) for arg in command]
            self.supervised_calls.append({'command_id': command_id, 'argv': command,
                                          'timeout': timeout})
            output = b''
            if command[0].endswith('vswhere.exe'):
                output = json.dumps([{'installationPath': str(self.installation)}]).encode()
            elif command[0] == 'git' and 'rev-parse' in command:
                output = b'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n'
            elif command[0] == 'git' and 'ls-files' in command:
                self.assertEqual(timeout, 30, 'source snapshot keeps its new explicit 30 s deadline')
                output = listing
            elif command[0] == 'cmake' and '-S' in command:
                self.configure_fixture('failure' if mode == 'failure-source-missing' else mode)
            elif command[0] == 'cmake' and '--build' in command:
                directory = self.work / 'build/Release'
                directory.mkdir()
                if mode != 'empty-binaries':
                    (directory / 'synthetic.exe').write_bytes(b'not executable\n')
            elif command[0] == 'ctest':
                self.assertEqual(command, ['ctest', '--test-dir', str(self.work / 'build'), '-C', 'Release',
                    '--output-on-failure', '--output-junit', str(self.work / 'evidence/ctest.xml')])
                self.assertEqual(timeout, 300)
                # Identities must already exist before the original CTest starts.
                identity_path = self.work / 'evidence/binary-inputs-before.json'
                self.binary_before_at_ctest_entry = identity_path.read_bytes() if identity_path.is_file() else None
                if mode in ('timeout', 'interrupt'):
                    temporary = self.work / 'build/Testing/Temporary'
                    temporary.mkdir(parents=True)
                    self.original_last_test = b'original partial CTest output\r\n'
                    (temporary / 'LastTest.log.tmp').write_bytes(self.original_last_test)
                    if mode == 'interrupt':
                        error = KeyboardInterrupt('synthetic original CTest interruption')
                        error.process_receipt = owned_receipt('ctest', proven=True)
                        raise error
                    raise_error = subprocess.TimeoutExpired(command, 300,
                        output=b'partial stdout\r\n', stderr=b'partial stderr\r\n')
                    raise_error.process_receipt = owned_receipt('ctest', proven=True)
                    raise raise_error
                result = real_run(command, cwd=cwd, env=env, timeout=timeout,
                                  capture_output=True)
                self.original_ctest_stdout = result.stdout
                self.original_last_test = (self.work / 'build/Testing/Temporary/LastTest.log').read_bytes()
                if mode == 'source-change':
                    with (self.source / 'platform/native-foundation/tools/verify_detached.py').open('ab') as stream:
                        stream.write(b'# changed during the synthetic CTest\n')
                elif mode == 'binary-change':
                    (self.work / 'build/Release/synthetic.exe').write_bytes(b'changed synthetic bytes\n')
                elif mode == 'failure-source-missing':
                    (self.source / 'platform/native-foundation/tools/verify_detached.py').unlink()
                if unproven_ctest:
                    return SimpleNamespace(stdout=result.stdout, stderr=result.stderr,
                        exit_code=0, outcome='UNPROVEN', tree_exit_proven=False,
                        receipt=owned_receipt('ctest', proven=False, outcome='UNPROVEN'))
                return SimpleNamespace(stdout=result.stdout, stderr=result.stderr,
                    exit_code=result.returncode, outcome='COMPLETED', tree_exit_proven=True,
                    receipt=owned_receipt('ctest', exit_code=result.returncode))
            elif command[0] == 'cmake' and '--install' in command:
                directory = self.work / 'installed/bin'
                directory.mkdir(parents=True)
                (directory / 'wls-native-foundation.exe').write_bytes(b'not executable\n')
            elif command[-1] == 'capabilities':
                output = b'{"token_elevation":"not_elevated","filesystem":"NTFS"}\n'
            return SimpleNamespace(stdout=output, stderr=b'', exit_code=0,
                outcome='COMPLETED', tree_exit_proven=True,
                receipt=owned_receipt(command_id, extra_env=extra_env))

        environment = dict(os.environ, GITHUB_ACTIONS='true', GITHUB_SHA='a' * 40,
                           GITHUB_RUN_ID='123', GITHUB_RUN_ATTEMPT='1', GITHUB_JOB='native-foundation',
                           ImageOS='synthetic-win22', ImageVersion='synthetic-image')
        environment['ProgramFiles(x86)'] = str(self.programs)
        fake_os = SimpleNamespace(name='nt', environ=environment, fspath=os.fspath)
        listing = b'\0'.join(str(path.relative_to(self.source)).replace('\\', '/').encode()
                              for path in self.source.rglob('*') if path.is_file()) + b'\0'
        fake_subprocess = SimpleNamespace(run=lambda command, **kwargs: (_ for _ in ()).throw(
            AssertionError('subprocess.run is not the supervision boundary')),
            check_output=lambda command: (_ for _ in ()).throw(
                AssertionError('subprocess.check_output is not the supervision boundary')),
            PIPE=subprocess.PIPE, TimeoutExpired=subprocess.TimeoutExpired)
        with patch.object(RUNNER, 'os', fake_os), patch.object(RUNNER, 'subprocess', fake_subprocess), \
             patch.object(RUNNER.windows_process, 'run_owned', owned), \
             patch.object(sys, 'getwindowsversion', return_value=(10, 0, 20348), create=True), \
             patch.object(RUNNER.platform, 'platform', return_value='synthetic-Windows-host'), \
             patch.object(sys, 'argv', ['run_windows_ci.py', '--source-root', str(self.source), '--work-root', str(self.work)]), \
             patch.object(sys, 'stdout', SimpleNamespace(buffer=io.BytesIO())), \
             patch.object(sys, 'stderr', SimpleNamespace(buffer=io.BytesIO())):
            if windows11:
                with patch.object(windows_host, 'observe_host', return_value=host or ordinary_windows11()):
                    RUNNER.run_native(self.source, self.work, windows11=True)
            else:
                snapshot = self.root / 'image-environment.json'
                capture = RUNNER.image_environment('a' * 40)
                if changed_image:
                    capture['image_environment']['ImageVersion'] = 'earlier-different-image'
                snapshot.write_text(json.dumps(capture), encoding='utf-8')
                with patch.object(sys, 'argv', [*sys.argv, '--image-environment-snapshot', str(snapshot)]):
                    RUNNER.main()

    def test_success_retains_original_full_lasttest_beyond_junit_truncation(self):
        self.invoke()
        evidence = self.work / 'evidence'
        self.assertEqual((evidence / 'ctest.stdout').read_bytes(), self.original_ctest_stdout)
        output = next(case.findtext('system-out', '') for case in ET.parse(evidence / 'ctest.xml').findall('.//testcase')
                      if case.get('name') == 'native_cli')
        self.assertIn('test output was removed', output)
        self.assertEqual((evidence / 'ctest-logs/LastTest.log').read_bytes(), self.original_last_test)
        receipt = json.loads((evidence / 'ctest-methods.json').read_text())
        self.assertEqual(receipt['status'], 'PASSED')
        self.assertEqual({row['suite']: row['passed_methods'] for row in receipt['suites']}, self.methods)
        self.assertEqual(sum(len(row['passed_methods']) for row in receipt['suites']), 34)
        result = json.loads((evidence / 'result.json').read_text())
        self.assertEqual(result['windows_11_machine_acceptance'], 'NOT_RUN')
        self.assertEqual(result['ordinary_user_security_acceptance'], 'NOT_RUN')
        self.assertEqual(result['exact_runner_image_identity'], 'UNRESOLVED_REQUIRES_JOB_HEADER_COMPARISON')
        provenance = json.loads((evidence / 'image-provenance.json').read_text())
        self.assertEqual(provenance['process_environment_consistency'], 'UNCHANGED')
        self.assertIn('same-job Runner Image', provenance['job_header_comparison'])
        self.assert_complete_identity_pairs()
        self.assertTrue(result['complete_input_identities_verified'])
        self.assertEqual((evidence / 'binary-inputs.json').read_bytes(),
                         (evidence / 'binary-inputs-before.json').read_bytes())

    def assert_complete_identity_pairs(self):
        evidence = self.work / 'evidence'
        self.assertIsNotNone(self.binary_before_at_ctest_entry)
        self.assertEqual(self.binary_before_at_ctest_entry,
                         (evidence / 'binary-inputs-before.json').read_bytes())
        for prefix in ('source', 'binary'):
            before = json.loads((evidence / (prefix + '-inputs-before.json')).read_text())
            after = json.loads((evidence / (prefix + '-inputs-after.json')).read_text())
            self.assertTrue(before)
            self.assertEqual(before, after)
        identity = json.loads((evidence / 'identity-finalization.json').read_text())
        self.assertTrue(identity['identities_complete'])
        self.assertTrue(identity['source_inputs_unchanged'])
        self.assertTrue(identity['binary_inputs_unchanged'])
        self.assertEqual(identity['capture_errors'], [])

    def test_failure_retains_original_lasttest_and_failed_index(self):
        with self.assertRaisesRegex(RuntimeError, 'ctest failed with exit'):
            self.invoke('failure')
        evidence = self.work / 'evidence'
        self.assertEqual((evidence / 'ctest-logs/LastTest.log').read_bytes(), self.original_last_test)
        self.assertIn(b'native_cli', (evidence / 'ctest-logs/LastTestsFailed.log').read_bytes())
        self.assertFalse((evidence / 'result.json').exists())
        self.assert_complete_identity_pairs()
        self.assertFalse((evidence / 'binary-inputs.json').exists())
        failure = json.loads((evidence / 'failure.json').read_text())
        self.assertEqual((failure['type'], failure['termination']), ('RuntimeError', 'FAILED'))
        self.assertEqual(json.loads((evidence / 'ctest.command.json').read_text())['exit_code'], 8)

    def test_timeout_retains_original_partial_log_and_stdout(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            self.invoke('timeout')
        evidence = self.work / 'evidence'
        self.assertEqual((evidence / 'ctest-logs/LastTest.log.tmp').read_bytes(), self.original_last_test)
        self.assertEqual((evidence / 'ctest.stdout').read_bytes(), b'partial stdout\r\n')
        self.assertEqual(json.loads((evidence / 'ctest.command.json').read_text())['status'], 'TIMEOUT')
        self.assertFalse((evidence / 'result.json').exists())
        self.assert_complete_identity_pairs()
        failure = json.loads((evidence / 'failure.json').read_text())
        self.assertEqual((failure['type'], failure['termination']), ('TimeoutExpired', 'TIMEOUT'))

    def test_interrupt_retains_identities_and_original_exception(self):
        with self.assertRaisesRegex(KeyboardInterrupt, 'synthetic original CTest interruption'):
            self.invoke('interrupt')
        evidence = self.work / 'evidence'
        self.assert_complete_identity_pairs()
        self.assertEqual((evidence / 'ctest-logs/LastTest.log.tmp').read_bytes(), self.original_last_test)
        self.assertEqual(json.loads((evidence / 'ctest.command.json').read_text())['status'], 'INTERRUPTED')
        failure = json.loads((evidence / 'failure.json').read_text())
        self.assertEqual((failure['type'], failure['termination']), ('KeyboardInterrupt', 'INTERRUPTED'))
        self.assertFalse((evidence / 'result.json').exists())

    def test_source_and_binary_changes_reject_apparently_successful_execution(self):
        for mode, prefix in (('source-change', 'source'), ('binary-change', 'binary')):
            with self.subTest(mode=mode):
                self.work = self.root / mode
                with self.assertRaisesRegex(RuntimeError, prefix + ' input bytes changed'):
                    self.invoke(mode)
                evidence = self.work / 'evidence'
                before = json.loads((evidence / (prefix + '-inputs-before.json')).read_text())
                after = json.loads((evidence / (prefix + '-inputs-after.json')).read_text())
                self.assertTrue(before)
                self.assertTrue(after)
                self.assertNotEqual(before, after)
                identity = json.loads((evidence / 'identity-finalization.json').read_text())
                self.assertFalse(identity[prefix + '_inputs_unchanged'])
                self.assertFalse((evidence / 'result.json').exists())

    def test_failed_source_capture_preserves_original_ctest_failure_and_binary_identity(self):
        with self.assertRaisesRegex(RuntimeError, '^ctest failed with exit 8$'):
            self.invoke('failure-source-missing')
        evidence = self.work / 'evidence'
        self.assertEqual((evidence / 'binary-inputs-before.json').read_bytes(),
                         (evidence / 'binary-inputs-after.json').read_bytes())
        identity = json.loads((evidence / 'identity-finalization.json').read_text())
        self.assertFalse(identity['identities_complete'])
        self.assertIsNone(identity['source_inputs_unchanged'])
        self.assertTrue(identity['binary_inputs_unchanged'])
        self.assertEqual([(item['evidence'], item['type']) for item in identity['capture_errors']],
                         [('source-inputs-after.json', 'FileNotFoundError')])
        failure = json.loads((evidence / 'failure.json').read_text())
        self.assertEqual((failure['type'], failure['reason']), ('RuntimeError', 'ctest failed with exit 8'))
        self.assertEqual(failure['capture_errors'], identity['capture_errors'])
        self.assertFalse((evidence / 'result.json').exists())

    def test_failed_log_capture_does_not_mask_original_ctest_failure(self):
        with patch.object(RUNNER, 'retain_logs', side_effect=OSError('synthetic log capture error')):
            with self.assertRaisesRegex(RuntimeError, '^ctest failed with exit 8$'):
                self.invoke('failure')
        evidence = self.work / 'evidence'
        self.assertEqual((evidence / 'source-inputs-before.json').read_bytes(),
                         (evidence / 'source-inputs-after.json').read_bytes())
        self.assertEqual((evidence / 'binary-inputs-before.json').read_bytes(),
                         (evidence / 'binary-inputs-after.json').read_bytes())
        failure = json.loads((evidence / 'failure.json').read_text())
        self.assertEqual((failure['type'], failure['reason']), ('RuntimeError', 'ctest failed with exit 8'))
        self.assertEqual([(item['evidence'], item['type']) for item in failure['capture_errors']],
                         [('ctest-log-files.json', 'OSError')])
        self.assertFalse((evidence / 'result.json').exists())

    def test_empty_binary_capture_refuses_before_ctest(self):
        with self.assertRaisesRegex(RuntimeError, '^native binary input snapshot is empty$'):
            self.invoke('empty-binaries')
        evidence = self.work / 'evidence'
        identity = json.loads((evidence / 'identity-finalization.json').read_text())
        self.assertFalse(identity['identities_complete'])
        self.assertFalse(identity['binary_pair_complete'])
        self.assertIsNone(identity['binary_inputs_unchanged'])
        self.assertFalse((evidence / 'ctest.command.json').exists())
        self.assertFalse((evidence / 'result.json').exists())

    def test_failed_command_output_capture_keeps_original_exit_and_other_records(self):
        original_write = Path.write_bytes
        def fail_ctest_stdout(path, data):
            if path == self.work / 'evidence/ctest.stdout':
                raise OSError('synthetic CTest output capture failure')
            return original_write(path, data)
        with patch.object(Path, 'write_bytes', fail_ctest_stdout):
            with self.assertRaisesRegex(RuntimeError, '^ctest failed with exit 8$'):
                self.invoke('failure')
        evidence = self.work / 'evidence'
        self.assertEqual(json.loads((evidence / 'ctest.command.json').read_text())['exit_code'], 8)
        self.assertTrue((evidence / 'ctest.stderr').is_file())
        self.assertEqual((evidence / 'ctest-logs/LastTest.log').read_bytes(), self.original_last_test)
        failure = json.loads((evidence / 'failure.json').read_text())
        self.assertEqual((failure['type'], failure['reason']), ('RuntimeError', 'ctest failed with exit 8'))
        self.assertEqual([(item['evidence'], item['type']) for item in failure['capture_errors']],
                         [('ctest.stdout', 'OSError')])
        identity = json.loads((evidence / 'identity-finalization.json').read_text())
        self.assertFalse(identity['identities_complete'])
        self.assertTrue(identity['source_pair_complete'])
        self.assertTrue(identity['binary_pair_complete'])
        self.assertFalse((evidence / 'result.json').exists())

    def test_missing_method_cannot_be_inferred_from_five_green_suites(self):
        with self.assertRaisesRegex(RuntimeError, 'method evidence'):
            self.invoke('missing')
        self.assertFalse((self.work / 'evidence/result.json').exists())

    def test_duplicate_method_is_not_a_second_pass(self):
        with self.assertRaisesRegex(RuntimeError, 'method evidence'):
            self.invoke('duplicate')

    def test_skipped_method_remains_incomplete(self):
        with self.assertRaisesRegex(RuntimeError, 'method evidence'):
            self.invoke('skipped')

    def test_changed_image_environment_stops_before_toolchain_or_build(self):
        with self.assertRaisesRegex(RuntimeError, 'environment or run/checkout binding changed'):
            self.invoke(changed_image=True)
        evidence = self.work / 'evidence'
        receipt = json.loads((evidence / 'image-provenance.json').read_text())
        self.assertEqual(receipt['changed_fields'], ['image_environment'])
        self.assertEqual(receipt['process_environment_consistency'], 'CHANGED')
        self.assertFalse((evidence / 'vswhere.command.json').exists())
        self.assertFalse((evidence / 'result.json').exists())

    def test_windows11_entry_retains_five_suites_without_claiming_security_acceptance(self):
        self.invoke(windows11=True)
        evidence = self.work / 'evidence'
        result = json.loads((evidence / 'result.json').read_text())
        self.assertEqual(result['native_suites'], 5)
        self.assertEqual(result['native_methods'], 34)
        self.assertEqual(result['windows_11_component_validation'], 'PASSED')
        self.assertEqual(result['windows_11_machine_acceptance'], 'NOT_RUN')
        self.assertEqual(result['ordinary_user_security_acceptance'], 'NOT_RUN')
        self.assertEqual(result['full_native_runtime'], 'NOT_IMPLEMENTED')
        self.assertTrue((evidence / 'windows11-host.json').is_file())
        self.assertEqual((evidence / 'ctest-logs/LastTest.log').read_bytes(), self.original_last_test)

    def test_server_cannot_start_the_windows11_toolchain_or_tests(self):
        host = dict(ordinary_windows11(), os_product_type=3)
        with self.assertRaisesRegex(RuntimeError, 'Server does not qualify'):
            self.invoke(windows11=True, host=host)
        evidence = self.work / 'evidence'
        self.assertTrue((evidence / 'windows11-host.json').is_file())
        self.assertFalse((evidence / 'vswhere.command.json').exists())
        self.assertFalse((evidence / 'configure.command.json').exists())
        self.assertFalse((evidence / 'result.json').exists())

    def test_owned_supervision_boundary_covers_all_command_entry_classes(self):
        self.invoke()
        entries = {row['command_id']: row for row in self.supervised_calls}
        for required in ('git-head', 'vswhere', 'git-status', 'source-snapshot-before',
                         'cmake-version', 'configure', 'build', 'ctest', 'install',
                         'host-capabilities', 'detached', 'source-snapshot-after'):
            self.assertIn(required, entries, 'every planned command must use the boundary')
        # The three external-command entry classes keep their distinct deadlines.
        self.assertEqual(entries['git-head']['timeout'], 120)
        self.assertEqual(entries['git-status']['timeout'], 120)
        self.assertEqual(entries['source-snapshot-before']['timeout'], 30)
        self.assertEqual(entries['source-snapshot-after']['timeout'], 30)
        self.assertEqual(entries['ctest']['timeout'], 300)
        evidence = self.work / 'evidence'
        self.assertTrue((evidence / 'ctest.process.json').is_file())
        self.assertTrue((evidence / 'git-head.process.json').is_file())

    def test_unproven_owned_tree_blocks_result_and_stable_after_snapshots(self):
        with self.assertRaisesRegex(RuntimeError, 'ctest owned process tree exit could not be proven'):
            self.invoke(unproven_ctest=True)
        evidence = self.work / 'evidence'
        self.assertFalse((evidence / 'result.json').exists())
        self.assertFalse((evidence / 'source-inputs-after.json').exists())
        identity = json.loads((evidence / 'identity-finalization.json').read_text())
        self.assertFalse(identity['identities_complete'])
        self.assertFalse(identity['after_snapshots_available'])
        self.assertEqual(identity['retained_originals_stability'], 'UNSTABLE_PARTIAL')
        self.assertEqual(identity['owned_process_supervision']['unproven_owned_trees'], ['ctest'])
        failure = json.loads((evidence / 'failure.json').read_text())
        self.assertFalse(failure['owned_process_supervision']['all_owned_process_trees_exited'])
        self.assertIn('unproven_owned_trees', failure['identity_finalization_availability'])
        ctest_command = json.loads((evidence / 'ctest.command.json').read_text())
        self.assertFalse(ctest_command['tree_exit_proven'])
        self.assertEqual(ctest_command['supervision_outcome'], 'UNPROVEN')

    def test_result_json_states_all_owned_process_trees_exited(self):
        self.invoke()
        evidence = self.work / 'evidence'
        result = json.loads((evidence / 'result.json').read_text())
        self.assertTrue(result['all_owned_process_trees_exited'])
        started = result['owned_process_supervision']['started']
        self.assertIn('ctest', started)
        self.assertTrue(all(row['tree_exit_proven'] for row in started.values()))
        receipt = json.loads((evidence / 'ctest.process.json').read_text())
        self.assertTrue(receipt['tree_exit_proven'])
        self.assertEqual(receipt['proof_rule'],
                         'root handle signaled AND actual exit code read AND successful '
                         'same-Job ActiveProcesses==0 query')

    def test_configure_and_build_carry_only_task_level_msbuild_override(self):
        self.invoke()
        entries = {row['command_id']: row for row in self.supervised_calls}
        ctest_argv = entries['ctest']['argv']
        self.assertNotIn('MSBUILDDISABLENODEREUSE', json.dumps(ctest_argv))
        evidence = self.work / 'evidence'
        for name in ('configure', 'build'):
            receipt = json.loads((evidence / (name + '.process.json')).read_text())
            self.assertEqual(receipt['task_level_env_overrides'], {'MSBUILDDISABLENODEREUSE': '1'})
        others = ('git-head', 'vswhere', 'git-status', 'ctest', 'install', 'host-capabilities')
        for name in others:
            receipt = json.loads((evidence / (name + '.process.json')).read_text())
            self.assertEqual(receipt['task_level_env_overrides'], {})


# --------------------------------------------------------------------------------------
# Configure-only owned-Job member diagnostics (plan-final-v7). The three checks
# below drive the actual runner configure branch and the actual
# windows_process.run_owned/_closeout path with low-level injected Win32-API and
# clock fakes; the diagnostic observer itself is never stubbed.
# --------------------------------------------------------------------------------------
import ctypes


class _DiagnosticFakeClock:
    """Injected clock; monotonic() may advance a fixed step to cross the 200 ms budget."""

    is_injected = True

    def __init__(self, start=2000.0, step=0.0):
        self.now = float(start)
        self.step = float(step)
        self.monotonic_calls = 0

    def monotonic(self):
        self.monotonic_calls += 1
        value = self.now
        self.now += self.step
        return value

    def sleep(self, seconds):
        self.now += max(0.0, seconds)

    def advance_milliseconds(self, milliseconds):
        self.now += max(0.0, milliseconds / 1000.0)


def _classify_flow_command(command_line):
    if 'vswhere.exe' in command_line:
        return 'vswhere'
    if 'rev-parse' in command_line:
        return 'git-head'
    if 'ls-files' in command_line:
        return 'ls-files'
    if 'status --porcelain' in command_line:
        return 'git-status'
    if '--version' in command_line:
        return 'cmake-version'
    if ' -S ' in command_line:
        return 'configure'
    if '--build' in command_line:
        return 'build'
    return 'unknown'


class _FlowFakeAPI:
    """Scripted Win32ProcessAPI stand-in driving the real run_owned lifecycle.

    One instance per supervised command; behaviour derives from the launched
    command line. The configure command leaves synthetic survivor members in
    its Job so the natural settle expires and the original owned cleanup must
    terminate the Job.
    """

    is_injected = True
    instances = []
    configure_members = (7001, 7002, 7003)
    configure_descendants = True
    installation_path = None
    work_path = None
    listing = b''

    def __init__(self):
        self.calls = []
        self.command_line = None
        self.kind = None
        self.job = 'flow-job-{}'.format(len(_FlowFakeAPI.instances))
        self.resumed = False
        self.terminated = False
        self.root_exit_code = 0
        self.spool_stdout_path = None
        self.observation_handles = {}
        self.closed_observation_handles = []
        _FlowFakeAPI.instances.append(self)

    def _log(self, name, **details):
        self.calls.append((name, details))

    # ---- Win32ProcessAPI surface -----------------------------------------------------
    def create_job(self):
        self._log('create_job')
        return self.job

    def set_job_kill_on_close(self, job):
        self._log('set_job_kill_on_close')

    def open_input_null(self):
        self._log('open_input_null')
        return 'flow-stdin'

    def open_output_spool(self, path):
        self._log('open_output_spool', path=str(path))
        Path(path).write_bytes(b'')
        if str(path).endswith('.stdout.spool'):
            self.spool_stdout_path = Path(path)
        return 'flow-spool-' + Path(path).name

    def create_process_suspended(self, **kwargs):
        self.command_line = kwargs['command_line']
        self.kind = _classify_flow_command(self.command_line)
        self._log('create_process_suspended', kind=self.kind)
        if self.kind == 'configure' and _FlowFakeAPI.work_path is not None:
            build = Path(_FlowFakeAPI.work_path) / 'build'
            (build / 'CMakeFiles/synthetic').mkdir(parents=True, exist_ok=True)
            for name in ('CMakeCache.txt', 'native-toolchain.txt',
                         'CMakeFiles/synthetic/CMakeCXXCompiler.cmake'):
                (build / name).write_text('synthetic metadata; no compiler executed\n',
                                          encoding='utf-8')
        if self.kind == 'build':
            self.root_exit_code = 1
        if self.spool_stdout_path is not None:
            self.spool_stdout_path.write_bytes(_flow_payload(self.kind))
        return 'flow-root-process', 'flow-root-thread', 4242, 133450000000000000

    def assign_process_to_job(self, job, process):
        self._log('assign_process_to_job', job=job)

    def resume_thread(self, thread):
        self._log('resume_thread')
        self.resumed = True
        return 1

    def wait_process(self, process, milliseconds):
        self._log('wait_process', handle=process, milliseconds=int(milliseconds))
        if process in self.observation_handles:
            return 'TIMEOUT'
        return 'SIGNALED' if self.resumed else 'TIMEOUT'

    def process_exit_code(self, process):
        self._log('process_exit_code')
        return self.root_exit_code

    def process_id(self, process):
        if process in self.observation_handles:
            return self.observation_handles[process]
        return 4242

    def query_job_active_processes(self, job):
        self._log('query_job_active_processes', job=job)
        if self.terminated:
            return 0
        if self.kind == 'configure' and _FlowFakeAPI.configure_descendants:
            return len(_FlowFakeAPI.configure_members)
        return 0

    def terminate_job(self, job, exit_code):
        self._log('terminate_job', job=job, exit_code=int(exit_code))
        self.terminated = True

    def close_handle(self, handle):
        self._log('close_handle', handle=handle)
        if handle in self.observation_handles:
            self.closed_observation_handles.append(handle)

    # ---- read-only observer surface --------------------------------------------------
    def _creation_time(self, process):
        if process in self.observation_handles:
            return 133450000000000000 + self.observation_handles[process]
        return 133450000000000000

    def job_process_ids(self, job):
        self._log('job_process_ids', job=job)
        if self.kind == 'configure' and _FlowFakeAPI.configure_descendants:
            return list(_FlowFakeAPI.configure_members)
        return []

    def open_process_snapshot(self, pid):
        self._log('open_process_snapshot', pid=int(pid))
        handle = 'flow-observe-{}'.format(pid)
        self.observation_handles[handle] = int(pid)
        return handle

    def IsProcessInJob(self, process, job, result):
        self._log('IsProcessInJob', handle=process, job=job)
        result._obj.value = 1 if process in self.observation_handles and job == self.job else 0
        return 1

    def QueryFullProcessImageNameW(self, process, flags, buffer, size):
        pid = self.observation_handles.get(process)
        self._log('QueryFullProcessImageNameW', handle=process, flags=int(flags))
        path = 'C:\\synthetic-survivor-{}.exe'.format(pid)
        buffer.value = path
        size._obj.value = len(path)
        return 1


def _flow_payload(kind):
    if kind == 'git-head':
        return b'a' * 40 + b'\n'
    if kind == 'vswhere':
        return json.dumps([{'installationPath': _FlowFakeAPI.installation_path}]).encode()
    if kind == 'ls-files':
        return _FlowFakeAPI.listing
    if kind == 'cmake-version':
        return b'cmake version 3.99.0-synthetic\n'
    return b''


class _ScriptedDiagnosticAPI:
    """Low-level scripted Win32ProcessAPI stand-in for the diagnostic subcases."""

    is_injected = True

    def __init__(self, script, clock):
        self.script = dict(script)
        self.clock = clock
        self.calls = []
        self.resumed = False
        self.terminated = False
        self.job = 'diag-job-1'
        self.members = list(script.get('members', ()))
        self.observation_handles = {}
        self.closed_observation_handles = []
        self._wait_error_fired = False

    def _log(self, name, **details):
        self.calls.append((name, details))

    def create_job(self):
        self._log('create_job')
        return self.job

    def set_job_kill_on_close(self, job):
        self._log('set_job_kill_on_close')

    def open_input_null(self):
        self._log('open_input_null')
        return 'diag-stdin'

    def open_output_spool(self, path):
        self._log('open_output_spool', path=str(path))
        return 'diag-spool-' + Path(path).name

    def create_process_suspended(self, **kwargs):
        self._log('create_process_suspended')
        return 'diag-root-process', 'diag-root-thread', 4242, 133450000000000000

    def assign_process_to_job(self, job, process):
        self._log('assign_process_to_job', job=job)

    def resume_thread(self, thread):
        self._log('resume_thread')
        self.resumed = True
        return 1

    def wait_process(self, process, milliseconds):
        self.clock.advance_milliseconds(milliseconds)
        self._log('wait_process', handle=process, milliseconds=int(milliseconds))
        if process in self.observation_handles:
            return 'SIGNALED' if process in self.script.get('member_signaled', ()) else 'TIMEOUT'
        if self.script.get('wait_raises') is not None and not self._wait_error_fired:
            self._wait_error_fired = True
            raise self.script['wait_raises']
        if self.script.get('wait_never_signals'):
            return 'TIMEOUT'
        return 'SIGNALED' if self.resumed else 'TIMEOUT'

    def process_exit_code(self, process):
        self._log('process_exit_code')
        return 0

    def process_id(self, process):
        if process in self.observation_handles:
            actual = self.observation_handles[process]
            return self.script.get('pid_reuse', {}).get(actual, actual)
        return 4242

    def query_job_active_processes(self, job):
        self._log('query_job_active_processes', job=job)
        if self.script.get('settle_query_error'):
            raise windows_process.Win32CallFailure('QueryInformationJobObject', 5)
        if self.terminated:
            return 0
        return int(self.script.get('active_during_settle', 3))

    def terminate_job(self, job, exit_code):
        self._log('terminate_job', job=job, exit_code=int(exit_code))
        if self.script.get('terminate_fails'):
            raise windows_process.Win32CallFailure('TerminateJobObject', 5)
        self.terminated = True

    def close_handle(self, handle):
        self._log('close_handle', handle=handle)
        if handle in self.observation_handles:
            self.closed_observation_handles.append(handle)

    def _creation_time(self, process):
        if process in self.observation_handles:
            pid = self.observation_handles[process]
            if pid in self.script.get('creation_none', ()):
                return None
            return 133450000000000000 + pid
        return 133450000000000000

    def job_process_ids(self, job):
        self._log('job_process_ids', job=job)
        failure = self.script.get('idlist_failure')
        if failure == 'win32':
            raise windows_process.Win32CallFailure('QueryInformationJobObject(idlist)', 5)
        if failure == 'truncated':
            raise windows_process.WindowsProcessError(
                'QueryInformationJobObject(idlist) copied 65 process IDs into a 64-entry '
                'buffer; refusing to report a truncated membership list')
        if callable(failure):
            raise failure()
        return list(self.members)

    def open_process_snapshot(self, pid):
        self._log('open_process_snapshot', pid=int(pid))
        if pid in self.script.get('open_denied', ()):
            raise windows_process.Win32CallFailure('OpenProcess', 5)
        handle = 'diag-observe-{}'.format(pid)
        self.observation_handles[handle] = int(pid)
        return handle

    def IsProcessInJob(self, process, job, result):
        self._log('IsProcessInJob', handle=process, job=job)
        pid = self.observation_handles.get(process)
        if pid in self.script.get('not_in_job', ()):
            result._obj.value = 0
            return 1
        if pid in self.script.get('in_job_error', ()):
            return 0
        result._obj.value = 1 if job == self.job else 0
        return 1

    def QueryFullProcessImageNameW(self, process, flags, buffer, size):
        pid = self.observation_handles.get(process)
        self._log('QueryFullProcessImageNameW', handle=process, flags=int(flags),
                  size_wchars=int(size._obj.value))
        if pid in self.script.get('image_fail', ()):
            size._obj.value = 2 * 4096
            return 0
        if pid in self.script.get('image_exact_full', ()):
            path = 'C:\\' + 'x' * 4092
            buffer.value = path
            size._obj.value = 4096
            return 1
        path = 'C:\\diag-member-{}.exe'.format(pid)
        buffer.value = path
        size._obj.value = len(path)
        return 1


class ConfigureJobDiagnosticsChecks(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='configure-diagnostics-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.work = self.installation = self.programs = self.bin = None

    def tearDown(self):
        if ARGS.retain_fixtures is not None:
            destination = ARGS.retain_fixtures / self._testMethodName
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copytree(self.root, destination)

    # ---- shared fixture: a full synthetic runner flow over the real run_owned --------
    def prepare_runner_flow(self):
        self.source = self.root / 'source'
        self.work = self.root / 'work'
        self.installation = self.root / 'visual-studio'
        self.programs = self.root / 'program-files'
        self.bin = self.root / 'bin'
        for _, (name, _) in SUITES.items():
            relative = Path('platform/native-foundation/tests') / name
            destination = self.source / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / relative, destination)
        tool = self.source / 'platform/native-foundation/tools/verify_detached.py'
        tool.parent.mkdir(parents=True, exist_ok=True)
        tool.write_text('# unused synthetic detached verifier\n', encoding='utf-8')
        for path in (self.installation / 'VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe',
                     self.programs / 'Windows Kits/10/Include/10.0.26100.0/um/Windows.h'):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b'synthetic pinned-tool input, never executed\n')
        vswhere = self.programs / 'Microsoft Visual Studio/Installer/vswhere.exe'
        vswhere.parent.mkdir(parents=True, exist_ok=True)
        vswhere.write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
        vswhere.chmod(0o755)
        self.bin.mkdir()
        for name in ('cmake', 'ctest'):
            stub = self.bin / name
            stub.write_text('#!/bin/sh\nexit 0\n', encoding='utf-8')
            stub.chmod(0o755)

    def run_runner_flow(self, *, configure_descendants):
        self.prepare_runner_flow()
        _FlowFakeAPI.instances = []
        _FlowFakeAPI.configure_descendants = configure_descendants
        _FlowFakeAPI.installation_path = str(self.installation)
        _FlowFakeAPI.work_path = str(self.work)
        _FlowFakeAPI.listing = b'\0'.join(
            str(path.relative_to(self.source)).replace('\\', '/').encode()
            for path in sorted(self.source.rglob('*')) if path.is_file()) + b'\0'
        environment = dict(os.environ, GITHUB_ACTIONS='true', GITHUB_SHA='a' * 40,
                           GITHUB_RUN_ID='321', GITHUB_RUN_ATTEMPT='1',
                           GITHUB_JOB='native-foundation', ImageOS='synthetic-win22',
                           ImageVersion='synthetic-image',
                           PATH=str(self.bin) + os.pathsep + os.environ.get('PATH', ''))
        environment['ProgramFiles(x86)'] = str(self.programs)
        fake_os = SimpleNamespace(name='nt', environ=environment, fspath=os.fspath)
        fake_subprocess = SimpleNamespace(
            run=lambda command, **kwargs: (_ for _ in ()).throw(
                AssertionError('subprocess.run is not the supervision boundary')),
            check_output=lambda command: (_ for _ in ()).throw(
                AssertionError('subprocess.check_output is not the supervision boundary')),
            PIPE=subprocess.PIPE, TimeoutExpired=subprocess.TimeoutExpired)
        snapshot = self.root / 'flow-image-environment.json'
        with patch.object(RUNNER, 'os', fake_os), \
             patch.object(RUNNER, 'subprocess', fake_subprocess), \
             patch.object(RUNNER.windows_process, 'Win32ProcessAPI', _FlowFakeAPI), \
             patch.object(sys, 'getwindowsversion', return_value=(10, 0, 20348), create=True), \
             patch.object(RUNNER.platform, 'platform', return_value='synthetic-Windows-host'), \
             patch.object(sys, 'stdout', SimpleNamespace(buffer=io.BytesIO())), \
             patch.object(sys, 'stderr', SimpleNamespace(buffer=io.BytesIO())):
            snapshot.write_text(json.dumps(RUNNER.image_environment('a' * 40)), encoding='utf-8')
            RUNNER.run_native(self.source, self.work, image_snapshot=snapshot)

    # ---- shared driver: the real proxy around a scripted API inside real run_owned ---
    def run_diagnostic_subcase(self, name, script, *, clock_step=0.0):
        case_root = self.root / 'diagnostic-subcases' / name
        clock = _DiagnosticFakeClock(step=clock_step)
        api = _ScriptedDiagnosticAPI(script, clock)
        proxy = RUNNER._ConfigureJobDiagnosticsProxy('configure', underlying=api, clock=clock)
        command = [sys.executable, '-c', 'pass']
        try:
            result = windows_process.run_owned(command, cwd=str(case_root),
                env={'PATH': os.environ.get('PATH', '')},
                timeout=float(script.get('timeout', 30.0)),
                spool_root=case_root / 'owned-spool', command_id='configure',
                api=proxy, clock=clock,
                settle_configured_seconds=float(script.get('settle_seconds', 1.0)),
                poll_seconds=float(script.get('poll_seconds', 0.1)))
            error = None
        except BaseException as caught:
            result, error = None, caught
        receipt = result.receipt if result is not None else getattr(error, 'process_receipt', None)
        proxy.attach_to(receipt)
        if isinstance(receipt, dict):
            case_root.mkdir(parents=True, exist_ok=True)
            (case_root / 'configure.process.json').write_text(
                json.dumps(receipt, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')
        return result, error, receipt, api, clock

    def assert_original_terminate_contract(self, receipt, api):
        terminates = [details for call, details in api.calls if call == 'terminate_job']
        self.assertEqual(len(terminates), 1, terminates)
        self.assertEqual(terminates[0]['exit_code'], 1)
        self.assertEqual(receipt['cleanup']['timeout_seconds'], 5.0)
        self.assertEqual(sorted(api.observation_handles), sorted(api.closed_observation_handles))

    def test_configure_cleanup_member_observation(self):
        with self.assertRaisesRegex(RuntimeError,
                'configure root exited 0 but its owned process tree did not settle'):
            self.run_runner_flow(configure_descendants=True)
        evidence = self.work / 'evidence'
        receipt = json.loads((evidence / 'configure.process.json').read_text())
        # The original failure semantics are preserved verbatim.
        self.assertEqual(receipt['schema'], 'wls-owned-process-receipt-v1')
        self.assertEqual(receipt['outcome'], 'FAILED_DESCENDANTS_AFTER_ROOT_EXIT')
        self.assertEqual(receipt['exit_code'], 0)
        self.assertEqual(receipt['settle']['status'], 'EXPIRED')
        self.assertTrue(receipt['tree_exit_proven'])
        self.assertEqual(receipt['termination'], {'reason': 'DESCENDANTS_AFTER_ROOT_EXIT',
            'requested': True, 'api': 'TerminateJobObject', 'requested_exit_code': 1,
            'job_terminate_succeeded': True, 'pre_assign_process_terminate_succeeded': None})
        self.assertEqual(receipt['execution_timeout_seconds'], 180.0)
        self.assertEqual(receipt['task_level_env_overrides'], {'MSBUILDDISABLENODEREUSE': '1'})
        self.assertEqual(receipt['cwd'], str(self.work))
        self.assertIn('-S', receipt['argv'])
        # Later pipeline commands never started.
        for later in ('build', 'ctest', 'install', 'host-capabilities', 'detached'):
            self.assertFalse((evidence / (later + '.command.json')).exists())
        failure = json.loads((evidence / 'failure.json').read_text())
        self.assertIn('build', failure['owned_process_supervision']['not_started'])
        self.assertIn('ctest', failure['owned_process_supervision']['not_started'])
        # Non-configure commands never gain the observer or the field.
        for other in ('git-head', 'vswhere', 'git-status', 'cmake-version'):
            other_receipt = json.loads((evidence / (other + '.process.json')).read_text())
            self.assertNotIn('configure_job_diagnostics', other_receipt)
        # The pre-termination owned-Job member identity this check exists for.
        self.assertIn('configure_job_diagnostics', receipt,
            'the original configure receipt lacks pre-termination owned-Job member identity')
        diagnostics = receipt['configure_job_diagnostics']
        # Truthful API provenance (plan-final-v8): the synthetic flow injects
        # _FlowFakeAPI through the runner's own lazy factory, so both the
        # top-level receipt and the attached diagnostic must read 'injected';
        # a constructor-time frozen False surfacing as native-win32 here is a
        # provenance mislabel.
        self.assertEqual((receipt['api_source'], diagnostics['api_source']),
                         ('injected', 'injected'),
                         'provenance mislabel: top-level api_source={}; '
                         'configure_job_diagnostics.api_source={}'.format(
                             receipt.get('api_source'),
                             diagnostics.get('api_source')))
        members = list(_FlowFakeAPI.configure_members)
        self.assertTrue(diagnostics['diagnostic_only'])
        self.assertEqual(diagnostics['scope'], 'THIS_CONFIGURE_JOB_ONLY')
        self.assertEqual(diagnostics['hook'], 'BEFORE_EXISTING_TERMINATE_JOB')
        self.assertTrue(diagnostics['triggered'])
        self.assertTrue(diagnostics['observer_ran'])
        self.assertEqual(diagnostics['membership_query']['status'], 'SUCCESS')
        self.assertEqual(diagnostics['membership_query']['returned'], members)
        self.assertEqual(diagnostics['membership_query']['returned_count'], 3)
        self.assertFalse(diagnostics['membership_query']['atomic_snapshot'])
        inspected = [entry for entry in diagnostics['inspected_members'] if entry.get('inspected')]
        self.assertEqual([entry['requested_pid'] for entry in inspected], members)
        for entry in inspected:
            self.assertEqual(entry['pid_confirmation'], 'CONFIRMED')
            self.assertEqual(entry['same_job_verification'], 'CONFIRMED')
            self.assertEqual(entry['creation_status'], 'OBSERVED')
            self.assertEqual(entry['creation_filetime'],
                             133450000000000000 + entry['requested_pid'])
            self.assertEqual(entry['image_status'], 'OBSERVED')
            self.assertEqual(entry['image_path'],
                             'C:\\synthetic-survivor-{}.exe'.format(entry['requested_pid']))
            self.assertEqual(entry['zero_wait_status'], 'TIMEOUT')
            self.assertTrue(entry['observation_handle_closed'])
        self.assertEqual(diagnostics['uninspected_member_count'], 0)
        self.assertEqual(diagnostics['duplicate_returned_entries'], 0)
        self.assertEqual(diagnostics['stopped_reasons'], [])
        self.assertTrue(diagnostics['observations_complete'])
        self.assertTrue(diagnostics['last_pre_termination_accounting']['query_ok'])
        self.assertEqual(diagnostics['last_pre_termination_accounting']['active'], 3)
        self.assertEqual(diagnostics['budget_seconds'], 0.2)
        self.assertEqual(diagnostics['member_limit'], 8)
        self.assertLessEqual(diagnostics['elapsed_seconds'], diagnostics['budget_seconds'])
        # The observation ran strictly before the single original terminate
        # forward, on the same assigned Job, and every new handle was closed.
        configure_api = next(api for api in _FlowFakeAPI.instances if api.kind == 'configure')
        names = [call for call, _ in configure_api.calls]
        terminate_index = names.index('terminate_job')
        for earlier in ('job_process_ids', 'open_process_snapshot', 'IsProcessInJob',
                        'QueryFullProcessImageNameW'):
            self.assertLess(names.index(earlier), terminate_index)
        observation_closes = [index for index, (call, details) in enumerate(configure_api.calls)
                              if call == 'close_handle'
                              and str(details['handle']).startswith('flow-observe-')]
        self.assertEqual(len(observation_closes), len(members))
        self.assertTrue(all(index < terminate_index for index in observation_closes))
        self.assertEqual(configure_api.calls[names.index('job_process_ids')][1]['job'],
                         configure_api.calls[terminate_index][1]['job'])
        self.assert_original_terminate_contract(receipt, configure_api)

    def test_configure_diagnostic_partial_and_bounds(self):
        # idlist failure (Win32 error): the membership list is null, never []
        result, error, receipt, api, _ = self.run_diagnostic_subcase('idlist-win32-failure',
            dict(idlist_failure='win32'))
        self.assertIsNone(error)
        self.assertEqual(receipt['outcome'], 'FAILED_DESCENDANTS_AFTER_ROOT_EXIT')
        diagnostics = receipt['configure_job_diagnostics']
        self.assertEqual(diagnostics['membership_query']['status'], 'QUERY_ERROR')
        self.assertIsNone(diagnostics['membership_query']['returned'])
        self.assertIsNone(diagnostics['membership_query']['returned_count'])
        self.assertEqual(diagnostics['inspected_members'], [])
        self.assertEqual(diagnostics['inspected_member_count'], 0)
        self.assertFalse(diagnostics['observations_complete'])
        self.assertIn('MEMBERSHIP_QUERY_ERROR', diagnostics['incomplete_reasons'])
        self.assertEqual(diagnostics['membership_query']['error']['function'],
                         'QueryInformationJobObject(idlist)')
        self.assertEqual(diagnostics['membership_query']['error']['win32_error'], 5)
        self.assert_original_terminate_contract(receipt, api)

        # >64-entry buffer refusal keeps the same strict null handling
        _, _, receipt, api, _ = self.run_diagnostic_subcase('idlist-truncated-refusal',
            dict(idlist_failure='truncated'))
        diagnostics = receipt['configure_job_diagnostics']
        self.assertEqual(diagnostics['membership_query']['status'], 'QUERY_ERROR')
        self.assertIsNone(diagnostics['membership_query']['returned'])
        self.assertIn('refusing to report a truncated membership list',
                      diagnostics['membership_query']['error']['reason'])
        self.assertEqual(diagnostics['idlist_capacity'], 64)
        self.assert_original_terminate_contract(receipt, api)

        # A successful empty query is [] — strictly distinct from null
        _, _, receipt, api, _ = self.run_diagnostic_subcase('successful-empty-list',
            dict(members=[], active_during_settle=3))
        diagnostics = receipt['configure_job_diagnostics']
        self.assertEqual(diagnostics['membership_query']['status'], 'SUCCESS')
        self.assertEqual(diagnostics['membership_query']['returned'], [])
        self.assertEqual(diagnostics['membership_query']['returned_count'], 0)
        self.assertEqual(diagnostics['inspected_members'], [])
        self.assertTrue(diagnostics['observations_complete'])
        self.assertFalse(diagnostics['membership_query']['atomic_snapshot'])
        self.assert_original_terminate_contract(receipt, api)

        # At most eight members are inspected; the rest are counted, not guessed
        members = list(range(7001, 7013))
        _, _, receipt, api, _ = self.run_diagnostic_subcase('eight-member-limit',
            dict(members=members))
        diagnostics = receipt['configure_job_diagnostics']
        self.assertEqual(diagnostics['membership_query']['returned'], members)
        self.assertEqual(diagnostics['inspected_member_count'], 8)
        self.assertEqual(diagnostics['uninspected_member_count'], 4)
        self.assertIn('MEMBER_LIMIT', diagnostics['incomplete_reasons'])
        self.assertFalse(diagnostics['observations_complete'])
        self.assertEqual(len([1 for call, _ in api.calls
                              if call == 'open_process_snapshot']), 8)
        self.assert_original_terminate_contract(receipt, api)

        # Duplicated returned IDs are recorded and never re-opened
        _, _, receipt, api, _ = self.run_diagnostic_subcase('duplicate-ids',
            dict(members=[7001, 7002, 7001, 7002]))
        diagnostics = receipt['configure_job_diagnostics']
        self.assertEqual(diagnostics['membership_query']['returned'], [7001, 7002, 7001, 7002])
        self.assertEqual(diagnostics['duplicate_returned_entries'], 2)
        self.assertEqual(diagnostics['inspected_member_count'], 2)
        duplicates = [entry for entry in diagnostics['inspected_members']
                      if entry.get('duplicate_of_inspected_pid')]
        self.assertEqual([entry['requested_pid'] for entry in duplicates], [7001, 7002])
        self.assertTrue(all(not entry['opened'] and not entry['inspected']
                            for entry in duplicates))
        self.assertEqual(len([1 for call, _ in api.calls
                              if call == 'open_process_snapshot']), 2)
        self.assert_original_terminate_contract(receipt, api)

        # PID reuse: the confirmed PID differs, so no image/creation is read
        _, _, receipt, api, _ = self.run_diagnostic_subcase('pid-reuse',
            dict(members=[7001, 7002], pid_reuse={7001: 9999}))
        diagnostics = receipt['configure_job_diagnostics']
        entries = {entry['requested_pid']: entry for entry in diagnostics['inspected_members']
                   if entry.get('inspected')}
        self.assertEqual(entries[7001]['confirmed_pid'], 9999)
        self.assertEqual(entries[7001]['pid_confirmation'], 'CHANGED')
        self.assertEqual(entries[7001]['same_job_verification'], 'SKIPPED_PID_NOT_CONFIRMED')
        self.assertNotIn('image_path', entries[7001])
        self.assertNotIn('creation_filetime', entries[7001])
        self.assertIn('PID_CHANGED', diagnostics['incomplete_reasons'])
        self.assertEqual(entries[7002]['same_job_verification'], 'CONFIRMED')
        self.assert_original_terminate_contract(receipt, api)

        # A member that left this exact Job is not claimed by PID alone
        _, _, receipt, api, _ = self.run_diagnostic_subcase('not-in-this-job',
            dict(members=[7001, 7002], not_in_job={7002}))
        diagnostics = receipt['configure_job_diagnostics']
        entries = {entry['requested_pid']: entry for entry in diagnostics['inspected_members']
                   if entry.get('inspected')}
        self.assertEqual(entries[7001]['same_job_verification'], 'CONFIRMED')
        self.assertEqual(entries[7002]['same_job_verification'], 'NOT_IN_THIS_JOB')
        self.assertNotIn('image_path', entries[7002])
        self.assertNotIn('creation_filetime', entries[7002])
        self.assertIn('NOT_IN_THIS_JOB', diagnostics['incomplete_reasons'])
        self.assert_original_terminate_contract(receipt, api)

        # OpenProcess denial is recorded with its original error, nothing inferred
        _, _, receipt, api, _ = self.run_diagnostic_subcase('openprocess-denied',
            dict(members=[7001, 7002], open_denied={7001}))
        diagnostics = receipt['configure_job_diagnostics']
        entries = {entry['requested_pid']: entry for entry in diagnostics['inspected_members']
                   if entry.get('inspected')}
        self.assertEqual(entries[7001]['open_status'], 'ERROR')
        self.assertEqual(entries[7001]['open_error']['function'], 'OpenProcess')
        self.assertEqual(entries[7001]['open_error']['win32_error'], 5)
        self.assertNotIn('image_path', entries[7001])
        self.assertIn('OPEN_ERROR', diagnostics['incomplete_reasons'])
        self.assertEqual(entries[7002]['same_job_verification'], 'CONFIRMED')
        self.assert_original_terminate_contract(receipt, api)

        # An insufficient image buffer is UNKNOWN, never a truncated path
        _, _, receipt, api, _ = self.run_diagnostic_subcase('image-insufficient-buffer',
            dict(members=[7001, 7002], image_fail={7001}, image_exact_full={7002}))
        diagnostics = receipt['configure_job_diagnostics']
        entries = {entry['requested_pid']: entry for entry in diagnostics['inspected_members']
                   if entry.get('inspected')}
        self.assertIsNone(entries[7001]['image_path'])
        self.assertEqual(entries[7001]['image_status'], 'UNKNOWN')
        self.assertEqual(entries[7001]['image_error']['type'], 'QueryFullProcessImageNameWFailed')
        self.assertIsNone(entries[7002]['image_path'])
        self.assertEqual(entries[7002]['image_status'], 'INSUFFICIENT_BUFFER')
        self.assertEqual(entries[7002]['image_error']['required_wchars'], 4096)
        self.assertEqual(entries[7001]['creation_status'], 'OBSERVED')
        self.assertIn('IMAGE_UNKNOWN', diagnostics['incomplete_reasons'])
        self.assertIn('IMAGE_INSUFFICIENT_BUFFER', diagnostics['incomplete_reasons'])
        self.assert_original_terminate_contract(receipt, api)

        # A member whose creation time is unavailable stays explicitly unknown
        _, _, receipt, api, _ = self.run_diagnostic_subcase('creation-unknown',
            dict(members=[7001], creation_none={7001}))
        diagnostics = receipt['configure_job_diagnostics']
        entry = diagnostics['inspected_members'][0]
        self.assertIsNone(entry['creation_filetime'])
        self.assertEqual(entry['creation_status'], 'UNKNOWN')
        self.assertIn('CREATION_UNKNOWN', diagnostics['incomplete_reasons'])
        self.assertEqual(entry['image_status'], 'OBSERVED')
        self.assert_original_terminate_contract(receipt, api)

        # The injected clock crossing the 200 ms budget stops new identity reads
        _, _, receipt, api, _ = self.run_diagnostic_subcase('time-budget',
            dict(members=list(range(7001, 7006))), clock_step=0.06)
        diagnostics = receipt['configure_job_diagnostics']
        self.assertEqual(diagnostics['stopped_reasons'], ['TIME_BUDGET'])
        self.assertIn('TIME_BUDGET', diagnostics['incomplete_reasons'])
        self.assertFalse(diagnostics['observations_complete'])
        self.assertGreater(diagnostics['overrun_seconds'], 0.0)
        self.assertEqual(len([1 for call, _ in api.calls
                              if call == 'open_process_snapshot']), 1)
        self.assertEqual(sorted(api.observation_handles), sorted(api.closed_observation_handles))
        self.assert_original_terminate_contract(receipt, api)
        self.assertEqual(receipt['cleanup']['timeout_seconds'], 5.0)

        # A member that exited between the list query and its zero-wait probe
        _, _, receipt, api, _ = self.run_diagnostic_subcase('member-exited-midway',
            dict(members=[7001], member_signaled={'diag-observe-7001'}))
        diagnostics = receipt['configure_job_diagnostics']
        entry = diagnostics['inspected_members'][0]
        self.assertEqual(entry['zero_wait_status'], 'SIGNALED')
        self.assertEqual(entry['same_job_verification'], 'CONFIRMED')
        self.assert_original_terminate_contract(receipt, api)

    def test_configure_diagnostic_binding_and_exception_fidelity(self):
        # The only two extra Win32 bindings and their declared ABI shapes.
        class _Declaring:
            def __init__(self, name):
                self.name = name

            def __call__(self, *args, **kwargs):
                return 1

        kernel32 = SimpleNamespace(IsProcessInJob=_Declaring('IsProcessInJob'),
                                   QueryFullProcessImageNameW=_Declaring(
                                       'QueryFullProcessImageNameW'))
        is_in_job, query_image = RUNNER._declare_configure_diagnostic_bindings(kernel32)
        self.assertIs(is_in_job, kernel32.IsProcessInJob)
        self.assertIs(query_image, kernel32.QueryFullProcessImageNameW)
        self.assertEqual(is_in_job.argtypes,
                         [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(ctypes.c_int)])
        self.assertIs(is_in_job.restype, ctypes.c_int)
        self.assertEqual(query_image.argtypes,
                         [ctypes.c_void_p, ctypes.c_uint32, ctypes.c_wchar_p,
                          ctypes.POINTER(ctypes.c_uint32)])
        self.assertIs(query_image.restype, ctypes.c_int)
        self.assertEqual(ctypes.sizeof(is_in_job.argtypes[0]), ctypes.sizeof(ctypes.c_void_p))
        self.assertEqual(ctypes.sizeof(is_in_job.argtypes[1]), ctypes.sizeof(ctypes.c_void_p))
        self.assertEqual(ctypes.sizeof(is_in_job.argtypes[2]), ctypes.sizeof(ctypes.c_void_p))
        self.assertEqual(ctypes.sizeof(is_in_job.restype), 4)
        self.assertEqual(ctypes.sizeof(query_image.argtypes[1]), 4)
        self.assertEqual(ctypes.sizeof(query_image.argtypes[3]), ctypes.sizeof(ctypes.c_void_p))

        # The image query is issued with a character count, never bytes.
        _, _, receipt, api, _ = self.run_diagnostic_subcase('binding-shape',
            dict(members=[7001]))
        diagnostics = receipt['configure_job_diagnostics']
        entry = diagnostics['inspected_members'][0]
        self.assertEqual(entry['image_status'], 'OBSERVED')
        image_calls = [details for call, details in api.calls
                       if call == 'QueryFullProcessImageNameW']
        self.assertEqual(len(image_calls), 1)
        self.assertEqual(image_calls[0]['flags'], 0)
        self.assertEqual(image_calls[0]['size_wchars'], 4096)
        in_job_calls = [details for call, details in api.calls if call == 'IsProcessInJob']
        terminate = next(details for call, details in api.calls if call == 'terminate_job')
        self.assertEqual(in_job_calls[0]['job'], terminate['job'])
        self.assert_original_terminate_contract(receipt, api)

        # Normal completion without a terminate request records NOT_REQUESTED.
        result, error, receipt, api, _ = self.run_diagnostic_subcase('not-requested',
            dict(active_during_settle=0, members=[]))
        self.assertIsNone(error)
        self.assertEqual(receipt['outcome'], 'COMPLETED')
        diagnostics = receipt['configure_job_diagnostics']
        self.assertFalse(diagnostics['triggered'])
        self.assertEqual(diagnostics['status'], 'NOT_REQUESTED')
        self.assertTrue(diagnostics['observer_ran'])
        self.assertNotIn('membership_query', diagnostics)
        self.assertEqual([call for call, _ in api.calls if call == 'terminate_job'], [])
        self.assertEqual(diagnostics['terminate_forwards'], 0)

        # Only the configure command is wired to the observer (actual runner flow).
        with self.assertRaisesRegex(RuntimeError, 'build failed with exit 1'):
            self.run_runner_flow(configure_descendants=False)
        evidence = self.work / 'evidence'
        configure_receipt = json.loads((evidence / 'configure.process.json').read_text())
        self.assertEqual(configure_receipt['outcome'], 'COMPLETED')
        self.assertEqual(configure_receipt['configure_job_diagnostics']['status'], 'NOT_REQUESTED')
        for other in ('git-head', 'vswhere', 'git-status', 'cmake-version', 'build'):
            other_receipt = json.loads((evidence / (other + '.process.json')).read_text())
            self.assertNotIn('configure_job_diagnostics', other_receipt)

        # An underlying settle query error keeps its original UNPROVEN outcome...
        _, _, receipt, api, _ = self.run_diagnostic_subcase('settle-query-error',
            dict(settle_query_error=True, members=[7001]))
        self.assertEqual(receipt['outcome'], 'UNPROVEN')
        self.assertEqual(receipt['settle']['status'], 'QUERY_ERROR')
        diagnostics = receipt['configure_job_diagnostics']
        self.assertTrue(diagnostics['triggered'])
        self.assertEqual(diagnostics['membership_query']['returned'], [7001])
        self.assert_original_terminate_contract(receipt, api)

        # ...and an underlying terminate error is preserved verbatim.
        _, _, receipt, api, _ = self.run_diagnostic_subcase('terminate-error',
            dict(terminate_fails=True, members=[7001]))
        cleanup_errors = receipt['cleanup']['errors']
        self.assertTrue(cleanup_errors)
        self.assertEqual(cleanup_errors[0]['function'], 'TerminateJobObject')
        self.assertEqual(cleanup_errors[0]['win32_error'], 5)
        self.assertTrue(receipt['configure_job_diagnostics']['triggered'])
        self.assertFalse(receipt['tree_exit_proven'])

        # TimeoutExpired keeps carrying the receipt with the triggered sample.
        _, error, receipt, api, _ = self.run_diagnostic_subcase('execution-timeout',
            dict(wait_never_signals=True, members=[7001], timeout=1.0, poll_seconds=0.1))
        self.assertIsInstance(error, subprocess.TimeoutExpired)
        self.assertTrue(receipt['configure_job_diagnostics']['triggered'])
        self.assertEqual(receipt['termination']['reason'], 'EXECUTION_DEADLINE')
        self.assert_original_terminate_contract(receipt, api)

        # KeyboardInterrupt and SystemExit keep their original instance.
        interrupt = KeyboardInterrupt('synthetic interrupt inside the owned wait')
        _, error, receipt, api, _ = self.run_diagnostic_subcase('keyboard-interrupt',
            dict(wait_raises=interrupt, members=[7001]))
        self.assertIs(error, interrupt)
        self.assertTrue(receipt['configure_job_diagnostics']['triggered'])
        self.assertEqual(receipt['termination']['reason'], 'INTERRUPTED')
        self.assert_original_terminate_contract(receipt, api)

        exit_request = SystemExit(7)
        _, error, receipt, api, _ = self.run_diagnostic_subcase('system-exit',
            dict(wait_raises=exit_request, members=[7001]))
        self.assertIs(error, exit_request)
        self.assertTrue(receipt['configure_job_diagnostics']['triggered'])
        self.assert_original_terminate_contract(receipt, api)

        # A crashing observer query never replaces the original outcome or forward.
        def crash():
            raise RuntimeError('synthetic observer crash inside job_process_ids')

        _, error, receipt, api, _ = self.run_diagnostic_subcase('observer-crash',
            dict(idlist_failure=crash))
        self.assertIsNone(error)
        self.assertEqual(receipt['outcome'], 'FAILED_DESCENDANTS_AFTER_ROOT_EXIT')
        diagnostics = receipt['configure_job_diagnostics']
        self.assertEqual(diagnostics['membership_query']['status'], 'QUERY_ERROR')
        self.assertEqual(diagnostics['membership_query']['error']['type'], 'RuntimeError')
        self.assert_original_terminate_contract(receipt, api)

        # Honest API-source marking and laziness (plan-final-v8). Explicit
        # injection reads True with no native library load.
        with patch.object(ctypes, 'WinDLL',
                          side_effect=AssertionError(
                              'WinDLL must not load for an injected observer'), create=True):
            injected_clock = _DiagnosticFakeClock()
            injected_api = _ScriptedDiagnosticAPI(dict(members=[7001]), injected_clock)
            injected_proxy = RUNNER._ConfigureJobDiagnosticsProxy(
                'configure', underlying=injected_api, clock=injected_clock)
            self.assertTrue(injected_proxy.is_injected)
            empty = {}
            self.assertFalse(injected_proxy.attach_to(empty))
            self.assertNotIn('configure_job_diagnostics', empty)
            self.assertFalse(injected_proxy.attach_to(None))

        # The production-shaped lazy proxy: construction itself must neither
        # materialize the underlying nor load any native library, and the first
        # source read resolves exactly one cached injected underlying that
        # every later read reuses. A bare source read never marks the observer
        # as run, so no diagnostic field is attached.
        factory_constructions = []
        factory_instances = []

        class _CountingFactoryAPI(_FlowFakeAPI):  # TEST_DOUBLE factory spy
            def __init__(self):
                factory_constructions.append(1)
                super().__init__()
                factory_instances.append(self)

        with patch.object(ctypes, 'WinDLL',
                          side_effect=AssertionError(
                              'proxy construction must not load WinDLL'), create=True), \
             patch.object(RUNNER.windows_process, 'Win32ProcessAPI', _CountingFactoryAPI):
            lazy_proxy = RUNNER._ConfigureJobDiagnosticsProxy('configure',
                                                              clock=_DiagnosticFakeClock())
            self.assertEqual(factory_constructions, [],
                             'proxy construction must not materialize the underlying API')
            self.assertTrue(lazy_proxy.is_injected)
            self.assertTrue(lazy_proxy.is_injected)
            self.assertEqual(len(factory_constructions), 1,
                             'repeated source reads must reuse one underlying object')
            self.assertIs(lazy_proxy._require_underlying(), factory_instances[0])
            source_only = {}
            self.assertFalse(lazy_proxy.attach_to(source_only),
                             'a bare source read must not mark the observer as run')
            self.assertNotIn('configure_job_diagnostics', source_only)

        # The supervisor's first source snapshot already reads the true
        # underlying: one real run_owned drive through the lazy factory proxy
        # must label the receipt 'injected' and construct the factory exactly
        # once.
        factory_constructions.clear()
        factory_instances.clear()
        drive_root = self.root / 'diagnostic-subcases' / 'lazy-provenance-drive'
        with patch.object(RUNNER.windows_process, 'Win32ProcessAPI', _CountingFactoryAPI):
            driven_proxy = RUNNER._ConfigureJobDiagnosticsProxy('configure',
                                                                clock=_DiagnosticFakeClock())
            driven = windows_process.run_owned([sys.executable, '-c', 'pass'],
                cwd=str(drive_root), env={'PATH': os.environ.get('PATH', '')},
                timeout=10.0, spool_root=drive_root / 'owned-spool', command_id='configure',
                api=driven_proxy, clock=_DiagnosticFakeClock())
        self.assertEqual(driven.receipt['api_source'], 'injected',
                         'the supervisor source snapshot must read the actual underlying')
        self.assertEqual(len(factory_constructions), 1)
        self.assertIs(driven_proxy._require_underlying(), factory_instances[0])

        # The real API class keeps its native marker; the TEST_DOUBLE false
        # marker below checks the property contract only — neither value is
        # ever published as native evidence of a real run.
        self.assertIs(windows_process.Win32ProcessAPI.is_injected, False)

        class _FalseMarkerUnderlyingTEST_DOUBLE:  # TEST_DOUBLE: contract check only
            is_injected = False

        false_marker_proxy = RUNNER._ConfigureJobDiagnosticsProxy(
            'configure', underlying=_FalseMarkerUnderlyingTEST_DOUBLE(),
            clock=_DiagnosticFakeClock())
        self.assertFalse(false_marker_proxy.is_injected)

        # An underlying-construction failure raised by the first source read
        # keeps its original exception instance and escapes before any
        # supervision state, receipt or fabricated diagnostic exists (the
        # declared initialization-timing correction of plan-final-v8).
        initialization_error = windows_process.UnsupportedPlatform(
            'synthetic pre-created underlying construction failure')
        failing_calls = []

        class _FailingUnderlyingFactory:  # TEST_DOUBLE factory spy
            def __call__(self):
                failing_calls.append(1)
                raise initialization_error

        failing_proxy = RUNNER._ConfigureJobDiagnosticsProxy('configure',
                                                             clock=_DiagnosticFakeClock())
        caught_initialization = None
        with patch.object(RUNNER.windows_process, 'Win32ProcessAPI',
                          _FailingUnderlyingFactory()):
            try:
                windows_process.run_owned([sys.executable, '-c', 'pass'],
                    cwd=str(self.root), env={'PATH': os.environ.get('PATH', '')},
                    timeout=10.0, spool_root=self.root / 'owned-spool-failing',
                    command_id='configure', api=failing_proxy, clock=_DiagnosticFakeClock())
            except BaseException as error:
                caught_initialization = error
        self.assertIs(caught_initialization, initialization_error,
                      'the original initialization exception instance must propagate')
        self.assertIsNone(getattr(caught_initialization, 'process_receipt', None))
        self.assertEqual(failing_calls, [1])
        no_diagnostic = {}
        self.assertFalse(failing_proxy.attach_to(no_diagnostic))
        self.assertNotIn('configure_job_diagnostics', no_diagnostic)

        # The original real API still refuses unsupported platforms before any
        # native library load; the proxy does not mask that refusal. (On
        # native Windows this refusal path does not exist; the supervisor
        # selfcheck keeps its own platform-conditional refusal case.)
        if os.name != 'nt':
            def _windll_spy(*args, **kwargs):
                raise AssertionError(
                    'the platform refusal must precede any WinDLL load')
            with patch.object(ctypes, 'WinDLL', _windll_spy, create=True):
                refusal_proxy = RUNNER._ConfigureJobDiagnosticsProxy('configure',
                                                                     clock=_DiagnosticFakeClock())
                refused = None
                try:
                    refusal_proxy.is_injected
                except windows_process.UnsupportedPlatform as error:
                    refused = error
            self.assertIsNotNone(refused,
                                 'the real API must keep refusing unsupported platforms')


def ordinary_windows11():
    return dict(os_major=10, os_build=26100, os_product_type=1, native_machine=0x8664,
        process_machine=0, python_pointer_bytes=8, token_elevated=0, token_elevation_type=1,
        effective_administrators_member=False,
        volumes={key: dict(filesystem='NTFS', drive_type=3) for key in ('source', 'work')})


class ProvenanceAndHostGuards(unittest.TestCase):
    def test_capture_entry_only_reads_checkout_and_writes_one_fresh_snapshot(self):
        with tempfile.TemporaryDirectory(prefix='native-image-capture-selfcheck-') as temporary:
            root = Path(temporary)
            (root / 'path-spelling').mkdir()
            source = root / 'path-spelling' / '..' / 'source'
            source.mkdir()
            resolved_source = source.resolve(strict=True)
            self.assertNotEqual(str(source), str(resolved_source))
            destination = root / 'capture.json'
            original_entries = {path.name for path in root.iterdir()}
            fake_os = SimpleNamespace(name='nt', environ={'GITHUB_ACTIONS': 'true',
                'ImageOS': 'win22', 'ImageVersion': '20261026.347.1', 'MODEL_API_KEY': 'synthetic-secret'})

            def owned(command, **kwargs):
                receipt = dict(schema='wls-owned-process-receipt-v1',
                    command_id='capture-git-head', argv=[os.fspath(item) for item in command],
                    exit_code_available=True, exit_code=0, tree_exit_proven=True,
                    outcome='COMPLETED', execution_timeout_seconds=kwargs['timeout'],
                    execution_elapsed_seconds=0.02, settle={'status': 'NATURAL_ZERO'})
                return SimpleNamespace(stdout=b'a' * 40 + b'\n', stderr=b'', exit_code=0,
                    outcome='COMPLETED', tree_exit_proven=True, receipt=receipt)

            with patch.object(RUNNER, 'os', fake_os), \
                 patch.object(RUNNER.windows_process, 'run_owned', side_effect=owned) as supervised_head, \
                 patch.object(RUNNER.subprocess, 'run', side_effect=AssertionError('no build or product')), \
                 patch.object(sys, 'stdout', io.StringIO()), \
                 patch.object(sys, 'argv', ['run_windows_ci.py', '--source-root', str(source),
                                           '--capture-image-environment', str(destination)]):
                RUNNER.main()
                self.assertEqual(len(supervised_head.call_args_list), 1)
                positional, keyword = supervised_head.call_args_list[0]
                self.assertEqual([os.fspath(item) for item in positional[0]],
                                 ['git', '-C', str(resolved_source), 'rev-parse', 'HEAD'])
                self.assertEqual(keyword['timeout'], 30)
            captured = json.loads(destination.read_text())
            self.assertEqual(captured['actual_checkout_sha'], 'a' * 40)
            self.assertEqual(captured['image_environment']['ImageVersion'], '20261026.347.1')
            self.assertNotIn('synthetic-secret', destination.read_text())
            self.assertEqual(captured['checkout_query_supervision']['tree_exit_proven'], True)
            self.assertEqual({path.name for path in root.iterdir()}, original_entries | {destination.name})
            self.assertEqual(list(resolved_source.iterdir()), [])

    def test_image_snapshot_is_allowlisted_and_never_claims_job_header_identity(self):
        with patch.dict(os.environ, {'ImageOS': 'win22', 'ImageVersion': '20261026.347.1',
                                    'MODEL_API_KEY': 'synthetic-must-not-copy'}):
            capture = RUNNER.image_environment('a' * 40)
        self.assertNotIn('MODEL_API_KEY', json.dumps(capture))
        self.assertIn('captured_at_utc', capture)
        provenance = RUNNER.image_provenance(capture, capture)
        self.assertEqual(provenance['process_environment_consistency'], 'UNCHANGED')
        self.assertEqual(provenance['exact_runner_image_identity'], 'UNRESOLVED_REQUIRES_JOB_HEADER_COMPARISON')
        self.assertEqual(set(capture['image_environment']), {'ImageOS', 'ImageVersion'})

    def test_different_run_attempt_job_and_checkout_are_distinct_from_image_consistency(self):
        capture = RUNNER.image_environment('a' * 40)
        for key in ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_JOB', 'GITHUB_SHA'):
            with self.subTest(key=key):
                other = json.loads(json.dumps(capture))
                other['github_context'][key] = 'different'
                self.assertEqual(RUNNER.image_provenance(capture, other)['changed_fields'], ['github_context'])
        other = dict(capture, actual_checkout_sha='b' * 40)
        self.assertEqual(RUNNER.image_provenance(capture, other)['changed_fields'], ['actual_checkout_sha'])

    def test_missing_image_version_cannot_be_reported_complete(self):
        capture = RUNNER.image_environment('a' * 40)
        capture['image_environment']['ImageVersion'] = None
        result = RUNNER.image_provenance(capture, capture)
        self.assertFalse(result['image_environment_fields_complete'])
        self.assertEqual(result['exact_runner_image_identity'], 'UNRESOLVED_REQUIRES_JOB_HEADER_COMPARISON')

    def test_machine_capture_time_is_separate_from_actual_machine_value_change(self):
        capture = RUNNER.image_environment('a' * 40)
        other = json.loads(json.dumps(capture))
        other['machine_image_environment']['captured_at_utc'] = '2026-11-02T08:01:00+00:00'
        self.assertEqual(RUNNER.image_provenance(capture, other)['changed_fields'], [])
        other['machine_image_environment']['values']['ImageVersion'] = 'different-machine-observation'
        result = RUNNER.image_provenance(capture, other)
        self.assertEqual(result['changed_fields'], ['machine_image_environment'])
        self.assertEqual(result['machine_environment_consistency'], 'CHANGED')
        self.assertEqual(result['process_environment_consistency'], 'UNCHANGED')
        self.assertEqual(result['exact_runner_image_identity'], 'UNRESOLVED_REQUIRES_JOB_HEADER_COMPARISON')

    def test_workstation_normal_token_fixed_ntfs_is_the_supported_host_selection(self):
        windows_host.require_windows11(ordinary_windows11())

    def test_server_old_os_arm_emulation_admin_and_non_ntfs_are_rejected(self):
        variants = [('os_product_type', 3), ('os_build', 20348), ('native_machine', 0xAA64),
                    ('process_machine', 0x014C), ('python_pointer_bytes', 4), ('token_elevated', 1),
                    ('token_elevation_type', 3), ('effective_administrators_member', True)]
        for key, value in variants:
            with self.subTest(key=key):
                facts = ordinary_windows11()
                facts[key] = value
                with self.assertRaises(RuntimeError):
                    windows_host.require_windows11(facts)
        for key, value in (('filesystem', 'ReFS'), ('drive_type', 4)):
            with self.subTest(volume_key=key):
                facts = ordinary_windows11()
                facts['volumes']['work'][key] = value
                with self.assertRaises(RuntimeError):
                    windows_host.require_windows11(facts)

    def test_original_actions_entry_does_not_allow_local_windows_or_linux(self):
        for name, actions in (('posix', 'true'), ('nt', 'false')):
            with self.subTest(name=name, actions=actions):
                with patch.object(RUNNER, 'os', SimpleNamespace(name=name, environ={'GITHUB_ACTIONS': actions})):
                    with self.assertRaisesRegex(RuntimeError, 'ephemeral native Windows Actions'):
                        RUNNER.require_actions()


class MachineImageRegistry(unittest.TestCase):
    def capture(self, values=None, *, open_error=None):
        values = values or {'ImageOS': ('win22', 1), 'ImageVersion': ('20261026.347.1', 1)}
        calls = []

        class Key:
            def __enter__(self):
                return self

            def __exit__(self, *_):
                return False

        key = Key()

        def open_key(*args):
            calls.append(('open', args))
            if open_error is not None:
                raise open_error
            return key

        def query_value(opened, name):
            self.assertIs(opened, key)
            self.assertIn(name, ('ImageOS', 'ImageVersion'))
            calls.append(('query', name))
            value = values[name]
            if isinstance(value, BaseException):
                raise value
            return value

        registry = SimpleNamespace(HKEY_LOCAL_MACHINE='machine-hive-only', KEY_QUERY_VALUE=1,
            KEY_WOW64_64KEY=0x100, REG_SZ=1, OpenKey=open_key, QueryValueEx=query_value)
        with patch.object(RUNNER, 'os', SimpleNamespace(name='nt')), \
             patch.dict(sys.modules, {'winreg': registry}):
            result = RUNNER.machine_image_environment()
        return result, calls

    def test_only_two_named_machine_values_are_read_with_query_only_native_view(self):
        result, calls = self.capture()
        self.assertEqual(calls, [('open', ('machine-hive-only', RUNNER.MACHINE_ENVIRONMENT_KEY, 0, 0x101)),
                                 ('query', 'ImageOS'), ('query', 'ImageVersion')])
        self.assertEqual(result['status'], 'OBSERVED')
        self.assertEqual(result['values'], {'ImageOS': 'win22', 'ImageVersion': '20261026.347.1'})
        self.assertEqual(result['field_status'], {'ImageOS': 'OBSERVED', 'ImageVersion': 'OBSERVED'})
        self.assertEqual(result['view'], '64-bit')

    def test_missing_or_denied_named_value_is_not_filled_from_process_environment(self):
        for error, state in ((FileNotFoundError(), 'MISSING'), (PermissionError(), 'DENIED'),
                             (OSError(), 'QUERY_FAILED')):
            with self.subTest(state=state):
                result, _ = self.capture({'ImageOS': ('win22', 1), 'ImageVersion': error})
                self.assertEqual(result['status'], 'PARTIAL')
                self.assertEqual(result['field_status']['ImageVersion'], state)
                self.assertIsNone(result['values']['ImageVersion'])

    def test_unavailable_machine_key_remains_not_checked_and_does_not_query_other_hives(self):
        for error, reason in ((FileNotFoundError(), 'MACHINE_ENVIRONMENT_KEY_MISSING'),
                              (PermissionError(), 'MACHINE_ENVIRONMENT_KEY_DENIED'),
                              (OSError(), 'MACHINE_ENVIRONMENT_KEY_UNAVAILABLE')):
            with self.subTest(reason=reason):
                result, calls = self.capture(open_error=error)
                self.assertEqual(result['status'], 'NOT_CHECKED')
                self.assertEqual(result['reason'], reason)
                self.assertEqual(result['values'], {'ImageOS': None, 'ImageVersion': None})
                self.assertEqual(len(calls), 1)

    def test_non_string_expandable_unbounded_or_control_values_are_not_expanded_or_published(self):
        for value in ((123, 1), ('%SECRET%', 2), ('x' * 257, 1), ('bad\nvalue', 1), ('', 1)):
            with self.subTest(kind=value[1]):
                result, _ = self.capture({'ImageOS': value, 'ImageVersion': value})
                self.assertEqual(result['status'], 'NOT_CHECKED')
                self.assertEqual(result['values'], {'ImageOS': None, 'ImageVersion': None})
                self.assertEqual(result['field_status'], {'ImageOS': 'UNSUPPORTED_VALUE',
                                                         'ImageVersion': 'UNSUPPORTED_VALUE'})


if __name__ == '__main__':
    unittest.main(argv=[__file__, *UNITTEST_ARGS], verbosity=2)
