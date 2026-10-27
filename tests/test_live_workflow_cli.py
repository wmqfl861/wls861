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
    response = json.loads(tls.response_body(tool=True))
    response['output'][0].update(name=call['name'], arguments=json.dumps(call['arguments']))
    return tls.encoded(response)


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

    def admit(self, port, key='request-one'):
        system = copy.deepcopy(self.system)
        provider = json.loads((SOURCE / 'config/examples/providers/responses-offline.json').read_text())
        provider.update(base_url=f'https://localhost:{port}', api_key_env=REFERENCE,
                        model_ref='cli-loopback', profile_id='synthetic-cli-loopback')
        provider['capabilities']['reasoning_efforts'] = []
        provider['capabilities']['reasoning_modes'] = ['provider_default', 'off']
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
        system['context']['request_format'] = 'structured-v2'
        self.configuration.write_text(json.dumps(system))
        result = self.invoke('submit', '--db', self.database, '--key', key,
            '--text-file', SOURCE / 'tests/fixtures/s1_request.txt', '--config', self.configuration,
            '--functions-dir', self.functions, '--cost-budget-micro-usd', 10000)
        return result['request']['request_id']

    def authorize(self, request):
        status = self.invoke('live-status', '--db', self.database, '--request', request)
        value = status['authorization_template']
        value.update(network_enabled=True, reserved_cost_micro_usd=100,
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
            request = self.admit(peer.port)
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


if __name__ == '__main__':
    unittest.main(verbosity=2)
