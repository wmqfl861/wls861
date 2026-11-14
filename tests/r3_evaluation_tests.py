#!/usr/bin/env python3
"""Evaluation framework tests against disposable real CLI/SQLite evidence."""
from contextlib import closing
import argparse
import base64
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import unittest

parser = argparse.ArgumentParser(description=__doc__, allow_abbrev=False)
parser.add_argument('wls_binary')
parser.add_argument('source_root')
parser.add_argument('--group', choices=('all', 'legacy', 'logical', 'plain'), default='all')
OPTIONS = parser.parse_args()
sys.argv = [sys.argv[0], OPTIONS.wls_binary, OPTIONS.source_root]

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
    rows = h.AcceptanceCliSmoke.rows
    snapshot = h.AcceptanceCliSmoke.snapshot

    def seed(self, task=0, key='seed'):
        material = self.directory / (key + '.txt')
        material.write_text(e.TASKS[task][1])
        return self.invoke('submit', '--db', self.database, '--key', key,
                           '--text-file', material)['request']['request_id']

    def plan(self, request, *, model_origin=False):
        with e.database(self.database) as db:
            return e.make_plan(db, request, decision_evidence='model-origin-v1' if model_origin else None)

    def run_entry(self, plan, request, condition='multi_agent', task=0):
        return {'task_id': e.TASKS[task][0], 'condition': condition,
                'database': str(self.database), 'request_id': request,
                'elapsed_ms': 1000, 'plan_sha256': plan['plan_sha256']}

    def execute(self, request, unknown=False, error=False, max_calls=3, worker_calls=1, script=None):
        fixture = copy.deepcopy(script) if script is not None else self.fixture()
        snapshot = json.loads(self.rows('SELECT config_snapshot FROM requests WHERE request_id=?', (request,))[0]['config_snapshot'])
        v3 = snapshot['system']['context'].get('request_format') == 'structured-v3'
        def original_constraints(value):
            if isinstance(value, dict):
                if v3 and isinstance(value.get('brief'), dict) and 'title' in value and 'title' in value['brief']:
                    value['brief']['title'] = value['title']  # Only the new v3 script copy is changed.
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
        return self.invoke(getattr(self, 'execution_command', 'pump-fake'), '--db', self.database, '--request', request,
            '--script', self.json_file('script.json', fixture), '--workspaces-file', self.mapping,
            '--worker-calls', worker_calls, '--max-calls', max_calls)

    def logical_seed(self, task=4, *, logical=True, key='logical-seed', context_change=None, request_format='structured-v2', plain=False):
        system = (h.SOURCE / 'config/examples/system.yaml').read_text()
        self.assertIn('request_format: structured-v3', system)
        system = system.replace('  tool_policy: fixed-role-v1\n', '').replace('  model_projection: lean-v1\n', '').replace('  worker_history: {mode: append-fold-v1, read_max_bytes: 24576, read_token_budget: 196608}\n', '').replace('request_format: structured-v3', f'request_format: {request_format}')
        if logical:
            mode = 'single-agent-plain-v1' if plain else 'single-agent-role-broker-v1'
            system = system.replace('context:\n', 'context:\n  logical_agent:\n'
                                    f'    mode: {mode}\n'
                                    '    max_attempts: 128\n    max_history_bytes: 524288\n', 1)
        if context_change:
            before, after = context_change
            self.assertIn(before, system)
            system = system.replace(before, after, 1)
        config = self.directory / (key + '-system.yaml')
        config.write_text(system)
        material = self.directory / (key + '-material.txt')
        material.write_text(e.TASKS[task][1])
        self.execution_command = 'pump-single-fake' if logical else 'pump-fake'
        return self.invoke('submit', '--db', self.database, '--key', key, '--text-file', material,
                           '--config', config, '--functions-dir',
                           h.SOURCE / 'config/examples/functions')['request']['request_id']

    def logical_plan(self, request):
        with e.database(self.database) as db:
            return e.make_plan(db, request, identity_contract=e.LOGICAL_AGENT_CONTRACT)

    def plain_run(self, task=0, *, key='plain', change=None, calls=4):
        request = self.logical_seed(task=task, key=key, plain=True, request_format='structured-v3')
        with e.database(self.database) as db:
            plan = e.make_plan(db, request, include_plain=True)
        self.workspace = self.directory / (key + '-workspace')
        self.workspace.mkdir(mode=0o700)
        self.mapping = self.json_file(key + '-workspaces.json', {'plain': str(self.workspace)})
        (self.workspace / 'seed.txt').write_bytes(b'alpha\nbeta\n')
        worker = [self.step('worker', 'fs_read', {'path': 'seed.txt'}, 1),
                  self.step('worker', 'fs_write', {'path': 'result.txt', 'content': 'alpha\nbeta\n'}, 2),
                  self.step('worker', 'fs_read', {'path': 'result.txt'}, 3),
                  self.fixture('s14_fake_submit.json')['nodes']['alpha'][2]]
        worker[-1]['role_call_index'] = 4
        if change:
            change(worker)
        result = self.execute(request, script={'default': [], 'nodes': {'plain': worker}},
                              max_calls=calls, worker_calls=4)
        return request, plan, result

    def tamper_last_logical_input(self, edit):
        # Disposable fixture only. Rehash the modified logical envelope so
        # the v3 relation itself, rather than a stale checksum, must reject it.
        with closing(sqlite3.connect(self.database)) as db, db:
            attempt, raw = db.execute('SELECT attempt_id,request_envelope FROM call_attempts ORDER BY rowid DESC LIMIT 1').fetchone()
            envelope = json.loads(raw)
            edit(envelope['context']['input'])
            envelope['context']['logical_input_sha256'] = e.digest(envelope['context']['input'])
            envelope['logical_request_sha256'] = e.digest({key: value for key, value in envelope.items()
                                                           if key not in ('logical_request_sha256', 'wire_request_sha256')})
            db.execute('UPDATE call_attempts SET request_envelope=?,state_version=state_version+1 WHERE attempt_id=?',
                       (e.canonical(envelope).decode(), attempt))
            self.assertEqual(json.loads(db.execute('SELECT request_envelope FROM call_attempts WHERE attempt_id=?',
                                                  (attempt,)).fetchone()[0]), envelope)

    def test_original_request_hash_preserves_cpp_numbers_and_nested_fields(self):
        # Generated by the C++ structured compiler and codec, then round-tripped
        # through model_request_from_json. No provider or Store send occurred.
        raw = (h.FIXTURES / 'r3_cpp_logical_request.json').read_text()
        envelope = e.strict_json(raw)
        original_hash = envelope['logical_request_sha256']
        self.assertEqual(original_hash, '23fbd65735a2b5552805e0e400ed251930a07340ccad96b95d601a85ffd4e092')
        reencoded = e.digest({key: value for key, value in envelope.items()
                              if key not in ('logical_request_sha256', 'wire_request_sha256')})
        self.assertEqual(reencoded, '613d0a2565a3970774a86df0cc723e5ab7454163610044e10492b74774ba051e')
        self.assertEqual(e.original_logical_request_sha256(raw, envelope), original_hash)
        self.assertEqual(e.original_canonical(copy.deepcopy(envelope)), raw.encode())
        # Historical plan/report fingerprints retain Python JSON spelling.
        self.assertEqual(e.canonical(e.strict_json('{"ratio":6E-1}')), b'{"ratio":0.6}')
        for spelling in ('NaN', 'Infinity', '-Infinity', '1e1000', '-1e1000'):
            with self.subTest(nonfinite=spelling), self.assertRaises(e.InvalidEvidence):
                e.strict_json('{"ratio":' + spelling + '}')
        # A nested field with the same name and an escaped member-looking
        # string remain part of the actual input, as do numeric value changes.
        for before, after in (('nested original field remains part of the input', 'changed nested input'),
                              ('Literal text:', 'Changed literal:'), ('"ratio":1E-1', '"ratio":2E-1')):
            with self.subTest(original=before):
                self.assertIn(before, raw)
                changed = raw.replace(before, after)
                self.assertNotEqual(e.original_logical_request_sha256(changed, e.strict_json(changed)), original_hash)

    def test_v3_explicit_plan_keeps_v1_v2_meanings(self):
        request = self.logical_seed()
        with e.database(self.database) as db:
            self.assertEqual(e.make_plan(db, request)['schema_version'], 1)
            self.assertEqual(e.make_plan(db, request, decision_evidence='model-origin-v1')['schema_version'], 2)
            plan = e.make_plan(db, request, identity_contract=e.LOGICAL_AGENT_CONTRACT)
            self.assertEqual(plan['schema_version'], 3)
            self.assertEqual(plan['decision_evidence'], 'model-origin-v1')
            self.assertEqual(plan['single_agent_policy'], e.DEFAULT_LOGICAL_AGENT_POLICY)
            e.validate_plan(plan)
            with self.assertRaises(e.InvalidEvidence):
                e.make_plan(db, request, decision_evidence='structural-v1', identity_contract=e.LOGICAL_AGENT_CONTRACT)
        report = e.build_report(plan, [])
        self.assertEqual(report['counts'], {'not_executed': 10})
        self.assertFalse(report['provider_quality_acceptance_established'])

    def test_v4_explicit_plan_keeps_legacy_contracts_and_fifteen_fixed_cells(self):
        requests = [self.logical_seed(task=0, logical=logical, plain=plain, key=key, request_format='structured-v3')
                    for logical, plain, key in ((False, False, 'multi'), (True, False, 'broker'), (True, True, 'plain'))]
        with e.database(self.database) as db:
            locks = [e.common_lock(db, request, 'fake', {'mode': 'provider_default'}, logical_agent_comparison=True)
                     for request in requests]
            self.assertEqual(locks, [locks[0]] * 3)
            for version, options in enumerate(({}, {'decision_evidence': 'model-origin-v1'},
                    {'identity_contract': e.LOGICAL_AGENT_CONTRACT}), 1):
                legacy = e.make_plan(db, requests[1], **options)
                self.assertEqual(legacy, e.make_plan(db, requests[1], include_plain=False, **options))
                self.assertEqual(legacy['schema_version'], version)
                self.assertEqual(e.build_report(legacy, [])['counts'], {'not_executed': 10})
                self.assertNotIn('plain_agent_policy', legacy)
            plan = e.make_plan(db, requests[1], include_plain=True)
            self.assertEqual(plan, e.make_plan(db, requests[2], include_plain=True))
            with self.assertRaises(e.InvalidEvidence):
                e.make_plan(db, requests[1], include_plain=True, decision_evidence='structural-v1')
        self.assertEqual((plan['schema_version'], plan['benchmark_id']), (4, 'r3-small-tasks-v4'))
        self.assertEqual(plan['conditions'], ['multi_agent', 'single_agent', 'single_agent_plain'])
        self.assertEqual(plan['tasks'], legacy['tasks'])
        self.assertEqual(plan['plain_agent_policy'], dict(plan['single_agent_policy'], mode='single-agent-plain-v1'))
        result = subprocess.run([sys.executable, str(h.SOURCE / 'scripts/evaluate_r3.py'), 'plan',
            '--db', str(self.database), '--request', requests[1], '--include-plain'],
            cwd=self.directory, capture_output=True, text=True, timeout=35, check=False)
        self.assertEqual((result.returncode, result.stderr), (0, ''), result.stdout)
        self.assertEqual(json.loads(result.stdout), plan)
        report = e.build_report(plan, [])
        self.assertEqual(report['counts'], {'not_executed': 15})
        self.assertEqual([(row['task_id'], row['condition']) for row in report['rows']],
                         [(task[0], condition) for task in e.TASKS for condition in plan['conditions']])
        self.assertEqual(report['expected_cells'], 15)
        self.assertFalse(report['paired_cells_complete'])
        self.assertFalse(report['quality_comparison_complete'])
        for mutation in ('condition', 'task', 'rubric', 'capacity'):
            with self.subTest(mutation=mutation):
                changed = copy.deepcopy(plan)
                if mutation == 'condition':
                    changed['conditions'].pop()
                elif mutation == 'task':
                    changed['tasks'].pop()
                elif mutation == 'rubric':
                    changed['tasks'][0]['checks'] = {}
                else:
                    changed['plain_agent_policy']['max_attempts'] -= 1
                changed['plan_sha256'] = e.digest({key: value for key, value in changed.items() if key != 'plan_sha256'})
                with self.assertRaises(e.InvalidEvidence):
                    e.build_report(changed, [])
        run = self.run_entry(plan, requests[2], 'single_agent_plain')
        for entries in ([run, run], [run, dict(run, condition='single_agent')]):
            with self.assertRaises(e.InvalidEvidence):
                e.build_report(plan, entries)

    def test_v4_chat_adapter_cannot_be_relabelled_as_supported_evidence(self):
        request = self.logical_seed()
        # A forged adapter label is a rejection fixture, never a Chat run.
        with closing(sqlite3.connect(self.database)) as db, db:
            snapshot = json.loads(db.execute('SELECT config_snapshot FROM requests WHERE request_id=?', (request,)).fetchone()[0])
            snapshot['system']['models']['fake']['adapter'] = 'openai-chat-completions'
            encoded = e.canonical(snapshot).decode()
            db.execute('UPDATE requests SET config_snapshot=?,state_version=state_version+1 WHERE request_id=?', (encoded, request))
            db.execute('DROP TRIGGER snapshots_immutable')
            db.execute('UPDATE request_runtime SET config_snapshot_sha256=? WHERE request_id=?',
                       (hashlib.sha256(encoded.encode()).hexdigest(), request))
        with e.database(self.database) as db, self.assertRaisesRegex(e.InvalidEvidence, 'only Fake and Responses'):
            e.make_plan(db, request, include_plain=True)

    def test_v4_plain_original_history_and_submission_are_observed_without_workflow_acceptance(self):
        request, plan, pump = self.plain_run()
        self.assertEqual(pump['manager_calls'], 0)
        self.assertTrue(pump['model_loop_complete'], pump)
        self.assertTrue(pump['business_acceptance_pending'])
        self.assertFalse(pump['business_delivered'])
        attempts = self.rows('SELECT * FROM call_attempts ORDER BY rowid')
        self.assertEqual([attempt['role'] for attempt in attempts], ['worker'] * 4)
        self.assertEqual(len({(item['principal_id'], item['team_id'], item['task_id']) for item in attempts}), 1)
        last = json.loads(attempts[-1]['request_envelope'])
        contract = e.captured_contract(e.strict_json(attempts[-1]['request_envelope']))
        self.assertEqual(contract['request']['texts'][0]['text'], e.TASKS[0][1])
        self.assertEqual(contract['logical_agent_execution']['history_count'], 3)
        messages = last['context']['input']['messages']
        self.assertEqual(len(messages), 6)
        for original, assistant, tool in zip(attempts[:-1], messages[::2], messages[1::2]):
            response = json.loads(original['response'])['tool_calls'][0]
            replay = assistant['tool_calls'][0]
            self.assertEqual((replay['name'], replay['arguments']), (response['name'], response['arguments']))
            self.assertEqual(tool['tool_call_id'], replay['call_id'])
            self.assertEqual(assistant['source']['attempt_id'], original['attempt_id'])
            feedback = self.rows('SELECT result FROM model_tool_feedback WHERE attempt_id=?', (original['attempt_id'],))[0]
            self.assertEqual(json.loads(tool['content']), json.loads(feedback['result']))
        submission = self.rows('SELECT * FROM workflow_submissions')[0]
        self.assertEqual(submission['status'], 'SUBMITTED')
        self.assertEqual(base64.b64decode(json.loads(submission['artifact_snapshot'])[0]['content_base64']), b'alpha\nbeta\n')
        before = self.snapshot()
        report = e.build_report(plan, [self.run_entry(plan, request, 'single_agent_plain')])
        row = report['rows'][2]
        self.assertEqual(row['status'], 'observed', row)
        self.assertTrue(all(row['safety_checks'].values()), row)
        self.assertTrue(row['workflow_checks']['model_tool_roundtrip'])
        self.assertFalse(row['workflow_checks']['artifact_sha256'])
        self.assertFalse(row['workflow_checks']['model_submission_source'])
        self.assertEqual(row['workflow_completion'], 'incomplete')
        self.assertEqual(row['plain_loop_completion'], 'complete')
        self.assertEqual(row['execution_outcome'], 'complete')
        self.assertTrue(row['business_acceptance_pending'])
        self.assertEqual(row['quality_assessment'], 'not_assessed')
        self.assertEqual(row['roles'], {'worker': 4})
        self.assertEqual(row['identity_evidence']['contract'], 'single-agent-plain-v1')
        self.assertEqual(row['identity_evidence']['global_steps'], 4)
        self.assertEqual(report['counts'], {'not_executed': 14, 'observed': 1})
        self.assertFalse(report['quality_comparison_complete'])
        for table in ('team_lead_checks', 'candidate_acceptances', 'experience_jobs', 'delivery_receipts'):
            self.assertEqual(self.rows(f'SELECT * FROM {table}'), [])
        (self.workspace / 'result.txt').write_text('Later workspace bytes must not replace the submitted original.')
        self.assertEqual(e.build_report(plan, [self.run_entry(plan, request, 'single_agent_plain')]), report)
        self.assertEqual(self.snapshot(), before)

    def test_v4_rehashed_history_role_cost_and_submission_corruption_refuse(self):
        request, plan, _ = self.plain_run()
        original_database = self.database
        for mutation in ('history', 'marker', 'role', 'cost', 'artifact', 'feedback'):
            with self.subTest(mutation=mutation):
                self.database = self.directory / (mutation + '.sqlite')
                with closing(sqlite3.connect(original_database.as_uri() + '?mode=ro', uri=True)) as original, \
                        closing(sqlite3.connect(self.database)) as changed:
                    original.backup(changed)
                if mutation in ('history', 'marker'):
                    def corrupt(logical):
                        if mutation == 'history':
                            logical['messages'] = logical['messages'][2:]
                        else:
                            contract = json.loads(logical['task_contract'])
                            contract['logical_agent_execution']['global_step'] = 1
                            logical['task_contract'] = e.canonical(contract).decode()
                            logical['current_state']['logical_agent_execution'] = contract['logical_agent_execution']
                    self.tamper_last_logical_input(corrupt)
                else:
                    with closing(sqlite3.connect(self.database)) as db, db:
                        if mutation == 'role':
                            db.execute("UPDATE call_attempts SET role='team_lead',state_version=state_version+1 WHERE rowid=1")
                        elif mutation == 'cost':
                            db.execute('UPDATE request_budgets SET spent_cost=spent_cost+1,state_version=state_version+1')
                        elif mutation == 'feedback':
                            db.execute('DELETE FROM model_tool_feedback WHERE rowid=(SELECT max(rowid) FROM model_tool_feedback)')
                        else:
                            artifact = json.loads(db.execute('SELECT artifact_snapshot FROM workflow_submissions').fetchone()[0])
                            artifact[0]['content_base64'] = base64.b64encode(b'forged bytes').decode()
                            db.execute('DROP TRIGGER workflow_submission_immutable')
                            db.execute('UPDATE workflow_submissions SET artifact_snapshot=?,state_version=state_version+1',
                                       (json.dumps(artifact),))
                report = e.build_report(plan, [self.run_entry(plan, request, 'single_agent_plain')])
                self.assertEqual(report['rows'][2]['status'], 'invalid_evidence', report['rows'][2])
                self.assertEqual(report['counts'], {'not_executed': 14, 'invalid_evidence': 1})
                self.assertEqual(report['expected_cells'], 15)
        self.database = original_database

    def test_v4_failed_cancelled_unknown_and_unexecuted_cells_keep_the_denominator(self):
        entries, common_plan = [], None
        for task, outcome in enumerate(('failed', 'cancelled', 'unknown')):
            def change(worker):
                if outcome == 'failed':
                    worker[0].update(status='error', tool_calls=[], error_code='synthetic_failure', error_message='synthetic only')
                if outcome == 'unknown':
                    worker[0]['usage']['cost_micro_usd'] = None
            request, plan, _ = self.plain_run(task, key=outcome, change=change, calls=1)
            if common_plan is not None:
                self.assertEqual(plan, common_plan)
            common_plan = plan
            if outcome == 'cancelled':
                self.invoke('cancel', '--db', self.database, '--request', request)
            entries.append(self.run_entry(plan, request, 'single_agent_plain', task=task))
        report = e.build_report(common_plan, entries)
        self.assertEqual(report['counts'], {'not_executed': 12, 'observed': 2, 'failed': 1})
        self.assertEqual((report['expected_cells'], len(report['rows']), report['observed_cells']), (15, 15, 3))
        self.assertEqual(report['rows'][2]['attempts'][0]['status'], 'ERROR')
        self.assertEqual(report['rows'][2]['execution_outcome'], 'failed')
        self.assertEqual(report['rows'][5]['request_status'], 'CANCELLED')
        self.assertEqual(report['rows'][5]['execution_outcome'], 'cancelled')
        self.assertEqual(report['rows'][8]['usage']['cost_micro_usd'], {'known_total': 0, 'unknown_attempts': 1})
        self.assertFalse(report['rows'][8]['safety_checks']['known_usage'])
        for index in (2, 5, 8):
            self.assertEqual(report['rows'][index]['workflow_completion'], 'incomplete')
            self.assertEqual(report['rows'][index]['plain_loop_completion'], 'incomplete')
            self.assertFalse(report['rows'][index]['business_acceptance_pending'])
        self.assertFalse(report['paired_cells_complete'])
        self.assertFalse(report['quality_comparison_complete'])

    def test_v3_one_logical_agent_reaches_original_confirmed_delivery(self):
        request = self.logical_seed(request_format='structured-v3')
        plan = self.logical_plan(request)
        legacy_plan = self.plan(request, model_origin=True)
        minimal = self.fixture()
        for node in minimal['default'][0]['tool_calls'][0]['arguments']['nodes']:
            for key in ('kind', 'depends_on', 'required'):
                node.pop(key)
            for key in ('title', 'constraint_refs', 'constraints_verbatim'):
                node['brief'].pop(key)
            for criterion in node['brief']['acceptance_criteria']:
                criterion.pop('id')
        order = minimal['nodes']['alpha'][0]['tool_calls'][0]['arguments']
        for key in ('constraint_refs', 'constraints_verbatim'):
            order.pop(key)
        for step in order['steps']:
            step.pop('id')
        minimal['nodes']['alpha'][2]['tool_calls'][0]['arguments'].pop('used_memories')
        write = next(step['tool_calls'][0] for step in minimal['nodes']['alpha']
                     if step['tool_calls'][0]['name'] == 'fs_write')
        write['arguments']['path'] = '/work/result.txt'
        self.execute(request, max_calls=12, worker_calls=2, script=minimal)
        intent = self.rows('SELECT arguments FROM operation_intents JOIN ops USING(op_id) WHERE request_id=?', (request,))
        self.assertEqual(json.loads(intent[0]['arguments'])['path'], 'result.txt')
        original = self.rows("SELECT response FROM call_attempts WHERE request_id=? AND role='worker' ORDER BY rowid", (request,))
        self.assertEqual(json.loads(original[0]['response'])['tool_calls'][0]['arguments']['path'], '/work/result.txt')
        self.ack(request, self.publish(request))
        row = e.build_report(plan, [self.run_entry(plan, request, 'single_agent', task=4)])['rows'][9]
        self.assertEqual(row['status'], 'passed', row)
        self.assertGreater(row['agent_identity_count'], 1)
        self.assertEqual(row['identity_evidence']['logical_agent_count'], 1)
        self.assertTrue(row['identity_evidence']['shared_request_history'])
        self.assertTrue(row['checks']['model_accepted_delivery_source'])
        self.assertEqual(row['identity_evidence']['global_steps'], len(row['attempts']))
        legacy = e.build_report(legacy_plan, [self.run_entry(legacy_plan, request, 'single_agent', task=4)])['rows'][9]
        self.assertEqual(legacy['status'], 'failed', legacy)
        self.assertFalse(legacy['checks']['condition_roles'])
        original = json.loads(self.rows("SELECT response FROM call_attempts WHERE role='dispatcher' ORDER BY rowid LIMIT 1")[0]['response'])
        self.assertNotIn('constraint_refs', original['tool_calls'][0]['arguments']['nodes'][0]['brief'])
        applications = []
        with e.database(self.database) as db:
            applications, _ = e.model_applications(db, request, e.rows(db, 'SELECT * FROM call_attempts WHERE request_id=? ORDER BY rowid', (request,)))
        submission = next(app for app in applications if app['tool_name'] == 'submit')
        self.assertNotIn('used_memories', submission['arguments'])
        self.assertEqual(submission['expected_arguments']['used_memories'], [])
        # A rehashed application cannot invent a host constraint absent from the captured catalog.
        with closing(sqlite3.connect(self.database)) as db, db:
            event_id, raw = db.execute("SELECT event_id,payload FROM events WHERE type='independent_plan'").fetchone()
            payload = e.strict_json(raw)
            payload['body']['nodes'][0]['brief']['constraints_verbatim'] = ['Forged host constraint']
            db.execute('DROP TRIGGER events_immutable')
            db.execute('UPDATE events SET payload=?,command_sha256=? WHERE event_id=?',
                       (e.original_canonical(payload).decode(), e.original_digest(payload), event_id))
        with e.database(self.database) as db, self.assertRaisesRegex(e.InvalidEvidence, 'applied plan differs'):
            e.model_applications(db, request, e.rows(db, 'SELECT * FROM call_attempts WHERE request_id=? ORDER BY rowid', (request,)))

    def test_submission_completion_requires_the_captured_new_v3_declaration(self):
        properties = dict.fromkeys(('summary', 'deliverables', 'verification', 'self_check', 'known_issues'), {})
        envelope = {'context': {'format': 'wls-compiled-context-v3', 'input': {'prefix': {'tools': [
            {'name': 'submit', 'parameters': {'properties': properties}}]}}}}
        arguments = {'summary': 'original'}
        self.assertEqual(e.v3_host_arguments(envelope, {}, 'submit', arguments), dict(arguments, used_memories=[]))
        self.assertNotIn('used_memories', arguments)
        self.assertEqual(e.v3_host_arguments(envelope, {}, 'submit', dict(arguments, used_memories=[])), dict(arguments, used_memories=[]))
        with self.assertRaisesRegex(e.InvalidEvidence, 'host field differs'):
            e.v3_host_arguments(envelope, {}, 'submit', dict(arguments, used_memories=[{'memory_id': 'invented'}]))
        for format_name, old_declaration in (('wls-compiled-context-v1', False), ('wls-compiled-context-v2', False), ('wls-compiled-context-v3', True)):
            envelope['context']['format'] = format_name
            if old_declaration:
                properties['used_memories'] = {}
            self.assertEqual(e.v3_host_arguments(envelope, {}, 'submit', arguments), arguments)
            explicit = dict(arguments, used_memories=[{'memory_id': 'original'}])
            self.assertEqual(e.v3_host_arguments(envelope, {}, 'submit', explicit), explicit)

    def test_v3_unapplied_fractional_response_keeps_complete_original_history(self):
        request = self.logical_seed(context_change=('model_calls:\n', 'model_calls:\n  runtime_policy: bounded-v1\n'))
        plan = self.logical_plan(request)
        with e.database(self.database) as db:
            legacy_plans = [e.make_plan(db, request, decision_evidence=decision)
                            for decision in (None, 'model-origin-v1')]
        rejected = self.fixture()
        rejected['default'][0]['tool_calls'][0]['arguments']['unexpected_fraction'] = 0.1
        self.execute(request, script=rejected, max_calls=1)
        feedback = self.rows('SELECT response,result FROM model_tool_feedback')[0]
        self.assertEqual(json.loads(feedback['response'])['tool_calls'][0]['arguments']['unexpected_fraction'], 0.1)
        self.assertEqual(json.loads(feedback['result'])['status'], 'FAILED')
        self.assertEqual(json.loads(feedback['result'])['correction_feedback']['reason'], 'model_arguments')
        extension = json.loads(feedback['result'])['correction_feedback']['bounded_runtime_v1']
        self.assertEqual(extension['same_error_repeats'], 1)
        self.assertEqual(len(extension['error_sha256']), 64)
        accepted = self.fixture()
        self.execute(request, script=accepted, max_calls=12, worker_calls=2)
        self.ack(request, self.publish(request))
        before = self.snapshot()
        row = e.build_report(plan, [self.run_entry(plan, request, 'single_agent', task=4)])['rows'][9]
        self.assertEqual(row['status'], 'passed', row)
        self.assertTrue(row['identity_evidence']['shared_request_history'])
        self.assertGreater(row['identity_evidence']['global_steps'], 1)
        for legacy_plan in legacy_plans:
            legacy = e.build_report(legacy_plan, [self.run_entry(legacy_plan, request, 'single_agent', task=4)])['rows'][9]
            self.assertEqual(legacy['status'], 'failed', legacy)
            self.assertFalse(legacy['checks']['condition_roles'])
        self.assertEqual(self.snapshot(), before)
        # A valid rejection marker cannot hide an actual applied plan, even rehashed.
        with closing(sqlite3.connect(self.database)) as db, db:
            db.row_factory = sqlite3.Row
            applied = db.execute("SELECT f.* FROM model_tool_feedback f JOIN call_attempts a USING(attempt_id) "
                                 "WHERE a.role='dispatcher' AND json_extract(f.result,'$.plan_revision')=1").fetchone()
            fact = {key: e.strict_json(applied[key]) for key in ('original_binding', 'response')}
            fact.update(attempt_id=applied['attempt_id'], result=e.strict_json(feedback['result']))
            db.execute('DROP TRIGGER model_tool_feedback_immutable')
            db.execute('UPDATE model_tool_feedback SET result=?,record_sha256=? WHERE attempt_id=?',
                       (e.original_canonical(fact['result']).decode(), e.original_digest(fact), applied['attempt_id']))
        with e.database(self.database) as db, self.assertRaisesRegex(e.InvalidEvidence, 'correction rejection has an application'):
            e.model_applications(db, request, e.rows(db, 'SELECT * FROM call_attempts WHERE request_id=? ORDER BY rowid', (request,)))

    def test_v3_rehashed_omission_of_actual_native_history_refuses(self):
        request = self.logical_seed()
        plan = self.logical_plan(request)
        self.execute(request, max_calls=12, worker_calls=2)
        self.tamper_last_logical_input(lambda logical: logical.update(messages=logical['messages'][2:]))
        row = e.build_report(plan, [self.run_entry(plan, request, 'single_agent', task=4)])['rows'][9]
        self.assertEqual(row['status'], 'invalid_evidence', row)
        self.assertIn('actual model input omitted or changed', row['reason'])

    def test_v3_rehashed_global_chain_fork_refuses(self):
        request = self.logical_seed()
        plan = self.logical_plan(request)
        self.execute(request, max_calls=12, worker_calls=2)
        def fork(logical):
            contract = json.loads(logical['task_contract'])
            contract['logical_agent_execution']['global_step'] = 1
            logical['task_contract'] = e.canonical(contract).decode()
        self.tamper_last_logical_input(fork)
        row = e.build_report(plan, [self.run_entry(plan, request, 'single_agent', task=4)])['rows'][9]
        self.assertEqual(row['status'], 'invalid_evidence', row)
        self.assertIn('chain is missing, reordered or forked', row['reason'])

    def test_v3_old_multi_role_run_cannot_gain_logical_identity_from_a_label(self):
        request = self.logical_seed(logical=False)
        plan = self.logical_plan(request)
        self.execute(request, max_calls=12, worker_calls=2)
        def relabel(logical):
            contract = json.loads(logical['task_contract'])
            contract['logical_agent_execution'] = {'logical_agent_id': 'same-label-for-all-roles'}
            logical['task_contract'] = e.canonical(contract).decode()
        self.tamper_last_logical_input(relabel)
        row = e.build_report(plan, [self.run_entry(plan, request, 'single_agent', task=4)])['rows'][9]
        self.assertEqual(row['status'], 'invalid_evidence', row)
        self.assertIn('frozen policy differs', row['reason'])

    def test_v3_fairness_removes_only_the_explicit_topology_variable(self):
        multi = self.logical_seed(logical=False, key='multi')
        single = self.logical_seed(key='single')
        changed = self.logical_seed(key='different-context', context_change=(
            'snapshot_decision_log_entries: 20', 'snapshot_decision_log_entries: 19'))
        with e.database(self.database) as db:
            original = e.common_lock(db, multi, 'fake', {'mode': 'provider_default'}, logical_agent_comparison=True)
            same = e.common_lock(db, single, 'fake', {'mode': 'provider_default'}, logical_agent_comparison=True)
            other = e.common_lock(db, changed, 'fake', {'mode': 'provider_default'}, logical_agent_comparison=True)
            self.assertEqual(original, same)
            self.assertNotEqual(original['workflow_policy_sha256'], other['workflow_policy_sha256'])

    def test_v3_single_fake_host_dispositions_refuse_before_takeover(self):
        request = self.logical_seed()
        before = self.snapshot()
        self.invoke('pump-single-fake', '--db', self.database, '--request', request,
                    '--script', h.FIXTURES / 's15_fake_complete.json',
                    '--dispositions-file', self.json_file('host-dispositions.json', {
                        'alpha': {'disposition': 'keep', 'rationale': 'Unexecuted host choice must not drive the model baseline.'}}),
                    error='unauthorized')
        self.assertEqual(self.snapshot(), before)

    def execute_across_two_recoveries(self, request):
        def continuation(counts):
            script = self.fixture()
            for steps in [script['default'], *script['nodes'].values()]:
                steps[:] = [step for step in steps if step['role_call_index'] > counts.get(step['role'], 0)]
                for step in steps:
                    step['role_call_index'] -= counts.get(step['role'], 0)
            return script
        # Fake script indices are local to each process. The first process
        # produces the plan/order; the next writes; the third submits/checks/
        # accepts. All original model bindings remain in the real Store.
        self.assertEqual(len(self.execute(request, max_calls=2, worker_calls=2)['calls']), 2)
        self.assertEqual(len(self.execute(request, max_calls=1, worker_calls=1,
            script=continuation({'dispatcher': 1, 'team_lead': 1}))['calls']), 1)
        self.execute(request, max_calls=12, worker_calls=2,
                     script=continuation({'dispatcher': 1, 'team_lead': 1, 'worker': 1}))

    def test_v3_recovered_original_instruction_links_both_conditions(self):
        for logical in (False, True):
            with self.subTest(logical=logical):
                workspace = self.directory / ('restart-workspace-' + str(logical))
                workspace.mkdir(mode=0o700)
                self.mapping = self.json_file('restart-workspaces-' + str(logical) + '.json', {'alpha': str(workspace)})
                request = self.logical_seed(logical=logical, key='restart-' + str(logical), request_format='structured-v3',
                    context_change=('context:\n', 'context:\n  model_projection: lean-v1\n'))
                plan = self.logical_plan(request)
                legacy_plan = self.plan(request, model_origin=True)
                with e.database(self.database) as db:
                    plain_plan = e.make_plan(db, request, include_plain=True)
                self.execute_across_two_recoveries(request)
                self.ack(request, self.publish(request), key='restart-receipt-' + str(logical))
                condition = 'single_agent' if logical else 'multi_agent'
                row = e.build_report(plan, [self.run_entry(plan, request, condition, task=4)])['rows'][8 + logical]
                self.assertEqual(row['status'], 'passed', row)
                current = e.build_report(plain_plan, [self.run_entry(plain_plan, request, condition, task=4)])['rows'][12 + logical]
                self.assertEqual(current['status'], 'observed', current)
                self.assertEqual(current['workflow_completion'], 'complete')
                self.assertEqual(current['plain_loop_completion'], 'not_applicable')
                self.assertEqual(current['quality_assessment'], 'not_assessed')
                recovery = row['decision_evidence']['recovery_projections']
                self.assertEqual(recovery['contract'], 'original-instruction-epoch-rebind-v1')
                self.assertEqual(len(recovery['witnesses']), 2)
                self.assertEqual(recovery['witnesses_sha256'], e.digest(recovery['witnesses']))
                # Historical v2 retains its exact-binding source interpretation.
                legacy = e.build_report(legacy_plan, [self.run_entry(legacy_plan, request, task=4)])['rows'][8]
                self.assertEqual(legacy['status'], 'failed', legacy)
                self.assertFalse(legacy['checks']['model_accepted_delivery_source'])
                self.assertNotIn('recovery_projections', legacy['decision_evidence'])

    def test_v3_recovery_projection_requires_original_body_and_exact_epochs(self):
        request = self.logical_seed(logical=False)
        plan = self.logical_plan(request)
        self.execute_across_two_recoveries(request)
        self.ack(request, self.publish(request))
        with closing(sqlite3.connect(self.database)) as db, db:
            recovered = db.execute("SELECT event_id,result FROM events WHERE request_id=? AND type='recover' "
                "AND seq<(SELECT MIN(seq) FROM events WHERE request_id=? AND type='submit_work') "
                "AND json_array_length(json_extract(result,'$.rebound_instructions'))>0 ORDER BY seq",
                (request, request)).fetchall()
            self.assertEqual(len(recovered), 2)
            event_id, raw = recovered[0]
            result = json.loads(raw)
            self.assertTrue(result['rebound_instructions'])
            db.execute('DROP TRIGGER events_immutable')
        for field, replacement in (('body_sha256', '0' * 64), ('execution_epoch', 999)):
            with self.subTest(field=field):
                changed = copy.deepcopy(result)
                changed['rebound_instructions'][0][field] = replacement
                with closing(sqlite3.connect(self.database)) as db, db:
                    db.execute('UPDATE events SET result=? WHERE event_id=?', (e.canonical(changed).decode(), event_id))
                row = e.build_report(plan, [self.run_entry(plan, request, task=4)])['rows'][8]
                self.assertEqual(row['status'], 'invalid_evidence', row)
                self.assertIn('does not prove the submitted epoch projection', row['reason'])
        # Remove the first projection rather than merely changing its final
        # target. The later original record cannot fill the missing interval.
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute('DELETE FROM events WHERE event_id=?', (event_id,))
        row = e.build_report(plan, [self.run_entry(plan, request, task=4)])['rows'][8]
        self.assertEqual(row['status'], 'invalid_evidence', row)
        self.assertIn('does not prove the submitted epoch projection', row['reason'])

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

    def corrupt_feedback(self, name, change):
        # Corruption is restricted to this disposable SQLite fixture. Hashes
        # are recomputed so provenance checks must inspect the actual relation.
        with closing(sqlite3.connect(self.database)) as db, db:
            row = db.execute("SELECT f.attempt_id,f.original_binding,f.response,f.result FROM model_tool_feedback f "
                             "WHERE json_extract(f.response,'$.tool_calls[0].name')=? LIMIT 1", (name,)).fetchone()
            self.assertIsNotNone(row)
            fact = {'attempt_id': row[0], 'original_binding': json.loads(row[1]),
                    'response': json.loads(row[2]), 'result': json.loads(row[3])}
            change(fact)
            db.execute('DROP TRIGGER model_tool_feedback_immutable')
            db.execute('UPDATE model_tool_feedback SET original_binding=?,response=?,result=?,record_sha256=? WHERE attempt_id=?',
                       (e.canonical(fact['original_binding']).decode(), e.canonical(fact['response']).decode(),
                        e.canonical(fact['result']).decode(), e.digest(fact), row[0]))

    def test_new_provider_plan_requires_versioned_model_origin_contract(self):
        request = self.seed()
        with e.database(self.database) as db:
            plan = e.make_plan(db, request, evidence_mode='provider')
            self.assertEqual(plan['schema_version'], 2)
            self.assertEqual(plan['decision_evidence'], 'model-origin-v1')
            e.validate_plan(plan)
            with self.assertRaisesRegex(e.InvalidEvidence, 'require model-origin'):
                e.make_plan(db, request, evidence_mode='provider', decision_evidence='structural-v1')

    def test_v1_report_keeps_structural_scope_explicit(self):
        request = self.seed(task=4)
        plan = self.plan(request)
        self.execute(request, max_calls=12, worker_calls=2)
        self.ack(request, self.publish(request))
        report = e.build_report(plan, [self.run_entry(plan, request, task=4)])
        self.assertEqual(plan['schema_version'], 1)
        self.assertEqual(report['rows'][8]['status'], 'passed')
        self.assertEqual(report['rows'][8]['pass_scope'], 'original_structural_checks_only')
        self.assertFalse(report['rows'][8]['decision_evidence']['model_decision_origin_established'])
        self.assertFalse(report['provider_quality_acceptance_established'])

    def test_v2_original_model_application_chain_reaches_confirmed_delivery(self):
        request = self.seed(task=4)
        plan = self.plan(request, model_origin=True)
        self.execute(request, max_calls=12, worker_calls=2)
        self.ack(request, self.publish(request))
        row = e.build_report(plan, [self.run_entry(plan, request, task=4)])['rows'][8]
        self.assertEqual(row['status'], 'passed', row)
        self.assertTrue(row['checks']['model_accepted_delivery_source'])
        self.assertTrue(row['decision_evidence']['model_decision_origin_established'])
        self.assertNotIn('arguments', e.canonical(row['decision_evidence']).decode())
        self.assertNotIn('alpha candidate version', e.canonical(row['decision_evidence']).decode())

    def test_v2_host_acceptance_fact_without_original_feedback_is_not_model_evidence(self):
        request = self.seed(task=4)
        plan = self.plan(request, model_origin=True)
        self.execute(request, max_calls=12, worker_calls=2)
        self.ack(request, self.publish(request))
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute("DELETE FROM model_tool_feedback WHERE json_extract(response,'$.tool_calls[0].name')='accept_candidate'")
        row = e.build_report(plan, [self.run_entry(plan, request, task=4)])['rows'][8]
        self.assertEqual(row['status'], 'failed', row)
        self.assertTrue(row['checks']['confirmed_delivery'])
        self.assertFalse(row['checks']['model_accepted_delivery_source'])
        self.assertFalse(row['checks']['model_application_records_complete'])

    def test_v2_host_command_cannot_be_relabelled_as_model_application(self):
        request = self.seed(task=4)
        plan = self.plan(request, model_origin=True)
        self.execute(request, max_calls=12, worker_calls=2)
        self.ack(request, self.publish(request))
        with closing(sqlite3.connect(self.database)) as db, db:
            db.execute('DROP TRIGGER events_immutable')
            db.execute("UPDATE events SET command_id='host-explicit-acceptance' WHERE type='accept_candidate'")
            self.assertEqual(db.execute("SELECT count(*) FROM events WHERE command_id='host-explicit-acceptance'").fetchone()[0], 1)
        row = e.build_report(plan, [self.run_entry(plan, request, task=4)])['rows'][8]
        self.assertEqual(row['status'], 'failed', row)
        self.assertTrue(row['checks']['confirmed_delivery'])
        self.assertFalse(row['checks']['model_accepted_delivery_source'])

    def test_v2_wrong_attempt_response_refuses_even_with_rehashed_feedback(self):
        request = self.seed(task=4)
        plan = self.plan(request, model_origin=True)
        self.execute(request, max_calls=12, worker_calls=2)
        self.ack(request, self.publish(request))
        self.corrupt_feedback('accept_candidate', lambda fact: fact['response'].update(attempt_id='attempt-another-original'))
        row = e.build_report(plan, [self.run_entry(plan, request, task=4)])['rows'][8]
        self.assertEqual(row['status'], 'invalid_evidence', row)
        self.assertIn('feedback changed original response', row['reason'])

    def test_v2_captured_target_must_match_actual_application(self):
        request = self.logical_seed(task=4, logical=False, request_format='framed-v1')
        plan = self.plan(request, model_origin=True)
        self.execute(request, max_calls=12, worker_calls=2)
        self.ack(request, self.publish(request))
        with closing(sqlite3.connect(self.database)) as db, db:
            row = db.execute("SELECT attempt_id,request_envelope FROM call_attempts WHERE json_extract(response,'$.tool_calls[0].name')='accept_candidate'").fetchone()
            envelope = json.loads(row[1])
            context = envelope['context']
            contract = e.captured_contract(envelope)
            contract['workflow_action']['target']['submission_id'] = 'sub_uncaptured_synthetic_target'
            content = e.canonical(contract)
            original = context['dynamic_suffix'].encode()
            header, _, tail = original.partition(b'\n')
            old_size = int(header.split()[1])
            context['dynamic_suffix'] = (b'task_contract ' + str(len(content)).encode() + b'\n' + content + b'\n' + tail[old_size + 1:]).decode()
            db.execute('UPDATE call_attempts SET request_envelope=?,state_version=state_version+1 WHERE attempt_id=?',
                       (e.canonical(envelope).decode(), row[0]))
        row = e.build_report(plan, [self.run_entry(plan, request, task=4)])['rows'][8]
        self.assertEqual(row['status'], 'invalid_evidence', row)
        self.assertIn('another target or version', row['reason'])

    def test_v2_diagnostic_read_write_readback_uses_original_feedback(self):
        request = self.seed()
        plan = self.plan(request, model_origin=True)
        (self.workspace / 'seed.txt').write_bytes(b'alpha\nbeta\n')
        fixture = self.fixture()
        nodes = fixture['nodes']['alpha']
        worker = [self.step('worker', 'fs_read', {'path': 'seed.txt'}, 1),
                  self.step('worker', 'fs_write', {'path': 'result.txt', 'content': 'alpha\nbeta\n'}, 2),
                  self.step('worker', 'fs_read', {'path': 'result.txt'}, 3),
                  copy.deepcopy(nodes[2])]
        worker[-1]['role_call_index'] = 4
        fixture['nodes']['alpha'] = [nodes[0], *worker, *nodes[3:]]
        self.execute(request, script=fixture, max_calls=16, worker_calls=4)
        row = e.build_report(plan, [self.run_entry(plan, request)])['rows'][0]
        self.assertEqual(row['status'], 'passed', row)
        self.assertTrue(row['checks']['model_tool_roundtrip'])
        self.assertTrue(row['checks']['artifact_sha256'])
        with closing(sqlite3.connect(self.database)) as db:
            self.assertEqual(db.execute("SELECT count(*) FROM ops WHERE tool_id='fs_read'").fetchone()[0], 0)

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
        origin_plan = self.plan(request, model_origin=True)
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
        origin = e.build_report(origin_plan, [self.run_entry(origin_plan, request, task=2)])['rows'][4]
        self.assertEqual(origin['status'], 'passed', origin)
        self.assertTrue(origin['checks']['model_rejection_correction_acceptance'])

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

    def test_logical_agent_projection_covers_opaque_responses_output(self):
        payload = {'format': 'wls-responses-output-v1', 'reference': 'openai-responses-2026-11-08',
                   'model': 'o3-2025-04-16', 'output': [
                       {'id': 'rs_1', 'type': 'reasoning', 'summary': [],
                        'encrypted_content': 'gAAAAABoriginal-opaque-bytes', 'status': 'completed'},
                       {'id': 'fc_1', 'type': 'function_call', 'call_id': 'call_provider_1',
                        'name': 'note', 'arguments': '{"m":1}', 'status': 'completed'}]}
        base = {'format': 'wls-model-response-v1', 'call_id': 'call-1', 'attempt_id': 'attempt-1',
                'role_call_index': 1, 'role': 'worker', 'binding': {'request_id': 'req-1'},
                'config_snapshot_id': 'c', 'status': 'completed', 'text': '',
                'tool_calls': [{'call_id': 'history_1', 'name': 'note', 'arguments': {'m': 1}}],
                'usage': {'input_tokens': 1, 'output_tokens': 1, 'cache_read_tokens': None,
                          'cache_write_tokens': None, 'cost_micro_usd': None},
                'error_code': '', 'error_message': ''}
        response = dict(base, responses_output=copy.deepcopy(payload))

        def record(value):
            return {'attempt_id': value['attempt_id'], 'call_id': value['call_id'], 'request_id': 'req-1',
                    'task_id': 'task-1', 'team_id': 'team-1', 'role': 'worker', 'principal_id': 'p1',
                    'config_snapshot_sha256': 'c', 'model_ref': 'm', 'reservation_sha256': 'r',
                    'status': 'COMPLETED', 'accepted': 1, 'settlement_sha256': 's',
                    'frozen_binding': {'request_id': 'req-1'}, 'role_call_index': 1,
                    'response': value,
                    'feedback': {'original_binding': {'request_id': 'req-1'}, 'response': value,
                                 'result': {'ok': True}, 'record_sha256': '0' * 64},
                    'history_sequence': 1, 'feedback_missing': False, 'usage': None,
                    'logical_agent_execution': {}}

        projected = e.logical_agent_messages([record(response)])
        assistant, tool = projected[0], projected[1]
        self.assertEqual(assistant['role'], 'assistant')
        self.assertEqual(assistant['responses_output'], payload)
        self.assertEqual(assistant['tool_calls'][0]['name'], 'note')
        self.assertEqual(tool['tool_call_id'], assistant['tool_calls'][0]['call_id'])
        legacy = e.logical_agent_messages([record(dict(base))])
        self.assertNotIn('responses_output', legacy[0])
        # Missing, empty or tampered replay ciphertext is invalid evidence.
        for mutation in ('empty', 'removed', 'unknown', 'envelope'):
            broken = copy.deepcopy(response)
            item = broken['responses_output']['output'][0]
            if mutation == 'empty':
                item['encrypted_content'] = ''
            elif mutation == 'removed':
                item.pop('encrypted_content')
            elif mutation == 'unknown':
                item['future_field'] = 1
            else:
                broken['responses_output']['extra'] = 1
            with self.assertRaises(e.InvalidEvidence):
                e.logical_agent_messages([record(broken)])


def evaluation_groups():
    names = set(unittest.defaultTestLoader.getTestCaseNames(Evaluation))
    logical = {name for name in names if name.startswith('test_v3_')} | {
        'test_submission_completion_requires_the_captured_new_v3_declaration',
        'test_logical_agent_projection_covers_opaque_responses_output',
    }
    plain = {name for name in names if name.startswith('test_v4_')}
    groups = {'legacy': names - logical - plain, 'logical': logical, 'plain': plain}
    grouped = [name for methods in groups.values() for name in methods]
    if (any(not methods for methods in groups.values()) or
            len(grouped) != len(names) or set(grouped) != names):
        raise ValueError('evaluation groups must cover every method exactly once')
    return {'all': sorted(names), **{group: sorted(methods) for group, methods in groups.items()}}


if __name__ == '__main__':
    if os.geteuid() == 0:
        raise SystemExit('Evaluation regressions require an ordinary user')
    methods = evaluation_groups()[OPTIONS.group]
    unittest.main(verbosity=2, defaultTest=[f'Evaluation.{name}' for name in methods])
