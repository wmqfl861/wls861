#!/usr/bin/env python3
"""Independent signed-billing and logical-agent CLI integration.

The endpoint, Ed25519 issuer, credentials, TLS CA, task responses and files are
disposable synthetic fixtures. Every model invocation uses the real CLI,
original Store and HTTPS transport. Signed cases use an independent OpenSSL
billing issuer; unsigned cases exercise frozen pricing and simulated prefix
cache observations. These tests establish engineering evidence, never provider
identity, vendor cache behavior or comparative model quality.
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
import signal
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
# Unsigned frozen price table for host-side estimation. Reported usage shape is
# 11 input / 7 output / 3 cached-read with no cache-write observation.
PRICING = {'format': 'wls-token-pricing-v1', 'currency': 'USD',
           'unit': 'micro_usd_per_million_tokens',
           'input_micro_usd_per_million_tokens': 1500000,
           'output_micro_usd_per_million_tokens': 4000000,
           'cache_read': {'applicability': 'reported', 'micro_usd_per_million_tokens': 250000},
           'cache_write': {'applicability': 'not_applicable'}}
PRICING_CALLS = 31  # Dispatcher 4 + TeamLead 9 + Worker 18 original wire calls.
# Independent hand computation with Python integers: ordinary input 8 tokens.
PRICED_UNIT_MICRO_USD = (8 * 1500000 + 3 * 250000 + 7 * 4000000 + 999999) // 1000000


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


def captured_json_values(raw):
    """Return value slices from the captured JSON, without reserialization."""
    source, decoder = raw.decode('utf-8'), json.JSONDecoder()
    mapping = source[0] == '{'
    result, offset = ({} if mapping else []), 1
    while True:
        while source[offset] in ' \r\n\t,':
            offset += 1
        if source[offset] in '}]':
            return result
        if mapping:
            key, offset = decoder.raw_decode(source, offset)
            while source[offset] in ' \r\n\t:':
                offset += 1
        start = offset
        _, offset = decoder.raw_decode(source, offset)
        value = source[start:offset].encode('utf-8')
        if mapping:
            result[key] = value
        else:
            result.append(value)


class SimulatedPrefixCache:
    """Synthetic cache, never vendor evidence: tools -> instructions -> items.

    Count ceil(UTF-8 bytes / 4) input tokens, require a 1024-token matching
    prefix, and round cached reads down to whole 64-token blocks. Compare the
    longest byte prefix of every prior request in the same model/profile.
    All input items, including the complete current_state, enter the denominator.
    """
    bytes_per_token, minimum_tokens, block_tokens = 4, 1024, 64

    def __init__(self):
        self.previous = {}

    def observe(self, raw, profile_sha256):
        fields = captured_json_values(raw)
        items = captured_json_values(fields['input'])
        material = fields['tools'] + fields['instructions'] + b''.join(items)
        key = (json.loads(fields['model']), profile_sha256)
        longest = 0
        for previous in self.previous.get(key, []):
            matched = 0
            for left, right in zip(previous, material):
                if left != right:
                    break
                matched += 1
            longest = max(longest, matched)
        self.previous.setdefault(key, []).append(material)
        cached = longest // self.bytes_per_token
        cached = cached // self.block_tokens * self.block_tokens if cached >= self.minimum_tokens else 0
        tokens = (len(material) + self.bytes_per_token - 1) // self.bytes_per_token
        return {'input_tokens': tokens, 'cache_read_tokens': cached,
                'input_bytes': len(material), 'matched_prefix_bytes': longest,
                'body_bytes': len(raw), 'ratio': cached / tokens,
                'stable_material': fields['tools'] + fields['instructions'] + b''.join(items[:-1]),
                'tools': fields['tools'], 'instructions': fields['instructions']}


def v2_comparison_request(captured):
    """Fixed v2 layout of the same original contract, tools and full messages.

    Keep parameter schemas and semantic materials. Drop only v3 layout selectors
    and the top-level tool descriptions absent from v2's frozen input shape.
    No Store record, business operation or original captured request is changed.
    """
    legacy = copy.deepcopy(captured)
    context = legacy['context']
    logical = context['input']
    prefix = copy.deepcopy(logical['prefix'])
    prefix['format'] = 'wls-logical-prefix-v1'
    for tool in prefix['tools']:
        tool.pop('description')
    original = {'format': 'wls-logical-input-v1', 'selection': logical['selection'],
        'prefix': prefix, 'task_contract': logical['task_contract'], 'messages': logical['messages']}
    context.update(format='wls-compiled-context-v2', input=original,
        prefix_sha256=digest(prefix), logical_input_sha256=digest(original),
        cache_identity=digest({'format': 'wls-context-cache-v2', 'selection': logical['selection'],
                               'prefix_sha256': digest(prefix)}))
    legacy['format'] = 'wls-model-request-v2'
    legacy['logical_request_sha256'] = digest({key: value for key, value in legacy.items()
        if key not in ('logical_request_sha256', 'wire_request_sha256')})
    return legacy


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


class PricingFlowEndpoint:
    """Unsigned priced loopback peer for three independent tasks.

    It never signs or emits a billing receipt. By default responses retain the
    fixed 11/7/3 usage shape; G1 instead derives input/cache tokens from captured
    outbound bytes. Neither mode reports cache-write or provider-reported cost.
    """

    def __init__(self, correctable_read=False, initialize_seed=False, simulate_cache=False):
        fixture = workflow.without_constraints(json.loads(
            (SOURCE / 'tests/fixtures/s15_fake_complete.json').read_text()))
        self.plan = fixture['default'][0]['tool_calls'][0]['arguments']
        for node in ('beta', 'gamma'):
            extra = copy.deepcopy(self.plan['nodes'][0])
            extra['node_id'] = node
            extra['title'] = f'Independent {node}'
            self.plan['nodes'].append(extra)
        self.order = fixture['nodes']['alpha'][0]['tool_calls'][0]['arguments']
        self.submission = fixture['nodes']['alpha'][2]['tool_calls'][0]['arguments']
        self.check = fixture['nodes']['alpha'][3]['tool_calls'][0]['arguments']
        self.acceptance = fixture['default'][1]['tool_calls'][0]['arguments']
        self.steps, self.actions = {}, []
        self.correctable_read = correctable_read
        self.initialize_seed = initialize_seed
        self.cache = SimulatedPrefixCache() if simulate_cache else None
        self.records = []

    @staticmethod
    def contract(body):
        content = body['input'][-1].get('content', '{}')
        tail = json.loads(content) if content.startswith('{') else {}
        if isinstance(tail, dict) and tail.get('format') == 'wls-responses-current-state-v3':
            return tail['current_state']
        return json.loads(json.loads(body['input'][0]['content'])['task_contract'])

    def choose(self, body):
        contract = self.contract(body)
        action, binding = contract.get('workflow_action'), contract['binding']
        if action:
            kind = action['kind']
            if kind == 'submission':
                return 'check_submission', self.check
            if kind == 'acceptance':
                return 'accept_candidate', self.acceptance
            if kind == 'experience':
                return 'report_experience', {'no_new_experience': True, 'entries': [], 'memory_assessments': []}
            raise AssertionError(f'unexpected priced workflow action {kind}')
        if not binding['task_id']:
            return 'submit_plan', self.plan
        if 'issue_work_order' in {tool['name'] for tool in body['tools']}:
            return 'issue_work_order', self.order
        if self.correctable_read:
            self.correctable_read = False
            return 'fs_read', {'path': 'missing-original.txt'}
        key = (binding['task_id'], binding['instruction_revision'])
        index = self.steps.get(key, 0)
        self.steps[key] = index + 1
        sequence = [('fs_read', {'path': 'seed.txt'}),
                    ('fs_write', {'path': 'result.txt', 'content': 'alpha\nbeta\n'}),
                    ('fs_read', {'path': 'result.txt'}),
                    ('fs_write', {'path': 'result.txt', 'content': 'alpha\nbeta\n',
                                  'expected_sha256': hashlib.sha256(b'alpha\nbeta\n').hexdigest()}),
                    ('fs_read', {'path': 'result.txt'}),
                    ('submit', self.submission)]
        if self.initialize_seed:
            sequence[0] = ('fs_write', {'path': 'seed.txt', 'content': 'alpha\nbeta\n'})
        return sequence[index]

    def __call__(self, peer, connection):
        observed = peer.request(connection)
        body = json.loads(observed['body'])
        name, arguments = self.choose(body)
        if name not in {tool['name'] for tool in body['tools']}:
            raise AssertionError(f'{name} was not declared')
        self.actions.append(name)
        value = json.loads(tls.response_body(tool=True))
        value['id'] = f'resp_priced_{len(self.actions)}'
        value['output'][0].update(call_id=f'pricing-call-{len(self.actions)}', name=name,
                                  arguments=json.dumps(arguments, ensure_ascii=False))
        value['usage'].update(input_tokens=11, output_tokens=7, total_tokens=18)
        value['usage']['input_tokens_details'] = {'cached_tokens': 3}
        if self.cache is not None:
            observation = self.cache.observe(observed['body'], self.profile_sha256)
            value['usage'].update(input_tokens=observation['input_tokens'],
                                  total_tokens=observation['input_tokens'] + 7)
            value['usage']['input_tokens_details']['cached_tokens'] = observation['cache_read_tokens']
            self.records.append({'request': observed['body'], 'cache': observation,
                                 'contract': self.contract(body), 'usage': copy.deepcopy(value['usage'])})
        encoded = tls.encoded(value)
        connection.sendall(tls.headers(len(encoded)) + encoded)


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
    def command(cls, *arguments, error=False, timeout=150, source_root=True):
        completed = subprocess.run([str(BINARY), *(str(value) for value in arguments),
            *(['--source-root', str(SOURCE)] if source_root else [])], cwd=cls.directory, env=cls.environment,
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
    def admit_case(cls, label, task, condition, *, receipt_mode='valid', register=True, database=None,
                   profile=None, cost_budget=100000, nodes=None, bounded=False):
        directory = cls.directory / label
        directory.mkdir(mode=0o700)
        db = database or directory / 'runtime.sqlite'
        system = copy.deepcopy(cls.system)
        profile = cls.profile if profile is None else profile
        system['models'][MODEL_REF] = copy.deepcopy(profile)
        system['defaults']['model_ref'] = MODEL_REF
        system['model_calls']['real_calls_enabled'] = True
        if bounded:
            system['model_calls'].update(runtime_policy='bounded-v1', max_total_calls=40)
        system['context']['request_format'] = 'structured-v2'
        system['context'].pop('model_projection', None); system['context'].pop('tool_policy', None)
        system['context'].pop('worker_history', None)
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
            '--cost-budget-micro-usd', cost_budget)
        request = result['request']['request_id']
        status = cls.command('live-status', '--db', db, '--request', request)
        policy = {'schema_version': 1, 'kind': 'wls-billing-trust-policy-v1',
            'trust_domain': 'local_tls_fixture', 'request_id': request,
            'config_snapshot_sha256': status['config_snapshot_sha256'], 'issuer': ISSUER,
            'account_id': ACCOUNT, 'public_keys': {KEY_ID: cls.endpoint.public_key_hex},
            'models': {MODEL_REF: digest(profile)}}
        policy_file = write_json(directory / 'billing-policy.json', policy)
        workspaces = {}
        for node in nodes if nodes is not None else (('alpha', 'beta') if task == 1 else ('alpha',)):
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
        if (authorization['schema_version'], authorization['kind']) not in (
                (2, 'wls-live-workflow-authorization-v2'), (3, 'wls-live-workflow-authorization-v3')):
            raise AssertionError('billing execution requires its versioned authorization')
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

    @classmethod
    def run_inputs(cls, label, peer):
        directory = cls.directory / label
        directory.mkdir(mode=0o700)
        shutil.copytree(cls.functions, directory / 'functions')
        system, profile = copy.deepcopy(cls.system), copy.deepcopy(cls.profile)
        provider = profile['responses']['provider_profile']
        provider.update(base_url=f'https://localhost:{peer.port}', profile_id='one-command-pricing', pricing=PRICING)
        provider.pop('billing_challenge', None)
        system['models'][MODEL_REF] = profile
        system['defaults']['model_ref'] = MODEL_REF
        system['context']['request_format'] = 'structured-v2'
        system['context'].pop('logical_agent', None)
        system['context'].pop('model_projection', None); system['context'].pop('tool_policy', None)
        system['context'].pop('worker_history', None)
        system['model_calls'].update(real_calls_enabled=True, runtime_policy='bounded-v1', max_total_calls=40)
        system['budget_defaults'].update(max_cost_usd=8, max_input_tokens=4000000, max_output_tokens=1000000)
        system['paths']['data'] = str(directory / 'data')
        configuration = write_json(directory / 'system.json', system)
        material = directory / 'request.txt'
        material.write_text('Create three independent original local file results.\n', encoding='utf-8')
        constraints = write_json(directory / 'constraints.json', [])
        return {'directory': directory, 'configuration': configuration, 'material': material,
                'constraints': constraints, 'system': system, 'data': directory / 'data'}

    @classmethod
    def run_arguments(cls, case, *extra):
        return [str(BINARY), 'run', '--config', str(case['configuration']), '--text-file', str(case['material']),
                '--constraints-file', str(case['constraints']), '--source-root', str(SOURCE), *(str(x) for x in extra)]

    def invoke_run(self, case, *extra, answer='yes\n'):
        result = subprocess.run(self.run_arguments(case, *extra), cwd=case['directory'], env=self.environment,
            input=answer, capture_output=True, text=True, encoding='utf-8', timeout=150, check=False)
        for private in (BEARER, PRIVATE_ERROR, 'signature_hex', 'raw_body_hex', 'raw_headers_hex'):
            self.assertNotIn(private, result.stdout + result.stderr)
        return result

    def test_06_run_once_delivers_three_tasks_and_two_requests_share_data(self):
        endpoint = PricingFlowEndpoint(initialize_seed=True)
        with tls.Peer(self.contexts['server'], endpoint) as peer:
            case = self.run_inputs('run-complete', peer)
            result = self.invoke_run(case, '--data', case['data'])
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertEqual(result.stderr.count('Authorize this bounded run?'), 1)
            preview = json.loads(result.stderr.splitlines()[0])
            summary = json.loads(result.stdout)
            self.assertEqual({item['endpoint_host'] for item in preview['models']}, {'localhost'})
            self.assertEqual({item['model_ref'] for item in preview['models']}, {MODEL_REF})
            self.assertEqual({item['model'] for item in preview['models']}, {self.profile['model']})
            self.assertEqual(summary['status'], 'DELIVERED')
            self.assertEqual(summary['role_calls'], {'dispatcher': 4, 'team_lead': 9, 'worker': 18})
            self.assertEqual(summary['original_tokens'], {'input_tokens': 341, 'output_tokens': 217,
                'cache_read_tokens': 93, 'cache_write_tokens': None})
            self.assertAlmostEqual(summary['cache_read_ratio'], 3 / 11)
            self.assertEqual(summary['budget']['cost_breakdown']['estimated_micro_usd'], 31 * PRICED_UNIT_MICRO_USD)
            self.assertEqual(summary['budget']['cost_breakdown']['reported_micro_usd'], 0)
            self.assertTrue(summary['technical_acknowledgement'])
            self.assertEqual(summary['user_review'], 'not_recorded')
            self.assertEqual(summary['blockers'], [])
            expected = digest({'format': 'run-key-v1', 'text': case['material'].read_text(),
                               'constraints': [], 'config_locator': str(case['configuration'].absolute())})
            alphabet = '0123456789ABCDEFGHJKMNPQRSTVWXYZ'
            number, encoded = int(expected[:32], 16), ''
            for _ in range(26):
                encoded = alphabet[number & 31] + encoded
                number >>= 5
            self.assertEqual(summary['request_id'], 'req_' + encoded)
            request = case['data'] / 'requests' / summary['request_id']
            database = request / 'control.sqlite'
            self.assertFalse((case['data'] / 'control.sqlite').exists())
            intent = json.loads((request / 'intent.json').read_text())
            self.assertEqual(intent['admission']['run_key_sha256'], expected)
            self.assertEqual((request / 'intent.json').stat().st_mode & 0o777, 0o600)
            self.assertEqual(database.stat().st_mode & 0o777, 0o600)
            self.assertEqual([row['status'] for row in rows(database, 'SELECT status FROM tasks')], ['ACCEPTED'] * 3)
            self.assertEqual(rows(database, 'SELECT count(*) n FROM live_send_claims')[0]['n'], 31)
            resources = rows(database, 'SELECT status,intent FROM owned_workspaces')
            self.assertEqual(len(resources), 3)
            self.assertTrue(all(row['status'] == 'OWNED' for row in resources))
            self.assertTrue(all(json.loads(row['intent'])['parent_identity']['path'] == str(request / 'workspaces') for row in resources))
            files = list(Path(summary['delivery_path']).glob('artifact-*.bin'))
            self.assertEqual([path.read_bytes() for path in files], [b'alpha\nbeta\n'] * 3)
            original = rows(database, 'SELECT created_at,config_snapshot,budget FROM requests')
            original_database = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in request.glob('control.sqlite*') if not path.name.endswith('-shm')}
            original_events = rows(database, 'SELECT seq,type,payload,result FROM events ORDER BY seq')
            # Canonical constraints identity survives spelling changes. A completed
            # repeat inspects the original durable package without another send.
            case['constraints'].write_text(' [ ] \n')
            replay = self.invoke_run(case, '--yes')
            self.assertEqual(replay.returncode, 0, replay.stderr)
            self.assertEqual(json.loads(replay.stdout)['stop_reason'], 'completed_replay')
            self.assertEqual(len(peer.requests), 31)
            self.assertEqual(rows(database, 'SELECT created_at,config_snapshot,budget FROM requests'), original)
            self.assertEqual(rows(database, 'SELECT seq,type,payload,result FROM events ORDER BY seq'), original_events)
            self.assertEqual({path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                              for path in request.glob('control.sqlite*') if not path.name.endswith('-shm')}, original_database)
            changed = self.invoke_run(case, '--yes', '--budget-usd', 4)
            self.assertEqual(changed.returncode, 3, changed.stderr)
            self.assertFalse(changed.stdout)
            self.assertNotIn('wls-run-confirmation', changed.stderr)
            self.assertEqual(len(peer.requests), 31)
            case['material'].write_text('A distinct request creates its own three file results.\n')
            other = self.invoke_run(case, '--yes')
            self.assertEqual(other.returncode, 0, other.stderr)
            self.assertNotEqual(json.loads(other.stdout)['request_id'], summary['request_id'])
            self.assertEqual(len(list((case['data'] / 'requests').glob('*/control.sqlite'))), 2)
            self.assertEqual(len(peer.requests), 62)

    def test_g1_exit_one_command_with_correction_and_simulated_prefix_cache(self):
        endpoint = PricingFlowEndpoint(correctable_read=True, initialize_seed=True, simulate_cache=True)
        with tls.Peer(self.contexts['server'], endpoint) as peer:
            case = self.run_inputs('g1-exit', peer)
            context = case['system']['context']
            context.update(request_format='structured-v3', model_projection='lean-v1',
                tool_policy='fixed-role-v1', worker_history={'mode': 'append-fold-v1',
                    'read_max_bytes': 24576, 'read_token_budget': 196608})
            write_json(case['configuration'], case['system'])
            provider = case['system']['models'][MODEL_REF]['responses']['provider_profile']
            endpoint.profile_sha256 = digest(provider)
            result = self.invoke_run(case)
            write_json(case['directory'] / 'command-result.json', {'returncode': result.returncode,
                'stdout': result.stdout, 'stderr': result.stderr})
            for index, observation in enumerate(endpoint.records):
                captured_path = case['directory'] / f'captured-wire-{index}.json'
                captured_path.write_bytes(observation['request'])
                captured_path.chmod(0o600)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertEqual(result.stderr.count('Authorize this bounded run?'), 1)
            summary = json.loads(result.stdout)
            self.assertEqual(summary['status'], 'DELIVERED')
            self.assertEqual(summary['blockers'], [])
            self.assertTrue(summary['accounting']['ready'])
            self.assertTrue(summary['technical_acknowledgement'])
            self.assertEqual(summary['user_review'], 'not_recorded')
            request = case['data'] / 'requests' / summary['request_id']
            database = request / 'control.sqlite'
            attempts = rows(database, 'SELECT * FROM call_attempts ORDER BY rowid')
            count = len(attempts)
            self.assertGreaterEqual(count, 30)
            self.assertEqual(len(peer.requests), count)
            self.assertEqual(len(endpoint.records), count)
            attempt_ids = {item['attempt_id'] for item in attempts}
            self.assertEqual(len(attempt_ids), count)
            claims = rows(database, 'SELECT attempt_id,claim_id FROM live_send_claims')
            self.assertEqual({item['attempt_id'] for item in claims}, attempt_ids)
            self.assertEqual(len({item['claim_id'] for item in claims}), count)
            for table in ('live_transport_observations', 'model_tool_feedback', 'live_usage_projection'):
                self.assertEqual(rows(database, f'SELECT count(*) n FROM {table}')[0]['n'], count, table)
            executions = {item['attempt_id']: item for item in rows(database, 'SELECT * FROM live_attempt_execution')}
            projections = {item['attempt_id']: item for item in rows(database, 'SELECT * FROM live_usage_projection')}
            transports = {item['attempt_id']: item for item in rows(database, 'SELECT * FROM live_transport_observations')}
            for originals in (executions, projections, transports):
                self.assertEqual(set(originals), attempt_ids)
            self.assertEqual(rows(database, 'SELECT * FROM usage_reconciliations'), [])
            self.assertEqual([item['status'] for item in rows(database, 'SELECT status FROM tasks')], ['ACCEPTED'] * 3)
            self.assertEqual([item['status'] for item in rows(database, 'SELECT status FROM experience_jobs')], ['DONE'] * 3)
            artifacts = list(Path(summary['delivery_path']).glob('artifact-*.bin'))
            self.assertEqual([item.read_bytes() for item in artifacts], [b'alpha\nbeta\n'] * 3)
            feedback = rows(database, 'SELECT attempt_id,result,record_sha256 FROM model_tool_feedback ORDER BY rowid')
            feedback_hashes = {item['attempt_id']: item['record_sha256'] for item in feedback}
            corrections = [item for item in feedback if 'bounded_runtime_v1' in item['result']]
            self.assertEqual(len(corrections), 1)
            corrected = next(index for index, item in enumerate(attempts)
                             if item['attempt_id'] == corrections[0]['attempt_id'])
            self.assertEqual(endpoint.actions[corrected:corrected + 2], ['fs_read', 'fs_write'])
            self.assertIn('missing-original.txt', endpoint.records[corrected + 1]['request'].decode())
            self.assertIn('correction_feedback', endpoint.records[corrected + 1]['request'].decode())
            self.assertEqual(json.loads(corrections[0]['result'])['correction_feedback']
                ['bounded_runtime_v1']['same_error_repeats'], 1)
            roles, workers, managers, worker_attempts = {}, {}, {}, {}
            totals = dict(input_tokens=0, output_tokens=0, cache_read_tokens=0, cache_write_tokens=None)
            expected_cost = 0
            legacy_cache = SimulatedPrefixCache()
            comparison = []
            profile_path = write_json(case['directory'] / 'offline-profile.json', provider)
            for index, (attempt, sent, observation) in enumerate(zip(attempts, peer.requests, endpoint.records)):
                self.assertNotIn(b'x-wls-billing-challenge', sent['headers'])
                self.assertEqual(sent['body'], observation['request'])
                self.assertEqual(executions[attempt['attempt_id']]['request_body_sha256'],
                                 hashlib.sha256(sent['body']).hexdigest())
                self.assertEqual(executions[attempt['attempt_id']]['profile_sha256'], endpoint.profile_sha256)
                native_response = json.loads(bytes.fromhex(transports[attempt['attempt_id']]['raw_body_hex']))
                self.assertEqual(native_response['usage'], observation['usage'])
                captured = json.loads(attempt['request_envelope'])
                logical = captured['context']['input']
                self.assertEqual(captured['context']['format'], 'wls-compiled-context-v3')
                self.assertEqual(logical['model_projection'], 'lean-v1')
                self.assertEqual(logical['tool_policy'], 'fixed-role-v1')
                self.assertTrue(attempt['settlement_sha256'])
                original_usage = json.loads(attempt['usage'])
                expected_usage = {'input_tokens': observation['usage']['input_tokens'], 'output_tokens': 7,
                    'cache_read_tokens': observation['usage']['input_tokens_details']['cached_tokens'],
                    'cache_write_tokens': None, 'cost_micro_usd': None}
                self.assertEqual(original_usage, expected_usage)
                for key in expected_usage:
                    self.assertEqual(attempt[key], expected_usage[key])
                    self.assertEqual(projections[attempt['attempt_id']][key], expected_usage[key])
                self.assertEqual(projections[attempt['attempt_id']]['disputed'], 0)
                self.assertEqual(projections[attempt['attempt_id']]['over_reserved'], 0)
                for key in ('input_tokens', 'output_tokens', 'cache_read_tokens'):
                    totals[key] += original_usage[key]
                expected_cost += ((original_usage['input_tokens'] - original_usage['cache_read_tokens']) * 1500000
                    + original_usage['cache_read_tokens'] * 250000 + 7 * 4000000 + 999999) // 1000000
                roles[attempt['role']] = roles.get(attempt['role'], 0) + 1
                cache = observation['cache']
                if attempt['role'] == 'worker':
                    self.assertEqual(logical['contract_updates'], [])
                    # Covers the first real-profile append-fold capture too:
                    # only the final bounded reservation reaches Store/TLS.
                    self.assertGreaterEqual(captured['reserved_input_tokens'], len(canonical(logical)))
                    self.assertGreaterEqual(captured['reserved_input_tokens'], len(sent['body']))
                    self.assertGreater(captured['max_output_tokens'], 0)
                    self.assertGreaterEqual(captured['reserved_output_tokens'], captured['max_output_tokens'])
                    self.assertLessEqual(captured['reserved_input_tokens'] + captured['reserved_output_tokens'],
                                         provider['capabilities']['context_window'])
                    self.assertEqual(attempt['reserved_input'], captured['reserved_input_tokens'])
                    self.assertEqual(attempt['reserved_output'], captured['reserved_output_tokens'])
                    history = logical['worker_history']
                    self.assertEqual(history['fold_generation'], 0)
                    previous_ids = worker_attempts.setdefault(attempt['task_id'], [])
                    self.assertEqual([turn['attempt_id'] for turn in history['visible_turns']], previous_ids)
                    self.assertTrue(all(turn['completed'] for turn in history['visible_turns']))
                    self.assertEqual([turn['record_sha256'] for turn in history['visible_turns']],
                                     [feedback_hashes[original] for original in previous_ids])
                    previous_ids.append(attempt['attempt_id'])
                    self.assertLess(captured['reserved_input_tokens'],
                                    CONTEXT_WINDOW * context['compaction_at_fraction'])
                    own = workers.setdefault(attempt['task_id'], [])
                    if own:
                        self.assertTrue(cache['stable_material'].startswith(own[-1]['stable_material']))
                    own.append(cache)
                    if len(own) >= 3:
                        self.assertGreater(cache['ratio'], 0.70, (attempt['attempt_id'], cache['ratio']))
                    self.assertIn(endpoint.actions[index], {'fs_read', 'fs_write', 'submit'})
                else:
                    stable = (cache['tools'], cache['instructions'])
                    previous = managers.setdefault(attempt['role'], stable)
                    self.assertEqual(stable, previous)
                # Re-encode these exact original materials through the existing
                # offline v2 codec. This creates no second business run or send.
                original_path = write_json(case['directory'] / f'offline-v2-{index}.json', v2_comparison_request(captured))
                offline = self.command('encode-model', '--profile', profile_path, '--request', original_path,
                                       source_root=False)
                self.assertEqual((offline['network_requests'], offline['paid_attempts'], offline['tool_dispatches']), (0, 0, 0))
                self.assertFalse(offline['store_opened'])
                legacy_body = offline['encoded_request']['body'].encode('utf-8')
                legacy_path = case['directory'] / f'offline-v2-wire-{index}.json'
                legacy_path.write_bytes(legacy_body)
                legacy_path.chmod(0o600)
                legacy_observation = legacy_cache.observe(legacy_body, endpoint.profile_sha256)
                comparison.append({'attempt_id': attempt['attempt_id'], 'role': attempt['role'],
                    'v3_body_bytes': len(sent['body']), 'v2_body_bytes': len(legacy_body),
                    'v3_cache_ratio': cache['ratio'], 'v2_cache_ratio': legacy_observation['ratio'],
                    'v3_input_tokens': cache['input_tokens'], 'v2_input_tokens': legacy_observation['input_tokens'],
                    'v3_cache_read_tokens': cache['cache_read_tokens'],
                    'v2_cache_read_tokens': legacy_observation['cache_read_tokens']})
            self.assertEqual(len(workers), 3)
            self.assertTrue(all(len(own) >= 6 for own in workers.values()))
            self.assertEqual(summary['role_calls'], roles)
            self.assertEqual(summary['original_tokens'], totals)
            self.assertAlmostEqual(summary['cache_read_ratio'], totals['cache_read_tokens'] / totals['input_tokens'])
            budget = summary['budget']
            self.assertEqual(budget['attempts'], count)
            self.assertEqual(budget['spent_cost'], expected_cost)
            self.assertEqual(budget['reserved_cost'], 0)
            self.assertEqual(budget['unknown_cost'], 0)
            self.assertEqual(budget['cost_breakdown']['estimated_micro_usd'], expected_cost)
            self.assertEqual(budget['cost_breakdown']['reported_micro_usd'], 0)
            self.assertEqual(len(peer.requests), count)
            total_v2 = sum(item['v2_body_bytes'] for item in comparison)
            total_v3 = sum(item['v3_body_bytes'] for item in comparison)
            self.assertLess(total_v3, total_v2)
            v2_ratio = sum(item['v2_cache_read_tokens'] for item in comparison) / sum(item['v2_input_tokens'] for item in comparison)
            report = {'evidence': 'synthetic-local-tls-prefix-cache-not-vendor-cache',
                'business_runs': 1, 'confirmations': 1, 'calls': count, 'role_calls': roles,
                'cache_rules': {'bytes_per_token': SimulatedPrefixCache.bytes_per_token,
                    'minimum_tokens': SimulatedPrefixCache.minimum_tokens, 'block_tokens': SimulatedPrefixCache.block_tokens,
                    'ordered_components': ['tools', 'instructions', 'input_items_including_current_state']},
                'v2_body_bytes': total_v2, 'v3_body_bytes': total_v3, 'body_ratio': total_v3 / total_v2,
                'v2_cache_read_ratio': v2_ratio, 'v3_cache_read_ratio': summary['cache_read_ratio'],
                'original_tokens': totals, 'estimated_micro_usd': expected_cost,
                'worker_min_ratio_from_third': min(item['ratio'] for own in workers.values() for item in own[2:]),
                'worker_ratios': {task: [item['ratio'] for item in own] for task, own in workers.items()},
                'attempts': comparison}
            write_json(case['directory'] / 'g1-exit-result.json', report)
            print('G1_EXIT_SYNTHETIC_RESULT ' + json.dumps({key: value for key, value in report.items()
                                                        if key != 'attempts'}, sort_keys=True))

    def test_07_run_refusal_config_budget_and_unsafe_paths_before_confirmation(self):
        endpoint = PricingFlowEndpoint(initialize_seed=True)
        with tls.Peer(self.contexts['server'], endpoint) as peer:
            case = self.run_inputs('run-refusals', peer)
            declined = self.invoke_run(case, answer='no\n')
            self.assertEqual(declined.returncode, 2)
            self.assertFalse(declined.stdout)
            self.assertFalse(case['data'].exists())
            self.assertEqual(len(peer.requests), 0)
            low = self.invoke_run(case, '--yes', '--budget-usd', '0.01')
            self.assertEqual(low.returncode, 2)
            self.assertNotIn('wls-run-confirmation', low.stderr)
            self.assertFalse(case['data'].exists())
            for enabled, base in ((False, f'https://localhost:{peer.port}'), (False, 'https://hostname'),
                                  (False, 'https://127.0.0.1:443')):
                system = copy.deepcopy(case['system'])
                system['model_calls']['real_calls_enabled'] = enabled
                system['models'][MODEL_REF]['responses']['provider_profile']['base_url'] = base
                write_json(case['configuration'], system)
                denied = self.invoke_run(case, '--yes')
                self.assertEqual(denied.returncode, 2, denied.stderr)
                self.assertNotIn('runtime_error', denied.stderr)
                self.assertNotIn('wls-run-confirmation', denied.stderr)
            write_json(case['configuration'], case['system'])
            unsafe = case['directory'] / 'unsafe'
            unsafe.mkdir(mode=0o700)
            unsafe.chmod(0o777)
            rejected = self.invoke_run(case, '--yes', '--data', unsafe)
            self.assertEqual(rejected.returncode, 2)
            self.assertNotIn('wls-run-confirmation', rejected.stderr)
            link = case['directory'] / 'linked'
            link.symlink_to(unsafe, target_is_directory=True)
            rejected = self.invoke_run(case, '--yes', '--data', link)
            self.assertEqual(rejected.returncode, 2)
            self.assertNotIn('wls-run-confirmation', rejected.stderr)
            # An existing deterministic leaf without its exclusive original
            # intent is ambiguous and must never be adopted.
            request_id = json.loads(declined.stderr.splitlines()[0])['request_id']
            abandoned = case['data'] / 'requests' / request_id
            abandoned.mkdir(mode=0o700, parents=True)
            rejected = self.invoke_run(case, '--yes')
            self.assertEqual(rejected.returncode, 3)
            self.assertNotIn('wls-run-confirmation', rejected.stderr)
            self.assertFalse(list(case['data'].rglob('control.sqlite')))
            self.assertEqual(len(peer.requests), 0)
            limited = self.run_inputs('run-total-cap', peer)
            limited['system']['model_calls']['max_total_calls'] = 30
            write_json(limited['configuration'], limited['system'])
            result = self.invoke_run(limited, '--yes')
            self.assertEqual(result.returncode, 0, result.stderr)
            summary = json.loads(result.stdout)
            self.assertEqual(summary['status'], 'RUNNING')
            self.assertEqual(summary['stop_reason'], 'total_call_limit')
            self.assertIsNone(summary['delivery_path'])
            self.assertEqual(len(peer.requests), 30)

    def test_08_run_ctrl_c_after_settlement_resumes_original_claims_and_effects(self):
        endpoint = PricingFlowEndpoint(initialize_seed=True)
        with tls.Peer(self.contexts['server'], endpoint) as peer:
            case = self.run_inputs('run-interrupted', peer)
            process = subprocess.Popen(self.run_arguments(case, '--yes'), cwd=case['directory'], env=self.environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.addCleanup(lambda: process.poll() is None and process.kill())
            deadline, database = time.monotonic() + 40, None
            interrupted = False
            while process.poll() is None and time.monotonic() < deadline:
                candidates = list(case['data'].glob('requests/*/control.sqlite'))
                if candidates:
                    database = candidates[0]
                    try:
                        attempts = rows(database, 'SELECT status,settlement_sha256 FROM call_attempts')
                        feedback = rows(database, 'SELECT count(*) n FROM model_tool_feedback')
                        if len(attempts) >= 4 and feedback[0]['n'] == len(attempts) and all(
                                row['settlement_sha256'] and row['status'] != 'UNKNOWN' for row in attempts):
                            process.send_signal(signal.SIGINT)
                            interrupted = True
                            break
                    except sqlite3.OperationalError:
                        pass
                time.sleep(0.001)
            stdout, stderr = process.communicate(timeout=30)
            self.assertTrue(interrupted, stderr + stdout)
            self.assertEqual(process.returncode, 130, stderr + stdout)
            summary = json.loads(stdout)
            self.assertEqual(summary['stop_reason'], 'cancelled')
            self.assertTrue(summary['accounting']['ready'], summary)
            original = rows(database, 'SELECT created_at,config_snapshot,budget FROM requests')
            claims = rows(database, 'SELECT attempt_id,claim_id FROM live_send_claims ORDER BY rowid')
            self.assertGreaterEqual(len(claims), 4)
            self.assertLess(len(claims), 31)
            operations = rows(database, 'SELECT op_id,status FROM ops ORDER BY rowid')
            self.assertTrue(operations)
            self.assertTrue(all(row['status'] == 'SUCCEEDED' for row in operations))
            resumed = self.invoke_run(case, '--yes')
            self.assertEqual(resumed.returncode, 0, resumed.stderr + resumed.stdout)
            final = json.loads(resumed.stdout)
            self.assertEqual(final['request_id'], summary['request_id'])
            self.assertEqual(final['status'], 'DELIVERED')
            self.assertEqual(final['role_calls'], {'dispatcher': 4, 'team_lead': 9, 'worker': 18})
            self.assertEqual(rows(database, 'SELECT created_at,config_snapshot,budget FROM requests'), original)
            self.assertEqual(rows(database, 'SELECT attempt_id,claim_id FROM live_send_claims ORDER BY rowid')[:len(claims)], claims)
            self.assertEqual(rows(database, 'SELECT op_id,status FROM ops ORDER BY rowid')[:len(operations)], operations)
            self.assertEqual(len(peer.requests), 31)

    def test_09_run_unknown_send_stops_and_changed_configuration_cannot_resume(self):
        requested, release = threading.Event(), threading.Event()
        def delayed(peer, connection):
            peer.request(connection)
            requested.set()
            release.wait(5)
        with tls.Peer(self.contexts['server'], delayed) as peer:
            case = self.run_inputs('run-unknown', peer)
            process = subprocess.Popen(self.run_arguments(case, '--yes'), cwd=case['directory'], env=self.environment,
                stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
            self.addCleanup(lambda: process.poll() is None and process.kill())
            self.assertTrue(requested.wait(20))
            process.send_signal(signal.SIGINT)
            stdout, stderr = process.communicate(timeout=30)
            self.assertEqual(process.returncode, 130, stderr + stdout)
            first = json.loads(stdout)
            self.assertFalse(first['accounting']['ready'])
            database = next(case['data'].glob('requests/*/control.sqlite'))
            original = rows(database, 'SELECT attempt_id,status,settlement_sha256 FROM call_attempts')
            self.assertEqual(len(original), 1)
            resumed = self.invoke_run(case, '--yes')
            self.assertEqual(resumed.returncode, 0, resumed.stderr)
            summary = json.loads(resumed.stdout)
            self.assertEqual(summary['stop_reason'], 'needs_accounting')
            self.assertFalse(summary['accounting']['ready'])
            self.assertIsNone(summary['original_tokens']['input_tokens'])
            self.assertEqual(rows(database, 'SELECT attempt_id,status,settlement_sha256 FROM call_attempts'), original)
            self.assertEqual(len(peer.requests), 1)
            system = copy.deepcopy(case['system'])
            system['budget_defaults']['warn_at_fraction'] = 0.75
            write_json(case['configuration'], system)
            rejected = self.invoke_run(case, '--yes')
            self.assertEqual(rejected.returncode, 3)
            self.assertFalse(rejected.stdout)
            self.assertNotIn('wls-run-confirmation', rejected.stderr)
            self.assertEqual(len(peer.requests), 1)
            release.set()

    def test_09b_run_resumes_exact_intent_after_empty_store_initialization(self):
        endpoint = PricingFlowEndpoint(initialize_seed=True)
        with tls.Peer(self.contexts['server'], endpoint) as peer:
            case = self.run_inputs('run-partial-admission', peer)
            declined = self.invoke_run(case, answer='no\n')
            self.assertEqual(declined.returncode, 2)
            request_id = json.loads(declined.stderr.splitlines()[0])['request_id']
            # Freeze the exact configuration via the original loader/admission
            # in a separate private reference DB. It is never moved or erased.
            reference = case['directory'] / 'reference.sqlite'
            self.command('submit', '--db', reference, '--key', 'freeze-reference',
                '--config', case['configuration'], '--text-file', case['material'],
                '--constraints-file', case['constraints'], '--max-attempts', 40,
                '--input-budget', 4000000, '--output-budget', 1000000, '--cost-budget-micro-usd', 8000000)
            frozen = rows(reference, 'SELECT config_snapshot,config_snapshot_sha256,budget '
                'FROM requests JOIN request_runtime USING(request_id)')[0]
            request = case['data'] / 'requests' / request_id
            paths = (case['data'], case['data'] / 'requests', request, request / 'workspaces', request / 'delivery')
            for path in paths:
                path.mkdir(mode=0o700)
            roots = [{'path': str(path), 'device': path.stat().st_dev, 'inode': path.stat().st_ino} for path in paths]
            key = digest({'format': 'run-key-v1', 'text': case['material'].read_text(),
                'constraints': [], 'config_locator': str(case['configuration'].absolute())})
            intent = {'format': 'wls-run-intent-v1', 'admission': {'run_key_sha256': key,
                'request_id': request_id, 'config_locator': str(case['configuration'].absolute()),
                'original_text': case['material'].read_text(), 'constraints': [],
                'config_snapshot': json.loads(frozen['config_snapshot']), 'budget': json.loads(frozen['budget'])},
                'roots': roots}
            with (request / 'intent.json').open('xb') as stream:
                stream.write(canonical(intent))
                stream.flush()
                os.fsync(stream.fileno())
            (request / 'intent.json').chmod(0o600)
            database = request / 'control.sqlite'
            self.command('init', '--db', database)
            self.assertEqual(rows(database, 'PRAGMA user_version')[0]['user_version'], 410)
            self.assertEqual(rows(database, 'SELECT count(*) n FROM requests')[0]['n'], 0)
            self.assertEqual(rows(database, 'SELECT count(*) n FROM events')[0]['n'], 0)
            self.assertEqual(len(peer.requests), 0)
            original_intent = (request / 'intent.json').read_bytes()
            resumed = self.invoke_run(case, '--yes')
            self.assertEqual(resumed.returncode, 0, resumed.stderr + resumed.stdout)
            summary = json.loads(resumed.stdout)
            self.assertEqual((summary['request_id'], summary['status']), (request_id, 'DELIVERED'))
            admitted = rows(database, 'SELECT request_id,config_snapshot,config_snapshot_sha256,budget '
                'FROM requests JOIN request_runtime USING(request_id)')
            self.assertEqual(len(admitted), 1)
            self.assertEqual({key: admitted[0][key] for key in frozen}, frozen)
            self.assertEqual(rows(database, "SELECT count(*) n FROM events WHERE type='admit'")[0]['n'], 1)
            self.assertEqual((request / 'intent.json').read_bytes(), original_intent)
            self.assertEqual(len(peer.requests), 31)

    def test_10_unsigned_estimated_billing_completes_thirty_one_original_calls(self):
        """One authorization: the first batch hits the frozen manager cap; an explicit second call completes 31 billings.

        Dispatcher serves submit_plan plus three accept_candidate (4), each task's
        TeamLead serves issue_work_order/check_submission/report_experience (9),
        and each task's Worker performs five file tools then submit (18). The
        frozen limit of 12 manager tool calls per wake retains gamma's final
        experience call after 30 settled wire calls; the unchanged
        authorization then drives one further explicit CLI batch that settles
        the 31st call. This test itself performs the continuation across two
        pump-live invocations: it is not single-pump completion and not
        product-side automatic multi-batch progress.
        """
        endpoint = PricingFlowEndpoint()
        with tls.Peer(self.contexts['server'], endpoint) as peer:
            profile = copy.deepcopy(self.profile)
            provider = profile['responses']['provider_profile']
            provider.update(base_url=f'https://localhost:{peer.port}',
                            profile_id='independent-estimated-billing-loopback')
            provider.pop('billing_challenge', None)
            provider['pricing'] = PRICING
            case = self.admit_case('estimated-billing', 0, 'multi_agent', register=False,
                                   profile=profile, cost_budget=8000000,
                                   nodes=('alpha', 'beta', 'gamma'))
            for node in ('alpha', 'beta', 'gamma'):
                (Path(case['workspaces'][node]) / 'seed.txt').write_bytes(b'alpha\nbeta\n')
            status = self.command('live-status', '--db', case['database'], '--request', case['request'])
            authorization = status['authorization_template']
            self.assertEqual(authorization['schema_version'], 1)
            authorization.update(network_enabled=True, reserved_cost_micro_usd=200000,
                max_calls=64, max_worker_calls_per_task=16, max_batch_duration_ms=120000)
            write_json(case['authorization'], authorization)
            authorization_sha256 = hashlib.sha256(case['authorization'].read_bytes()).hexdigest()
            frozen = json.loads(rows(case['database'], 'SELECT config_snapshot FROM requests '
                'WHERE request_id=?', (case['request'],))[0]['config_snapshot'])
            self.assertEqual(frozen['system']['limits']['manager_max_tool_calls_per_wake'], 12)
            first = self.command('pump-live', '--db', case['database'], '--request', case['request'],
                '--authorization-file', case['authorization'], '--workspaces-file', case['mapping'])
            # First batch: the frozen manager cap retains gamma's experience work.
            self.assertEqual(first['stop_reason'], 'quiescent', first['blocked'])
            self.assertEqual(len(first['calls']), 30)
            self.assertEqual(first['network_started_calls'], 30)
            self.assertEqual(first['manager_calls'], 12)
            first_roles = {}
            for call in first['calls']:
                first_roles[call['role']] = first_roles.get(call['role'], 0) + 1
            self.assertEqual(first_roles, {'dispatcher': 4, 'team_lead': 8, 'worker': 18})
            self.assertEqual(len(peer.requests), 30)
            self.assertEqual(len(endpoint.actions), 30)
            self.assertEqual(first['budget']['attempts'], 30)
            self.assertEqual(first['budget']['spent_cost'], 30 * PRICED_UNIT_MICRO_USD)
            self.assertEqual(first['budget']['reserved_cost'], 0)
            self.assertEqual(first['budget']['unknown_cost'], 0)
            self.assertTrue(first['accounting']['ready'], first['accounting'])
            self.assertEqual(first['accounting']['blocked_count'], 0)
            self.assertEqual(first['accounting']['blocked_attempts'], [])
            self.assertEqual(len(first['blocked']), 1)
            self.assertEqual((first['blocked'][0]['code'], first['blocked'][0]['role'],
                              first['blocked'][0]['node_id']), ('manager_limit', 'team_lead', 'gamma'))
            self.assertEqual((first['request_id'], first['config_snapshot_sha256'],
                              first['host_authorization_sha256']),
                             (case['request'], status['config_snapshot_sha256'], authorization_sha256))
            self.assertEqual(first['request_status'], 'RUNNING')
            self.assertFalse(first['business_delivered'])
            task_rows = rows(case['database'], 'SELECT task_id,node_id,status FROM tasks '
                             'WHERE request_id=?', (case['request'],))
            node_tasks = {row['node_id']: row['task_id'] for row in task_rows}
            task_nodes = {task: node for node, task in node_tasks.items()}
            self.assertEqual({row['node_id']: row['status'] for row in task_rows},
                             {'alpha': 'ACCEPTED', 'beta': 'ACCEPTED', 'gamma': 'ACCEPTED'})
            self.assertEqual(first['blocked'][0]['task_id'], node_tasks['gamma'])
            self.assertEqual({task_nodes[row['task_id']]: row['status'] for row in rows(case['database'],
                'SELECT task_id,status FROM experience_jobs WHERE request_id=?', (case['request'],))},
                {'alpha': 'DONE', 'beta': 'DONE', 'gamma': 'PENDING'})
            self.assertEqual([call['node_id'] for call in first['calls']
                              if call['workflow_kind'] == 'experience'], ['alpha', 'beta'])
            for table in ('call_attempts', 'live_send_claims', 'live_transport_observations',
                          'model_tool_feedback'):
                self.assertEqual(rows(case['database'], f'SELECT count(*) AS n FROM {table}')[0]['n'],
                                 30, table)
            batch_one = rows(case['database'],
                "SELECT a.attempt_id,a.role,a.team_id,"
                "json_extract(a.response,'$.tool_calls[0].name') AS tool FROM call_attempts a "
                "WHERE a.request_id=? ORDER BY rowid", (case['request'],))
            self.assertEqual(len(batch_one), 30)
            self.assertEqual([row['tool'] for row in batch_one], endpoint.actions)
            self.assertEqual([row['tool'] for row in batch_one if row['role'] == 'dispatcher'],
                             ['submit_plan', 'accept_candidate', 'accept_candidate', 'accept_candidate'])
            nodes = {row['team_id']: row['node_id'] for row in
                     rows(case['database'], 'SELECT t.team_id,k.node_id FROM teams t '
                          'JOIN tasks k ON k.task_id=t.task_id WHERE t.request_id=?',
                          (case['request'],))}
            for node in ('alpha', 'beta'):
                own = [row for row in batch_one if nodes.get(row['team_id']) == node]
                self.assertEqual([row['tool'] for row in own if row['role'] == 'team_lead'],
                                 ['issue_work_order', 'check_submission', 'report_experience'])
                self.assertEqual([row['tool'] for row in own if row['role'] == 'worker'],
                                 ['fs_read', 'fs_write', 'fs_read', 'fs_write', 'fs_read', 'submit'])
            own = [row for row in batch_one if nodes.get(row['team_id']) == 'gamma']
            self.assertEqual([row['tool'] for row in own if row['role'] == 'team_lead'],
                             ['issue_work_order', 'check_submission'])
            self.assertEqual([row['tool'] for row in own if row['role'] == 'worker'],
                             ['fs_read', 'fs_write', 'fs_read', 'fs_write', 'fs_read', 'submit'])
            # Explicit second batch: same request, database, authorization and workspaces.
            second = self.command('pump-live', '--db', case['database'], '--request', case['request'],
                '--authorization-file', case['authorization'], '--workspaces-file', case['mapping'])
            self.assertEqual(hashlib.sha256(case['authorization'].read_bytes()).hexdigest(),
                             authorization_sha256)
            self.assertEqual(len(second['calls']), 1)
            completion = second['calls'][0]
            self.assertEqual((completion['role'], completion['node_id'], completion['workflow_kind']),
                             ('team_lead', 'gamma', 'experience'))
            self.assertNotIn(completion['attempt_id'], {row['attempt_id'] for row in batch_one})
            self.assertEqual(second['manager_calls'], 1)
            self.assertEqual(second['network_started_calls'], 1)
            self.assertEqual(second['blocked'], [])
            self.assertEqual(second['stop_reason'], 'quiescent')
            self.assertEqual(second['host_authorization_sha256'], authorization_sha256)
            self.assertEqual(second['request_status'], 'RUNNING')
            self.assertFalse(second['business_delivered'])
            self.assertEqual(len(endpoint.actions), PRICING_CALLS)
            self.assertEqual(len(peer.requests), PRICING_CALLS)
            for request in peer.requests:
                self.assertNotIn(b'x-wls-billing-challenge', request['headers'])
            attempts = rows(case['database'],
                "SELECT a.attempt_id,a.role,a.team_id,a.status,a.input_tokens,a.output_tokens,"
                "a.cache_read_tokens,a.cache_write_tokens,a.cost_micro_usd,a.settlement_sha256,"
                "json_extract(a.response,'$.tool_calls[0].name') AS tool FROM call_attempts a "
                "WHERE a.request_id=? ORDER BY rowid", (case['request'],))
            self.assertEqual(len(attempts), PRICING_CALLS)
            self.assertLess({row['attempt_id'] for row in batch_one},
                            {row['attempt_id'] for row in attempts})
            self.assertTrue(all(row['settlement_sha256'] for row in attempts))
            self.assertTrue(all(row['input_tokens'] == 11 and row['output_tokens'] == 7 and
                                row['cache_read_tokens'] == 3 for row in attempts))
            self.assertTrue(all(row['cache_write_tokens'] is None and row['cost_micro_usd'] is None
                                for row in attempts))
            roles = {}
            for row in attempts:
                roles[row['role']] = roles.get(row['role'], 0) + 1
            self.assertEqual(roles, {'dispatcher': 4, 'team_lead': 9, 'worker': 18})
            self.assertEqual([row['tool'] for row in attempts], endpoint.actions)
            self.assertEqual([row['tool'] for row in attempts if row['role'] == 'dispatcher'],
                             ['submit_plan', 'accept_candidate', 'accept_candidate', 'accept_candidate'])
            nodes = {row['team_id']: row['node_id'] for row in
                     rows(case['database'], 'SELECT t.team_id,k.node_id FROM teams t '
                          'JOIN tasks k ON k.task_id=t.task_id WHERE t.request_id=?',
                          (case['request'],))}
            for node in ('alpha', 'beta', 'gamma'):
                own = [row for row in attempts if nodes.get(row['team_id']) == node]
                self.assertEqual([row['tool'] for row in own if row['role'] == 'team_lead'],
                                 ['issue_work_order', 'check_submission', 'report_experience'])
                self.assertEqual([row['tool'] for row in own if row['role'] == 'worker'],
                                 ['fs_read', 'fs_write', 'fs_read', 'fs_write', 'fs_read', 'submit'])
            for table, expected in (('live_send_claims', PRICING_CALLS),
                                    ('live_transport_observations', PRICING_CALLS),
                                    ('model_tool_feedback', PRICING_CALLS),
                                    ('usage_reconciliations', 0),
                                    ('live_usage_projection', PRICING_CALLS)):
                self.assertEqual(rows(case['database'], f'SELECT count(*) AS n FROM {table}')[0]['n'],
                                 expected, table)
            self.assertTrue(all(row['cost_micro_usd'] is None and row['cache_write_tokens'] is None and
                                not row['disputed'] and not row['over_reserved'] for row in
                                rows(case['database'], 'SELECT cost_micro_usd,cache_write_tokens,'
                                     'disputed,over_reserved FROM live_usage_projection')))
            budget = self.command('budget', '--db', case['database'], '--request', case['request'])
            expected_total = PRICING_CALLS * PRICED_UNIT_MICRO_USD
            self.assertEqual(budget['spent_cost'], expected_total)
            self.assertEqual(budget['reserved_cost'], 0)
            self.assertEqual(budget['unknown_cost'], 0)
            self.assertEqual(budget['accounting_format'], 'wls-estimated-budget-v1')
            self.assertEqual(budget['cost_breakdown']['estimated_micro_usd'], expected_total)
            self.assertEqual(budget['cost_breakdown']['reported_micro_usd'], 0)
            self.assertEqual(budget['cost_breakdown']['unknown_reserved_micro_usd'], 0)
            status = self.command('live-status', '--db', case['database'], '--request', case['request'])
            self.assertTrue(status['accounting']['ready'], status['accounting'])
            self.assertEqual(status['accounting']['blocked_count'], 0)
            self.assertEqual(status['accounting']['estimated_cost_micro_usd'], expected_total)
            self.assertEqual(status['accounting']['reported_cost_micro_usd'], 0)
            self.assertNotIn('actual', json.dumps(status['accounting']))
            self.assertNotIn('actual', json.dumps(budget['cost_breakdown']))
            # The removed retry loop's exit conditions stay explicit assertions.
            self.assertEqual([row['status'] for row in rows(case['database'],
                'SELECT status FROM tasks WHERE request_id=? ORDER BY rowid', (case['request'],))],
                ['ACCEPTED'] * 3)
            self.assertEqual([row['status'] for row in rows(case['database'],
                'SELECT status FROM experience_jobs WHERE request_id=? ORDER BY rowid',
                (case['request'],))], ['DONE'] * 3)
            print('INDEPENDENT_PRICED_BATCH_SUMMARY ' + json.dumps({'first_calls': len(first['calls']),
                'second_calls': len(second['calls']), 'first_manager_calls': first['manager_calls'],
                'first_spent_micro_usd': first['budget']['spent_cost'],
                'final_spent_micro_usd': second['budget']['spent_cost'],
                'first_blocked': [entry['code'] for entry in first['blocked']],
                'final_request_status': second['request_status'],
                'final_business_delivered': second['business_delivered']}, sort_keys=True))

    def test_11_bounded_command_completes_three_tasks_with_one_corrected_failure(self):
        endpoint = PricingFlowEndpoint(correctable_read=True)
        with tls.Peer(self.contexts['server'], endpoint) as peer:
            profile = copy.deepcopy(self.profile)
            provider = profile['responses']['provider_profile']
            provider.update(base_url=f'https://localhost:{peer.port}')
            provider['capabilities']['max_output_tokens'] = 16384
            provider.pop('billing_challenge', None)
            provider['pricing'] = PRICING
            profile['capabilities']['max_output_tokens'] = 16384
            case = self.admit_case('bounded-three-tasks', 0, 'multi_agent', register=False,
                profile=profile, cost_budget=32000000, nodes=('alpha', 'beta', 'gamma'), bounded=True)
            for node in ('alpha', 'beta', 'gamma'):
                (Path(case['workspaces'][node]) / 'seed.txt').write_bytes(b'alpha\nbeta\n')
            status = self.command('live-status', '--db', case['database'], '--request', case['request'])
            authorization = status['authorization_template']
            self.assertEqual((authorization['schema_version'], authorization['max_total_calls']), (3, 40))
            authorization.update(network_enabled=True, reserved_cost_micro_usd=800000,
                max_calls=64, max_worker_calls_per_task=16, max_batch_duration_ms=120000)
            write_json(case['authorization'], authorization)
            report = self.command('pump-live', '--db', case['database'], '--request', case['request'],
                '--authorization-file', case['authorization'], '--workspaces-file', case['mapping'])
            self.assertEqual((report['wake_count'], len(report['calls']), report['manager_calls']), (2, 32, 13))
            self.assertEqual(report['stop_reason'], 'quiescent', report['blocked'])
            self.assertEqual(report['blocked'], [])
            self.assertEqual(report['network_started_calls'], 32)
            self.assertEqual(report['host_authorization_sha256'], hashlib.sha256(case['authorization'].read_bytes()).hexdigest())
            self.assertEqual(report['budget']['spent_cost'], 32 * PRICED_UNIT_MICRO_USD)
            self.assertTrue(report['accounting']['ready'])
            attempts = rows(case['database'], 'SELECT attempt_id,request_envelope,response,usage '
                'FROM call_attempts WHERE request_id=? ORDER BY rowid', (case['request'],))
            self.assertEqual(len({row['attempt_id'] for row in attempts}), 32)
            self.assertEqual(len(peer.requests), 32)
            for attempt, sent, call in zip(attempts, peer.requests, report['calls']):
                captured = json.loads(attempt['request_envelope'])
                wire = json.loads(sent['body'])
                self.assertEqual(wire['max_output_tokens'], 16384)
                self.assertGreaterEqual(captured['reserved_input_tokens'], len(sent['body']))
                self.assertEqual(call['input_estimate']['basis'], 'utf8_bytes_div_3_rough')
                self.assertGreater(call['input_estimate']['reserved_input_tokens'], call['input_estimate']['tokens'])
                self.assertIsNone(json.loads(attempt['usage'])['cost_micro_usd'])
            corrections = [json.loads(row['result']) for row in rows(case['database'],
                'SELECT result FROM model_tool_feedback') if 'bounded_runtime_v1' in row['result']]
            self.assertEqual(len(corrections), 1)
            self.assertEqual(corrections[0]['correction_feedback']['bounded_runtime_v1']['same_error_repeats'], 1)
            for table in ('live_send_claims', 'live_transport_observations', 'model_tool_feedback'):
                self.assertEqual(rows(case['database'], f'SELECT count(*) AS n FROM {table}')[0]['n'], 32)
            for table in ('tasks', 'experience_jobs'):
                self.assertEqual([row['status'] for row in rows(case['database'],
                    f'SELECT status FROM {table} ORDER BY rowid')], ['ACCEPTED' if table == 'tasks' else 'DONE'] * 3)

    def test_12_signed_v3_total_cap_and_legacy_authorization_limits(self):
        case = self.admit_case('bounded-signed-limits', 0, 'multi_agent', bounded=True)
        authorization = self.authorization(case, 1)
        for version in (1, 2, 3):
            selected = copy.deepcopy(authorization)
            selected.update(schema_version=version, kind=f'wls-live-workflow-authorization-v{version}')
            if version != 3:
                selected.pop('max_total_calls')
            if version == 1:
                selected.pop('billing_policy_sha256')
                write_json(case['authorization'], selected)
                self.command('pump-live', '--db', case['database'], '--request', case['request'],
                    '--authorization-file', case['authorization'], '--billing-policy-file', case['policy'],
                    '--workspaces-file', case['mapping'], error=True)
                self.assertEqual(len(self.endpoint.records[case['request']]), 0)
                continue
            if version == 3:
                selected['max_total_calls'] = 2
            write_json(case['authorization'], selected)
            report = self.command('pump-live', '--db', case['database'], '--request', case['request'],
                '--authorization-file', case['authorization'], '--billing-policy-file', case['policy'],
                '--workspaces-file', case['mapping'])
            self.assertEqual(len(report['calls']), 1 if version == 2 else 2)
            self.assertEqual(report['stop_reason'], 'call_limit' if version == 2 else 'total_call_limit')
        self.assertEqual(len(self.endpoint.records[case['request']]), 3)

    def test_13_bounded_continuation_stops_at_a_missing_workspace(self):
        endpoint = PricingFlowEndpoint()
        with tls.Peer(self.contexts['server'], endpoint) as peer:
            profile = copy.deepcopy(self.profile)
            provider = profile['responses']['provider_profile']
            provider.update(base_url=f'https://localhost:{peer.port}')
            provider.pop('billing_challenge', None)
            provider['pricing'] = PRICING
            case = self.admit_case('bounded-missing-workspace', 0, 'multi_agent', register=False,
                profile=profile, cost_budget=8000000, nodes=('alpha', 'beta', 'gamma'), bounded=True)
            for node in ('alpha', 'beta', 'gamma'):
                (Path(case['workspaces'][node]) / 'seed.txt').write_bytes(b'alpha\nbeta\n')
            write_json(case['mapping'], {node: path for node, path in case['workspaces'].items() if node != 'gamma'})
            authorization = self.command('live-status', '--db', case['database'],
                                         '--request', case['request'])['authorization_template']
            authorization.update(network_enabled=True, reserved_cost_micro_usd=200000,
                max_calls=1, max_worker_calls_per_task=16, max_batch_duration_ms=120000)
            write_json(case['authorization'], authorization)
            report = self.command('pump-live', '--db', case['database'], '--request', case['request'],
                '--authorization-file', case['authorization'], '--workspaces-file', case['mapping'])
            self.assertEqual(report['wake_count'], len(report['calls']))  # No extra empty wake after the real blocker.
            self.assertEqual(len(report['calls']), 22)
            self.assertEqual(len(peer.requests), 22)
            self.assertFalse(report['resume_after_wake_limit'])
            self.assertTrue(any(item['code'] == 'workspace_missing' and item['node_id'] == 'gamma'
                                for item in report['blocked']))
            self.assertEqual(report['budget']['attempts'], 22)
            self.assertEqual(rows(case['database'], 'SELECT count(*) AS n FROM call_attempts')[0]['n'], 22)
            self.assertEqual(rows(case['database'], 'SELECT count(*) AS n FROM live_send_claims')[0]['n'], 22)

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

    def test_91_pricing_does_not_replace_a_required_signed_receipt(self):
        # A frozen price table on a challenge profile cannot silently stand in
        # for the failed signature; the original signed path stays stopped.
        profile = copy.deepcopy(self.profile)
        profile['responses']['provider_profile']['pricing'] = PRICING
        case = self.admit_case('invalid-priced-signature', 4, 'multi_agent',
                               receipt_mode='bad_signature', profile=profile, cost_budget=1000000)
        template = self.command('live-status', '--db', case['database'], '--request', case['request'],
                                '--billing-policy-file', case['policy'])['authorization_template']
        template.update(network_enabled=True, reserved_cost_micro_usd=200000,
                        max_calls=6, max_worker_calls_per_task=16, max_batch_duration_ms=60000)
        write_json(case['authorization'], template)
        report = self.command('pump-live', '--db', case['database'], '--request', case['request'],
            '--authorization-file', case['authorization'], '--billing-policy-file', case['policy'],
            '--workspaces-file', case['mapping'])
        self.assertIn(report['stop_reason'], ('error', 'needs_accounting'))
        self.assertEqual(len(self.endpoint.records[case['request']]), 1)
        self.assertEqual(rows(case['database'], 'SELECT * FROM usage_reconciliations'), [])
        self.assertEqual(rows(case['database'], 'SELECT * FROM tasks'), [])
        again = self.command('pump-live', '--db', case['database'], '--request', case['request'],
            '--authorization-file', case['authorization'], '--billing-policy-file', case['policy'],
            '--workspaces-file', case['mapping'])
        self.assertEqual(again['stop_reason'], 'needs_accounting')
        self.assertEqual(len(self.endpoint.records[case['request']]), 1)
        attempt = rows(case['database'], 'SELECT cost_micro_usd FROM call_attempts')[0]
        self.assertIsNone(attempt['cost_micro_usd'])
        status = self.command('live-status', '--db', case['database'], '--request', case['request'],
                              '--billing-policy-file', case['policy'])
        self.assertFalse(status['accounting']['ready'])
        self.assertEqual(status['accounting']['estimated_cost_micro_usd'], 0)

    def test_91_partial_unknown_and_over_reserved_remain_stopped(self):
        for variant in ('partial_unknown', 'over_reserved'):
            with self.subTest(variant=variant):
                case = self.admit_case('accounting-' + variant, 4, 'multi_agent', receipt_mode=variant, bounded=True)
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
