#!/usr/bin/env python3
"""Synthetic runner evidence checks; never build or execute the native product.

Only Windows/toolchain discovery and build/install are substituted. The CTest
process and its stdout, JUnit truncation and LastTest files are real. The timeout
case injects a partial subprocess result without waiting for a product deadline.
"""
import argparse
import ast
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

    def invoke(self, mode='success', *, windows11=False, changed_image=False, host=None):
        real_run = subprocess.run

        def run(command, **kwargs):
            command = [os.fspath(arg) for arg in command]
            output = b''
            if command[0].endswith('vswhere.exe'):
                output = json.dumps([{'installationPath': str(self.installation)}]).encode()
            elif command[0] == 'git' and 'rev-parse' in command:
                output = b'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\n'
            elif command[0] == 'cmake' and '-S' in command:
                self.configure_fixture(mode)
            elif command[0] == 'cmake' and '--build' in command:
                directory = self.work / 'build/Release'
                directory.mkdir()
                (directory / 'synthetic.exe').write_bytes(b'not executable\n')
            elif command[0] == 'ctest':
                self.assertEqual(command, ['ctest', '--test-dir', str(self.work / 'build'), '-C', 'Release',
                    '--output-on-failure', '--output-junit', str(self.work / 'evidence/ctest.xml')])
                self.assertEqual(kwargs['timeout'], 300)
                if mode == 'timeout':
                    temporary = self.work / 'build/Testing/Temporary'
                    temporary.mkdir(parents=True)
                    self.original_last_test = b'original partial CTest output\r\n'
                    (temporary / 'LastTest.log.tmp').write_bytes(self.original_last_test)
                    raise subprocess.TimeoutExpired(command, 300, output=b'partial stdout\r\n', stderr=b'partial stderr\r\n')
                result = real_run(command, **kwargs)
                self.original_ctest_stdout = result.stdout
                self.original_last_test = (self.work / 'build/Testing/Temporary/LastTest.log').read_bytes()
                return result
            elif command[0] == 'cmake' and '--install' in command:
                directory = self.work / 'installed/bin'
                directory.mkdir(parents=True)
                (directory / 'wls-native-foundation.exe').write_bytes(b'not executable\n')
            elif command[-1] == 'capabilities':
                output = b'{"token_elevation":"not_elevated","filesystem":"NTFS"}\n'
            return subprocess.CompletedProcess(command, 0, output, b'')

        environment = dict(os.environ, GITHUB_ACTIONS='true', GITHUB_SHA='a' * 40,
                           GITHUB_RUN_ID='123', GITHUB_RUN_ATTEMPT='1', GITHUB_JOB='native-foundation',
                           ImageOS='synthetic-win22', ImageVersion='synthetic-image')
        environment['ProgramFiles(x86)'] = str(self.programs)
        fake_os = SimpleNamespace(name='nt', environ=environment, fspath=os.fspath)
        listing = b'\0'.join(str(path.relative_to(self.source)).replace('\\', '/').encode()
                              for path in self.source.rglob('*') if path.is_file()) + b'\0'
        fake_subprocess = SimpleNamespace(run=run, check_output=lambda command: listing,
                                         PIPE=subprocess.PIPE, TimeoutExpired=subprocess.TimeoutExpired)
        with patch.object(RUNNER, 'os', fake_os), patch.object(RUNNER, 'subprocess', fake_subprocess), \
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

    def test_failure_retains_original_lasttest_and_failed_index(self):
        with self.assertRaisesRegex(RuntimeError, 'ctest failed with exit'):
            self.invoke('failure')
        evidence = self.work / 'evidence'
        self.assertEqual((evidence / 'ctest-logs/LastTest.log').read_bytes(), self.original_last_test)
        self.assertIn(b'native_cli', (evidence / 'ctest-logs/LastTestsFailed.log').read_bytes())
        self.assertFalse((evidence / 'result.json').exists())

    def test_timeout_retains_original_partial_log_and_stdout(self):
        with self.assertRaises(subprocess.TimeoutExpired):
            self.invoke('timeout')
        evidence = self.work / 'evidence'
        self.assertEqual((evidence / 'ctest-logs/LastTest.log.tmp').read_bytes(), self.original_last_test)
        self.assertEqual((evidence / 'ctest.stdout').read_bytes(), b'partial stdout\r\n')
        self.assertEqual(json.loads((evidence / 'ctest.command.json').read_text())['status'], 'TIMEOUT')
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
            with patch.object(RUNNER, 'os', fake_os), \
                 patch.object(RUNNER.subprocess, 'check_output', return_value=b'a' * 40 + b'\n') as git_head, \
                 patch.object(RUNNER.subprocess, 'run', side_effect=AssertionError('no build or product')), \
                 patch.object(sys, 'stdout', io.StringIO()), \
                 patch.object(sys, 'argv', ['run_windows_ci.py', '--source-root', str(source),
                                           '--capture-image-environment', str(destination)]):
                RUNNER.main()
                git_head.assert_called_once_with(['git', '-C', str(resolved_source), 'rev-parse', 'HEAD'], timeout=30)
            captured = json.loads(destination.read_text())
            self.assertEqual(captured['actual_checkout_sha'], 'a' * 40)
            self.assertEqual(captured['image_environment']['ImageVersion'], '20261026.347.1')
            self.assertNotIn('synthetic-secret', destination.read_text())
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


if __name__ == '__main__':
    unittest.main(argv=[__file__, *UNITTEST_ARGS], verbosity=2)
