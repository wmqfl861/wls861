#!/usr/bin/env python3
"""Independent signed-billing and logical-agent CLI integration.

The endpoint, Ed25519 issuer, credentials, TLS CA, task responses and files are
disposable synthetic fixtures. Every model invocation uses the real CLI,
original Store, HTTPS transport and signed billing verifier. The billing issuer
uses OpenSSL, independently of the production verifier. These tests establish
engineering evidence, never provider identity or comparative model quality.
"""
from __future__ import annotations

from contextlib import closing
import copy
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import sqlite3
import subprocess
import sys
import tempfile
import threading
import time
import unittest

if len(sys.argv) < 3:
    raise SystemExit('usage: test_billing_baseline_e2e.py WLS SOURCE [unittest options]')
BINARY, SOURCE = (Path(value).resolve() for value in sys.argv[1:3])
TEST_ARGUMENTS = sys.argv[3:]
sys.argv = [sys.argv[0], str(BINARY), str(SOURCE)]
spec = importlib.util.spec_from_file_location('independent_workflow_fixture',
                                            SOURCE / 'tests/test_live_workflow.py')
workflow = importlib.util.module_from_spec(spec)
spec.loader.exec_module(workflow)
tls, evaluator = workflow.tls, workflow.e
sys.argv = [sys.argv[0], *TEST_ARGUMENTS]

REFERENCE = 'WLS_SYNTHETIC_INDEPENDENT_BILLING_KEY_41BD'
BEARER = 'WLS_SYNTHETIC_INDEPENDENT_BEARER_41BD'
PRIVATE_ERROR = 'WLS_PRIVATE_INDEPENDENT_HTTP_ERROR_41BD'
MODEL_REF = 'independent-loopback'
IDENTITY = 'single-agent-role-broker-v1'
ISSUER = 'independent-local-tls-issuer'
ACCOUNT = 'independent-local-tls-account'
KEY_ID = 'independent-ed25519-key'
CONTEXT_WINDOW = 262144
USAGE = {'input_tokens': 9, 'output_tokens': 3, 'cache_read_tokens': 2,
         'cache_write_tokens': 1, 'cost_micro_usd': 17}


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True,
                      separators=(',', ':'), allow_nan=False).encode('utf-8')


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def write_json(path, value):
    path.write_bytes(canonical(value))
    path.chmod(0o600)
    return path


def rows(database, query, parameters=()):
    with closing(sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)) as connection:
        connection.row_factory = sqlite3.Row
        return [dict(row) for row in connection.execute(query, parameters)]


def snapshot(database):
    """Compare all original rows without printing private fixture evidence."""
    with closing(sqlite3.connect(database.as_uri() + '?mode=ro', uri=True)) as connection:
        names = [row[0] for row in connection.execute(
            "SELECT name FROM sqlite_schema WHERE type='table' "
            "AND name NOT LIKE 'sqlite_%' ORDER BY name")]
        result = {}
        for name in names:
            values = sorted(connection.execute('SELECT * FROM "' + name.replace('"', '""') + '"'), key=repr)
            result[name] = {'rows': len(values),
                            'sha256': hashlib.sha256(repr(values).encode()).hexdigest()}
        return result


class SignedEndpoint:
    def __init__(self, directory):
        self.directory = directory
        self.private_key = directory / 'billing-private.pem'
        subprocess.run(['openssl', 'genpkey', '-algorithm', 'ED25519', '-out', str(self.private_key)],
                       capture_output=True, check=True, timeout=15)
        self.private_key.chmod(0o600)
        public = subprocess.run(['openssl', 'pkey', '-in', str(self.private_key),
                                 '-pubout', '-outform', 'DER'], capture_output=True, check=True,
                                timeout=15).stdout
        if len(public) != 44 or public[:12] != bytes.fromhex('302a300506032b6570032100'):
            raise AssertionError('unexpected Ed25519 SubjectPublicKeyInfo')
        self.public_key_hex = public[12:].hex()
        self.models, self.records = {}, {}
        self.sequence = 0
        self.lock = threading.Lock()

    def register(self, request_id, directory, mode, receipt_mode='valid'):
        model = workflow.ModelPeer(directory, mode)
        model.receipt_mode = receipt_mode
        self.models[request_id] = model
        self.records[request_id] = []
        return model

    def signed_receipt(self, payload, number):
        message = self.directory / f'signing-input-{number}.bin'
        message.write_bytes(b'wls-signed-billing-receipt-v1\n' + canonical(payload))
        message.chmod(0o600)
        try:
            signature = subprocess.run(['openssl', 'pkeyutl', '-sign', '-rawin',
                '-inkey', str(self.private_key), '-in', str(message)],
                capture_output=True, check=True, timeout=15).stdout
        finally:
            message.unlink()
        if len(signature) != 64:
            raise AssertionError('unexpected Ed25519 signature size')
        return {'schema_version': 1, 'kind': 'wls-signed-billing-receipt-v1',
                'payload': payload, 'signature_hex': signature.hex()}

    def __call__(self, peer, connection):
        observed = peer.request(connection)
        body = json.loads(observed['body'])
        contract = json.loads(json.loads(body['input'][0]['content'])['task_contract'])
        request_id = contract['binding']['request_id']
        model = self.models[request_id]
        mutation = getattr(model, 'on_first_request', None)
        if mutation is not None:
            model.on_first_request = None
            mutation()
        http_status = 429 if model.receipt_mode == 'http_429_missing' else (
            503 if model.receipt_mode == 'http_503' else 200)
        if http_status == 200:
            name, arguments = model.choose(body)
            if name not in {tool['name'] for tool in body['tools']}:
                raise AssertionError('synthetic response requested an undeclared tool')
        with self.lock:
            self.sequence += 1
            number = self.sequence
        call_id = f'independent-call-{number}'
        if http_status == 200:
            model.actions.append(name)
            value = json.loads(tls.response_body(tool=True))
            value['id'] = f'resp_independent_{number}'
            value['output'][0].update(call_id=call_id, name=name,
                                      arguments=json.dumps(arguments, ensure_ascii=False))
        else:
            value = {'error': {'code': 'synthetic_http_failure', 'message': PRIVATE_ERROR}}
        usage = dict(USAGE)
        if model.receipt_mode == 'partial_unknown':
            value['usage']['input_tokens_details'].pop('cache_write_tokens', None)
            usage['cache_write_tokens'] = None
        if model.receipt_mode == 'over_reserved':
            usage['cost_micro_usd'] = 101
        encoded = tls.encoded(value)
        challenge = observed['headers'].get(b'x-wls-billing-challenge', b'').decode('ascii')
        if len(challenge) != 64 or any(char not in '0123456789abcdef' for char in challenge):
            raise AssertionError('frozen signed billing did not emit its unique challenge')
        payload = {'trust_domain': 'local_tls_fixture', 'issuer': ISSUER,
            'account_id': ACCOUNT, 'key_id': KEY_ID, 'receipt_id': f'independent-receipt-{number}',
            'revision': 1, 'billing_challenge': challenge,
            'request_body_sha256': hashlib.sha256(observed['body']).hexdigest(),
            'response_body_sha256': hashlib.sha256(encoded).hexdigest(),
            'provider_response_id': value.get('id'), 'provider_model': body['model'],
            'token_unit': 'tokens', 'cost_unit': 'micro_usd', 'usage': usage}
        if model.receipt_mode == 'wrong_challenge':
            payload['billing_challenge'] = '0' * 64
        if model.receipt_mode == 'wrong_response_hash':
            payload['response_body_sha256'] = '0' * 64
        receipt = self.signed_receipt(payload, number)
        if model.receipt_mode == 'bad_signature':
            receipt['signature_hex'] = '0' * 128
        receipt_bytes = canonical(receipt)
        extra = b'X-WLS-Usage-Receipt: ' + receipt_bytes + b'\r\n'
        if model.receipt_mode in ('missing', 'http_429_missing'):
            extra = b''
        elif model.receipt_mode == 'duplicate_header':
            extra += extra
        self.records[request_id].append({'receipt': receipt, 'request': observed['body'],
                                         'response': encoded, 'wire_input': body})
        connection.sendall(tls.headers(len(encoded), status=http_status, extra=extra) + encoded)


class BillingBaselineIntegration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        previous_umask = os.umask(0o077)
        cls.addClassCleanup(os.umask, previous_umask)
        tls.HttpsTransport.setUpClass.__func__(cls)
        temporary = tempfile.TemporaryDirectory(prefix='wls-billing-baseline-independent-')
        cls.addClassCleanup(temporary.cleanup)
        cls.directory = Path(temporary.name)
        cls.directory.chmod(0o700)
        evidence_directory = os.environ.get('WLS_BILLING_BASELINE_EVIDENCE_DIR')
        if evidence_directory:
            destination = Path(evidence_directory).absolute()

            def preserve_evidence():
                shutil.copytree(cls.directory, destination,
                                ignore=shutil.ignore_patterns('billing-private.pem', 'signing-input-*.bin'))
                destination.chmod(0o700)
                print('INDEPENDENT_EVIDENCE_DIRECTORY ' + str(destination))

            cls.addClassCleanup(preserve_evidence)
        cls.endpoint = SignedEndpoint(cls.directory)
        cls.peer = tls.Peer(cls.contexts['server'], cls.endpoint)
        cls.addClassCleanup(cls.peer.close)
        cls.functions = cls.directory / 'functions'
        shutil.copytree(SOURCE / 'config/examples/functions', cls.functions)
        cls.environment = dict(os.environ)
        cls.environment[REFERENCE] = BEARER
        seed = cls.directory / 'initial.sqlite'
        initial = cls.command('submit', '--db', seed, '--key', 'initial',
                              '--text-file', SOURCE / 'tests/fixtures/s1_request.txt')
        cls.system = json.loads(rows(seed, 'SELECT config_snapshot FROM requests WHERE request_id=?',
                                     (initial['request']['request_id'],))[0]['config_snapshot'])['system']
        cls.profile = cls.model_profile()
        cls.run_index = []
        cls.counter = 0
        seed_case = cls.admit_case('plan-seed', 0, 'multi_agent', register=False)
        with evaluator.database(seed_case['database']) as database:
            cls.plan = evaluator.make_plan(database, seed_case['request'], MODEL_REF,
                {'mode': 'provider_default'}, 'synthetic', 'model-origin-v1', IDENTITY)
        if cls.plan['schema_version'] != 3:
            raise AssertionError('the independent paired plan must use the new v3 identity contract')
        write_json(cls.directory / 'independent-plan.json', cls.plan)
        write_json(cls.directory / 'independent-run-index.json', cls.run_index)

    @classmethod
    def tearDownClass(cls):
        tls.HttpsTransport.tearDownClass.__func__(cls)

    @classmethod
    def command(cls, *arguments, error=False, timeout=150):
        completed = subprocess.run([str(BINARY), *(str(value) for value in arguments),
            '--source-root', str(SOURCE)], cwd=cls.directory, env=cls.environment,
            capture_output=True, text=True, encoding='utf-8', timeout=timeout, check=False)
        for private in (BEARER, PRIVATE_ERROR, 'BEGIN PRIVATE KEY', 'signature_hex', 'raw_body_hex', 'raw_headers_hex'):
            if private in completed.stdout + completed.stderr:
                raise AssertionError('public CLI output exposed private fixture evidence')
        if error:
            if completed.returncode == 0 or completed.stdout:
                raise AssertionError('expected a preflight rejection without successful stdout')
            return json.loads(completed.stderr)
        if completed.returncode != 0 or completed.stderr:
            raise AssertionError(completed.stderr or completed.stdout)
        return json.loads(completed.stdout)

    @classmethod
    def model_profile(cls):
        provider = json.loads((SOURCE / 'config/examples/providers/responses-offline.json').read_text())
        provider.update(base_url=f'https://localhost:{cls.peer.port}', api_key_env=REFERENCE,
            model_ref=MODEL_REF, profile_id='independent-signed-billing-loopback',
            billing_challenge='wls-billing-challenge-v1')
        provider['capabilities']['reasoning_efforts'] = []
        provider['capabilities']['reasoning_modes'] = ['provider_default', 'off']
        # The same synthetic model window is frozen for the plan and all cells.
        # Full history remains subject to the unchanged production capacity guard.
        provider['capabilities']['context_window'] = CONTEXT_WINDOW
        return {'adapter': 'openai-responses', 'adapter_version': '1',
            'protocol': 'openai-responses-v1', 'model': provider['model'],
            'capabilities': {'streaming': 'supported', 'tool_calling': 'supported',
                'parallel_tool_calls': 'supported', 'reasoning_modes': ['provider_default', 'off'],
                'context_window': CONTEXT_WINDOW, 'max_output_tokens': 4096, 'cache_mode': 'none'},
            'responses': {'provider_profile': provider, 'transport': {
                'ca_bundle_pem': (cls.certificates / 'ca.pem').read_text(), 'resolved_ipv4': '127.0.0.1',
                'max_header_bytes': 65536, 'max_header_line_bytes': 8192,
                'max_total_response_bytes': 3 * 1024 * 1024, 'poll_interval_ms': 25},
                'stream': False, 'execution_kind': 'local_tls',
                'capability_evidence_source': 'independent-local-tls-engineering-fixture-v1',
                'capability_evidence_sha256': hashlib.sha256(b'independent synthetic capabilities only').hexdigest()}}

    @classmethod
    def admit_case(cls, label, task, condition, *, receipt_mode='valid', register=True, database=None, profile=None):
        directory = cls.directory / label
        directory.mkdir(mode=0o700)
        db = database or directory / 'runtime.sqlite'
        system = copy.deepcopy(cls.system)
        profile = cls.profile if profile is None else profile
        system['models'][MODEL_REF] = copy.deepcopy(profile)
        system['defaults']['model_ref'] = MODEL_REF
        system['model_calls']['real_calls_enabled'] = True
        system['context']['request_format'] = 'structured-v2'
        system['context'].pop('logical_agent', None)
        if condition == 'single_agent':
            system['context']['logical_agent'] = {'mode': IDENTITY, 'max_attempts': 128,
                                                   'max_history_bytes': 524288}
        configuration = write_json(directory / 'system.json', system)
        material = directory / 'material.txt'
        material.write_text(evaluator.TASKS[task][1], encoding='utf-8')
        constraints = write_json(directory / 'constraints.json', [])
        result = cls.command('submit', '--db', db, '--key', label, '--text-file', material,
            '--constraints-file', constraints, '--config', configuration, '--functions-dir', cls.functions,
            '--max-attempts', 64, '--input-budget', 4000000, '--output-budget', 1000000,
            '--cost-budget-micro-usd', 100000)
        request = result['request']['request_id']
        status = cls.command('live-status', '--db', db, '--request', request)
        policy = {'schema_version': 1, 'kind': 'wls-billing-trust-policy-v1',
            'trust_domain': 'local_tls_fixture', 'request_id': request,
            'config_snapshot_sha256': status['config_snapshot_sha256'], 'issuer': ISSUER,
            'account_id': ACCOUNT, 'public_keys': {KEY_ID: cls.endpoint.public_key_hex},
            'models': {MODEL_REF: digest(profile)}}
        policy_file = write_json(directory / 'billing-policy.json', policy)
        workspaces = {}
        for node in ('alpha', 'beta') if task == 1 else ('alpha',):
            path = directory / node
            path.mkdir(mode=0o700)
            workspaces[node] = str(path)
        if task == 0:
            (Path(workspaces['alpha']) / 'seed.txt').write_bytes(b'alpha\nbeta\n')
        mapping = write_json(directory / 'workspaces.json', workspaces)
        case = {'directory': directory, 'database': db, 'request': request, 'task': task,
            'condition': condition, 'policy': policy_file, 'authorization': directory / 'authorization.json',
            'mapping': mapping, 'workspaces': workspaces, 'reports': []}
        if register:
            mode = ('roundtrip', 'amend', 'rework', 'exhausted', 'complete')[task]
            case['model'] = cls.endpoint.register(request, directory, mode, receipt_mode)
        return case

    @classmethod
    def authorization(cls, case, max_calls):
        status = cls.command('live-status', '--db', case['database'], '--request', case['request'],
                             '--billing-policy-file', case['policy'])
        authorization = status['authorization_template']
        if authorization['schema_version'] != 2 or authorization['kind'] != 'wls-live-workflow-authorization-v2':
            raise AssertionError('billing execution requires the explicit v2 authorization')
        if authorization['billing_policy_sha256'] != hashlib.sha256(case['policy'].read_bytes()).hexdigest():
            raise AssertionError('authorization must bind the captured policy bytes')
        authorization.update(network_enabled=True, reserved_cost_micro_usd=100,
            max_calls=max_calls, max_worker_calls_per_task=16, max_batch_duration_ms=60000)
        write_json(case['authorization'], authorization)
        return authorization

    @classmethod
    def pump(cls, case, *, max_calls=6, command=None, policy=None, authorization=None, error=False):
        cls.authorization(case, max_calls)
        selected = command or ('pump-single-live' if case['condition'] == 'single_agent' else 'pump-live')
        report = cls.command(selected, '--db', case['database'], '--request', case['request'],
            '--authorization-file', authorization or case['authorization'],
            '--billing-policy-file', policy or case['policy'], '--workspaces-file', case['mapping'], error=error)
        if not error:
            case['reports'].append(report)
        return report

    def finish_case(self, case):
        for _ in range(8):
            report = self.pump(case)
            self.assertNotIn(report['stop_reason'], ('error', 'deadline', 'cancelled', 'needs_accounting'),
                             {key: report.get(key) for key in ('stop_reason', 'blocked', 'pending')})
            if case['task'] == 3:
                questions = rows(case['database'], "SELECT q.question_id FROM questions q "
                    "JOIN question_runtime qr USING(question_id) WHERE q.request_id=? "
                    "AND qr.status='OPEN' AND q.holder='user'", (case['request'],))
                if questions:
                    return
            else:
                tasks = rows(case['database'], 'SELECT status FROM tasks WHERE request_id=?', (case['request'],))
                jobs = rows(case['database'], 'SELECT status FROM experience_jobs WHERE request_id=?', (case['request'],))
                if tasks and all(row['status'] == 'ACCEPTED' for row in tasks) and jobs and all(row['status'] == 'DONE' for row in jobs):
                    return
        self.fail('finite independent CLI batches did not reach the fixed scenario terminal condition')

    def publish(self, case):
        directory = case['directory'] / 'deliveries'
        directory.mkdir(mode=0o700)
        published = self.command('publish-delivery', '--db', case['database'], '--request', case['request'],
                                 '--delivery-dir', directory)
        delivery = published['delivery']
        self.command('ack-delivery', '--db', case['database'], '--request', case['request'],
            '--delivery', delivery['delivery_id'], '--delivery-dir', directory,
            '--manifest-sha256', delivery['manifest_sha256'],
            '--requirements-revision', delivery['requirements_revision'], '--plan-revision', delivery['plan_revision'],
            '--recipient', 'independent-fixture-owner', '--key', 'independent-exact-receipt')

    def check_original_accounting(self, case):
        attempts = rows(case['database'], 'SELECT attempt_id,response,cost_micro_usd,settlement_sha256 FROM call_attempts WHERE request_id=? ORDER BY rowid',
                        (case['request'],))
        self.assertTrue(attempts)
        self.assertTrue(all(row['cost_micro_usd'] is None and row['settlement_sha256'] for row in attempts))
        projections = rows(case['database'], 'SELECT p.* FROM live_usage_projection p JOIN call_attempts a USING(attempt_id) WHERE a.request_id=?',
                           (case['request'],))
        self.assertEqual(len(projections), len(attempts))
        self.assertTrue(all(row['cost_micro_usd'] == 17 and not row['disputed'] and not row['over_reserved'] for row in projections))
        reconciliations = rows(case['database'], 'SELECT r.source_kind FROM usage_reconciliations r JOIN call_attempts a USING(attempt_id) WHERE a.request_id=?',
                              (case['request'],))
        self.assertEqual(len(reconciliations), len(attempts))
        budget = self.command('budget', '--db', case['database'], '--request', case['request'])
        if 'budget' in budget:
            budget = budget['budget']
        self.assertEqual(budget['spent_cost'], 17 * len(attempts))
        self.assertEqual(budget['reserved_cost'], 0)
        self.assertEqual(budget['unknown_cost'], 0)
        wire = self.endpoint.records[case['request']]
        self.assertEqual(len(wire), len(attempts))
        challenges = [record['receipt']['payload']['billing_challenge'] for record in wire]
        self.assertEqual(len(challenges), len(set(challenges)))
        if case['condition'] == 'single_agent':
            previous = {}
            for attempt, record in zip(attempts, wire):
                received_items = [item for item in record['wire_input']['input']
                                  if item.get('type') == 'function_call']
                output_items = [item for item in record['wire_input']['input']
                                if item.get('type') == 'function_call_output']
                received = {item['call_id']: item for item in received_items}
                outputs = {item['call_id']: item for item in output_items}
                self.assertEqual(len(received_items), len(received),
                                 'the actual single-agent wire must not duplicate an original call ID')
                self.assertEqual(len(output_items), len(outputs),
                                 'the actual single-agent wire must not duplicate an original tool result ID')
                self.assertEqual(set(received), set(previous),
                                 'the actual single-agent wire must contain every original native turn exactly once')
                self.assertEqual(set(outputs), set(previous), 'the actual wire must pair every earlier tool result')
                for call_id, fact in previous.items():
                    self.assertEqual(received[call_id]['name'], fact['call']['name'])
                    self.assertEqual(json.loads(received[call_id]['arguments']), fact['call']['arguments'])
                    self.assertEqual(json.loads(outputs[call_id]['output']), fact['result'])
                response = json.loads(attempt['response'])
                original_call = response['tool_calls'][0]
                call_id = 'history_' + digest({'format': 'wls-history-tool-id-v1',
                    'attempt_id': attempt['attempt_id'], 'call_id': original_call['call_id']})
                feedback = rows(case['database'], 'SELECT result FROM model_tool_feedback WHERE attempt_id=?',
                                (attempt['attempt_id'],))[0]
                previous[call_id] = {'call': original_call, 'result': json.loads(feedback['result'])}

    def run_pair(self, task):
        for condition in ('multi_agent', 'single_agent'):
            with self.subTest(condition=condition):
                case = self.admit_case(evaluator.TASKS[task][0] + '-' + condition, task, condition)
                entry = {'task_id': evaluator.TASKS[task][0], 'condition': condition,
                    'database': str(case['database'].relative_to(self.directory)), 'request_id': case['request'],
                    'elapsed_ms': 0, 'plan_sha256': self.plan['plan_sha256']}
                self.run_index.append(entry)
                write_json(self.directory / 'independent-run-index.json', self.run_index)
                started = time.monotonic()
                try:
                    if task == 1:
                        self.pump(case, max_calls=5)
                        amendment = case['directory'] / 'amendment.txt'
                        amendment.write_text('Apply the explicit synthetic AMEND to alpha; retain beta under the current requirement.\n')
                        self.command('amend', '--db', case['database'], '--request', case['request'],
                                     '--text-file', amendment, '--constraints-file', case['directory'] / 'constraints.json')
                        self.pump(case, max_calls=3)
                    else:
                        # A separate CLI process resumes the durable logical agent.
                        self.pump(case, max_calls=2)
                    self.finish_case(case)
                    if task == 4:
                        self.publish(case)
                    self.check_original_accounting(case)
                finally:
                    entry['elapsed_ms'] = int((time.monotonic() - started) * 1000)
                    write_json(self.directory / 'independent-run-index.json', self.run_index)
                report = evaluator.build_report(self.plan, [entry], self.directory)
                write_json(case['directory'] / 'independent-cell-report.json', report)
                selected = next(row for row in report['rows'] if row['task_id'] == entry['task_id'] and row['condition'] == condition)
                self.assertEqual(selected['status'], 'passed', selected)
                self.assertFalse(report['provider_quality_acceptance_established'])
                self.assertFalse(report['general_quality_or_cost_advantage_established'])

    def test_01_original_native_roundtrip_pair(self):
        self.run_pair(0)

    def test_02_two_task_partial_amend_pair(self):
        self.run_pair(1)

    def test_03_original_rejected_rework_pair(self):
        self.run_pair(2)

    def test_04_exhausted_rework_owner_question_pair(self):
        self.run_pair(3)

    def test_05_original_delivery_exact_receipt_pair(self):
        self.run_pair(4)

    def test_90_invalid_signed_sources_never_advance_business_or_repeat_send(self):
        for variant in ('bad_signature', 'wrong_challenge', 'wrong_response_hash', 'duplicate_header'):
            with self.subTest(variant=variant):
                case = self.admit_case('invalid-' + variant, 4, 'multi_agent', receipt_mode=variant)
                report = self.pump(case)
                self.assertIn(report['stop_reason'], ('error', 'needs_accounting'))
                self.assertEqual(len(self.endpoint.records[case['request']]), 1)
                self.assertEqual(rows(case['database'], 'SELECT * FROM usage_reconciliations'), [])
                self.assertEqual(rows(case['database'], 'SELECT * FROM tasks'), [])
                self.assertEqual(rows(case['database'], 'SELECT * FROM ops'), [])
                again = self.pump(case)
                self.assertEqual(again['stop_reason'], 'needs_accounting')
                self.assertEqual(len(self.endpoint.records[case['request']]), 1)

    def test_91_partial_unknown_and_over_reserved_remain_stopped(self):
        for variant in ('partial_unknown', 'over_reserved'):
            with self.subTest(variant=variant):
                case = self.admit_case('accounting-' + variant, 4, 'multi_agent', receipt_mode=variant)
                report = self.pump(case)
                self.assertEqual(report['stop_reason'], 'needs_accounting')
                self.assertEqual(len(self.endpoint.records[case['request']]), 1)
                projection = rows(case['database'], 'SELECT * FROM live_usage_projection')[0]
                if variant == 'partial_unknown':
                    self.assertIsNone(projection['cache_write_tokens'])
                    self.assertEqual(projection['cost_micro_usd'], 17)
                else:
                    self.assertEqual(projection['cost_micro_usd'], 101)
                    self.assertEqual(projection['over_reserved'], 1)
                self.pump(case)
                self.assertEqual(len(self.endpoint.records[case['request']]), 1)

    def test_92_explicit_reconciliation_is_accounting_only_and_exact_replay(self):
        case = self.admit_case('explicit-accounting', 4, 'multi_agent', receipt_mode='missing')
        self.assertEqual(self.pump(case)['stop_reason'], 'needs_accounting')
        attempt = rows(case['database'], 'SELECT * FROM call_attempts')[0]
        original = {key: attempt[key] for key in ('response', 'settlement_sha256', 'cost_micro_usd', 'accepted')}
        receipt = write_json(case['directory'] / 'late-receipt.json', self.endpoint.records[case['request']][0]['receipt'])
        arguments = ('reconcile-billing', '--db', case['database'], '--request', case['request'],
            '--attempt', attempt['attempt_id'], '--billing-policy-file', case['policy'], '--receipt-file', receipt,
            '--expected-version', 1, '--key', 'independent-late-accounting')
        first = self.command(*arguments)
        after = snapshot(case['database'])
        repeated = self.command(*arguments)
        self.assertFalse(first['replayed'])
        self.assertTrue(repeated['replayed'])
        self.assertEqual({key: value for key, value in repeated.items() if key != 'replayed'},
                         {key: value for key, value in first.items() if key != 'replayed'})
        self.assertFalse(repeated['business_authority_restored'])
        self.assertEqual(repeated['model_calls'], 0)
        self.assertEqual(repeated['network_calls'], 0)
        self.assertEqual(snapshot(case['database']), after)
        settled = rows(case['database'], 'SELECT * FROM call_attempts')[0]
        self.assertEqual({key: settled[key] for key in original}, original)
        self.assertEqual(len(self.endpoint.records[case['request']]), 1)
        self.assertEqual(len(rows(case['database'], 'SELECT * FROM usage_reconciliations')), 1)
        self.assertEqual(rows(case['database'], 'SELECT * FROM ops'), [])
        self.assertEqual(len(rows(case['database'], 'SELECT * FROM model_tool_feedback')), 1)

    def test_93_trust_inputs_inside_sibling_workspace_refuse_before_takeover(self):
        first = self.admit_case('bound-sibling', 4, 'multi_agent')
        self.pump(first, max_calls=3)
        self.assertTrue(rows(first['database'], 'SELECT * FROM ops'))
        second = self.admit_case('sibling-target', 4, 'multi_agent', receipt_mode='missing', database=first['database'])
        self.assertEqual(self.pump(second)['stop_reason'], 'needs_accounting')
        workspace = Path(first['workspaces']['alpha'])
        policy = workspace / 'other-request-policy.json'
        authorization = workspace / 'other-request-authorization.json'
        receipt = workspace / 'other-request-receipt.json'
        shutil.copyfile(second['policy'], policy)
        self.authorization(second, 4)
        shutil.copyfile(second['authorization'], authorization)
        write_json(receipt, self.endpoint.records[second['request']][0]['receipt'])
        before = snapshot(first['database'])
        sends = len(self.peer.requests)
        self.pump(second, policy=policy, error=True)
        self.assertEqual(snapshot(first['database']), before)
        self.pump(second, authorization=authorization, error=True)
        self.assertEqual(snapshot(first['database']), before)
        attempt = rows(second['database'], 'SELECT attempt_id FROM call_attempts WHERE request_id=?', (second['request'],))[0]['attempt_id']
        self.command('reconcile-billing', '--db', second['database'], '--request', second['request'],
            '--attempt', attempt, '--billing-policy-file', second['policy'], '--receipt-file', receipt,
            '--expected-version', 1, '--key', 'sibling-path-refusal', error=True)
        self.assertEqual(snapshot(first['database']), before)
        self.assertEqual(len(self.peer.requests), sends)
        allowed = first['directory'] / 'alpha-controls'
        allowed.mkdir(mode=0o700)
        allowed_policy = allowed / 'policy.json'
        allowed_receipt = allowed / 'receipt.json'
        shutil.copyfile(second['policy'], allowed_policy)
        shutil.copyfile(receipt, allowed_receipt)
        status = self.command('live-status', '--db', second['database'], '--request', second['request'],
                              '--billing-policy-file', allowed_policy)
        self.assertEqual(status['authorization_template']['billing_policy_sha256'],
                         hashlib.sha256(allowed_policy.read_bytes()).hexdigest())
        self.assertEqual(snapshot(first['database']), before)
        self.command('reconcile-billing', '--db', second['database'], '--request', second['request'],
            '--attempt', attempt, '--billing-policy-file', allowed_policy, '--receipt-file', allowed_receipt,
            '--expected-version', 1, '--key', 'allowed-prefix-sibling')
        self.assertEqual(len(self.peer.requests), sends)

    def test_94_opposite_runner_mode_cannot_relabel_frozen_identity(self):
        for condition, wrong in (('multi_agent', 'pump-single-live'), ('single_agent', 'pump-live')):
            with self.subTest(condition=condition):
                case = self.admit_case('wrong-mode-' + condition, 4, condition)
                before = snapshot(case['database'])
                self.pump(case, command=wrong, error=True)
                self.assertEqual(snapshot(case['database']), before)
                self.assertEqual(self.endpoint.records[case['request']], [])

    def test_95_complete_http_error_can_reconcile_without_becoming_business_success(self):
        for variant in ('http_429_missing', 'http_503'):
            with self.subTest(variant=variant):
                case = self.admit_case('http-error-' + variant, 4, 'multi_agent', receipt_mode=variant)
                report = self.pump(case)
                self.assertIn(report['stop_reason'], ('error', 'needs_accounting'))
                attempt = rows(case['database'], 'SELECT * FROM call_attempts')[0]
                original = {key: attempt[key] for key in ('status', 'accepted', 'response', 'settlement_sha256', 'cost_micro_usd')}
                self.assertEqual(attempt['status'], 'ERROR')
                self.assertEqual(attempt['accepted'], 0)
                self.assertIsNone(attempt['cost_micro_usd'])
                if variant == 'http_429_missing':
                    self.assertEqual(rows(case['database'], 'SELECT * FROM usage_reconciliations'), [])
                    receipt = write_json(case['directory'] / 'late-error-receipt.json',
                                         self.endpoint.records[case['request']][0]['receipt'])
                    self.command('reconcile-billing', '--db', case['database'], '--request', case['request'],
                        '--attempt', attempt['attempt_id'], '--billing-policy-file', case['policy'],
                        '--receipt-file', receipt, '--expected-version', 1, '--key', 'independent-late-error')
                settled = rows(case['database'], 'SELECT * FROM call_attempts')[0]
                self.assertEqual({key: settled[key] for key in original}, original)
                projection = rows(case['database'], 'SELECT * FROM live_usage_projection')[0]
                self.assertEqual(projection['cost_micro_usd'], 17)
                self.assertEqual(len(rows(case['database'], 'SELECT * FROM usage_reconciliations')), 1)
                self.assertEqual(rows(case['database'], 'SELECT * FROM tasks'), [])
                self.assertEqual(rows(case['database'], 'SELECT * FROM ops'), [])
                self.assertEqual(len(self.endpoint.records[case['request']]), 1)

    def test_96_policy_identity_and_keys_use_the_once_captured_bytes(self):
        case = self.admit_case('captured-billing-policy', 4, 'multi_agent')
        original = case['policy'].read_bytes()
        original_hash = hashlib.sha256(original).hexdigest()
        changed = json.loads(original)
        changed['public_keys'][KEY_ID] = '0' * 64
        case['model'].on_first_request = lambda: write_json(case['policy'], changed)
        report = self.pump(case, max_calls=2)
        self.assertNotEqual(case['policy'].read_bytes(), original)
        self.assertEqual(report['billing_policy_sha256'], original_hash)
        self.assertEqual(len(self.endpoint.records[case['request']]), 2)
        self.assertEqual(len(rows(case['database'], 'SELECT * FROM usage_reconciliations')), 2)
        before = snapshot(case['database'])
        self.command('pump-live', '--db', case['database'], '--request', case['request'],
            '--authorization-file', case['authorization'], '--billing-policy-file', case['policy'],
            '--workspaces-file', case['mapping'], error=True)
        self.assertEqual(snapshot(case['database']), before)
        self.assertEqual(len(self.endpoint.records[case['request']]), 2)

    def test_97_live_evaluation_request_must_match_original_admission(self):
        case = self.admit_case('evaluation-original-input', 4, 'multi_agent')
        with evaluator.database(case['database']) as database:
            plans = [evaluator.make_plan(database, case['request'], MODEL_REF,
                {'mode': 'provider_default'}, 'synthetic', decision, identity)
                for decision, identity in ((None, None), ('model-origin-v1', None), ('model-origin-v1', IDENTITY))]
        self.pump(case, max_calls=64)
        self.publish(case)
        entry = {'task_id': evaluator.TASKS[4][0], 'condition': 'multi_agent',
                 'database': str(case['database']), 'request_id': case['request'], 'elapsed_ms': 1000}
        for plan in plans:
            original = evaluator.build_report(plan, [dict(entry, plan_sha256=plan['plan_sha256'])])['rows'][8]
            self.assertEqual(original['status'], 'passed', original)
        corrupted = case['directory'] / 'changed-original-input.sqlite'
        with closing(sqlite3.connect(case['database'])) as source, closing(sqlite3.connect(corrupted)) as target:
            source.backup(target)
        with closing(sqlite3.connect(corrupted)) as database, database:
            attempt, raw = database.execute('SELECT attempt_id,request_envelope FROM call_attempts '
                                            'ORDER BY rowid DESC LIMIT 1').fetchone()
            envelope = json.loads(raw)
            envelope['context']['input']['messages'].append({'role': 'user', 'content': 'unadmitted fixture input',
                'tool_calls': [], 'tool_call_id': '', 'source': None})
            envelope['context']['logical_input_sha256'] = digest(envelope['context']['input'])
            envelope['logical_request_sha256'] = digest({key: value for key, value in envelope.items()
                if key not in ('logical_request_sha256', 'wire_request_sha256')})
            database.execute('UPDATE call_attempts SET request_envelope=?,state_version=state_version+1 WHERE attempt_id=?',
                             (canonical(envelope).decode(), attempt))
        before = snapshot(corrupted)
        for plan in plans:
            with self.subTest(version=plan['schema_version']):
                run = dict(entry, database=str(corrupted), plan_sha256=plan['plan_sha256'])
                row = evaluator.build_report(plan, [run])['rows'][8]
                self.assertEqual(row['status'], 'invalid_evidence', row)
                self.assertIn('request admitted for original live transport', row['reason'])
        self.assertEqual(snapshot(corrupted), before)

    def test_97_evaluation_source_kind_must_match_original_frozen_settings(self):
        profile = copy.deepcopy(self.profile)
        # Valid historical JSON can list provider capability sets in any
        # order, while the original live settings serializer orders them.
        for capabilities in (profile['capabilities'], profile['responses']['provider_profile']['capabilities']):
            capabilities.update(reasoning_modes=['off', 'effort', 'provider_default'], reasoning_efforts=['low', 'high'])
        case = self.admit_case('evaluation-source-identity', 4, 'multi_agent', profile=profile)
        with evaluator.database(case['database']) as database:
            plans = [evaluator.make_plan(database, case['request'], MODEL_REF,
                {'mode': 'provider_default'}, 'synthetic', decision, identity)
                for decision, identity in ((None, None), ('model-origin-v1', None), ('model-origin-v1', IDENTITY))]
        self.pump(case, max_calls=64)
        self.publish(case)
        entry = {'task_id': evaluator.TASKS[4][0], 'condition': 'multi_agent',
                 'database': str(case['database']), 'request_id': case['request'], 'elapsed_ms': 1000}
        for plan in plans:
            original = evaluator.build_report(plan, [dict(entry, plan_sha256=plan['plan_sha256'])])['rows'][8]
            self.assertEqual(original['status'], 'passed', original)
            self.assertEqual(original['source_kinds'], {'local_tls': len(original['attempts'])})

        # Only disposable database copies are damaged. A changed source label
        # (even with rehashed settings) cannot replace the original admission.
        for rewrite_settings in (False, True):
            corrupted = case['directory'] / ('source-kind-' + str(rewrite_settings) + '.sqlite')
            with closing(sqlite3.connect(case['database'])) as source, closing(sqlite3.connect(corrupted)) as target:
                source.backup(target)
            with closing(sqlite3.connect(corrupted)) as database, database:
                database.execute('DROP TRIGGER live_execution_no_update')
                database.execute("UPDATE live_attempt_execution SET execution_kind='provider'")
                if rewrite_settings:
                    for attempt_id, raw in database.execute('SELECT attempt_id,settings FROM live_attempt_execution').fetchall():
                        settings = json.loads(raw)
                        settings['execution_kind'] = 'provider'
                        database.execute('UPDATE live_attempt_execution SET settings=?,settings_sha256=? WHERE attempt_id=?',
                            (canonical(settings).decode(), digest(settings), attempt_id))
                self.assertEqual(database.execute("SELECT count(*) FROM live_attempt_execution WHERE execution_kind<>'provider'").fetchone()[0], 0)
                database.execute("CREATE TRIGGER live_execution_no_update BEFORE UPDATE ON live_attempt_execution "
                                 "BEGIN SELECT RAISE(ABORT,'immutable live attempt identity'); END")
            before = snapshot(corrupted)
            for original_plan in plans:
                with self.subTest(version=original_plan['schema_version'], rehashed_settings=rewrite_settings):
                    changed_plan = copy.deepcopy(original_plan)
                    changed_plan['evidence_mode'] = 'provider'
                    changed_plan['plan_sha256'] = digest({key: value for key, value in changed_plan.items() if key != 'plan_sha256'})
                    run = dict(entry, database=str(corrupted), plan_sha256=changed_plan['plan_sha256'])
                    row = evaluator.build_report(changed_plan, [run])['rows'][8]
                    self.assertEqual(row['status'], 'invalid_evidence', row)
                    self.assertIn('original live execution', row['reason'])
            self.assertEqual(snapshot(corrupted), before)

    def test_98_run_index_labels_cannot_change_the_original_agent_condition(self):
        self.assertEqual(len(self.run_index), 10)
        for original in self.run_index:
            with self.subTest(task=original['task_id'], original_condition=original['condition']):
                changed = dict(original)
                changed['condition'] = 'single_agent' if original['condition'] == 'multi_agent' else 'multi_agent'
                before = snapshot(self.directory / original['database'])
                report = evaluator.build_report(self.plan, [changed], self.directory)
                selected = next(row for row in report['rows'] if row['task_id'] == changed['task_id']
                                and row['condition'] == changed['condition'])
                self.assertEqual(selected['status'], 'invalid_evidence', selected)
                self.assertEqual(snapshot(self.directory / original['database']), before)
                self.assertEqual(report['expected_cells'], 10)
                self.assertEqual(report['counts'].get('not_executed'), 9)

    def test_99_all_ten_fixed_cells_have_original_evidence(self):
        report = evaluator.build_report(self.plan, self.run_index, self.directory)
        write_json(self.directory / 'independent-paired-report.json', report)
        self.assertEqual(report['expected_cells'], 10)
        self.assertEqual(report['observed_cells'], 10)
        self.assertEqual(report['counts'], {'passed': 10}, report)
        self.assertTrue(report['paired_cells_complete'])
        self.assertFalse(report['provider_quality_acceptance_established'])
        self.assertFalse(report['general_quality_or_cost_advantage_established'])
        self.assertTrue(all(row['source_kinds'] == {'local_tls': len(row['attempts'])} for row in report['rows']))
        self.assertTrue(all(row['usage']['cost_micro_usd']['unknown_attempts'] == 0 for row in report['rows']))
        print('INDEPENDENT_PAIRED_RESULT ' + json.dumps({'contract': report['schema_version'],
            'counts': report['counts'], 'expected_cells': report['expected_cells'],
            'observed_cells': report['observed_cells'], 'provider_quality_acceptance_established': False,
            'rows': [{'task_id': row['task_id'], 'condition': row['condition'],
                      'attempts': len(row['attempts']), 'elapsed_ms': row['elapsed_ms'],
                      'known_cost_micro_usd': row['usage']['cost_micro_usd']['known_total']}
                     for row in report['rows']]}, sort_keys=True))


if __name__ == '__main__':
    unittest.main(verbosity=2)
