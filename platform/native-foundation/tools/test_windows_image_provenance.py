#!/usr/bin/env python3
"""Synthetic source-binding tests; not Windows execution or product CTest."""
import copy
import contextlib
import hashlib
import io
import json
import os
from pathlib import Path
import tempfile
import time
import unittest
from unittest import mock
import zipfile

import windows_image_provenance as audit


class EvidenceFileIdentity(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='windows-file-identity-selfcheck-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.path = self.root / 'evidence.bin'
        self.raw = b'original-evidence-bytes\x00\xff\n'
        self.path.write_bytes(self.raw)
        os.utime(self.path, ns=(1700000000123456700, 1700000000123456700))

    def set_windows_basic_time(self, name, value):
        # Change a real temporary file's metadata. No stat result is fabricated.
        import ctypes
        from ctypes import wintypes

        class BasicInfo(ctypes.Structure):
            _fields_ = [('CreationTime', ctypes.c_int64), ('LastAccessTime', ctypes.c_int64),
                        ('LastWriteTime', ctypes.c_int64), ('ChangeTime', ctypes.c_int64),
                        ('FileAttributes', wintypes.DWORD)]

        kernel = ctypes.WinDLL('kernel32', use_last_error=True)
        kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                      wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        kernel.CreateFileW.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.CloseHandle.restype = wintypes.BOOL
        for function in (kernel.GetFileInformationByHandleEx, kernel.SetFileInformationByHandle):
            function.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
            function.restype = wintypes.BOOL
        handle = kernel.CreateFileW(str(self.path), 0x80 | 0x100, 7, None, 3, 0x00200000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            basic = BasicInfo()
            if not kernel.GetFileInformationByHandleEx(handle, 0, ctypes.byref(basic), ctypes.sizeof(basic)):
                raise ctypes.WinError(ctypes.get_last_error())
            setattr(basic, name, value)
            if not kernel.SetFileInformationByHandle(handle, 0, ctypes.byref(basic), ctypes.sizeof(basic)):
                raise ctypes.WinError(ctypes.get_last_error())
        finally:
            kernel.CloseHandle(handle)

    def rewrite_preserving_size_and_mtime(self):
        before = self.path.stat()
        native_before = audit.windows_identity(path=self.path) if os.name == 'nt' else None
        if native_before is None:
            # ext4 ctime can use the same coarse kernel tick for adjacent writes.
            # Cross that fixture tick before injecting; still prove the real
            # ctime changed below. No reader/CI timeout or assertion is changed.
            time.sleep(0.02)
        self.path.write_bytes(b'x' * len(self.raw))
        os.utime(self.path, ns=(before.st_atime_ns, before.st_mtime_ns))
        if native_before is not None:
            self.set_windows_basic_time('ChangeTime', native_before[5] - 10000000)
            native_after = audit.windows_identity(path=self.path)
            self.assertEqual(native_before[:5], native_after[:5])
            self.assertNotEqual(native_before[5], native_after[5])
        else:
            self.assertNotEqual(before.st_ctime_ns, self.path.stat().st_ctime_ns)
        after = self.path.stat()
        self.assertEqual((before.st_dev, before.st_ino, before.st_size, before.st_mtime_ns),
                         (after.st_dev, after.st_ino, after.st_size, after.st_mtime_ns))
        self.assertEqual(self.path.read_bytes(), b'x' * len(self.raw))

    @contextlib.contextmanager
    def observe_reader_calls(self):
        original_fdopen = os.fdopen
        reads = []

        @contextlib.contextmanager
        def observed_fdopen(*args, **kwargs):
            with original_fdopen(*args, **kwargs) as stream:
                class ObservedStream:
                    def fileno(self):
                        return stream.fileno()

                    def read(self, *read_args):
                        reads.append(True)
                        return stream.read(*read_args)

                yield ObservedStream()

        with mock.patch.object(audit.os, 'fdopen', side_effect=observed_fdopen):
            yield reads

    def test_stable_explicit_creation_and_write_times_preserve_original_bytes(self):
        if os.name == 'nt':
            self.set_windows_basic_time('CreationTime', 133444736000000000)
            self.set_windows_basic_time('ChangeTime', 133444736010000000)
            native = audit.windows_identity(path=self.path)
            self.assertEqual(native[3], 133444736000000000)
            self.assertEqual(native[5], 133444736010000000)
        raw, identity = audit.read_file(self.path)
        self.assertEqual(raw, self.raw)
        self.assertEqual(identity['bytes'], len(self.raw))
        self.assertEqual(identity['sha256'], hashlib.sha256(self.raw).hexdigest())

    def test_rewrite_after_path_observation_before_open_with_restored_mtime_rejects(self):
        original_open = os.open
        injected = []

        def open_after_change(path, flags, *args, **kwargs):
            if Path(path) == self.path and not injected:
                injected.append(True)
                self.rewrite_preserving_size_and_mtime()
            return original_open(path, flags, *args, **kwargs)

        # This hook chooses a real filesystem interleaving; it returns the real fd.
        with self.observe_reader_calls() as reads, mock.patch.object(audit.os, 'open', side_effect=open_after_change):
            with self.assertRaisesRegex(ValueError, 'changed while reading'):
                audit.read_file(self.path)
        self.assertEqual(injected, [True])
        if os.name == 'nt':
            self.assertEqual(reads, [])

    def test_rewrite_after_open_with_restored_mtime_rejects(self):
        original_fstat = os.fstat
        injected = []

        def observe_then_change(fd):
            observation = original_fstat(fd)
            if not injected:
                injected.append(True)
                self.rewrite_preserving_size_and_mtime()
            return observation

        with self.observe_reader_calls() as reads, mock.patch.object(audit.os, 'fstat', side_effect=observe_then_change):
            with self.assertRaisesRegex(ValueError, 'changed while reading'):
                audit.read_file(self.path)
        self.assertEqual(injected, [True])
        if os.name == 'nt':
            self.assertEqual(reads, [])

    def test_same_bytes_path_replacement_after_descriptor_close_rejects(self):
        replacement = self.root / 'replacement.bin'
        replacement.write_bytes(self.raw)
        before = self.path.stat()
        os.utime(replacement, ns=(before.st_atime_ns, before.st_mtime_ns))
        self.assertNotEqual(before.st_ino, replacement.stat().st_ino)
        original_fdopen = os.fdopen

        @contextlib.contextmanager
        def replace_after_close(*args, **kwargs):
            with original_fdopen(*args, **kwargs) as stream:
                yield stream
            os.replace(replacement, self.path)

        with mock.patch.object(audit.os, 'fdopen', side_effect=replace_after_close):
            with self.assertRaisesRegex(ValueError, 'changed while reading'):
                audit.read_file(self.path)
        self.assertEqual(self.path.read_bytes(), self.raw)
        self.assertNotEqual(before.st_ino, self.path.stat().st_ino)


class SourceComparison(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='windows-image-source-selfcheck-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.environment = dict(schema='wls-native-image-environment-v1',
            captured_at_utc='2026-11-02T08:00:05+00:00',
            actual_checkout_sha='a' * 40,
            image_environment=dict(ImageOS='win22', ImageVersion='20261026.347.1'),
            github_context=dict(GITHUB_RUN_ID='123', GITHUB_RUN_ATTEMPT='1',
                GITHUB_JOB='native-foundation', GITHUB_SHA='a' * 40, GITHUB_EVENT_NAME='push'))
        self.header_version = '20261025.346.1'
        self.printed_environment = None

    def capture(self, name, data, endpoint):
        raw = data if isinstance(data, bytes) else json.dumps(data).encode()
        path = self.root / name
        path.write_bytes(raw)
        receipt = path.with_name(path.name + '.receipt.json')
        receipt.write_text(json.dumps(dict(argv=['gh', 'api', '--method', 'GET', endpoint],
            http_method='GET', remote_mutation=False, exit_code=0, timed_out=False, launch_error_type=None,
            started_at_utc='2026-11-02T09:00:00+00:00', ended_at_utc='2026-11-02T09:00:01+00:00',
            files={path.name: dict(bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())})))
        return dict(data=str(path), receipt=str(receipt))

    def fixture(self):
        prefix = 'repos/example/repository/actions/'
        result = dict(schema='wls-windows-image-source-input-v1', repository='example/repository',
            run_id=123, job_id=456, artifact_id=789, attempt=1, workflow_job_key='native-foundation')
        result['run'] = self.capture('run.json', dict(id=123, run_attempt=1, head_sha='a' * 40, event='push'),
                                     prefix + 'runs/123/attempts/1')
        result['jobs'] = self.capture('jobs.json', dict(jobs=[dict(id=456, run_id=123, run_attempt=1,
            head_sha='a' * 40, name='Windows native', started_at='2026-11-02T08:00:00Z',
            completed_at='2026-11-02T08:00:10Z')]), prefix + 'runs/123/attempts/1/jobs?per_page=100')
        printed = self.environment if self.printed_environment is None else self.printed_environment
        log = '\n'.join([
            '2026-11-02T08:00:00.100Z ##[group]Runner Image Provisioner',
            '2026-11-02T08:00:00.101Z Version: unrelated-provisioner',
            '2026-11-02T08:00:00.102Z ##[endgroup]',
            '2026-11-02T08:00:01.000Z ##[group]Runner Image',
            '2026-11-02T08:00:01.001Z Image: windows-2022',
            '2026-11-02T08:00:01.002Z Version: ' + self.header_version,
            '2026-11-02T08:00:01.003Z ##[endgroup]',
            '2026-11-02T08:00:02.000Z Complete job name: Windows native',
            '2026-11-02T08:00:03.000Z [command]"C:\\Git\\git.exe" log -1 --format=%H',
            '2026-11-02T08:00:03.001Z ' + 'a' * 40,
            '2026-11-02T08:00:05.001Z ' + json.dumps(printed),
            '2026-11-02T08:00:08.000Z Artifact download URL: https://github.com/example/repository/actions/runs/123/artifacts/789',
            ''])
        result['log'] = self.capture('job.log', ('\ufeff' + log.replace('\n', '\r\n')).encode(), prefix + 'jobs/456/logs')
        archive = io.BytesIO()
        with zipfile.ZipFile(archive, 'w') as zipped:
            zipped.writestr(audit.ENVIRONMENT_MEMBER, json.dumps(self.environment).encode())
            zipped.writestr('never-execute.py', 'raise AssertionError("archive code must not run")\n')
        raw_archive = archive.getvalue()
        result['archive'] = self.capture('archive.zip', raw_archive, prefix + 'artifacts/789/zip')
        result['artifacts'] = self.capture('artifacts.json', dict(artifacts=[dict(id=789,
            workflow_run=dict(id=123, head_sha='a' * 40), digest='sha256:' + hashlib.sha256(raw_archive).hexdigest(),
            size_in_bytes=len(raw_archive))]), prefix + 'runs/123/artifacts?per_page=100')
        return result

    def test_conflict_remains_unresolved_with_both_sources_times_and_hashes(self):
        result = audit.compare(self.fixture())
        self.assertEqual(result['exit_code'], 1)
        self.assertEqual(result['exact_runner_image_identity'], 'UNRESOLVED')
        self.assertEqual(result['runner_header']['fields']['Version'], self.header_version)
        self.assertEqual(result['process_environment']['values']['ImageVersion'], '20261026.347.1')
        self.assertEqual(result['runner_header']['emitted_at_utc'], '2026-11-02T08:00:01.000Z')
        self.assertEqual(result['process_environment']['source']['captured_at_utc'], self.environment['captured_at_utc'])
        self.assertIsNone(result['preferred_source'])
        self.assertIsNone(result['explanation'])
        self.assertEqual(result['causal_explanation_evidence'], [])
        self.assertEqual(result['machine_registry_environment']['status'], 'NOT_COLLECTED')
        self.assertFalse(result['artifact_code_executed'])
        for source in (result['runner_header']['source'], result['process_environment']['source']):
            self.assertEqual(len(source['sha256']), 64)

    def test_matching_fields_are_not_signed_host_attestation(self):
        self.header_version = self.environment['image_environment']['ImageVersion']
        result = audit.compare(self.fixture())
        self.assertEqual(result['exit_code'], 0)
        self.assertEqual(result['exact_runner_image_identity'], 'CONSISTENT_OBSERVATIONS_NOT_HOST_ATTESTATION')
        self.assertFalse(result['prior_audit_rewritten'])

    def test_wrong_run_attempt_job_key_or_checkout_rejects(self):
        for key, value in [('GITHUB_RUN_ID', '124'), ('GITHUB_RUN_ATTEMPT', '2'),
                           ('GITHUB_JOB', 'other'), ('GITHUB_SHA', 'b' * 40)]:
            with self.subTest(key=key):
                original = self.environment['github_context'][key]
                self.environment['github_context'][key] = value
                with self.assertRaisesRegex(ValueError, 'binding differs'):
                    audit.compare(self.fixture())
                self.environment['github_context'][key] = original

    def test_env_from_another_capture_cannot_substitute_for_printed_original(self):
        self.printed_environment = copy.deepcopy(self.environment)
        self.printed_environment['image_environment']['ImageVersion'] = 'different'
        with self.assertRaisesRegex(ValueError, 'same-job printed capture'):
            audit.compare(self.fixture())

    def test_modified_raw_log_fails_original_capture_hash(self):
        inputs = self.fixture()
        path = Path(inputs['log']['data'])
        path.write_bytes(path.read_bytes().replace(self.header_version.encode(), b'fake-version'))
        with self.assertRaisesRegex(ValueError, 'receipt hash or size differs'):
            audit.compare(inputs)

    def test_archive_api_digest_mismatch_refuses(self):
        inputs = self.fixture()
        inputs['artifacts'] = self.capture('bad-artifacts.json', dict(artifacts=[dict(id=789,
            workflow_run=dict(id=123, head_sha='a' * 40), digest='sha256:' + '0' * 64,
            size_in_bytes=Path(inputs['archive']['data']).stat().st_size)]),
            'repos/example/repository/actions/runs/123/artifacts?per_page=100')
        with self.assertRaisesRegex(ValueError, 'API digest/size'):
            audit.compare(inputs)

    def test_provisioner_is_not_runner_image_and_duplicate_header_refuses(self):
        with self.assertRaisesRegex(ValueError, 'exactly one complete'):
            audit.header_observation('2026-11-02T08:00:00Z ##[group]Runner Image Provisioner\n'
                '2026-11-02T08:00:01Z Version: irrelevant\n2026-11-02T08:00:02Z ##[endgroup]\n')
        group = ('2026-11-02T08:00:00Z ##[group]Runner Image\n2026-11-02T08:00:01Z Image: win\n'
                 '2026-11-02T08:00:02Z Version: value\n2026-11-02T08:00:03Z ##[endgroup]\n')
        with self.assertRaisesRegex(ValueError, 'exactly one complete'):
            audit.header_observation(group + group)

    def test_missing_capture_time_or_outside_job_refuses(self):
        for observed in ['2026-11-02T08:00:05', '2026-11-02T09:00:05+00:00']:
            with self.subTest(observed=observed):
                self.environment['captured_at_utc'] = observed
                with self.assertRaises(ValueError):
                    audit.compare(self.fixture())

    def machine(self, version):
        return dict(schema='wls-native-machine-image-environment-v1',
            captured_at_utc='2026-11-02T08:00:05.000100+00:00',
            source='read-only Windows Registry HKEY_LOCAL_MACHINE',
            key=audit.MACHINE_ENVIRONMENT_KEY, view='64-bit',
            values={'ImageOS': 'win22', 'ImageVersion': version},
            field_status={'ImageOS': 'OBSERVED', 'ImageVersion': 'OBSERVED'},
            status='OBSERVED', reason=None)

    def test_machine_matching_process_does_not_resolve_original_header_conflict(self):
        self.environment['machine_image_environment'] = self.machine('20261026.347.1')
        result = audit.compare(self.fixture())
        self.assertEqual(result['exit_code'], 1)
        self.assertEqual(result['exact_runner_image_identity'], 'UNRESOLVED')
        self.assertEqual(result['machine_registry_environment']['field_comparison'],
                         {'ImageOS': 'MATCH', 'ImageVersion': 'MATCH'})
        self.assertIsNone(result['machine_registry_environment']['causal_explanation'])
        self.assertIsNone(result['preferred_source'])

    def test_machine_matching_header_does_not_erase_conflicting_process_observation(self):
        self.environment['machine_image_environment'] = self.machine(self.header_version)
        result = audit.compare(self.fixture())
        self.assertEqual(result['exit_code'], 1)
        self.assertEqual(result['exact_runner_image_identity'], 'UNRESOLVED')
        self.assertEqual(result['machine_registry_environment']['field_comparison']['ImageVersion'], 'CONFLICT')
        self.assertEqual(result['process_environment']['values']['ImageVersion'], '20261026.347.1')
        self.assertEqual(result['runner_header']['fields']['Version'], self.header_version)
        self.assertIsNone(result['preferred_source'])

    def test_matching_header_and_process_do_not_erase_conflicting_machine_observation(self):
        self.header_version = self.environment['image_environment']['ImageVersion']
        self.environment['machine_image_environment'] = self.machine('different-machine-version')
        result = audit.compare(self.fixture())
        self.assertTrue(result['image_version_fields_match'])
        self.assertEqual(result['exit_code'], 1)
        self.assertEqual(result['exact_runner_image_identity'], 'UNRESOLVED')
        self.assertEqual(result['machine_registry_environment']['field_comparison']['ImageVersion'], 'CONFLICT')
        self.assertIsNone(result['preferred_source'])

    def test_missing_or_denied_machine_fields_keep_not_checked_without_fallback(self):
        for state in ('MISSING', 'DENIED', 'QUERY_FAILED', 'NOT_CHECKED'):
            with self.subTest(state=state):
                machine = self.machine(None)
                machine['values'] = {'ImageOS': None, 'ImageVersion': None}
                machine['field_status'] = {'ImageOS': state, 'ImageVersion': state}
                machine['status'] = 'NOT_CHECKED'
                self.environment['machine_image_environment'] = machine
                result = audit.compare(self.fixture())
                self.assertEqual(result['machine_registry_environment']['status'], 'NOT_CHECKED')
                self.assertEqual(result['machine_registry_environment']['field_comparison'],
                                 {'ImageOS': 'NOT_CHECKED', 'ImageVersion': 'NOT_CHECKED'})
                self.assertEqual(result['exit_code'], 1)

    def test_machine_observation_rejects_extra_scope_fabricated_status_or_outside_job(self):
        changes = [('schema', 'unknown'), ('key', r'Software\Other'), ('view', '32-bit'),
                   ('values', {'ImageOS': 'win22', 'ImageVersion': None}), ('status', 'NOT_CHECKED'),
                   ('captured_at_utc', '2026-11-02T08:00:04+00:00'),
                   ('captured_at_utc', '2026-11-02T08:00:11+00:00')]
        for key, value in changes:
            with self.subTest(key=key, value=value):
                machine = self.machine('20261026.347.1')
                machine[key] = value
                self.environment['machine_image_environment'] = machine
                with self.assertRaises(ValueError):
                    audit.compare(self.fixture())
        machine = self.machine('20261026.347.1')
        machine['values']['MODEL_API_KEY'] = 'never-a-machine-image-field'
        self.environment['machine_image_environment'] = machine
        with self.assertRaisesRegex(ValueError, 'exactly allowlisted'):
            audit.compare(self.fixture())


if __name__ == '__main__':
    if os.name == 'nt':
        # A bounded synthetic observation, not an extra test or a relaxation
        # of read_file. Keep the original eight assertions and failure exits.
        from windows_stat_probe import emit_windows_stat_probe
        emit_windows_stat_probe()
    unittest.main(verbosity=2)
