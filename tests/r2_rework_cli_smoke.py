#!/usr/bin/env python3
"""Rejected submission follow-up through the real non-root CLI and SQLite."""
import copy
import importlib.util
import json
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('structured_helpers', Path(__file__).with_name('r1_structured_cli_smoke.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)

class ReworkCli(unittest.TestCase):
    setUp = h.StructuredCli.setUp
    configure = h.StructuredCli.configure
    write = h.StructuredCli.write
    call = h.StructuredCli.call
    admit = h.StructuredCli.admit
    rows = h.StructuredCli.rows
    pump = h.StructuredCli.pump
    envelopes = h.StructuredCli.envelopes

    def sequence(self, action='rework'):
        script = json.loads((h.FIXTURES / 's15_fake_complete.json').read_text())
        accept = copy.deepcopy(script['default'][1])
        reject = script['default'][1]
        reject['tool_calls'][0]['arguments'] = {'passed': False, 'notes': 'The required output needs a correction.'}
        resolve = copy.deepcopy(accept)
        resolve['role_call_index'] = 3
        resolve['tool_calls'] = [{'call_id': 'resolve-original', 'name': 'resolve_blocked_submission',
                                 'arguments': {'action': action, 'reason': 'Correct the saved candidate without weakening its criteria.'}}]
        script['default'].append(resolve)
        steps = script['nodes']['alpha']
        order, write, submit, check, experience = map(copy.deepcopy, steps)
        if action == 'rework':
            order['role_call_index'] = 3
            check['role_call_index'] = 4
            experience['role_call_index'] = 5
            write['role_call_index'] = 3
            write['tool_calls'][0]['arguments']['path'] = 'repaired.txt'
            submit['role_call_index'] = 4
            submit['tool_calls'][0]['arguments']['deliverables'][0]['path'] = '/work/repaired.txt'
            script['nodes']['alpha'] = steps[:4] + [order, write, submit, check, experience]
            accept['role_call_index'] = 4
            script['default'].append(accept)
        else:
            script['nodes']['alpha'] = steps[:4]
        return script

    def test_rejection_rework_new_order_ack_and_acceptance(self):
        request = self.admit('s13')
        result = self.pump(request, self.sequence(), 4)
        self.assertEqual(result['tasks'][0]['status'], 'ACCEPTED', result)
        submissions = self.rows('SELECT * FROM workflow_submissions ORDER BY rowid')
        self.assertEqual([r['status'] for r in submissions], ['RETURNED_FOR_REWORK', 'CANDIDATE_READY'])
        self.assertEqual([r['status'] for r in self.rows('SELECT status FROM candidate_acceptances ORDER BY rowid')], ['REJECTED', 'ACCEPTED'])
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state')[0]['rework_count'], 1)
        task = result['tasks'][0]
        self.assertEqual(task['task_revision'], 2)
        self.assertEqual(task['instruction_revision'], 2)
        self.assertEqual(task['applied_task_revision'], 2)
        self.assertEqual(task['applied_instruction_revision'], 2)
        self.assertEqual(len(self.rows("SELECT * FROM events WHERE type='resolve_blocked_submission'")), 1)

    def test_exhaustion_creates_one_user_question_across_reopen(self):
        self.config.write_text(self.config.read_text().replace('team_internal_rework_max: 3', 'team_internal_rework_max: 0'))
        request = self.admit('s13')
        result = self.pump(request, self.sequence('ask_user'), 2)
        self.assertEqual(len(result['questions']), 1, result)
        question = result['questions'][0]
        self.assertEqual(question['holder'], 'user')
        self.assertEqual(question['asked_by'], 'dispatcher')
        self.assertEqual(result['tasks'][0]['status'], 'BLOCKED')
        again = self.pump(request, self.sequence('ask_user'), 2)
        self.assertEqual(len(again['calls']), 0, again)
        self.assertEqual(len(again['questions']), 1)
        self.call('wake', '--db', self.db, '--request', request, '--task', result['tasks'][0]['task_id'], '--wait-generation', question['wait_generation'], error='not_ready')

    def answer_question(self, request, question):
        return self.call('answer', '--db', self.db, '--request', request,
            '--question', question['question_id'], '--expected-version', question['state_version'],
            '--text-file', h.FIXTURES / 's14_answer.txt')

    def continuation(self):
        original = self.sequence()
        # Every explicit pump restarts FakeModel indices, never the durable budget.
        resolve, accept = copy.deepcopy(original['default'][2:4])
        resolve['role_call_index'], accept['role_call_index'] = 1, 2
        order, write, submit, check, experience = copy.deepcopy(original['nodes']['alpha'][4:])
        order['role_call_index'], check['role_call_index'], experience['role_call_index'] = 1, 2, 3
        write['role_call_index'], submit['role_call_index'] = 1, 2
        return {'default': [resolve, accept], 'nodes': {'alpha': [order, write, submit, check, experience]}}

    def test_user_answer_does_not_grant_exhausted_quota(self):
        self.config.write_text(self.config.read_text().replace('team_internal_rework_max: 3', 'team_internal_rework_max: 0'))
        request = self.admit()
        result = self.pump(request, self.sequence('ask_user'))
        question = result['questions'][0]
        self.answer_question(request, question)
        result = self.pump(request, self.continuation())
        self.assertEqual(result['tasks'][0]['status'], 'BLOCKED')
        self.assertEqual(result['questions'][0]['status'], 'ANSWERED')
        self.assertEqual(len(result['calls']), 0, result)
        self.assertTrue(any(p['type'] == 'answer_question' for p in result['pending']), result)
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state')[0]['rework_count'], 0)
        self.assertEqual(len(self.rows('SELECT * FROM questions')), 1)

    def test_answered_notice_survives_missing_script_then_reworks(self):
        request = self.admit()
        original = self.pump(request, self.sequence('ask_user'))
        self.answer_question(request, original['questions'][0])
        waiting = self.pump(request, {'default': [], 'nodes': {}})
        self.assertEqual(len(waiting['calls']), 0)
        self.assertTrue(any(p['type'] == 'answer_question' for p in waiting['pending']), waiting)
        completed = self.pump(request, self.continuation())
        self.assertEqual(completed['tasks'][0]['status'], 'ACCEPTED', completed)
        self.assertEqual(len(completed['questions']), 1)
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state')[0]['rework_count'], 1)
        calls = [c for c in completed['calls'] if c['role'] == 'dispatcher']
        self.assertEqual(calls[0]['workflow_action']['followup_question']['question_id'], original['questions'][0]['question_id'])
        self.assertFalse(any(p['type'] == 'answer_question' for p in completed['pending']))

    def test_missing_followup_script_preserves_rejected_notice(self):
        request = self.admit()
        script = self.sequence(); script['default'] = script['default'][:2]
        first = self.pump(request, script)
        self.assertEqual(first['tasks'][0]['status'], 'BLOCKED')
        self.assertTrue(any(p['type'] == 'accept_candidate' for p in first['pending']), first)
        second = self.pump(request, self.continuation())
        self.assertEqual(second['tasks'][0]['status'], 'ACCEPTED', second)
        self.assertEqual(len(self.rows("SELECT * FROM events WHERE type='resolve_blocked_submission'")), 1)

    def test_forged_target_is_recorded_but_not_applied(self):
        request = self.admit()
        script = self.sequence()
        script['default'][2]['tool_calls'][0]['arguments']['submission_id'] = 'forged'
        result = self.pump(request, script)
        self.assertEqual(result['tasks'][0]['status'], 'BLOCKED')
        self.assertEqual(len(self.rows("SELECT * FROM events WHERE type='resolve_blocked_submission'")), 0)
        self.assertTrue(any(b['code'] == 'invalid_input' for b in result['blocked']), result)
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM call_attempts')[0]['n'], 7)
        self.assertEqual(self.rows('SELECT COUNT(*) AS n FROM model_tool_feedback')[0]['n'], 7)

    def test_framed_legacy_request_keeps_explicit_rework_path(self):
        self.configure(None)
        request = self.admit()
        result = self.pump(request, self.sequence(), 4)
        self.assertEqual(result['tasks'][0]['status'], 'ACCEPTED', result)
        self.assertEqual(result['real_model_calls'], 0)

    def test_team_lead_and_dispatcher_share_the_same_frozen_count(self):
        self.config.write_text(self.config.read_text().replace('team_internal_rework_max: 3', 'team_internal_rework_max: 1'))
        request = self.admit()
        script = self.sequence()
        steps = script['nodes']['alpha']
        # Local negative check consumes the only rework, producing its own order.
        negative = copy.deepcopy(steps[3])
        negative['tool_calls'][0]['arguments'] = {'passed': False, 'notes': 'Local repair needed.',
            'work_order': copy.deepcopy(steps[0]['tool_calls'][0]['arguments'])}
        positive = copy.deepcopy(steps[7]); positive['role_call_index'] = 3
        script['nodes']['alpha'] = steps[:3] + [negative, steps[5], steps[6], positive]
        script['default'] = script['default'][:3]
        script['default'][2]['tool_calls'][0]['arguments']['action'] = 'ask_user'
        result = self.pump(request, script, 4)
        self.assertEqual(result['tasks'][0]['status'], 'BLOCKED', result)
        self.assertEqual(len(result['questions']), 1)
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state')[0]['rework_count'], 1)
        self.assertEqual(len(self.rows('SELECT * FROM workflow_submissions')), 2)
        target = next(c['workflow_action'] for c in result['calls'] if c['role'] == 'dispatcher' and c['role_call_index'] == 3)
        self.assertFalse(target['rework_available'])
        self.assertEqual(target['rework_limit'], 1)

    def test_waiting_rejected_task_does_not_block_sibling_acceptance(self):
        self.config.write_text(self.config.read_text().replace('team_internal_rework_max: 3', 'team_internal_rework_max: 0'))
        beta = self.root / 'beta'; beta.mkdir(mode=0o700)
        self.mapping = self.write('workspaces.json', {'alpha': str(self.work), 'beta': str(beta)})
        request = self.admit()
        script = self.sequence('ask_user')
        normal = json.loads((h.FIXTURES / 's15_fake_complete.json').read_text())
        node = copy.deepcopy(script['default'][0]['tool_calls'][0]['arguments']['nodes'][0])
        node['node_id'] = 'beta'; node['title'] = 'Independent beta'
        script['default'][0]['tool_calls'][0]['arguments']['nodes'].append(node)
        accept = copy.deepcopy(normal['default'][1]); accept['role_call_index'] = 4
        script['default'].append(accept)
        script['nodes']['beta'] = normal['nodes']['alpha']
        result = self.pump(request, script)
        tasks = {t['node_id']: t for t in result['tasks']}
        self.assertEqual(tasks['alpha']['status'], 'BLOCKED', result)
        self.assertEqual(tasks['beta']['status'], 'ACCEPTED', result)
        self.assertEqual(len(result['questions']), 1)
        self.assertEqual(result['questions'][0]['task_id'], tasks['alpha']['task_id'])
        self.assertEqual(result['real_model_calls'], 0)

if __name__ == '__main__':
    unittest.main(verbosity=2)
