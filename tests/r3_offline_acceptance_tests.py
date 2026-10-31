#!/usr/bin/env python3
"""Pure local preparation checks; this suite does not execute wls."""
import copy
import hashlib
import os
from pathlib import Path
import runpy
import socket
import sqlite3
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

if len(sys.argv) != 2:
    raise SystemExit('usage: r3_offline_acceptance_tests.py SOURCE_ROOT')
sys.path.insert(0, str(Path(sys.argv.pop()).resolve() / 'scripts'))
import r3_offline_acceptance as a


class OfflinePreparation(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='wls-a2-contract-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.package = self.root / 'package'
        a.prepare(self.package)

    def hashes(self):
        return {str(path.relative_to(self.root)): hashlib.sha256(path.read_bytes()).hexdigest()
                for path in self.root.rglob('*') if path.is_file()}

    def test_cli_fixture_preserves_real_unittest_dispatch_and_cleanup(self):
        script = a.SOURCE / 'tests/r3_offline_acceptance_cli_tests.py'
        binary = self.root / 'must-not-execute-wls'
        with patch.object(sys, 'argv', [str(script), str(binary), str(a.SOURCE)]), \
             patch.object(sys, 'path', list(sys.path)):
            module = runpy.run_path(str(script), run_name='a2_cli_lifecycle_probe')
        observed = []

        class LifecycleProbe(module['OfflineAcceptanceCLI']):
            def runTest(probe):
                observed.append(probe.directory)
                probe.assertTrue(probe.directory.is_dir())

        # Only replace the expensive workload, not unittest's real class/case
        # lifecycle. These empty files are harness data, never Store evidence.
        def no_business_workload(package, selected_binary):
            self.assertEqual(selected_binary, binary)
            directory = a.mkdir(package / 'run')
            a.write_json(directory / 'plan.json', {})
            a.write_json(directory / 'runs.json', [])
            return {'harness_probe_only': True}

        self.addCleanup(LifecycleProbe.doClassCleanups)
        with patch.object(a, 'run_fake', side_effect=no_business_workload) as workload, \
             patch('subprocess.Popen', side_effect=AssertionError('CLI harness spawned a process')), \
             patch('sqlite3.connect', side_effect=AssertionError('CLI harness opened a Store')):
            result = unittest.TestResult()
            unittest.TestSuite([LifecycleProbe('runTest')]).run(result)
        workload.assert_called_once()
        self.assertTrue(result.wasSuccessful(), (result.errors, result.failures))
        self.assertEqual(result.testsRun, 1)
        self.assertEqual(len(observed), 1)
        self.assertFalse(observed[0].exists())
        self.assertFalse(LifecycleProbe.root.exists())

    def test_prepare_freezes_exact_ten_cells_materials_topology_and_private_modes(self):
        result = a.dry_run(self.package)
        self.assertEqual(result['counts'], {'not_executed': 10})
        self.assertEqual([(r['task_id'], r['condition']) for r in result['rows']], a.CELLS)
        self.assertFalse(result['execution_authorized'])
        for task, material, _ in a.evaluation.TASKS:
            self.assertEqual((self.package / 'materials' / (task + '.txt')).read_bytes(), material.encode())
        multi = (self.package / 'config/multi_agent.yaml').read_text()
        single = (self.package / 'config/single_agent.yaml').read_text()
        self.assertNotIn('  logical_agent:', multi)
        self.assertIn('mode: single-agent-role-broker-v1', single)
        self.assertIn('real_calls_enabled: false', multi)
        self.assertIn('worker_tools: [fs_read, fs_write, ask, ack_change, submit]',
                      (self.package / 'functions/general/function.yaml').read_text())
        for path in self.package.rglob('*'):
            self.assertEqual(path.stat().st_mode & 0o077, 0, path)

    def test_dry_run_cannot_open_store_resolve_credentials_spawn_or_connect(self):
        # Guard the real APIs that could produce these side effects, rather
        # than trusting the zero counters printed by the preparation tool.
        sentinel = 'WLS_A2_CREDENTIAL_MUST_NEVER_BE_RESOLVED'
        before = self.hashes()
        original_open = os.open
        opened = []

        def readonly_open(path, flags, *args, **kwargs):
            self.assertEqual(flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC), 0)
            opened.append(str(path))
            return original_open(path, flags, *args, **kwargs)

        with patch.dict(os.environ, {sentinel: 'synthetic-secret-never-print'}):
            with patch('sqlite3.connect', side_effect=AssertionError('Store opened')), \
                 patch('socket.socket', side_effect=AssertionError('socket created')), \
                 patch('socket.create_connection', side_effect=AssertionError('network connection')), \
                 patch('subprocess.run', side_effect=AssertionError('subprocess')), \
                 patch('subprocess.Popen', side_effect=AssertionError('subprocess')), \
                 patch('os.getenv', side_effect=AssertionError('credential environment resolved')), \
                 patch.object(os._Environ, '__getitem__', side_effect=AssertionError('environment resolved')), \
                 patch('os.open', side_effect=readonly_open):
                result = a.dry_run(self.package)
                card = a.real_template()
                card['credential']['host_reference'] = 'env:' + sentinel
                real = a.check_real_template(card)
        self.assertTrue(opened)
        self.assertEqual(self.hashes(), before)
        self.assertFalse((self.package / 'run').exists())
        for key in ('store_opens', 'reservations', 'credential_resolutions', 'external_calls'):
            self.assertEqual(result[key], 0)
        self.assertFalse(result['send_authority_granted'])
        self.assertFalse(real['execution_authorized'])
        self.assertNotIn('synthetic-secret', a.encoded(result).decode())

    def test_material_and_rehashed_fake_config_mutation_refuse_before_execution(self):
        material = self.package / 'materials/tool_roundtrip.txt'
        original = material.read_bytes()
        material.write_bytes(original + b' change the requirement')
        with self.assertRaisesRegex(a.PackageError, 'changed package member'):
            a.dry_run(self.package)
        material.write_bytes(original)
        target = self.package / 'config/multi_agent.yaml'
        target.write_text(target.read_text().replace('adapter: fake', 'adapter: responses'))
        manifest = a.read_json(self.package / 'manifest.json')
        manifest['files']['config/multi_agent.yaml'] = a.file_hash(target)
        manifest['package_sha256'] = a.evaluation.digest({k: v for k, v in manifest.items()
                                                        if k != 'package_sha256'})
        (self.package / 'manifest.json').write_bytes(a.encoded(manifest))
        with patch('subprocess.run', side_effect=AssertionError('execution before validation')):
            with self.assertRaisesRegex(a.PackageError, 'versioned offline'):
                a.run_fake(self.package, '/nonexistent-wls')
        self.assertFalse((self.package / 'run').exists())

    def test_existing_output_and_symlink_inputs_refuse_without_overwrite(self):
        before = self.hashes()
        with self.assertRaises(FileExistsError):
            a.prepare(self.package)
        link = self.root / 'package-link'
        link.symlink_to(self.package, target_is_directory=True)
        with self.assertRaisesRegex(a.PackageError, 'symlink'):
            a.dry_run(link)
        self.assertEqual(before, self.hashes())
        target = self.package / 'materials/seed.txt'
        target.unlink()
        target.symlink_to(self.package / 'materials/tool_roundtrip.txt')
        with self.assertRaisesRegex(a.PackageError, 'symlink'):
            a.dry_run(self.package)

    def test_fifo_and_duplicate_json_refuse(self):
        fifo = self.root / 'fifo'
        os.mkfifo(fifo, 0o600)
        with self.assertRaisesRegex(a.PackageError, 'regular file'):
            a.read_json(fifo)
        duplicate = self.root / 'duplicate.json'
        duplicate.write_text('{"contract":1,"contract":2}')
        with self.assertRaises(a.evaluation.InvalidEvidence):
            a.read_json(duplicate)

    def test_real_prerequisites_missing_never_imply_authority_or_default_money(self):
        card = a.real_template()
        self.assertTrue(all(value is None for value in card['budget'].values()))
        checked = a.check_real_template(card)
        self.assertFalse(checked['required_fields_complete'])
        self.assertIn('endpoint.capability_source', checked['missing_fields'])
        self.assertIn('billing.trust_provenance', checked['missing_fields'])
        self.assertIn('approval.coordinator_execution_card', checked['missing_fields'])
        for section in card.values():
            if isinstance(section, dict):
                for key in section:
                    section[key] = 'synthetic preparation evidence reference'
        for key in card['budget']:
            if key not in ('cancellation_rule', 'remaining_allowance'):
                card['budget'][key] = 0 if key == 'max_paid_retries' else 1
        card['endpoint']['base_url'] = 'https://example.invalid'
        card['endpoint']['path'] = '/v1/responses'
        card['credential']['host_reference'] = 'env:UNRESOLVED_SYNTHETIC_REFERENCE'
        for key in ('materials_sha256', 'config_sha256', 'profile_sha256'):
            card['approval'][key] = 'a' * 64
        complete = a.check_real_template(card)
        self.assertTrue(complete['required_fields_complete'])
        self.assertFalse(complete['execution_authorized'])
        self.assertEqual(complete['independent_verification'], 'NOT_RUN')
        invalid = copy.deepcopy(card)
        invalid['endpoint']['base_url'] = 'https://synthetic-secret@example.invalid'
        with self.assertRaisesRegex(a.PackageError, 'without embedded credentials'):
            a.check_real_template(invalid)
        invalid = copy.deepcopy(card)
        invalid['credential']['host_reference'] = 'literal-credential-values-are-forbidden'
        with self.assertRaisesRegex(a.PackageError, 'host env:NAME'):
            a.check_real_template(invalid)
        card['execution_authorized'] = True
        with self.assertRaisesRegex(a.PackageError, 'cannot grant'):
            a.check_real_template(card)

    def test_fixed_phases_have_no_host_dispositions_or_external_tool_calls(self):
        for task, _, _ in a.evaluation.TASKS:
            phases = a.fake_phases(self.package, task)
            for _, script in phases:
                if script is None:
                    continue
                steps = script['default'] + [s for group in script.get('nodes', {}).values() for s in group]
                self.assertTrue(all(s['status'] == 'completed' for s in steps))
                self.assertTrue(all(s['usage']['cost_micro_usd'] == 0 for s in steps))
                self.assertNotIn('run', {call['name'] for step in steps for call in step['tool_calls']})
        partial = a.fake_phases(self.package, 'partial_amend')
        self.assertEqual([name for name, _ in partial], ['initial', 'amend', 'update', 'keep', 'acceptances'])
        self.assertEqual(partial[2][1]['nodes']['beta'], [])
        self.assertEqual(a.fake_phases(self.package, 'exhausted_rework')[-1][0], 'escalation')

    def test_human_form_preserves_denominator_and_does_not_preapprove_semantics(self):
        review = a.human_review('a' * 64)
        self.assertEqual(len(review['rows']), 10)
        self.assertTrue(all(row['semantic_result'] == 'NOT_RUN' and row['reviewer'] is None
                            for row in review['rows']))
        self.assertEqual(review['coordinator_acceptance'], 'PENDING')


if __name__ == '__main__':
    if os.geteuid() == 0:
        raise SystemExit('Offline acceptance regressions require an ordinary user')
    unittest.main(verbosity=2)
