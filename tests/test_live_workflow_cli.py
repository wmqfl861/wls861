#!/usr/bin/env python3
"""Public live CLI authorization and accounting boundaries over loopback TLS.

All credentials, certificates, peer responses, files and databases are disposable.
No provider is contacted and the CLI has no synthetic billing import authority.
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
import time
import unittest

if len(sys.argv) != 3:
    raise SystemExit('usage: test_live_workflow_cli.py WLS SOURCE')
BINARY, SOURCE = (Path(value).resolve() for value in sys.argv[1:])
spec = importlib.util.spec_from_file_location('live_cli_tls_fixture', SOURCE / 'tests/test_https_transport.py')
tls = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tls)
sys.argv = [sys.argv[0]]
REFERENCE = 'WLS_SYNTHETIC_CLI_AUTH_5B37'
BEARER = 'WLS_SYNTHETIC_CLI_BEARER_5B37'
PRIVATE = 'PRIVATE_CLI_PROVIDER_ERROR_5B37'
# Frozen synthetic price table: reported input/output/cache-read, cache-write
# declared not applicable. Not a provider quote or signed billing evidence.
PRICING = {'format': 'wls-token-pricing-v1', 'currency': 'USD',
           'unit': 'micro_usd_per_million_tokens',
           'input_micro_usd_per_million_tokens': 1500000,
           'output_micro_usd_per_million_tokens': 4000000,
           'cache_read': {'applicability': 'reported', 'micro_usd_per_million_tokens': 250000},
           'cache_write': {'applicability': 'not_applicable'}}
# Independent expected estimate for 11/7/3 reported tokens with no cache write:
# ceil((8*1.5 + 3*0.25 + 7*4) micro USD) with Python integers only.
PRICED_UNIT_MICRO_USD = (8 * 1500000 + 3 * 250000 + 7 * 4000000 + 999999) // 1000000


def priced(body):
    """Replace the fixture usage with the frozen priced shape (no cache write)."""
    value = json.loads(body)
    value['usage'] = {'input_tokens': 11, 'input_tokens_details': {'cached_tokens': 3},
                      'output_tokens': 7, 'output_tokens_details': {'reasoning_tokens': 0},
                      'total_tokens': 18}
    return tls.encoded(value)


def without_constraints(value):
    if isinstance(value, dict):
        return {key: ([] if key in ('constraint_refs', 'constraints_verbatim') else without_constraints(item))
                for key, item in value.items()}
    if isinstance(value, list):
        return [without_constraints(item) for item in value]
    return value


def plan_response():
    fixture = without_constraints(json.loads((SOURCE / 'tests/fixtures/s15_fake_complete.json').read_text()))
    call = fixture['default'][0]['tool_calls'][0]
    for node in call['arguments']['nodes']:  # Derived v3 response; keep the frozen legacy fixture untouched.
        node['brief']['title'] = node['title']
    response = json.loads(tls.response_body(tool=True))
    response['output'][0].update(name=call['name'], arguments=json.dumps(call['arguments']))
    return tls.encoded(response)


class ScriptedPeer:
    """Unsigned priced peer: serves fixed tool answers, then HTTP 429."""

    def __init__(self):
        fixture = without_constraints(json.loads((SOURCE / 'tests/fixtures/s15_fake_complete.json').read_text()))
        self.calls = [fixture['default'][0]['tool_calls'][0]]
        for node in self.calls[0]['arguments']['nodes']:
            node['brief']['title'] = node['title']
        self.actions = []

    def __call__(self, peer, connection):
        observed = peer.request(connection)
        body = json.loads(observed['body'])
        if not self.calls:
            failure = json.dumps({'error': {'code': 'synthetic_priced_exhausted',
                                            'message': 'fixture script exhausted'}}).encode()
            connection.sendall(tls.headers(len(failure), status=429) + failure)
            return
        call = self.calls.pop(0)
        if call['name'] not in {tool['name'] for tool in body['tools']}:
            raise AssertionError(f"{call['name']} was not declared")
        self.actions.append(call['name'])
        value = json.loads(priced(tls.response_body(tool=True)))
        value['id'] = f'resp_priced_{len(self.actions)}'
        value['output'][0].update(call_id=f'pricing-call-{len(self.actions)}', name=call['name'],
                                  arguments=json.dumps(call['arguments']))
        encoded = tls.encoded(value)
        connection.sendall(tls.headers(len(encoded)) + encoded)


class LiveWorkflowCli(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        tls.HttpsTransport.setUpClass.__func__(cls)
        seed = tempfile.TemporaryDirectory(prefix='wls-live-cli-seed-')
        cls.addClassCleanup(seed.cleanup)
        directory = Path(seed.name)
        directory.chmod(0o700)
        database = directory / 'seed.sqlite'
        result = subprocess.run([str(BINARY), 'submit', '--db', str(database), '--key', 'seed',
            '--text-file', str(SOURCE / 'tests/fixtures/s1_request.txt'), '--source-root', str(SOURCE)],
            cwd=directory, capture_output=True, text=True, timeout=30, check=False)
        if result.returncode:
            raise AssertionError(result.stderr or result.stdout)
        with closing(sqlite3.connect(database)) as connection:
            cls.system = json.loads(connection.execute('SELECT config_snapshot FROM requests').fetchone()[0])['system']

    @classmethod
    def tearDownClass(cls):
        tls.HttpsTransport.tearDownClass.__func__(cls)

    def setUp(self):
        previous = os.umask(0o077)
        self.addCleanup(os.umask, previous)
        temporary = tempfile.TemporaryDirectory(prefix='wls-live-cli-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.directory.chmod(0o700)
        self.database = self.directory / 'runtime.sqlite'
        self.configuration = self.directory / 'system.json'
        self.functions = self.directory / 'functions'
        shutil.copytree(SOURCE / 'config/examples/functions', self.functions)
        self.authorization_file = self.directory / 'authorization.json'
        self.environment = dict(os.environ)
        self.environment.pop(REFERENCE, None)

    def invoke(self, *arguments, error=False, credential=None):
        environment = dict(self.environment)
        if credential is not None:
            environment[REFERENCE] = credential
        result = subprocess.run([str(BINARY), *(str(value) for value in arguments),
            '--source-root', str(SOURCE)], cwd=self.directory, env=environment,
            capture_output=True, text=True, encoding='utf-8', timeout=30, check=False)
        for private in (BEARER, PRIVATE, 'raw_body_hex', 'raw_headers_hex'):
            self.assertNotIn(private, result.stdout + result.stderr)
        if error:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(result.stdout, '')
            failure = json.loads(result.stderr)
            self.assertTrue(failure['error'])
            return failure
        self.assertEqual(result.returncode, 0, result.stderr or result.stdout)
        self.assertEqual(result.stderr, '')
        return json.loads(result.stdout)

    def admit(self, port, key='request-one', pricing=None, cost_budget=10000, retries=0, timeout_ms=None,
              total=None, challenge=False):
        system = copy.deepcopy(self.system)
        provider = json.loads((SOURCE / 'config/examples/providers/responses-offline.json').read_text())
        provider.update(base_url=f'https://localhost:{port}', api_key_env=REFERENCE,
                        model_ref='cli-loopback', profile_id='synthetic-cli-loopback')
        provider['capabilities']['reasoning_efforts'] = []
        provider['capabilities']['reasoning_modes'] = ['provider_default', 'off']
        if pricing is not None:
            provider['pricing'] = copy.deepcopy(pricing)
        if timeout_ms is not None:
            provider['limits']['max_timeout_ms'] = timeout_ms
        if challenge:
            provider['billing_challenge'] = 'wls-billing-challenge-v1'
        system['models']['cli-loopback'] = {
            'adapter': 'openai-responses', 'adapter_version': '1', 'protocol': 'openai-responses-v1',
            'model': provider['model'], 'capabilities': {'streaming': 'supported',
                'tool_calling': 'supported', 'parallel_tool_calls': 'supported',
                'reasoning_modes': ['provider_default', 'off'], 'context_window': 100000,
                'max_output_tokens': 4096, 'cache_mode': 'none'},
            'responses': {'provider_profile': provider, 'transport': {
                'ca_bundle_pem': (self.certificates / 'ca.pem').read_text(), 'resolved_ipv4': '127.0.0.1',
                'max_header_bytes': 65536, 'max_header_line_bytes': 8192,
                'max_total_response_bytes': 3 * 1024 * 1024, 'poll_interval_ms': 25},
                'stream': False, 'execution_kind': 'local_tls',
                'capability_evidence_source': 'synthetic-cli-loopback-v1',
                'capability_evidence_sha256': hashlib.sha256(b'synthetic CLI capabilities only').hexdigest()}}
        system['defaults']['model_ref'] = 'cli-loopback'
        system['model_calls']['real_calls_enabled'] = True
        system['model_calls']['max_paid_retries'] = retries
        if total is not None:
            system['model_calls'].update(runtime_policy='bounded-v1', max_total_calls=total)
        system['context']['request_format'] = 'structured-v3'
        system['context'].pop('model_projection', None); system['context'].pop('tool_policy', None)  # Keep the original priced fixture's frozen request size.
        system['context'].pop('worker_history', None)
        self.configuration.write_text(json.dumps(system))
        result = self.invoke('submit', '--db', self.database, '--key', key,
            '--text-file', SOURCE / 'tests/fixtures/s1_request.txt', '--config', self.configuration,
            '--functions-dir', self.functions, '--cost-budget-micro-usd', cost_budget)
        return result['request']['request_id']

    def authorize(self, request, reserved=100):
        status = self.invoke('live-status', '--db', self.database, '--request', request)
        value = status['authorization_template']
        value.update(network_enabled=True, reserved_cost_micro_usd=reserved,
                     max_calls=8, max_worker_calls_per_task=4, max_batch_duration_ms=10000)
        self.authorization_file.write_text(json.dumps(value))
        return value

    def pump(self, request, *extra, credential=BEARER, error=False, path=None):
        return self.invoke('pump-live', '--db', self.database, '--request', request,
            '--authorization-file', path or self.authorization_file, *extra,
            credential=credential, error=error)

    def rows(self, query, parameters=()):
        with closing(sqlite3.connect(self.database.as_uri() + '?mode=ro', uri=True)) as connection:
            connection.row_factory = sqlite3.Row
            return [dict(row) for row in connection.execute(query, parameters)]

    def snapshot(self):
        with closing(sqlite3.connect(self.database.as_uri() + '?mode=ro', uri=True)) as connection:
            tables = [row[0] for row in connection.execute(
                "SELECT name FROM sqlite_schema WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")]
            return {name: sorted(connection.execute('SELECT * FROM "' + name.replace('"', '""') + '"').fetchall(),
                                 key=repr) for name in tables}

    def test_status_is_read_only_and_template_grants_no_authority(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            before = self.snapshot()
            status = self.invoke('live-status', '--db', self.database, '--request', request)
            self.assertEqual(self.snapshot(), before)
            self.assertFalse(status['credentials_resolved'])
            self.assertTrue(status['accounting']['ready'])
            self.assertFalse(status['authorization_template']['network_enabled'])
            self.assertFalse(status['authorization_template']['provider_enabled'])
            self.assertEqual(status['authorization_template']['reserved_cost_micro_usd'], 0)
            self.assertEqual(peer.accepted, 0)

    def test_invalid_authorization_is_rejected_before_any_store_effect(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            valid = self.authorize(request)
            variants = {
                'disabled': {'network_enabled': False}, 'wrong_type': {'network_enabled': 1},
                'snapshot': {'config_snapshot_sha256': '0' * 64},
                'request': {'request_id': 'req_' + '0' * 32},
                'profile': {'models': {'cli-loopback': '0' * 64}},
                'provider_switch': {'provider_enabled': True}, 'kind': {'execution_kind': 'provider'},
                'extra': {'secret': 'not accepted'}, 'zero_cost': {'reserved_cost_micro_usd': 0},
                'over_cost': {'reserved_cost_micro_usd': 10001}, 'call_limit': {'max_calls': 65},
                'worker_limit': {'max_worker_calls_per_task': 17}, 'deadline': {'max_batch_duration_ms': 0}}
            before = self.snapshot()
            for name, change in variants.items():
                with self.subTest(name=name):
                    self.authorization_file.write_text(json.dumps({**valid, **change}))
                    self.pump(request, error=True)
                    self.assertEqual(self.snapshot(), before)
            missing = dict(valid)
            del missing['models']
            self.authorization_file.write_text(json.dumps(missing))
            self.pump(request, error=True)
            self.assertEqual(self.snapshot(), before)
            self.assertEqual(peer.accepted, 0)

    def test_nonregular_and_missing_authorization_are_bounded_preflight_failures(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            self.authorize(request)
            before = self.snapshot()
            link, fifo = self.directory / 'link.json', self.directory / 'fifo.json'
            link.symlink_to(self.authorization_file)
            os.mkfifo(fifo, 0o600)
            for path in (link, fifo, self.directory / 'absent.json'):
                with self.subTest(path=path.name):
                    self.pump(request, path=path, error=True)
                    self.assertEqual(self.snapshot(), before)
            self.assertEqual(peer.accepted, 0)

    def test_missing_and_invalid_credentials_never_reserve_or_send(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            self.authorize(request)
            for credential in (None, 'invalid credential with spaces'):
                with self.subTest(credential_present=credential is not None):
                    report = self.pump(request, credential=credential)
                    self.assertEqual(report['stop_reason'], 'error')
                    self.assertEqual(report['network_started_calls'], 0)
                    self.assertEqual(self.rows('SELECT * FROM call_attempts'), [])
            self.assertEqual(peer.accepted, 0)

    def test_original_plan_applies_then_unknown_cost_stops_and_repeat_cannot_resend(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            self.authorize(request)
            report = self.pump(request)
            self.assertEqual(report['stop_reason'], 'needs_accounting', report)
            self.assertEqual(report['network_started_calls'], 1)
            self.assertEqual(report['provider_calls'], 0)
            self.assertFalse(report['accounting']['ready'])
            self.assertFalse(report['cost_reservation_is_actual_usage'])
            attempts = self.rows('SELECT * FROM call_attempts')
            self.assertEqual(len(attempts), 1)
            self.assertIsNone(attempts[0]['cost_micro_usd'])
            self.assertEqual(attempts[0]['status'], 'COMPLETED')
            self.assertEqual(len(self.rows('SELECT * FROM model_tool_feedback')), 1)
            self.assertEqual(len(self.rows('SELECT * FROM tasks')), 1)
            repeat = self.pump(request, credential=None)
            self.assertEqual(repeat['stop_reason'], 'needs_accounting')
            self.assertEqual(repeat['calls'], [])
            self.assertEqual(self.rows('SELECT * FROM call_attempts'), attempts)
            self.assertEqual(len(peer.requests), 1)
            self.assertEqual(peer.requests[0]['headers'][b'authorization'], f'Bearer {BEARER}'.encode())

    def test_authorization_hash_uses_bytes_captured_before_execution(self):
        def mutate(peer, connection):
            peer.request(connection)
            self.authorization_file.write_text('{"changed_after_capture":true}')
            body = plan_response()
            connection.sendall(tls.headers(len(body)) + body)
        with tls.Peer(self.contexts['server'], mutate) as peer:
            request = self.admit(peer.port)
            self.authorize(request)
            original = self.authorization_file.read_bytes()
            report = self.pump(request)
            self.assertEqual(report['host_authorization_sha256'], hashlib.sha256(original).hexdigest())
            self.assertNotEqual(self.authorization_file.read_bytes(), original)
            self.assertEqual(report['stop_reason'], 'needs_accounting')
            self.assertEqual(len(peer.requests), 1)

    def test_http_failure_has_no_automatic_retry_or_private_echo(self):
        body = json.dumps({'error': {'message': PRIVATE}}).encode()
        with tls.Peer(self.contexts['server'], tls.fixed_response(body, status=429,
                      extra=b'Retry-After: 0\r\nX-Private: ' + PRIVATE.encode() + b'\r\n')) as peer:
            request = self.admit(peer.port, retries=2)  # Unresolved accounting still forbids retry.
            self.authorize(request)
            report = self.pump(request)
            self.assertEqual(report['network_started_calls'], 1)
            self.assertIn(report['stop_reason'], ('error', 'needs_accounting'))
            self.assertEqual(len(self.rows('SELECT * FROM call_attempts')), 1)
            self.assertEqual(self.rows('SELECT * FROM tasks'), [])
            self.assertEqual(len(peer.requests), 1)

    def test_current_configuration_cannot_change_frozen_endpoint(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as original, \
             tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as replacement:
            request = self.admit(original.port)
            self.authorize(request)
            changed = json.loads(self.configuration.read_text())
            changed['models']['cli-loopback']['responses']['provider_profile']['base_url'] = f'https://localhost:{replacement.port}'
            self.configuration.write_text(json.dumps(changed))
            report = self.pump(request)
            self.assertEqual(report['network_started_calls'], 1)
            self.assertEqual(len(original.requests), 1)
            self.assertEqual(replacement.accepted, 0)

    def test_bounded_paid_retries_preserve_independent_attempts_and_feedback(self):
        for status, failures, expected in ((429, 1, 2), (429, 3, 3),
                                          *((code, 1, 2) for code in (408, 500, 502, 503, 504)),
                                          *((code, 1, 1) for code in (400, 401, 403, 404, 422))):
            with self.subTest(status=status, failures=failures):
                self.database = self.directory / f'retry-{status}-{failures}.sqlite'
                observed = []
                def respond(peer, connection):
                    peer.request(connection); observed.append(time.monotonic())
                    failed = len(observed) <= failures
                    body = b'{}' if failed else priced(plan_response())
                    extra = b'Retry-After: 0\r\n' if status == 429 and failures == 1 else b''
                    connection.sendall(tls.headers(len(body), status=status if failed else 200, extra=extra) + body)
                with tls.Peer(self.contexts['server'], respond) as peer:
                    request = self.admit(peer.port, pricing=PRICING, retries=2, cost_budget=1000000)
                    auth = self.authorize(request)
                    auth.update(max_calls=4 if failures == 3 else 2, max_batch_duration_ms=25000)
                    self.authorization_file.write_text(json.dumps(auth))
                    report = self.pump(request)
                    self.assertEqual(len(peer.requests), expected, report)
                    version = json.loads(subprocess.check_output([str(BINARY), '--version']))['version']
                    self.assertEqual(peer.requests[0]['headers'][b'user-agent'], f'wls861/{version}'.encode())
                attempts = self.rows('SELECT * FROM call_attempts ORDER BY rowid')
                feedback = self.rows('SELECT * FROM model_tool_feedback ORDER BY rowid')
                self.assertEqual(len(attempts), expected)
                self.assertEqual(len(self.rows('SELECT * FROM live_send_claims')), expected)
                self.assertEqual(len(feedback), expected)
                self.assertTrue(report['accounting']['ready'])
                self.assertTrue(all(row['settlement_sha256'] for row in attempts))
                budget = self.invoke('budget', '--db', self.database, '--request', request)
                self.assertEqual(budget['attempts'], expected)
                self.assertEqual(budget['spent_cost'], sum(row['reserved_cost'] for row in attempts[:failures]) +
                                 (PRICED_UNIT_MICRO_USD if expected > failures else 0))
                self.assertEqual(budget['reserved_cost'], 0)
                for i, row in enumerate(feedback):
                    result = json.loads(row['result'])
                    self.assertEqual(result.get('retry_of_attempt_id'), attempts[i - 1]['attempt_id'] if i else None)
                    if i < failures:
                        self.assertTrue(result['runtime_error'])
                        self.assertIsNone(attempts[i]['input_tokens'])
                        self.assertIsNone(attempts[i]['cost_micro_usd'])
                if failures == 3:
                    self.assertEqual(report['stop_reason'], 'paid_retry_limit')
                    self.assertTrue(any('HTTP 429 rate limited; retried 2 times' in x['reason'] for x in report['blocked']))
                    self.assertGreaterEqual(observed[1] - observed[0], 2)
                    self.assertGreaterEqual(observed[2] - observed[1], 8)
                self.assertEqual(len(self.rows('SELECT * FROM tasks')), int(expected > failures))

    def test_client_timeout_is_not_retried_and_next_invocation_continues(self):
        def respond(peer, connection):
            peer.request(connection)
            if len(peer.requests) == 1:
                peer.stop.wait(2)
            else:
                body = priced(plan_response())
                connection.sendall(tls.headers(len(body)) + body)
        with tls.Peer(self.contexts['server'], respond) as peer:
            request = self.admit(peer.port, pricing=PRICING, retries=2, timeout_ms=300, cost_budget=1000000)
            auth = self.authorize(request); auth['max_calls'] = 1
            self.authorization_file.write_text(json.dumps(auth))
            first = self.pump(request)
            self.assertEqual(len(peer.requests), 1, first)
            self.assertEqual(first['calls'][0]['transport']['status'], 'deadline_exceeded')
            original = self.rows('SELECT * FROM call_attempts')[0]
            original_feedback = self.rows('SELECT * FROM model_tool_feedback')[0]
            second = self.pump(request)
            self.assertEqual(len(peer.requests), 2, second)
            self.assertEqual(self.rows('SELECT * FROM call_attempts ORDER BY rowid')[0], original)
            self.assertEqual(self.rows('SELECT * FROM model_tool_feedback ORDER BY rowid')[0], original_feedback)
            self.assertEqual(len(self.rows('SELECT * FROM tasks')), 1)

    def test_retry_after_wait_obeys_original_batch_deadline(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(b'{}', status=429,
                extra=b'Retry-After: 999999999999999999999999\r\n')) as peer:
            request = self.admit(peer.port, pricing=PRICING, retries=2, cost_budget=1000000)
            auth = self.authorize(request); auth['max_batch_duration_ms'] = 1500
            self.authorization_file.write_text(json.dumps(auth))
            report = self.pump(request)
            self.assertEqual(report['stop_reason'], 'deadline', report)
            self.assertEqual(report['calls'][0]['retry_delay_seconds'], 60)
            self.assertEqual(len(peer.requests), 1)
            self.assertEqual(len(self.rows('SELECT * FROM call_attempts')), 1)

    def test_no_upload_connection_failure_retries_but_partial_response_does_not(self):
        for partial in (False, True):
            with self.subTest(partial=partial):
                self.database = self.directory / f'connection-{partial}.sqlite'
                def respond(peer, connection):
                    peer.request(connection)
                    connection.sendall(tls.headers(1000, status=429) + b'{}')
                with tls.Peer(self.contexts['server'], respond) as peer:
                    request = self.admit(peer.port, pricing=PRICING, retries=1, cost_budget=1000000)
                    self.authorize(request)
                    if not partial:
                        peer.close()
                    report = self.pump(request)
                self.assertEqual(len(report['calls']), 1 if partial else 2, report)
                for call in report['calls']:
                    self.assertFalse(call['transport']['transfer_complete'])
                    self.assertEqual(call['transport']['upload_may_have_occurred'], partial)

    def test_retries_respect_total_calls_budget_and_signed_accounting(self):
        for guard in ('total_calls', 'budget', 'signed'):
            with self.subTest(guard=guard):
                self.database = self.directory / f'guard-{guard}.sqlite'
                pricing = copy.deepcopy(PRICING)
                if guard == 'budget':
                    pricing.update(input_micro_usd_per_million_tokens=0, output_micro_usd_per_million_tokens=1000000)
                    pricing['cache_read']['micro_usd_per_million_tokens'] = 0
                with tls.Peer(self.contexts['server'], tls.fixed_response(b'{}', status=429,
                        extra=b'Retry-After: 0\r\n')) as peer:
                    request = self.admit(peer.port, pricing=pricing, retries=2,
                                         cost_budget=4096 if guard == 'budget' else 1000000,
                                         total=1 if guard == 'total_calls' else None, challenge=guard == 'signed')
                    self.authorize(request, reserved=1000000 if guard == 'signed' else 100)
                    report = self.pump(request)
                    self.assertEqual(len(peer.requests), 1, report)
                    self.assertEqual(len(self.rows('SELECT * FROM call_attempts')), 1)
                    self.assertEqual(len(self.rows('SELECT * FROM live_send_claims')), 1)
                    self.assertEqual(report['accounting']['ready'], guard != 'signed')
                    if guard == 'signed':
                        self.assertEqual(report['stop_reason'], 'needs_accounting')
                    if guard == 'total_calls':
                        self.assertEqual(report['max_total_calls'], 1)
                        self.assertIn(report['stop_reason'], ('call_limit', 'total_call_limit'))
                    if guard == 'budget':
                        self.assertTrue(any(x['code'] == 'quota' for x in report['blocked']), report)

    def test_authorization_inside_business_workspace_is_rejected_before_takeover(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            self.authorize(request)
            workspace = self.directory / 'workspace'
            workspace.mkdir(mode=0o700)
            inside = workspace / 'authorization.json'
            inside.write_bytes(self.authorization_file.read_bytes())
            mapping = self.directory / 'workspaces.json'
            mapping.write_text(json.dumps({'alpha': str(workspace)}))
            before = self.snapshot()
            self.pump(request, '--workspaces-file', mapping, path=inside, error=True)
            self.assertEqual(self.snapshot(), before)
            self.assertEqual(peer.accepted, 0)

    def test_unknown_cli_arguments_and_host_dispositions_are_rejected_before_takeover(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            self.authorize(request)
            before = self.snapshot()
            for option in ('--unknown-option', '--dispositions-file', '--config'):
                self.pump(request, option, self.configuration, error=True)
                self.assertEqual(self.snapshot(), before)
            self.assertEqual(peer.accepted, 0)

    def test_scoped_live_execution_preserves_sibling_request_and_budget(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            sibling = self.admit(peer.port, key='sibling')
            self.authorize(request)
            queries = ('SELECT * FROM requests WHERE request_id=?',
                       'SELECT * FROM request_budgets WHERE request_id=?',
                       'SELECT * FROM events WHERE request_id=?')
            before = [self.rows(query, (sibling,)) for query in queries]
            self.pump(request)
            self.assertEqual([self.rows(query, (sibling,)) for query in queries], before)
            self.assertEqual(len(peer.requests), 1)

    def test_baseline_unknown_cost_records_cumulative_and_incremental_sends(self):
        # The old no-pricing profile keeps its original business gate: one real
        # send, then zero new sends for the same request while cost stays unknown.
        with tls.Peer(self.contexts['server'], tls.fixed_response(plan_response())) as peer:
            request = self.admit(peer.port)
            self.authorize(request)
            report = self.pump(request)
            self.assertEqual(report['stop_reason'], 'needs_accounting', report)
            self.assertEqual(len(peer.requests), 1)  # incremental send for this pump
            self.assertEqual(self.rows('SELECT count(*) AS n FROM live_send_claims')[0]['n'], 1)
            self.assertFalse(report['accounting']['ready'])
            repeat = self.pump(request, credential=None)
            self.assertEqual(repeat['stop_reason'], 'needs_accounting')
            self.assertEqual(repeat['calls'], [])
            self.assertEqual(len(peer.requests), 1)  # incremental send 0, cumulative stays 1
            self.assertEqual(self.rows('SELECT count(*) AS n FROM live_send_claims')[0]['n'], 1)
            self.assertIsNone(self.rows('SELECT cost_micro_usd FROM call_attempts')[0]['cost_micro_usd'])

    def test_pricing_admission_reserves_the_frozen_minimum_before_send(self):
        with tls.Peer(self.contexts['server'], tls.fixed_response(priced(plan_response()))) as peer:
            request = self.admit(peer.port, key='pricing-minimum', pricing=PRICING)
            self.authorize(request, reserved=1)  # positive authorization; actual priced input exceeds the request budget
            report = self.pump(request)
            self.assertEqual(report['stop_reason'], 'error', report)
            self.assertTrue(any(item['code'] == 'quota' for item in report['blocked']), report)
            self.assertEqual(report['network_started_calls'], 0)
            self.assertEqual(self.rows('SELECT * FROM call_attempts'), [])
            self.assertEqual(self.rows('SELECT * FROM live_send_claims'), [])
            self.assertEqual(peer.accepted, 0)

    def test_pricing_estimate_completes_budget_without_relabeling_actual(self):
        peer_model = ScriptedPeer()
        with tls.Peer(self.contexts['server'], peer_model) as peer:
            request = self.admit(peer.port, key='pricing-estimate', pricing=PRICING,
                                 cost_budget=100000)
            self.authorize(request, reserved=50000)
            report = self.pump(request)
            self.assertEqual(report['stop_reason'], 'error', report)
            self.assertTrue(report['accounting']['ready'])
            self.assertEqual(report['network_started_calls'], 2)
            self.assertEqual(peer_model.actions, ['submit_plan'])
            attempts = self.rows('SELECT status,input_tokens,output_tokens,cache_read_tokens,'
                                 'cache_write_tokens,cost_micro_usd,reserved_cost FROM call_attempts ORDER BY rowid')
            self.assertEqual([row['status'] for row in attempts], ['COMPLETED', 'ERROR'])
            self.assertEqual((attempts[0]['input_tokens'], attempts[0]['output_tokens'],
                              attempts[0]['cache_read_tokens']), (11, 7, 3))
            self.assertTrue(all(row['cache_write_tokens'] is None and row['cost_micro_usd'] is None
                                for row in attempts))
            self.assertTrue(all(attempts[1][field] is None for field in
                                ('input_tokens', 'output_tokens', 'cache_read_tokens')))
            failed_body = peer.requests[1]['body']
            charged = (len(failed_body) * 1500000 + json.loads(failed_body)['max_output_tokens'] * 4000000 + 999999) // 1000000
            self.assertEqual(attempts[1]['reserved_cost'], charged)
            projections = self.rows('SELECT cost_micro_usd,cache_write_tokens,disputed,over_reserved '
                                    'FROM live_usage_projection ORDER BY rowid')
            self.assertTrue(all(row['cost_micro_usd'] is None and row['cache_write_tokens'] is None and
                                not row['disputed'] and not row['over_reserved'] for row in projections))
            budget = self.invoke('budget', '--db', self.database, '--request', request)
            self.assertEqual(budget['spent_cost'], PRICED_UNIT_MICRO_USD + charged)
            self.assertEqual(budget['reserved_cost'], 0)
            self.assertEqual(budget['unknown_cost'], 0)
            self.assertEqual(budget['accounting_format'], 'wls-estimated-budget-v1')
            self.assertEqual(budget['cost_breakdown']['estimated_micro_usd'], PRICED_UNIT_MICRO_USD)
            self.assertEqual(budget['cost_breakdown']['reported_micro_usd'], 0)
            self.assertEqual(budget['cost_breakdown']['unknown_reserved_micro_usd'], 0)
            self.assertEqual(budget['cost_breakdown']['reservation_charged_micro_usd'], charged)
            self.assertEqual(len(budget['cost_breakdown']['pricing_sha256']), 1)
            status = self.invoke('live-status', '--db', self.database, '--request', request)
            self.assertTrue(status['accounting']['ready'])
            self.assertEqual(status['accounting']['blocked_count'], 0)
            self.assertEqual(status['accounting']['reservation_charged_micro_usd'], charged)
            self.assertEqual(status['accounting']['estimated_cost_micro_usd'], PRICED_UNIT_MICRO_USD)
            self.assertEqual(status['accounting']['reported_cost_micro_usd'], 0)
            self.assertNotIn('actual', json.dumps(budget['cost_breakdown']))

    def test_pricing_budget_exhaustion_blocks_second_reservation_before_send(self):
        def consume_reservation(peer, connection):
            observed = peer.request(connection)
            body = json.loads(observed['body'])
            response = json.loads(priced(plan_response()))
            response['usage'].update(input_tokens=len(observed['body']), output_tokens=body['max_output_tokens'],
                                     total_tokens=len(observed['body']) + body['max_output_tokens'])
            response['usage']['input_tokens_details']['cached_tokens'] = 0
            encoded = tls.encoded(response)
            connection.sendall(tls.headers(len(encoded)) + encoded)
        with tls.Peer(self.contexts['server'], consume_reservation) as peer:
            request = self.admit(peer.port, key='pricing-budget', pricing=PRICING,
                                 cost_budget=50010)
            self.authorize(request, reserved=50000)
            report = self.pump(request)  # Original usage consumes the first encoded-body reservation.
            self.assertEqual(report['stop_reason'], 'error', report)
            self.assertTrue(any(item['code'] == 'quota' and item['role'] == 'team_lead' for item in report['blocked']), report)
            self.assertEqual(len(peer.requests), 1)
            self.assertEqual(len(self.rows('SELECT * FROM call_attempts')), 1)
            self.assertEqual(len(self.rows('SELECT * FROM live_send_claims')), 1)
            budget = self.invoke('budget', '--db', self.database, '--request', request)
            body = peer.requests[0]['body']
            spent = (len(body) * 1500000 + json.loads(body)['max_output_tokens'] * 4000000 + 999999) // 1000000
            self.assertEqual(self.rows('SELECT reserved_cost FROM call_attempts')[0]['reserved_cost'], spent)
            self.assertEqual(budget['spent_cost'], spent)
            self.assertGreater(spent, budget['cost_limit'] - spent)
            self.assertEqual(budget['reserved_cost'], 0)
            self.assertEqual(budget['unknown_cost'], 0)
            self.assertTrue(self.invoke('live-status', '--db', self.database,
                                        '--request', request)['accounting']['ready'])


if __name__ == '__main__':
    unittest.main(verbosity=2)
