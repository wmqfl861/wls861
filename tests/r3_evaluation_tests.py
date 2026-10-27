#!/usr/bin/env python3
"""Evaluation framework tests against disposable real CLI/SQLite evidence."""
from contextlib import closing
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import unittest

spec = importlib.util.spec_from_file_location('acceptance_helpers', Path(__file__).with_name('s15_cli_smoke.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)
spec = importlib.util.spec_from_file_location('evaluation', h.SOURCE / 'scripts/evaluate_r3.py')
e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(e)


class Evaluation(unittest.TestCase):
    setUp = h.AcceptanceCliSmoke.setUp
    invoke = h.AcceptanceCliSmoke.invoke
    json_file = h.AcceptanceCliSmoke.json_file
    fixture = h.AcceptanceCliSmoke.fixture
    publish = h.AcceptanceCliSmoke.publish
    ack = h.AcceptanceCliSmoke.ack

    def seed(self, task=0, key='seed'):
        material = self.directory / (key + '.txt')
        material.write_text(e.TASKS[task][1])
        return self.invoke('submit', '--db', self.database, '--key', key,
                           '--text-file', material)['request']['request_id']

    def plan(self, request):
        with e.database(self.database) as db:
            return e.make_plan(db, request)

    def run_entry(self, plan, request, condition='multi_agent', task=0):
        return {'task_id': e.TASKS[task][0], 'condition': condition,
                'database': str(self.database), 'request_id': request,
                'elapsed_ms': 1000, 'plan_sha256': plan['plan_sha256']}

    def execute(self, request, unknown=False, error=False, max_calls=3, worker_calls=1, script=None):
        fixture = copy.deepcopy(script) if script is not None else self.fixture()
        def original_constraints(value):
            if isinstance(value, dict):
                for key, child in value.items():
                    if key in ('constraints_verbatim', 'constraint_refs'):
                        value[key] = []
                    else:
                        original_constraints(child)
            elif isinstance(value, list):
                for child in value:
                    original_constraints(child)
        original_constraints(fixture)
        if unknown:
            fixture['default'][0]['usage']['cost_micro_usd'] = None
        if error:
            fixture['default'][0]['status'] = 'error'
            fixture['default'][0]['error_code'] = 'synthetic_failure'
            fixture['default'][0]['error_message'] = 'synthetic only'
            fixture['default'][0]['tool_calls'] = []
        return self.invoke('pump-fake', '--db', self.database, '--request', request,
            '--script', self.json_file('script.json', fixture), '--workspaces-file', self.mapping,
            '--worker-calls', worker_calls, '--max-calls', max_calls)

    def step(self, role, name, arguments, index=1):
        return {'role': role, 'role_call_index': index, 'status': 'completed',
                'tool_calls': [{'call_id': f'{role}-{name}-{index}', 'name': name, 'arguments': arguments}],
                'usage': {'input_tokens': 100, 'output_tokens': 20, 'cache_read_tokens': 0,
                          'cache_write_tokens': 0, 'cost_micro_usd': 0}}

    def two_workers(self, request):
        second = self.directory / 'beta'
        second.mkdir(mode=0o700)
        self.mapping = self.json_file('workspaces.json', {'alpha': str(self.workspace), 'beta': str(second)})
        return self.execute(request, script=self.fixture('s13_fake_initial.json'), max_calls=5)

    def test_multiple_worker_principals_cannot_be_a_single_agent(self):
        request = self.seed()
        plan = self.plan(request)
        self.two_workers(request)
        # A deliberately reduced evidence fixture keeps two actual Workers but
        # removes management attempts. A role-only check would mislabel it.
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute("DELETE FROM call_attempts WHERE role<>'worker'")
            db.execute('UPDATE request_budgets SET attempts=2,spent_input=200,spent_output=40,state_version=state_version+1 WHERE request_id=?', (request,))
        row = e.build_report(plan, [self.run_entry(plan, request, 'single_agent')])['rows'][1]
        self.assertEqual(row['status'], 'failed', row)
        self.assertEqual(row['roles'], {'worker': 2})
        self.assertEqual(row['agent_identity_count'], 2)
        self.assertFalse(row['checks']['condition_roles'])

    def test_cancelled_amendments_do_not_satisfy_update_and_keep(self):
        request = self.seed(task=1)
        plan = self.plan(request)
        self.two_workers(request)
        amendment = self.directory / 'amendment.txt'
        amendment.write_text('An explicit new requirement for this synthetic test.')
        self.invoke('amend', '--db', self.database, '--request', request,
            '--text-file', amendment, '--constraints-file', self.json_file('constraints.json', []))
        changed = self.fixture('s13_fake_amended.json')['default'][0]['tool_calls'][0]['arguments']
        script = {'default': [self.step('dispatcher', 'submit_amendment_plan', {
            'plan': changed, 'dispositions': {node: {'disposition': 'cancel', 'rationale': 'Explicit cancellation for a negative rubric fixture.'}
                                            for node in ('alpha', 'beta')}})]}
        self.execute(request, script=script, max_calls=1)
        row = e.build_report(plan, [self.run_entry(plan, request, task=1)])['rows'][2]
        self.assertEqual(row['status'], 'failed', row)
        self.assertTrue(row['checks']['all_tasks_current'])
        self.assertFalse(row['checks']['accepted_amend_update_and_keep'])

    def test_rework_counter_without_original_rejection_is_not_success(self):
        request = self.seed(task=2)
        plan = self.plan(request)
        self.execute(request, max_calls=12, worker_calls=2)
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute('UPDATE team_workflow_state SET rework_count=1')
        row = e.build_report(plan, [self.run_entry(plan, request, task=2)])['rows'][4]
        self.assertEqual(row['status'], 'failed', row)
        self.assertFalse(row['checks']['accepted_rework_same_task'])

    def test_original_rejection_correction_and_same_task_acceptance_pass(self):
        request = self.seed(task=2)
        plan = self.plan(request)
        rejection = self.fixture()
        rejection['default'][1] = self.step('dispatcher', 'accept_candidate', {'passed': False, 'notes': 'Synthetic rejection.'}, 2)
        rejection['nodes']['alpha'] = [step for step in rejection['nodes']['alpha'] if step['tool_calls'][0]['name'] != 'report_experience']
        self.execute(request, script=rejection, max_calls=12, worker_calls=2)
        correction = self.fixture()
        correction['default'][0] = self.step('dispatcher', 'change_task',
            {'rationale': 'Original candidate was rejected.', 'correction': 'Correct the same original task under its frozen constraints.'})
        writer = correction['nodes']['alpha'][1]['tool_calls'][0]['arguments']
        writer['content'] = 'corrected synthetic candidate\n'
        writer['expected_sha256'] = hashlib.sha256((self.workspace / 'result.txt').read_bytes()).hexdigest()
        self.execute(request, script=correction, max_calls=12, worker_calls=2)
        row = e.build_report(plan, [self.run_entry(plan, request, task=2)])['rows'][4]
        self.assertEqual(row['status'], 'passed', row)
        self.assertTrue(row['checks']['accepted_rework_same_task'])

    def test_confirmed_original_delivery_passes_fixed_evidence_rubric(self):
        request = self.seed(task=4)
        plan = self.plan(request)
        self.execute(request, max_calls=12, worker_calls=2)
        delivery = self.publish(request)
        self.ack(request, delivery)
        row = e.build_report(plan, [self.run_entry(plan, request, task=4)])['rows'][8]
        self.assertEqual(row['status'], 'passed', row)
        self.assertEqual(row['request_status'], 'DELIVERED')
        self.assertTrue(row['checks']['confirmed_delivery'])

    def test_missing_database_is_retained_as_invalid_cell(self):
        request = self.seed()
        plan = self.plan(request)
        run = self.run_entry(plan, request)
        run['database'] = str(self.directory / 'missing.sqlite')
        report = e.build_report(plan, [run])
        self.assertEqual(report['counts'], {'invalid_evidence': 1, 'not_executed': 9})
        self.assertFalse(Path(run['database']).exists())

    def test_hard_link_does_not_allow_reusing_the_same_request(self):
        request = self.seed()
        plan = self.plan(request)
        second = self.directory / 'alias.sqlite'
        os.link(self.database, second)
        other = self.run_entry(plan, request, 'single_agent')
        other['database'] = str(second)
        with self.assertRaises(e.InvalidEvidence):
            e.build_report(plan, [self.run_entry(plan, request), other])

    def test_budget_disagreement_is_visible_as_invalid_evidence(self):
        request = self.seed()
        plan = self.plan(request)
        self.execute(request)
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute('UPDATE request_budgets SET spent_input=spent_input+1,state_version=state_version+1 WHERE request_id=?', (request,))
        row = e.build_report(plan, [self.run_entry(plan, request)])['rows'][0]
        self.assertEqual(row['status'], 'invalid_evidence')
        self.assertIn('reconstruct budget', row['reason'])

    def test_plan_fixes_five_tasks_and_ten_missing_cells(self):
        request = self.seed()
        plan = self.plan(request)
        report = e.build_report(plan, [])
        self.assertEqual(report['counts'], {'not_executed': 10})
        self.assertFalse(report['paired_cells_complete'])
        self.assertFalse(report['general_quality_or_cost_advantage_established'])

    def test_original_ledger_counts_and_missing_baseline_remain_visible(self):
        request = self.seed()
        plan = self.plan(request)
        result = self.execute(request)
        report = e.build_report(plan, [self.run_entry(plan, request)])
        row = report['rows'][0]
        self.assertEqual(row['status'], 'failed', row)
        self.assertEqual(len(row['attempts']), len(result['calls']))
        self.assertTrue(row['checks']['condition_roles'])
        self.assertEqual(row['source_kinds'], {'synthetic_fake': len(result['calls'])})
        self.assertEqual(report['counts']['not_executed'], 9)
        self.assertNotIn('config_snapshot', e.canonical(report).decode())
        self.assertNotIn(e.TASKS[0][1], e.canonical(report).decode())

    def test_unknown_cost_is_not_zero_or_dropped(self):
        request = self.seed()
        plan = self.plan(request)
        self.execute(request, unknown=True)
        row = e.build_report(plan, [self.run_entry(plan, request)])['rows'][0]
        self.assertEqual(row['status'], 'failed', row)
        self.assertEqual(row['usage']['cost_micro_usd']['unknown_attempts'], 1)
        self.assertEqual(row['usage']['cost_micro_usd']['known_total'], 0)
        self.assertEqual(row['budget']['unknown_cost'], 0)  # Zero reserve is still unknown.

    def test_failed_model_attempt_remains_in_report(self):
        request = self.seed()
        plan = self.plan(request)
        self.execute(request, error=True)
        row = e.build_report(plan, [self.run_entry(plan, request)])['rows'][0]
        self.assertEqual(row['status'], 'failed', row)
        self.assertGreater(len(row['attempts']), 0)
        self.assertEqual(row['attempts'][0]['status'], 'ERROR')

    def test_multi_role_run_cannot_be_called_single_agent(self):
        request = self.seed()
        plan = self.plan(request)
        self.execute(request)
        row = e.build_report(plan, [self.run_entry(plan, request, 'single_agent')])['rows'][1]
        self.assertEqual(row['status'], 'failed', row)
        self.assertFalse(row['checks']['condition_roles'])

    def test_plan_mutation_and_rehashed_rubric_mutation_refuse(self):
        plan = self.plan(self.seed())
        changed = copy.deepcopy(plan)
        changed['common']['budget']['max_attempts'] += 1
        with self.assertRaises(e.InvalidEvidence):
            e.build_report(changed, [])
        changed = copy.deepcopy(plan)
        changed['tasks'][0]['checks'] = {}
        changed['plan_sha256'] = e.digest({k: v for k, v in changed.items() if k != 'plan_sha256'})
        with self.assertRaises(e.InvalidEvidence):
            e.build_report(changed, [])

    def test_duplicate_cells_and_request_reuse_refuse(self):
        request = self.seed()
        plan = self.plan(request)
        run = self.run_entry(plan, request)
        with self.assertRaises(e.InvalidEvidence):
            e.build_report(plan, [run, run])
        with self.assertRaises(e.InvalidEvidence):
            e.build_report(plan, [run, self.run_entry(plan, request, 'single_agent')])

    def test_changed_materials_invalid_and_all_other_cells_retained(self):
        request = self.seed()
        plan = self.plan(request)
        run = self.run_entry(plan, request, task=1)
        report = e.build_report(plan, [run])
        self.assertEqual(report['counts'], {'not_executed': 9, 'invalid_evidence': 1})
        self.assertIn('materials differ', report['rows'][2]['reason'])

    def test_changed_budget_condition_is_not_comparable(self):
        request = self.seed()
        plan = self.plan(request)
        plan['common']['budget']['input_limit'] -= 1
        plan['plan_sha256'] = e.digest({k: v for k, v in plan.items() if k != 'plan_sha256'})
        row = e.build_report(plan, [self.run_entry(plan, request)])['rows'][0]
        self.assertEqual(row['status'], 'invalid_evidence')
        self.assertIn('conditions differ', row['reason'])

    def test_synthetic_evidence_cannot_claim_provider(self):
        request = self.seed()
        plan = self.plan(request)
        self.execute(request)
        plan['evidence_mode'] = 'provider'
        plan['plan_sha256'] = e.digest({k: v for k, v in plan.items() if k != 'plan_sha256'})
        row = e.build_report(plan, [self.run_entry(plan, request)])['rows'][0]
        self.assertEqual(row['status'], 'invalid_evidence')
        self.assertIn('cannot be relabelled', row['reason'])

    def test_deadline_exhaustion_is_reported_as_failure(self):
        request = self.seed()
        plan = self.plan(request)
        run = self.run_entry(plan, request)
        run['elapsed_ms'] = plan['common']['max_wall_ms'] + 1
        row = e.build_report(plan, [run])['rows'][0]
        self.assertEqual(row['status'], 'failed', row)
        self.assertFalse(row['checks']['wall_bound'])

    def test_plan_is_required_before_attempts(self):
        request = self.seed()
        self.execute(request)
        with self.assertRaises(e.InvalidEvidence):
            self.plan(request)

    def test_reporting_preserves_all_original_database_rows(self):
        request = self.seed()
        plan = self.plan(request)
        self.execute(request)
        def dump():
            with closing(sqlite3.connect(self.database.as_uri() + '?mode=ro', uri=True)) as db:
                return '\n'.join(db.iterdump())
        before = dump()
        e.build_report(plan, [self.run_entry(plan, request)])
        self.assertEqual(dump(), before)

    def test_nonregular_and_duplicate_json_inputs_refuse(self):
        fifo = self.directory / 'input.fifo'
        os.mkfifo(fifo, 0o600)
        with self.assertRaises(e.InvalidEvidence):
            e.read_json(fifo)
        original = self.json_file('original.json', {})
        link = self.directory / 'link.json'
        link.symlink_to(original)
        with self.assertRaises(e.InvalidEvidence):
            e.read_json(link)
        with self.assertRaises(e.InvalidEvidence):
            e.strict_json('{"x":1,"x":2}')

    def test_query_byte_bound_stops_before_fetching_the_complete_result(self):
        seen = []
        with closing(sqlite3.connect(':memory:')) as db:
            db.row_factory = sqlite3.Row
            def payload(index):
                seen.append(index)
                return 'x' * (1024 * 1024)
            db.create_function('bounded_payload', 1, payload)
            with self.assertRaisesRegex(e.InvalidEvidence, 'byte limit'):
                e.rows(db, 'WITH RECURSIVE n(x) AS (VALUES(1) UNION ALL SELECT x+1 FROM n WHERE x<100) SELECT bounded_payload(x) AS payload FROM n')
        self.assertLess(len(seen), 35)


if __name__ == '__main__':
    if os.geteuid() == 0:
        raise SystemExit('Evaluation regressions require an ordinary user')
    unittest.main(verbosity=2)
