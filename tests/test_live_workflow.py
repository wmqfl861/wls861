#!/usr/bin/env python3
"""Original live workflow over disposable loopback TLS, never provider quality.

The peer emits native function calls from captured request targets. Its separate
synthetic billing receipt is checked by the trusted host hook. Original SQLite
responses, targets, feedback, business events and transport bytes remain the
source for assertions; safe pump summaries cannot substitute for those records.
"""
from __future__ import annotations

from contextlib import closing
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

from immediate_unittest import ImmediateTextRunner

if len(sys.argv) != 3:
    raise SystemExit('usage: test_live_workflow.py DRIVER SOURCE')
DRIVER, SOURCE = (Path(value).resolve() for value in sys.argv[1:])
spec = importlib.util.spec_from_file_location('workflow_tls_fixture', SOURCE / 'tests/test_https_transport.py')
tls = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tls)
spec = importlib.util.spec_from_file_location('workflow_evaluation', SOURCE / 'scripts/evaluate_r3.py')
e = importlib.util.module_from_spec(spec)
spec.loader.exec_module(e)
sys.argv = [sys.argv[0]]
BEARER = 'WLS_SYNTHETIC_WORKFLOW_BEARER_7C91'


def without_constraints(value):
    if isinstance(value, dict):
        return {key: ([] if key in ('constraint_refs', 'constraints_verbatim') else without_constraints(item))
                for key, item in value.items()}
    if isinstance(value, list):
        return [without_constraints(item) for item in value]
    return value


class ModelPeer:
    """A deterministic test peer, explicitly synthetic rather than an LLM."""
    def __init__(self, directory, mode='complete', bad_receipt=False):
        self.receipts = directory / 'receipts'
        self.receipts.mkdir(mode=0o700)
        self.mode, self.bad_receipt = mode, bad_receipt
        fixture = without_constraints(json.loads((SOURCE / 'tests/fixtures/s15_fake_complete.json').read_text()))
        self.plan = fixture['default'][0]['tool_calls'][0]['arguments']
        if mode in ('amend', 'cross_task'):
            beta = copy.deepcopy(self.plan['nodes'][0])
            beta['node_id'] = 'beta'
            beta['title'] = 'Independent beta'
            self.plan['nodes'].append(beta)
        self.order = fixture['nodes']['alpha'][0]['tool_calls'][0]['arguments']
        self.submission = fixture['nodes']['alpha'][2]['tool_calls'][0]['arguments']
        self.check = fixture['nodes']['alpha'][3]['tool_calls'][0]['arguments']
        self.acceptance = fixture['default'][1]['tool_calls'][0]['arguments']
        self.steps, self.contents = {}, {}
        self.actions, self.contracts = [], []
        self.primary_task = None

    def choose(self, body):
        contract = json.loads(json.loads(body['input'][0]['content'])['task_contract'])
        self.contracts.append(contract)
        action, binding = contract.get('workflow_action'), contract['binding']
        names = {tool['name'] for tool in body['tools']}
        if action:
            kind, target = action['kind'], action['target']
            if kind == 'amendment':
                if target['plan_required']:
                    return 'submit_amendment_plan', {'plan': self.plan, 'dispositions': {
                        'alpha': {'disposition': 'update', 'rationale': 'Model fixture applies the explicit AMEND to alpha.'}}}
                return 'resolve_amendment', {'dispositions': {
                    'beta': {'disposition': 'keep', 'rationale': 'Model fixture retains beta under the current AMEND.'}}}
            if kind == 'submission':
                return 'check_submission', self.check
            if kind == 'acceptance':
                if self.mode == 'exhausted' or (self.mode == 'rework' and target['original_binding']['task_revision'] == 1):
                    return 'accept_candidate', {'passed': False, 'notes': 'Synthetic independent rejection of this captured candidate.'}
                return 'accept_candidate', self.acceptance
            if kind == 'rejection':
                if target['rework_remaining'] == 0:
                    return 'escalate_task', {'text': 'How should this original rejected task proceed?',
                        'context': 'The original frozen rework allowance is exhausted; synthetic original-source escalation.'}
                return 'change_task', {'rationale': 'The original model rejected this candidate.',
                    'correction': 'Correct the same original task under all existing constraints.'}
            if kind == 'experience':
                return 'report_experience', {'no_new_experience': True, 'entries': [], 'memory_assessments': []}
            raise AssertionError(f'unexpected synthetic workflow action {kind}')
        if not binding['task_id']:
            return 'submit_plan', self.plan
        if self.primary_task is None:
            self.primary_task = binding['task_id']
        if 'issue_work_order' in names:
            return 'issue_work_order', self.order
        key = (binding['task_id'], binding['instruction_revision'])
        index = self.steps.get(key, 0)
        self.steps[key] = index + 1
        if self.mode == 'roundtrip' or (self.mode == 'cross_task' and binding['task_id'] == self.primary_task):
            sequence = [('fs_read', {'path': 'seed.txt'}),
                        ('fs_write', {'path': 'result.txt', 'content': 'alpha\nbeta\n'}),
                        ('fs_read', {'path': 'result.txt'}), ('submit', self.submission)]
            return sequence[index]
        if index == 0:
            content = f'Synthetic candidate task revision {binding["task_revision"]}.\n'
            arguments = {'path': 'other.txt' if self.mode == 'cross_task' else 'result.txt', 'content': content}
            if binding['task_id'] in self.contents:
                arguments['expected_sha256'] = hashlib.sha256(self.contents[binding['task_id']].encode()).hexdigest()
            self.contents[binding['task_id']] = content
            return 'fs_write', arguments
        if index == 1:
            if self.mode == 'cross_task':
                submission = copy.deepcopy(self.submission)
                submission['deliverables'][0]['path'] = '/work/other.txt'
                return 'submit', submission
            return 'submit', self.submission
        raise AssertionError('unexpected extra Worker call; fixture must not hide scheduler retries')

    def __call__(self, peer, connection):
        observed = peer.request(connection)
        body = json.loads(observed['body'])
        name, arguments = self.choose(body)
        names = {tool['name'] for tool in body['tools']}
        if name not in names:
            raise AssertionError(f'{name} was not declared: {sorted(names)}')
        call_id = f'fixture-call-{len(self.actions) + 1}'
        self.actions.append(name)
        usage = {'input_tokens': 9, 'output_tokens': 3, 'cache_read_tokens': 2,
                 'cache_write_tokens': 1, 'cost_micro_usd': 17}
        receipt = {'fixture': 'loopback-workflow-billing-v1', 'call_id': call_id,
                   'request_body_sha256': hashlib.sha256(observed['body']).hexdigest(), 'usage': usage}
        if self.bad_receipt:
            receipt['request_body_sha256'] = '0' * 64
        (self.receipts / (call_id + '.json')).write_text(json.dumps(receipt))
        value = json.loads(tls.response_body(tool=True))
        if self.mode == 'cache_unknown':
            del value['usage']['input_tokens_details']['cache_write_tokens']
        value['id'] = f'resp_workflow_{len(self.actions)}'
        value['output'][0].update(call_id=call_id, name=name, arguments=json.dumps(arguments, ensure_ascii=False))
        encoded = tls.encoded(value)
        connection.sendall(tls.headers(len(encoded)) + encoded)


class LiveWorkflow(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tls.HttpsTransport.setUpClass.__func__(cls)

    @classmethod
    def tearDownClass(cls):
        tls.HttpsTransport.tearDownClass.__func__(cls)

    def setUp(self):
        original_umask = os.umask(0o077)
        self.addCleanup(os.umask, original_umask)
        temporary = tempfile.TemporaryDirectory(prefix='wls-live-workflow-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.directory.chmod(0o700)
        self.workspaces = self.directory / 'workspaces'
        self.workspaces.mkdir(mode=0o700)
        self.database = self.directory / 'runtime.sqlite'
        self.counter = 0

    def invoke(self, peer=None, model=None, **options):
        self.counter += 1
        config = {'database': str(self.database), 'workspaces': str(self.workspaces), **options}
        if peer:
            config.update(port=peer.port, ca_path=str(self.certificates / 'ca.pem'))
        if model:
            config['receipts'] = str(model.receipts)
        path = self.directory / f'configuration-{self.counter}.json'
        path.write_text(json.dumps(config))
        # Each normal host batch explicitly allows 60 seconds. A scenario can
        # contain three finite batches; this independent process bound includes
        # startup/settlement overhead under ASan without assuming Release speed.
        result = subprocess.run([str(DRIVER), str(path), str(SOURCE)], cwd=self.directory,
                                capture_output=True, text=True, encoding='utf-8', timeout=240, check=False)
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertEqual(result.stderr, '')
        parsed = json.loads(result.stdout)
        if 'ledger' in parsed:
            self.assertTrue(parsed['ledger']['consistent'], parsed)
        return parsed

    def rows(self, query, parameters=()):
        with closing(sqlite3.connect(self.database)) as db:
            db.row_factory = sqlite3.Row
            return [dict(row) for row in db.execute(query, parameters)]

    def origin(self, request_id, task):
        with e.database(self.database) as db:
            request = e.one(db, 'SELECT * FROM requests WHERE request_id=?', (request_id,))
            attempts = e.rows(db, 'SELECT * FROM call_attempts WHERE request_id=? ORDER BY rowid', (request_id,))
            applications, evidence = e.model_applications(db, request_id, attempts)
            fixed = {'task_id': e.TASKS[task][0], 'checks': e.TASKS[task][2]}
            return applications, evidence, e.model_origin_checks(db, request, fixed, applications, evidence)

    def assert_private_safe(self, value):
        encoded = json.dumps(value)
        for private in (BEARER, 'PRIVATE_RESOLVER_FAILURE_7C91', 'private-server-receipt:',
                        'raw_body_hex', 'raw_headers_hex', 'Synthetic candidate task revision'):
            self.assertNotIn(private, encoded)

    def test_original_native_workflow_acceptance_experience_and_safe_summary(self):
        model = ModelPeer(self.directory)
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True, repeat_batch=True)
            self.assertEqual(model.actions, ['submit_plan', 'issue_work_order', 'fs_write', 'submit',
                'check_submission', 'accept_candidate', 'report_experience'])
            self.assertEqual(len(peer.requests), 7)
            for request in peer.requests:
                self.assertEqual(request['headers'][b'authorization'], f'Bearer {BEARER}'.encode())
        report = result['reports'][0]
        self.assertEqual(report['mode'], 'event_live')
        self.assertEqual(report['stop_reason'], 'quiescent', result)
        self.assertEqual(result['repeat']['calls'], [])
        self.assertEqual(result['credential_reads'], 7)
        self.assertEqual(result['evidence_reads'], 7)
        self.assertTrue(result['accounting']['ready'])
        self.assertEqual([row['status'] for row in self.rows('SELECT status FROM tasks')], ['ACCEPTED'])
        self.assertEqual(self.rows('SELECT status FROM experience_jobs')[0]['status'], 'DONE')
        self.assertEqual(self.rows('SELECT count(*) AS n FROM model_tool_feedback')[0]['n'], 7)
        self.assertEqual(self.rows('SELECT count(*) AS n FROM live_send_claims')[0]['n'], 7)
        self.assertEqual(self.rows('SELECT count(*) AS n FROM usage_reconciliations')[0]['n'], 7)
        self.assertTrue(all(row['cost_micro_usd'] is None for row in self.rows('SELECT cost_micro_usd FROM call_attempts')))
        self.assertEqual(result['budget']['spent_cost'], 7 * 17)
        applications, evidence, _ = self.origin(result['request_id'], 4)
        self.assertEqual(len(applications), 6)  # Experience is outside the fixed business rubric.
        self.assertEqual(evidence['missing'], [])
        self.assertTrue(all(app['execution_kind'] == 'local_tls' for app in applications))
        self.assert_private_safe(result)

    def test_native_diagnostic_roundtrip_uses_original_feedback(self):
        (self.workspaces / 'alpha').mkdir(mode=0o700)
        (self.workspaces / 'alpha' / 'seed.txt').write_bytes(b'alpha\nbeta\n')
        model = ModelPeer(self.directory, 'roundtrip')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True)
        self.assertEqual(result['reports'][0]['stop_reason'], 'quiescent', result)
        _, _, checks = self.origin(result['request_id'], 0)
        self.assertTrue(all(checks.values()), checks)
        self.assertEqual(self.rows("SELECT count(*) AS n FROM ops WHERE tool_id='fs_read'")[0]['n'], 0)
        self.assertEqual((self.workspaces / 'alpha' / 'result.txt').read_bytes(), b'alpha\nbeta\n')

    def test_cross_task_host_submission_cannot_complete_model_roundtrip_witness(self):
        (self.workspaces / 'alpha').mkdir(mode=0o700)
        (self.workspaces / 'alpha' / 'seed.txt').write_bytes(b'alpha\nbeta\n')
        model = ModelPeer(self.directory, 'cross_task')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True, worker_calls=3,
                phases=[{'max_calls': 32, 'host_submit_alpha': True}, {'max_calls': 32}],
                host_submission=model.submission, host_check=model.check)
        self.assertNotIn('threw', result, result)
        self.assertEqual(result['reports'][-1]['stop_reason'], 'quiescent', result)
        applications, evidence, checks = self.origin(result['request_id'], 0)
        self.assertEqual(evidence['missing'], [])
        self.assertTrue(checks['model_tool_roundtrip'])  # Model A really performed the tool roundtrip.
        submissions = [app for app in applications if app['tool_name'] == 'submit']
        self.assertEqual(len(submissions), 1)  # Model B really submitted and was checked.
        self.assertNotEqual(submissions[0]['binding']['task_id'], model.primary_task)
        self.assertTrue(any(app['tool_name'] == 'check_submission' and
                            app['submission_id'] == submissions[0]['submission_id'] for app in applications))
        self.assertFalse(checks['model_submission_source'])
        with e.database(self.database) as db:
            request = e.one(db, 'SELECT * FROM requests WHERE request_id=?', (result['request_id'],))
            host = e.one(db, 'SELECT * FROM workflow_submissions WHERE submission_id=?', (result['host_submission_id'],))
            expected = e.TASKS[0][2]['artifact_sha256']
            self.assertTrue(e.artifact_matches(host, expected, logical_paths=True))
            task = {'task_id': e.TASKS[0][0], 'checks': e.TASKS[0][2]}
            self.assertFalse(e.evidence_checks(db, request, task, applications)['artifact_sha256'])
            # Preserve the old structural path semantics: it still sees the
            # host's exact original artifact when asked for its original path.
            old_task = {'checks': {'artifact_sha256': {'/work/result.txt': expected['result.txt']}}}
            self.assertTrue(e.evidence_checks(db, request, old_task)['artifact_sha256'])

    def test_old_model_rework_and_escalation_cannot_score_a_later_requirement(self):
        for mode, task, check in (('rework', 2, 'model_rejection_correction_acceptance'),
                                  ('exhausted', 3, 'model_exhaustion_escalation')):
            with self.subTest(mode=mode):
                directory = self.directory / mode
                directory.mkdir(mode=0o700)
                self.database = directory / 'runtime.sqlite'
                self.workspaces = directory / 'workspaces'
                self.workspaces.mkdir(mode=0o700)
                model = ModelPeer(directory, mode)
                with tls.Peer(self.contexts['server'], model) as peer:
                    result = self.invoke(peer, model, reconcile=True, amend_after=True,
                        phases=[{}, {}] if mode == 'exhausted' else [{}])
                self.assertNotIn('threw', result, result)
                self.assertEqual(self.rows('SELECT requirements_revision FROM requests')[0]['requirements_revision'], 2)
                self.assertEqual({row['revision'] for row in self.rows("SELECT json_extract(frozen_binding,'$.requirements_revision') AS revision FROM call_attempts")}, {1})
                self.assertIn('change_task' if mode == 'rework' else 'escalate_task', model.actions)
                _, evidence, checks = self.origin(result['request_id'], task)
                self.assertEqual(evidence['missing'], [])
                self.assertFalse(checks[check])

    def test_model_rejection_correction_and_new_acceptance_share_original_task(self):
        model = ModelPeer(self.directory, 'rework')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True)
        self.assertEqual(result['reports'][0]['stop_reason'], 'quiescent', result)
        self.assertIn('change_task', model.actions)
        _, _, checks = self.origin(result['request_id'], 2)
        self.assertTrue(all(checks.values()), checks)
        self.assertEqual(len(self.rows('SELECT task_id FROM tasks')), 1)
        self.assertEqual(len(self.rows('SELECT acceptance_id FROM candidate_acceptances')), 2)
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state')[0]['rework_count'], 1)

    def test_model_exhaustion_escalates_original_rejected_submission(self):
        model = ModelPeer(self.directory, 'exhausted')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True, phases=[{}, {}])
        # The frozen per-wakeup management cap leaves the final rejection
        # pending. A second explicit bounded pump must consume that original
        # target; this test does not increase the frozen limit to finish early.
        self.assertTrue(any(item['code'] == 'manager_limit' for item in result['reports'][0]['blocked']))
        self.assertEqual(result['reports'][-1]['stop_reason'], 'quiescent', result)
        self.assertEqual(model.actions[-1], 'escalate_task')
        _, _, checks = self.origin(result['request_id'], 3)
        self.assertTrue(all(checks.values()), checks)
        self.assertEqual(self.rows("SELECT count(*) AS n FROM questions WHERE holder='user'")[0]['n'], 1)

    def test_partial_model_amend_then_keep_preserves_pending_original_task(self):
        model = ModelPeer(self.directory, 'amend')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True,
                phases=[{'max_calls': 5}, {'amend': True, 'max_calls': 3}, {'max_calls': 32}])
        self.assertEqual(result['reports'][-1]['stop_reason'], 'quiescent', result)
        self.assertEqual(model.actions.count('submit_amendment_plan'), 1)
        self.assertEqual(model.actions.count('resolve_amendment'), 1)
        self.assertEqual(self.rows('SELECT requirements_revision,plan_revision FROM requests')[0],
                         {'requirements_revision': 2, 'plan_revision': 2})
        self.assertEqual({row['disposition'] for row in self.rows('SELECT disposition FROM amend_dispositions')}, {'update', 'keep'})
        self.assertTrue(all(row['status'] == 'ACCEPTED' for row in self.rows('SELECT status FROM tasks')))
        _, _, checks = self.origin(result['request_id'], 1)
        self.assertTrue(all(checks.values()), checks)
        amendments = [contract['workflow_action'] for contract in model.contracts
                      if (contract.get('workflow_action') or {}).get('kind') == 'amendment']
        self.assertEqual(amendments[1]['target']['pending_node_ids'], ['beta'])

    def test_missing_usage_stops_batch_and_next_batch_before_credential_lookup(self):
        model = ModelPeer(self.directory)
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, repeat_batch=True)
            self.assertEqual(len(peer.requests), 1)
        self.assertEqual(result['reports'][0]['stop_reason'], 'needs_accounting', result)
        self.assertEqual(result['repeat']['stop_reason'], 'needs_accounting')
        self.assertEqual(result['repeat']['calls'], [])
        self.assertEqual(result['credential_reads'], 1)
        self.assertEqual(result['task_count'], 1)  # Original permitted plan is applied once.
        self.assertFalse(result['accounting']['ready'])
        self.assertEqual(self.rows('SELECT count(*) AS n FROM model_tool_feedback')[0]['n'], 1)
        self.assertEqual(self.rows('SELECT count(*) AS n FROM instructions')[0]['n'], 0)

    def test_cache_only_unknown_is_not_zero_balance_readiness(self):
        model = ModelPeer(self.directory, 'cache_unknown')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True, partial_evidence=True, repeat_batch=True)
            self.assertEqual(len(peer.requests), 1)
        self.assertEqual(result['reports'][0]['stop_reason'], 'needs_accounting', result)
        self.assertEqual(result['budget']['unknown_cost'], 0)
        self.assertEqual(result['budget']['reserved_cost'], 0)
        self.assertEqual(result['budget']['reserved_input'], 0)
        self.assertEqual(result['budget']['reserved_output'], 0)
        blocker = result['accounting']['blocked_attempts'][0]
        self.assertEqual(blocker['unknown_cache_write'], 1)
        self.assertEqual(result['credential_reads'], 1)

    def test_original_settlement_feedback_replay_has_no_second_send_or_apply(self):
        model = ModelPeer(self.directory)
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, action='replay')
            self.assertEqual(len(peer.requests), 1)
        self.assertTrue(result['replayed_original_feedback'])
        self.assertFalse(result['replay_sent'])
        self.assertEqual(result['hook_calls'], 1)
        self.assertEqual(result['task_count'], 1)
        self.assertTrue(result['accounting']['ready'])

    def test_old_settled_response_without_feedback_cannot_be_applied_later(self):
        model = ModelPeer(self.directory)
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, action='settled_without_feedback')
            self.assertEqual(len(peer.requests), 1)
        self.assertTrue(result['refused_missing_feedback'])
        self.assertEqual(result['hook_calls'], 0)
        self.assertEqual(result['task_count'], 0)
        self.assertEqual(self.rows('SELECT count(*) AS n FROM model_tool_feedback')[0]['n'], 0)

    def test_independent_receipt_must_match_observed_request_bytes(self):
        model = ModelPeer(self.directory, bad_receipt=True)
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True)
            self.assertEqual(len(peer.requests), 1)
        self.assertEqual(result['reports'][0]['stop_reason'], 'needs_accounting', result)
        self.assertEqual(result['task_count'], 0)
        self.assertEqual(self.rows('SELECT count(*) AS n FROM usage_reconciliations')[0]['n'], 0)
        feedback = json.loads(self.rows('SELECT result FROM model_tool_feedback')[0]['result'])
        self.assertTrue(feedback['runtime_error'])
        self.assert_private_safe(result)

    def test_verified_hook_statement_must_match_this_original_attempt_and_settlement(self):
        for option in ('wrong_evidence_attempt', 'wrong_evidence_settlement'):
            with self.subTest(option=option):
                directory = self.directory / option
                directory.mkdir(mode=0o700)
                self.database = directory / 'runtime.sqlite'
                model = ModelPeer(directory)
                with tls.Peer(self.contexts['server'], model) as peer:
                    result = self.invoke(peer, model, reconcile=True, **{option: True})
                    self.assertEqual(len(peer.requests), 1)
                self.assertEqual(result['reports'][0]['stop_reason'], 'needs_accounting', result)
                self.assertEqual(result['task_count'], 0)
                self.assertEqual(self.rows('SELECT count(*) AS n FROM usage_reconciliations')[0]['n'], 0)
                original = self.rows('SELECT accepted,settlement_sha256,cost_micro_usd FROM call_attempts')[0]
                self.assertEqual(original['accepted'], 1)
                self.assertIsNotNone(original['settlement_sha256'])
                self.assertIsNone(original['cost_micro_usd'])
                self.assertTrue(json.loads(self.rows('SELECT result FROM model_tool_feedback')[0]['result'])['runtime_error'])

    def test_deadline_preserves_original_partial_response_without_business_apply(self):
        body = tls.response_body(tool=True)
        partial_body = body[:80]
        def partial(peer, connection):
            peer.request(connection)
            connection.sendall(tls.headers(len(body)) + partial_body)
            # Remain connected beyond the explicit batch deadline. Context
            # teardown sets stop, so the fixture never needs this full wait.
            peer.stop.wait(30)
        with tls.Peer(self.contexts['server'], partial) as peer:
            result = self.invoke(peer, timeout_ms=5000)
            self.assertEqual(len(peer.requests), 1)
        self.assertEqual(result['reports'][0]['stop_reason'], 'deadline', result)
        self.assertEqual(result['task_count'], 0)
        self.assertFalse(result['accounting']['ready'])
        response = json.loads(self.rows('SELECT response FROM call_attempts')[0]['response'])
        self.assertEqual(response['tool_calls'], [])
        self.assertIsNone(response['usage']['cost_micro_usd'])
        observed = self.rows('SELECT safe_summary,raw_body_hex FROM live_transport_observations')[0]
        self.assertEqual(bytes.fromhex(observed['raw_body_hex']), partial_body)
        self.assertTrue(json.loads(observed['safe_summary'])['network_started'])

    def test_preconditions_refuse_before_credentials_and_reservation(self):
        for option in ('disabled', 'unauthorized', 'host_dispositions'):
            with self.subTest(option=option):
                # Each subcase owns its original request and database.
                self.database = self.directory / (option + '.sqlite')
                with tls.Peer(self.contexts['server'], tls.fixed_response(tls.response_body())) as peer:
                    result = self.invoke(peer, **{option: True})
                    self.assertEqual(peer.accepted, 0)
                self.assertTrue(result['threw'], result)
                self.assertEqual(result['credential_reads'], 0)
                self.assertEqual(result['budget']['attempts'], 0)
                self.assertTrue(result['accounting']['ready'])

    def test_explicit_cancellation_before_and_during_transport_remains_cancelled(self):
        marker = self.directory / 'peer-received-request'
        def partial(peer, connection):
            peer.request(connection)
            body = tls.response_body(tool=True)
            connection.sendall(tls.headers(len(body)) + body[:80])
            # This stage signal is emitted only after the independent server
            # receives the complete actual request and starts the reply.
            marker.write_text('complete request received; partial reply sent\n')
            peer.stop.wait(30)
        for before in (True, False):
            with self.subTest(before_transport=before):
                self.database = self.directory / ('pre-cancel.sqlite' if before else 'in-flight-cancel.sqlite')
                options = {'pre_cancel': True} if before else {'cancel_marker': str(marker)}
                with tls.Peer(self.contexts['server'], partial) as peer:
                    result = self.invoke(peer, **options)
                    self.assertEqual(len(peer.requests), 0 if before else 1)
                self.assertEqual(result['reports'][0]['stop_reason'], 'cancelled', result)
                self.assertEqual(result['task_count'], 0)
                self.assertEqual(result['credential_reads'], 0 if before else 1)
                self.assertEqual(result['budget']['attempts'], 0 if before else 1)
                self.assertEqual(result['accounting']['ready'], before)
                if not before:
                    self.assertTrue(result['cancellation_marker_observed'])
                    self.assertFalse(result['cancellation_marker_error'])
                    response = json.loads(self.rows('SELECT response FROM call_attempts')[0]['response'])
                    self.assertEqual(response['tool_calls'], [])
                    self.assertIsNone(response['usage']['cost_micro_usd'])
                    observed = json.loads(self.rows('SELECT safe_summary FROM live_transport_observations')[0]['safe_summary'])
                    self.assertTrue(observed['network_started'])

    def test_live_accounting_status_preserves_all_unknown_and_disputed_boundaries(self):
        result = self.invoke(action='accounting')
        self.assertTrue(result['empty']['ready'])
        self.assertFalse(result['pending']['ready'])
        self.assertEqual(result['pending']['blocked_attempts'][0]['unsettled'], 1)
        self.assertFalse(result['unknown']['ready'])
        self.assertFalse(result['cache_unknown']['ready'])
        for dimension in ('input', 'output', 'cost'):
            self.assertEqual(result['zero_budget']['unknown_' + dimension], 0)
            self.assertEqual(result['zero_budget']['reserved_' + dimension], 0)
        self.assertTrue(result['complete']['ready'])
        self.assertFalse(result['disputed']['ready'])
        self.assertEqual(result['disputed']['blocked_attempts'][0]['disputed'], 1)
        self.assertFalse(result['over_reserved']['ready'])
        self.assertEqual(result['over_reserved']['blocked_attempts'][0]['over_reserved'], 1)
        self.assertFalse(result['over_reserved']['budget_within_limits'])
        self.assertNotIn('PRIVATE_FIXTURE_SOURCE', json.dumps(result))
        self.assert_private_safe(result)

    def test_transport_response_cannot_be_swapped_under_original_model_decision(self):
        model = ModelPeer(self.directory)
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True)
        self.assertEqual(result['reports'][0]['stop_reason'], 'quiescent', result)
        with closing(sqlite3.connect(self.database)) as db, db:
            row = db.execute("SELECT attempt_id,response FROM live_transport_observations WHERE json_extract(response,'$.tool_calls[0].name')='accept_candidate'").fetchone()
            changed = json.loads(row[1])
            changed['tool_calls'][0]['arguments']['passed'] = False
            db.execute('DROP TRIGGER live_observation_no_update')
            db.execute('UPDATE live_transport_observations SET response=? WHERE attempt_id=?', (json.dumps(changed), row[0]))
            self.assertEqual(db.execute("SELECT json_extract(response,'$.tool_calls[0].arguments.passed') FROM live_transport_observations WHERE attempt_id=?", (row[0],)).fetchone()[0], 0)
        with self.assertRaisesRegex(e.InvalidEvidence, 'original transport response'):
            self.origin(result['request_id'], 4)


if __name__ == '__main__':
    unittest.main(verbosity=2, testRunner=ImmediateTextRunner)
