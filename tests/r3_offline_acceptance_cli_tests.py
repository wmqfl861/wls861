#!/usr/bin/env python3
"""One CLI integration suite: ten fixed Fake cells plus evidence negatives.

Mutations affect SQLite backups in separate disposable fixtures only. Every
positive cell is produced by the actual Store/runtime, never by fabricated rows.
"""
from contextlib import closing
import copy
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import sys
import tempfile
import unittest

if len(sys.argv) != 3:
    raise SystemExit('usage: r3_offline_acceptance_cli_tests.py WLS_BINARY SOURCE_ROOT')
BINARY, SOURCE = (Path(value).resolve() for value in sys.argv[1:])
sys.path.insert(0, str(SOURCE / 'scripts'))
sys.argv = [sys.argv[0]]
import r3_offline_acceptance as a
e = a.evaluation


class OfflineAcceptanceCLI(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='wls-a2-cli-')
        cls.addClassCleanup(cls.temporary.cleanup)
        cls.root = Path(cls.temporary.name)
        cls.package = cls.root / 'package'
        a.prepare(cls.package)
        cls.result = a.run_fake(cls.package, BINARY)
        cls.run = cls.package / 'run'
        cls.plan = a.read_json(cls.run / 'plan.json')
        cls.index = a.read_json(cls.run / 'runs.json')
        cls.original_hashes = {str(path.relative_to(cls.run)): a.file_hash(path)
                               for path in cls.run.rglob('store.sqlite')}

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='negative-', dir=self.root)
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def cell(self, task='delivery_receipt', condition='multi_agent'):
        return copy.deepcopy(next(row for row in self.index
                                  if row['task_id'] == task and row['condition'] == condition))

    def backup(self, task='delivery_receipt', condition='multi_agent'):
        entry = self.cell(task, condition)
        target = self.directory / 'negative.sqlite'
        with e.database(self.run / entry['database']) as original, closing(sqlite3.connect(target)) as output:
            original.backup(output)
        entry['database'] = str(target)
        return target, entry

    def inspect(self, entry, plan=None):
        result = e.build_report(plan or self.plan, [entry], self.run)
        self.assertEqual(len(result['rows']), 10)
        self.assertEqual(result['counts']['not_executed'], 9)
        return next(row for row in result['rows']
                    if row['task_id'] == entry['task_id'] and row['condition'] == entry['condition'])

    def test_01_all_ten_real_store_cells_preserve_original_source_identity_and_accounting(self):
        self.assertEqual(self.result['evaluation']['counts'], {'passed': 10}, self.result)
        self.assertTrue(self.result['evaluation']['paired_cells_complete'])
        self.assertEqual(len(self.index), 10)
        self.assertEqual(len({(row['database'], row['request_id']) for row in self.index}), 10)
        self.assertTrue(all(row['runner_status'] == 'COMPLETED' for row in self.result['runner_outcomes']))
        for row in self.result['evaluation']['rows']:
            self.assertGreater(len(row['attempts']), 0)
            self.assertEqual(row['source_kinds'], {'synthetic_fake': len(row['attempts'])})
            self.assertTrue(row['checks']['condition_roles'])
            self.assertTrue(row['decision_evidence']['model_decision_origin_established'])
            self.assertEqual(row['usage']['cost_micro_usd'], {'known_total': 0, 'unknown_attempts': 0})
            self.assertEqual(row['usage']['input_tokens']['known_total'], row['budget']['spent_input'])
            self.assertEqual(row['usage']['output_tokens']['known_total'], row['budget']['spent_output'])
            if row['condition'] == 'single_agent':
                self.assertEqual(row['identity_evidence']['logical_agent_count'], 1)
                self.assertTrue(row['identity_evidence']['shared_request_history'])
        self.assertFalse(self.result['evaluation']['provider_quality_acceptance_established'])
        self.assertEqual(self.result['actual_provider_cost'], 'NOT_RUN')

    def test_02_partial_amend_omitted_subject_stays_paused_until_model_keep(self):
        for condition in e.CONDITIONS:
            entry = self.cell('partial_amend', condition)
            cell = (self.run / entry['database']).parent
            update = a.read_json(cell / 'update.result.json')
            keep = a.read_json(cell / 'keep.result.json')
            self.assertFalse(any(call['node_id'] == 'beta' and call['role'] == 'worker'
                                 for call in update['calls']))
            self.assertTrue(any(call['node_id'] == 'beta' and call['role'] == 'worker'
                                for call in keep['calls']))
            with e.database(self.run / entry['database']) as db:
                decisions = e.rows(db, 'SELECT disposition FROM amend_dispositions ORDER BY rowid')
                self.assertEqual(decisions, [{'disposition': 'update'}, {'disposition': 'keep'}])
                self.assertEqual({row['status'] for row in e.rows(db, 'SELECT status FROM tasks')}, {'ACCEPTED'})
                self.assertEqual(e.one(db, 'SELECT COUNT(*) AS n FROM request_texts', ())['n'], 2)

    def test_03_rework_exhaustion_and_exact_delivery_keep_original_bytes(self):
        for condition in e.CONDITIONS:
            rework = self.cell('rejected_rework', condition)
            with e.database(self.run / rework['database']) as db:
                self.assertEqual([row['status'] for row in e.rows(db, 'SELECT status FROM candidate_acceptances ORDER BY rowid')],
                                 ['REJECTED', 'ACCEPTED'])
                self.assertEqual(len(e.rows(db, 'SELECT * FROM workflow_submissions')), 2)
                self.assertEqual(e.one(db, 'SELECT rework_count FROM team_workflow_state', ())['rework_count'], 1)
            exhausted = self.cell('exhausted_rework', condition)
            with e.database(self.run / exhausted['database']) as db:
                self.assertEqual(e.one(db, 'SELECT rework_count FROM team_workflow_state', ())['rework_count'], 1)
                self.assertEqual(e.one(db, 'SELECT q.holder,r.status FROM questions q JOIN question_runtime r USING(question_id)', ()),
                                 {'holder': 'user', 'status': 'OPEN'})
            delivery = self.cell('delivery_receipt', condition)
            with e.database(self.run / delivery['database']) as db:
                self.assertEqual(e.one(db, 'SELECT status FROM delivery_runtime', ())['status'], 'CONFIRMED')
                self.assertEqual(len(e.rows(db, 'SELECT * FROM delivery_receipts')), 1)
                frozen = e.strict_json(e.one(db, 'SELECT frozen_plan FROM delivery_runtime', ())['frozen_plan'])
                self.assertTrue(frozen['files'])
                for item in frozen['files']:
                    import base64
                    content = base64.b64decode(item['content_base64'], validate=True)
                    self.assertEqual(len(content), item['size_bytes'])
                    self.assertEqual(hashlib.sha256(content).hexdigest(), item['sha256'])
                    self.assertEqual(content, b'alpha candidate version 1\n')

    def test_04_reporting_and_failed_rerun_do_not_modify_original_evidence(self):
        before = {str(path.relative_to(self.run)): a.file_hash(path) for path in self.run.rglob('store.sqlite')}
        self.assertEqual(a.report(self.package), self.result)
        with self.assertRaises(FileExistsError):
            a.run_fake(self.package, BINARY)
        after = {str(path.relative_to(self.run)): a.file_hash(path) for path in self.run.rglob('store.sqlite')}
        self.assertEqual(before, after)
        self.assertEqual(self.original_hashes, after)
        public = a.encoded(self.result).decode()
        self.assertNotIn('alpha candidate version', public)
        self.assertNotIn('config_snapshot"', public)

    def test_05_missing_or_changed_original_feedback_cannot_be_host_filled(self):
        target, entry = self.backup()
        with closing(sqlite3.connect(target)) as db, db:
            db.execute("DELETE FROM model_tool_feedback WHERE attempt_id=(SELECT attempt_id FROM call_attempts WHERE json_extract(response,'$.tool_calls[0].name')='accept_candidate')")
        row = self.inspect(entry)
        self.assertIn(row['status'], ('failed', 'invalid_evidence'))
        self.assertNotEqual(row['status'], 'passed')

    def test_06_accounting_conflict_cannot_be_completed_or_hidden_by_duplicate_cells(self):
        target, entry = self.backup()
        with closing(sqlite3.connect(target)) as db, db:
            db.execute('UPDATE request_budgets SET spent_input=spent_input+1,state_version=state_version+1')
        row = self.inspect(entry)
        self.assertEqual(row['status'], 'invalid_evidence')
        self.assertIn('reconstruct budget', row['reason'])
        with self.assertRaisesRegex(e.InvalidEvidence, 'duplicate run cell'):
            e.build_report(self.plan, [entry, entry], self.run)
        alias = self.directory / 'same-evidence.sqlite'
        os.link(target, alias)
        relabel = dict(entry, condition='single_agent', database=str(alias))
        with self.assertRaisesRegex(e.InvalidEvidence, 'cannot be reused'):
            e.build_report(self.plan, [entry, relabel], self.run)

    def test_07_rehashed_logical_label_cannot_replace_native_history(self):
        target, entry = self.backup(condition='single_agent')
        with closing(sqlite3.connect(target)) as db, db:
            attempt, raw = db.execute('SELECT attempt_id,request_envelope FROM call_attempts ORDER BY rowid DESC LIMIT 1').fetchone()
            envelope = json.loads(raw)
            logical = envelope['context']['input']
            self.assertGreater(len(logical['messages']), 2)
            logical['messages'] = logical['messages'][2:]
            envelope['context']['logical_input_sha256'] = e.digest(logical)
            envelope['logical_request_sha256'] = e.digest({key: value for key, value in envelope.items()
                                                           if key not in ('logical_request_sha256', 'wire_request_sha256')})
            db.execute('UPDATE call_attempts SET request_envelope=?,state_version=state_version+1 WHERE attempt_id=?',
                       (e.canonical(envelope).decode(), attempt))
        row = self.inspect(entry)
        self.assertEqual(row['status'], 'invalid_evidence')
        self.assertIn('actual model input omitted or changed', row['reason'])

    def test_08_synthetic_source_cannot_be_relabelled_provider(self):
        entry = self.cell()
        plan = copy.deepcopy(self.plan)
        plan['evidence_mode'] = 'provider'
        plan['plan_sha256'] = e.digest({key: value for key, value in plan.items() if key != 'plan_sha256'})
        entry['plan_sha256'] = plan['plan_sha256']
        row = self.inspect(entry, plan)
        self.assertEqual(row['status'], 'invalid_evidence')
        self.assertIn('cannot be relabelled', row['reason'])

    def test_09_original_unknown_cost_retains_nullable_ledger_and_failed_cell(self):
        runner = a.FakeRunner(BINARY, self.directory)
        cell = a.mkdir(self.directory / 'unknown')
        request = runner.admit(self.package, cell, 'tool_roundtrip', 'multi_agent')
        fixture = a.fake_phases(self.package, 'tool_roundtrip')[0][1]
        fixture['default'][0]['usage']['cost_micro_usd'] = None
        a.write_json(cell / 'unknown.json', fixture)
        result = runner.invoke('pump-fake', '--db', cell / 'store.sqlite', '--request', request,
                               '--script', cell / 'unknown.json', '--max-calls', 1)
        self.assertEqual(result['real_model_calls'], 0)
        entry = {'task_id': 'tool_roundtrip', 'condition': 'multi_agent',
                 'database': str(cell / 'store.sqlite'), 'request_id': request,
                 'elapsed_ms': 1, 'plan_sha256': self.plan['plan_sha256']}
        row = self.inspect(entry)
        self.assertEqual(row['status'], 'failed')
        self.assertEqual(row['usage']['cost_micro_usd']['unknown_attempts'], 1)
        self.assertEqual(row['usage']['cost_micro_usd']['known_total'], 0)
        with e.database(cell / 'store.sqlite') as db:
            self.assertIsNone(e.one(db, 'SELECT cost_micro_usd FROM call_attempts', ())['cost_micro_usd'])

    def test_10_v1_v2_historical_single_worker_meaning_remains_unchanged(self):
        with e.database(self.run / 'plan-seed/store.sqlite') as db:
            request = e.one(db, 'SELECT request_id FROM requests', ())['request_id']
            legacy = [e.make_plan(db, request, decision_evidence=contract)
                      for contract in (None, 'model-origin-v1')]
        for version, plan in enumerate(legacy, 1):
            self.assertEqual(plan['schema_version'], version)
            entry = self.cell(condition='single_agent')
            entry['plan_sha256'] = plan['plan_sha256']
            # The unchanged v1/v2 lock includes logical_agent in workflow
            # policy. Derive each legacy seed from the actual single config,
            # without retroactively changing original attempt bytes.
            runner = a.FakeRunner(BINARY, a.mkdir(self.directory / str(version)))
            seed = a.mkdir(runner.directory / 'seed')
            request = runner.admit(self.package, seed, 'delivery_receipt', 'single_agent')
            with e.database(seed / 'store.sqlite') as db:
                plan = e.make_plan(db, request, decision_evidence=None if version == 1 else 'model-origin-v1')
            entry['plan_sha256'] = plan['plan_sha256']
            row = self.inspect(entry, plan)
            self.assertEqual(row['status'], 'failed')
            self.assertFalse(row['checks']['condition_roles'])


if __name__ == '__main__':
    if os.geteuid() == 0:
        raise SystemExit('Offline CLI acceptance requires an ordinary user')
    unittest.main(verbosity=2)
