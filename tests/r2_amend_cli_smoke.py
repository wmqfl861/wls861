#!/usr/bin/env python3
"""Model AMEND choices through real CLI/SQLite with fixed offline scripts.

Only disposable private directories are used. These tests validate state,
evidence and dispatch boundaries, not the quality of a live model decision.
"""
from contextlib import closing
import copy
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import unittest

spec = importlib.util.spec_from_file_location('amend_event_helpers', Path(__file__).with_name('s13_cli_smoke.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


class ModelAmendCli(unittest.TestCase):
    # Reuse fixture helpers, not inherited tests, so suite counts are explicit.
    setUp = h.EventCliSmoke.setUp
    invoke = h.EventCliSmoke.invoke
    rows = h.EventCliSmoke.rows
    json_file = h.EventCliSmoke.json_file
    fixture = h.EventCliSmoke.fixture
    show = h.EventCliSmoke.show
    tasks = h.EventCliSmoke.tasks
    pump = h.EventCliSmoke.pump
    amend = h.EventCliSmoke.amend
    pending = h.EventCliSmoke.pending

    def admit(self, key='model-amend', structured=False):
        args = ['submit', '--db', self.database, '--key', key,
                '--text-file', h.FIXTURES / 's13_request.txt',
                '--constraints-file', h.FIXTURES / 's13_constraints.json']
        if structured:
            config = self.directory / (key + '-system.yaml')
            config.write_text((h.SOURCE / 'config/examples/system.yaml').read_text().replace(
                'request_format: framed-v1', 'request_format: structured-v2'))
            args += ['--config', config, '--functions-dir', h.SOURCE / 'config/examples/functions']
        return self.invoke(*args)['request']['request_id']

    def initial(self, request, mapping=None):
        result = self.pump(request, workers=1, mapping=mapping)
        self.assertEqual(len(result['calls']), 5, result)

    def decision(self, kind):
        return {'disposition': kind, 'rationale': 'Explicit current AMEND decision for this original node.'}

    def step(self, name, arguments, index=1):
        return {'role': 'dispatcher', 'role_call_index': index, 'status': 'completed',
                'tool_calls': [{'call_id': f'amend-{index}', 'name': name, 'arguments': arguments}],
                'usage': {'input_tokens': 100, 'output_tokens': 20, 'cache_read_tokens': 0,
                          'cache_write_tokens': 0, 'cost_micro_usd': 0}}

    def model_plan(self, decisions, nodes=('alpha', 'beta')):
        script = self.fixture('s13_fake_amended.json')
        plan = copy.deepcopy(script['default'][0]['tool_calls'][0]['arguments'])
        script['default'] = [self.step('submit_amendment_plan', {'plan': plan, 'dispositions': decisions})]
        script['nodes'] = {node: script['nodes'][node] if node in nodes else [] for node in ('alpha', 'beta')}
        return script

    def resolution(self, decisions, nodes=('beta',)):
        script = self.fixture('s13_fake_amended.json')
        script['default'] = [self.step('resolve_amendment', {'dispositions': decisions})]
        script['nodes'] = {node: script['nodes'][node] if node in nodes else [] for node in ('alpha', 'beta')}
        return script

    def semantic_tasks(self, request):
        fields = ('task_id', 'node_id', 'task_revision', 'based_on_requirements_revision',
                  'plan_revision', 'instruction_revision', 'status', 'brief')
        return {node: {field: task[field] for field in fields} for node, task in self.tasks(request).items()}

    def request_snapshot(self, request):
        result = {}
        with closing(sqlite3.connect(self.database.as_uri() + '?mode=ro', uri=True)) as db:
            tables = [row[0] for row in db.execute(
                "SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")]
            parents = {'attempt_id': 'call_attempts', 'op_id': 'ops', 'team_id': 'teams',
                       'task_id': 'tasks', 'experience_id': 'experience_jobs', 'delivery_id': 'delivery_intents'}
            for table in tables:
                columns = {row[1] for row in db.execute(f'PRAGMA table_info("{table}")')}
                if 'request_id' in columns:
                    result[table] = db.execute(f'SELECT * FROM "{table}" WHERE request_id=? ORDER BY rowid', (request,)).fetchall()
                else:
                    for key, parent in parents.items():
                        if key in columns and parent in tables:
                            pcols = {row[1] for row in db.execute(f'PRAGMA table_info("{parent}")')}
                            if key in pcols and 'request_id' in pcols:
                                result[table] = db.execute(
                                    f'SELECT c.* FROM "{table}" c JOIN "{parent}" p USING("{key}") '
                                    'WHERE p.request_id=? ORDER BY c.rowid', (request,)).fetchall()
                                break
        self.assertIn('call_attempts', result)
        self.assertIn('model_tool_feedback', result)
        self.assertIn('operation_results', result)
        self.assertIn('event_inbox', result)
        return result

    def partial_then_keep(self, structured=False):
        request = self.admit(structured=structured)
        self.initial(request)
        old = self.tasks(request)
        instructions = self.rows('SELECT * FROM instructions ORDER BY rowid')
        self.amend(request)
        first = self.pump(request, workers=1, script=self.json_file('model-plan.json',
            self.model_plan({'alpha': self.decision('update')})))
        self.assertEqual(len(first['calls']), 3, first)
        self.assertEqual(first['calls'][0]['workflow_action']['kind'], 'amendment')
        current = self.tasks(request)
        self.assertEqual(current['alpha']['based_on_requirements_revision'], 2)
        self.assertEqual(current['beta']['based_on_requirements_revision'], 1)
        self.assertEqual((self.workspaces['alpha'] / 'r2.txt').read_text(), 'alpha revision 2\n')
        self.assertFalse((self.workspaces['beta'] / 'r2.txt').exists())
        pending = self.rows("SELECT i.* FROM event_inbox i JOIN events e ON e.seq=i.event_seq "
                            "WHERE i.subject_task_id=? AND i.role='dispatcher' AND i.status='PENDING' AND e.type='amend'",
                            (current['beta']['task_id'],))
        self.assertTrue(pending)
        alpha_order_count = self.rows('SELECT COUNT(*) AS n FROM instructions WHERE task_id=?',
                                      (current['alpha']['task_id'],))[0]['n']
        second = self.pump(request, workers=1, script=self.json_file('model-keep.json',
            self.resolution({'beta': self.decision('keep')})))
        self.assertEqual(len(second['calls']), 3, second)
        self.assertEqual(second['calls'][0]['workflow_action']['target']['plan_required'], False)
        after = self.tasks(request)
        self.assertEqual(after['alpha']['task_revision'], current['alpha']['task_revision'])
        self.assertEqual(after['alpha']['instruction_revision'], current['alpha']['instruction_revision'])
        self.assertEqual(self.show(request)['request']['plan_revision'], 2)
        self.assertEqual(after['beta']['task_revision'], old['beta']['task_revision'] + 1)
        self.assertEqual(after['beta']['instruction_revision'], old['beta']['instruction_revision'] + 1)
        self.assertEqual(after['beta']['applied_task_revision'], after['beta']['task_revision'])
        self.assertEqual(after['beta']['applied_instruction_revision'], after['beta']['instruction_revision'])
        self.assertEqual((self.workspaces['beta'] / 'r2.txt').read_text(), 'beta revision 2\n')
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM instructions WHERE task_id=?',
                                  (current['alpha']['task_id'],))[0]['n'], alpha_order_count)
        self.assertEqual(self.rows('SELECT * FROM instructions ORDER BY rowid')[:len(instructions)], instructions)
        self.assertCountEqual(self.rows('SELECT disposition FROM amend_dispositions'),
                              [{'disposition': 'update'}, {'disposition': 'keep'}])
        self.assertFalse(second['business_delivered'])
        self.assertEqual(second['real_model_calls'], 0)

    def test_partial_model_plan_then_same_plan_keep_framed(self):
        self.partial_then_keep()

    def test_partial_model_plan_then_same_plan_keep_structured(self):
        self.partial_then_keep(structured=True)

    def test_model_cancel_keeps_original_bytes_and_has_no_cancelled_worker_call(self):
        request = self.admit(); self.initial(request); self.amend(request)
        original = (self.workspaces['beta'] / 'r1.txt').read_bytes()
        result = self.pump(request, workers=1, script=self.json_file('model-cancel.json',
            self.model_plan({'alpha': self.decision('update'), 'beta': self.decision('cancel')})))
        self.assertEqual(self.tasks(request)['beta']['status'], 'CANCELLED', result)
        self.assertFalse(any(call['node_id'] == 'beta' for call in result['calls']), result)
        self.assertEqual((self.workspaces['beta'] / 'r1.txt').read_bytes(), original)
        self.assertFalse((self.workspaces['beta'] / 'r2.txt').exists())
        self.assertEqual(len(result['calls']), 3, result)

    def test_empty_model_decisions_publish_nothing_and_stop_after_one_attempt(self):
        request = self.admit(); self.initial(request); self.amend(request)
        before = self.semantic_tasks(request)
        count = len(self.rows('SELECT * FROM call_attempts'))
        result = self.pump(request, workers=1, max_calls=64,
            script=self.json_file('model-empty.json', self.model_plan({})))
        self.assertEqual(len(result['calls']), 1, result)
        self.assertTrue(any(item['code'] == 'invalid_input' for item in result['blocked']), result)
        self.assertEqual(self.semantic_tasks(request), before)
        self.assertEqual(self.show(request)['request']['plan_revision'], 1)
        self.assertEqual(len(self.rows('SELECT * FROM call_attempts')), count + 1)
        self.assertEqual(self.rows('SELECT * FROM amend_dispositions'), [])

    def test_missing_dispatcher_step_retains_exact_amend_notices_for_later_pump(self):
        request = self.admit(); self.initial(request); self.amend(request)
        before = self.rows("SELECT i.inbox_id FROM event_inbox i JOIN events e ON e.seq=i.event_seq "
                           "WHERE i.request_id=? AND i.role='dispatcher' AND i.status='PENDING' AND e.type='amend'", (request,))
        attempts = self.rows('SELECT * FROM call_attempts')
        result = self.pump(request, script=self.json_file('no-dispatcher.json', {'default': [], 'nodes': {}}))
        self.assertEqual(result['calls'], [], result)
        self.assertEqual(self.rows('SELECT * FROM call_attempts'), attempts)
        remaining = self.rows("SELECT inbox_id FROM event_inbox WHERE request_id=? AND status='PENDING'", (request,))
        self.assertTrue(before)
        self.assertTrue({row['inbox_id'] for row in before} <= {row['inbox_id'] for row in remaining})
        resumed = self.pump(request, workers=1, script=self.json_file('resume.json', self.model_plan({
            'alpha': self.decision('update'), 'beta': self.decision('keep')})))
        self.assertEqual(len(resumed['calls']), 5, resumed)
        self.assertEqual(self.tasks(request)['beta']['based_on_requirements_revision'], 2)

    def test_forged_runtime_revision_is_accounted_and_preserves_all_task_semantics(self):
        request = self.admit(); self.initial(request); self.amend(request)
        before = self.semantic_tasks(request)
        script = self.model_plan({'alpha': self.decision('update')})
        script['default'][0]['tool_calls'][0]['arguments']['plan']['plan_revision'] = 999
        result = self.pump(request, script=self.json_file('forged.json', script), workers=1)
        self.assertEqual(len(result['calls']), 1, result)
        self.assertTrue(any(row['code'] == 'invalid_input' for row in result['blocked']), result)
        self.assertEqual(self.semantic_tasks(request), before)
        attempt = result['calls'][0]['attempt_id']
        receipt = self.rows('SELECT * FROM model_tool_feedback WHERE attempt_id=?', (attempt,))
        self.assertEqual(len(receipt), 1)
        self.assertIsNotNone(self.rows('SELECT settlement_sha256 FROM call_attempts WHERE attempt_id=?', (attempt,))[0]['settlement_sha256'])
        self.assertEqual(self.rows('SELECT * FROM amend_dispositions'), [])

    def test_exhausted_attempt_budget_retains_omitted_amend_subject(self):
        request = self.invoke('submit', '--db', self.database, '--key', 'limited-amend',
            '--text-file', h.FIXTURES / 's13_request.txt',
            '--constraints-file', h.FIXTURES / 's13_constraints.json',
            '--max-attempts', 6)['request']['request_id']
        self.initial(request); self.amend(request)
        first = self.pump(request, workers=1, script=self.json_file('last-attempt.json',
            self.model_plan({'alpha': self.decision('update')})))
        self.assertEqual(len(first['calls']), 1, first)
        task = self.tasks(request)['beta']
        self.assertEqual(task['based_on_requirements_revision'], 1)
        before = self.rows("SELECT i.inbox_id FROM event_inbox i JOIN events e ON e.seq=i.event_seq "
                           "WHERE i.subject_task_id=? AND i.role='dispatcher' AND i.status='PENDING' AND e.type='amend'",
                           (task['task_id'],))
        self.assertTrue(before)
        attempts = self.rows('SELECT * FROM call_attempts')
        second = self.pump(request, workers=1, script=self.json_file('over-budget.json',
            self.resolution({'beta': self.decision('keep')})))
        self.assertEqual(second['calls'], [], second)
        self.assertTrue(any(item['code'] == 'quota' for item in second['blocked']), second)
        self.assertEqual(self.rows('SELECT * FROM call_attempts'), attempts)
        self.assertEqual(self.tasks(request)['beta']['based_on_requirements_revision'], 1)
        remaining = self.rows("SELECT inbox_id FROM event_inbox WHERE subject_task_id=? AND status='PENDING'",
                              (task['task_id'],))
        self.assertTrue({row['inbox_id'] for row in before} <= {row['inbox_id'] for row in remaining})
        self.assertEqual(self.show(request)['request']['plan_revision'], 2)

    def test_model_amend_leaves_other_request_rows_files_and_wait_unchanged(self):
        request = self.admit(); self.initial(request)
        sibling = self.admit('sibling')
        sibling_workspaces = {node: self.directory / ('sibling-' + node) for node in ('alpha', 'beta')}
        for path in sibling_workspaces.values(): path.mkdir(mode=0o700)
        mapping = self.json_file('sibling-map.json', {node: str(path) for node, path in sibling_workspaces.items()})
        self.initial(sibling, mapping)
        task = self.tasks(sibling)['beta']['task_id']
        self.invoke('wait', '--db', self.database, '--request', sibling, '--task', task, '--reason', 'Independent owner wait')
        before = self.request_snapshot(sibling)
        contents = {node: (path / 'r1.txt').read_bytes() for node, path in sibling_workspaces.items()}
        self.amend(request)
        self.assertEqual(self.request_snapshot(sibling), before)
        self.pump(request, workers=1, script=self.json_file('isolated-plan.json',
            self.model_plan({'alpha': self.decision('update')})))
        self.assertEqual(self.request_snapshot(sibling), before)
        self.pump(request, workers=1, script=self.json_file('isolated-keep.json',
            self.resolution({'beta': self.decision('keep')})))
        self.assertEqual(self.request_snapshot(sibling), before)
        self.assertEqual({node: (path / 'r1.txt').read_bytes() for node, path in sibling_workspaces.items()}, contents)

    def test_host_disposition_file_remains_compatible(self):
        request = self.admit(); self.initial(request); self.amend(request)
        first = self.pump(request, workers=1, script=h.FIXTURES / 's13_fake_amended.json',
            dispositions=h.FIXTURES / 's13_update_alpha.json')
        self.assertEqual(len(first['calls']), 3, first)
        self.assertEqual(first['calls'][0]['response']['tool_calls'][0]['name'], 'submit_plan')
        second = self.pump(request, workers=1, script=h.FIXTURES / 's13_fake_keep_beta.json',
            dispositions=h.FIXTURES / 's13_keep_beta.json')
        self.assertEqual(len(second['calls']), 2, second)
        self.assertEqual(self.show(request)['request']['plan_revision'], 2)
        self.assertEqual((self.workspaces['beta'] / 'r2.txt').read_text(), 'beta revision 2\n')


if __name__ == '__main__':
    if os.geteuid() == 0:
        raise SystemExit('Model AMEND CLI regressions require an ordinary user')
    unittest.main(verbosity=2)
