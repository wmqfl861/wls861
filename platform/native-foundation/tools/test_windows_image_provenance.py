#!/usr/bin/env python3
"""Synthetic source-binding tests; not Windows execution or product CTest."""
import copy
import hashlib
import io
import json
from pathlib import Path
import tempfile
import unittest
import zipfile

import windows_image_provenance as audit


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


if __name__ == '__main__':
    unittest.main(verbosity=2)
