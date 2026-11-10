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
import stat
import subprocess
import sys
import tempfile
import unittest
import uuid

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
    def __init__(self, directory, mode='complete', bad_receipt=False, text_receipt='valid'):
        self.receipts = directory / 'receipts'
        self.receipts.mkdir(mode=0o700)
        self.mode, self.bad_receipt = mode, bad_receipt
        self.text_receipt = text_receipt
        fixture = without_constraints(json.loads((SOURCE / 'tests/fixtures/s15_fake_complete.json').read_text()))
        self.plan = fixture['default'][0]['tool_calls'][0]['arguments']
        if mode in ('amend', 'cross_task'):
            beta = copy.deepcopy(self.plan['nodes'][0])
            beta['node_id'] = 'beta'
            beta['title'] = 'Independent beta'
            self.plan['nodes'].append(beta)
        if mode in ('correction_siblings', 'correction_guard_siblings', 'correction_worker_cap_sibling'):
            # The plan keeps its original [beta, alpha] node bytes; that byte
            # order is preserved identity, not a claim about runtime order.
            # Actual run order is only what the captured contracts and report
            # calls show.
            beta = copy.deepcopy(self.plan['nodes'][0])
            beta['node_id'] = 'beta'
            beta['title'] = 'Independent beta sibling'
            self.plan['nodes'] = [beta, self.plan['nodes'][0]]
        self.order = fixture['nodes']['alpha'][0]['tool_calls'][0]['arguments']
        self.submission = fixture['nodes']['alpha'][2]['tool_calls'][0]['arguments']
        self.check = fixture['nodes']['alpha'][3]['tool_calls'][0]['arguments']
        self.acceptance = fixture['default'][1]['tool_calls'][0]['arguments']
        self.steps, self.contents = {}, {}
        self.actions, self.contracts = [], []
        self.primary_task = None
        # G1-3 controlled-local-TLS correction scripts. A None tool name is a
        # deliberate zero-tool text round; sentinel names are peer-internal and
        # never reach the protocol as a tool call.
        self.correction_sequences = {
            'correction_text': [
                (None, 'synthetic text-only round before a corrected tool call'),
                ('fs_write', {'path': 'result.txt', 'content': 'text corrected candidate\n'}),
                ('submit', self.submission)],
            'correction_schema': [
                ('fs_write', {'path': 'result.txt'}),
                ('fs_write', {'path': 'result.txt', 'content': 'args corrected candidate\n'}),
                ('submit', self.submission)],
            'correction_missing': [
                ('fs_read', {'path': 'absent.txt'}),
                ('fs_read', {'path': 'present.txt'}),
                ('fs_write', {'path': 'result.txt', 'content': 'read corrected candidate\n'}),
                ('submit', self.submission)],
            'correction_precondition': [
                ('fs_write', {'path': 'result.txt', 'content': 'must not overwrite\n'}),
                ('fs_write', {'path': 'result.txt', 'content': 'cas corrected candidate\n',
                              'expected_sha256': hashlib.sha256(b'original candidate\n').hexdigest()}),
                ('submit', self.submission)],
            'two_tool_calls': [
                ('__two_calls__', {'left': {'name': 'fs_write', 'arguments': {'path': 'left.txt', 'content': 'left\n'}},
                                   'right': {'name': 'fs_write', 'arguments': {'path': 'right.txt', 'content': 'right\n'}}}),
                ('fs_write', {'path': 'result.txt', 'content': 'never reached\n'})],
            # G1-3 v8 C1/C2 scripts: admissions must accumulate across a
            # completed claim (the success ends one claim; the next failure
            # arrives on a fresh claim of the same binding), and consecutive
            # failures exhaust the frozen cap inside one claim.
            'correction_cross_claim': [
                (None, 'first text-only failure before a corrected write'),
                ('fs_write', {'path': 'corrected.txt', 'content': 'cross-claim corrected\n'}),
                (None, 'second text-only failure on a fresh completed claim'),
                ('fs_write', {'path': 'extra.txt', 'content': 'must never run after exhaustion\n'}),
                ('submit', self.submission)],
            'correction_exhaust': [
                (None, 'first text-only failure'),
                (None, 'second text-only failure'),
                (None, 'third text-only failure exhausting the frozen cap'),
                ('fs_write', {'path': 'never.txt', 'content': 'never\n'})],
            'undeclared_tool': [
                ('submit_plan', {'summary': 'worker must not gain planning authority'}),
                ('fs_write', {'path': 'result.txt', 'content': 'never reached\n'})],
            'server_error': [
                ('__http_error__', None),
                ('fs_write', {'path': 'result.txt', 'content': 'never reached\n'})],
        }

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
        if self.mode in ('correction_siblings', 'correction_guard_siblings', 'correction_worker_cap_sibling'):
            # G1-3 FINAL v18 F2/F3: the two explicit sibling modes select their
            # scripts from the original contract's task.node_id -- alpha is the
            # first successful still-runnable sibling, beta the Worker that
            # owns the ordinary failures -- never from first-seen task ids or
            # plan node order. The retained guard mode below keeps its original
            # first-seen semantics untouched.
            if self.mode in ('correction_siblings', 'correction_worker_cap_sibling'):
                task = contract['task']
                if task is None or task['task_id'] != binding['task_id']:
                    raise AssertionError(f'sibling contract task/binding mismatch: {task!r}')
                node = task['node_id']
                if node not in ('alpha', 'beta'):
                    raise AssertionError(f'unexpected sibling node {node!r}')
                if node == 'alpha':
                    sibling = [('fs_write', {'path': 'result.txt', 'content': 'sibling progress\n'})]
                    if self.mode == 'correction_siblings':
                        # G1-3 FINAL v18 F2 L8: one original write plus fifteen
                        # reads of the same path fill exactly the finite default
                        # per-task host capacity of sixteen; beta's calls never
                        # consume alpha slots and there is no fallback entry.
                        sibling.extend([('fs_read', {'path': 'result.txt'}) for _ in range(15)])
                    else:
                        # FINAL v12 2.2/4 L7: alpha's own per-task cap is two,
                        # so the original second step stays a read of the path.
                        sibling.append(('fs_read', {'path': 'result.txt'}))
                elif self.mode == 'correction_siblings':
                    sibling = [(None, 'beta text-only failure without its own cap shortfall'),
                               (None, 'beta second text-only failure')]
                else:
                    # FINAL v12 2.2/4 L7: beta owns the two ordinary Worker
                    # failures that exhaust its own legal per-task cap of two
                    # while the schema cap still has a free admission.
                    sibling = [(None, 'beta first text-only failure before the host worker cap'),
                               (None, 'beta second text-only failure while schema retry remains')]
                return sibling[index]
            if binding['task_id'] == self.primary_task:
                sibling = [('fs_write', {'path': 'result.txt', 'content': 'sibling progress\n'}),
                           ('fs_read', {'path': 'result.txt'})]
            else:
                sibling = [(None, 'alpha text-only failure before a host worker cap')]
            return sibling[index]
        correction = self.correction_sequences.get(self.mode)
        if correction is not None:
            return correction[index]
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
        usage = {'input_tokens': 9, 'output_tokens': 3, 'cache_read_tokens': 2,
                 'cache_write_tokens': 1, 'cost_micro_usd': 17}
        if name == '__http_error__':
            # A transport failure never enters a correction branch.
            self.actions.append('http_error')
            payload = tls.encoded({'error': {'message': 'synthetic worker failure', 'code': 'fixture'}})
            connection.sendall(tls.headers(len(payload), status=500) + payload)
            return
        if name is None:
            # Deliberate zero-tool Worker round: non-empty text, no native call
            # and no fabricated output. The body-keyed independent receipt is
            # persisted before the reply, exactly like a call-ID receipt.
            self.actions.append('text_only')
            value = json.loads(tls.response_body())
            value['id'] = f'resp_workflow_{len(self.actions)}'
            value['output'][0]['content'][0]['text'] = arguments
            encoded = tls.encoded(value)
            body_hash = hashlib.sha256(observed['body']).hexdigest()
            if self.text_receipt != 'missing':
                receipt = {'fixture': 'loopback-workflow-billing-v1', 'kind': 'text_only_zero_tool',
                           'request_body_sha256': body_hash,
                           'response_body_sha256': hashlib.sha256(encoded).hexdigest(),
                           'response_id': value['id'], 'usage': usage}
                if self.text_receipt == 'wrong_request':
                    receipt['request_body_sha256'] = '0' * 64
                if self.text_receipt == 'wrong_digest':
                    receipt['response_body_sha256'] = '1' * 64
                if self.text_receipt == 'wrong_id':
                    receipt['response_id'] = 'resp_wrong_zero_tool_fixture'
                (self.receipts / (body_hash + '.json')).write_text(json.dumps(receipt))
            connection.sendall(tls.headers(len(encoded)) + encoded)
            return
        if name == '__two_calls__':
            left, right = arguments['left'], arguments['right']
            for spec in (left, right):
                if spec['name'] not in names:
                    raise AssertionError(f"{spec['name']} was not declared: {sorted(names)}")
            first_id = f'fixture-call-{len(self.actions) + 1}'
            second_id = f'fixture-call-{len(self.actions) + 2}'
            self.actions.append('two_fs_write')
            for call_id in (first_id, second_id):
                receipt = {'fixture': 'loopback-workflow-billing-v1', 'call_id': call_id,
                           'request_body_sha256': hashlib.sha256(observed['body']).hexdigest(), 'usage': usage}
                (self.receipts / (call_id + '.json')).write_text(json.dumps(receipt))
            value = json.loads(tls.response_body(tool=True))
            item = value['output'][0]
            second = json.loads(json.dumps(item))
            item.update(call_id=first_id, name=left['name'],
                        arguments=json.dumps(left['arguments'], ensure_ascii=False))
            second.update(id='fc_second_fixture_call', call_id=second_id, name=right['name'],
                          arguments=json.dumps(right['arguments'], ensure_ascii=False))
            value['output'] = [item, second]
            encoded = tls.encoded(value)
            connection.sendall(tls.headers(len(encoded)) + encoded)
            return
        if name not in names and self.mode != 'undeclared_tool':
            raise AssertionError(f'{name} was not declared: {sorted(names)}')
        call_id = f'fixture-call-{len(self.actions) + 1}'
        self.actions.append(name)
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

    # G1-3 FINAL v14 3 (D2) local original capture for the C1/C2 live
    # regressions (L5-L8). The enable state is frozen by the execution
    # environment's WLS_G1_EVIDENCE_ROOT key and is constant for one test
    # process: a completely absent key disables only this optional capture
    # I/O while every business action, assertion and fixture cleanup still
    # runs (such a run never counts as retained evidence); a present value
    # must name this run's bound owner-only directory -- absolute, a real
    # directory rather than a symlink, owned by the current user, mode 0o700
    # -- and any other present value is an explicit evidence failure, never a
    # silent fallback to a default location.
    def evidence_root(self):
        if 'WLS_G1_EVIDENCE_ROOT' not in os.environ:
            return None
        root = os.environ['WLS_G1_EVIDENCE_ROOT']
        if not root.startswith('/'):
            raise AssertionError(f'G1 evidence root must be absolute: {root!r}')
        try:
            info = os.lstat(root)
        except OSError as error:
            raise AssertionError(f'G1 evidence root not usable: {root!r}: {error}') from error
        if (os.path.islink(root) or not stat.S_ISDIR(info.st_mode)
                or info.st_uid != os.geteuid()
                or stat.S_IMODE(info.st_mode) != 0o700):
            raise AssertionError('G1 evidence root is not a bound absolute '
                                 f'owner-only 0700 directory: {root!r}')
        return Path(root)

    def capture_bytes(self, name, payload):
        root = self.evidence_root()
        if root is None:
            return None
        target = root / f'live_workflow-{self._testMethodName}-{uuid.uuid4().hex}-{name}'
        try:
            descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL
                                 | os.O_NOFOLLOW, 0o600)
        except OSError as error:
            raise AssertionError('G1 evidence target cannot be created '
                                 f'exclusively: {target}: {error}') from error
        with os.fdopen(descriptor, 'wb') as stream:
            stream.write(payload)
            os.fchmod(stream.fileno(), 0o600)
        return target

    def capture_wire(self, tag, peer, model):
        # The in-memory original wire bytes are copied out while this test
        # still owns the live peer object; the synthetic billing receipts the
        # peer persisted before each reply are preserved with them. The
        # absent-key branch returns before reading any of them.
        if self.evidence_root() is None:
            return
        for index, observation in enumerate(peer.requests, 1):
            frame = {
                'index': index,
                'request_line': observation['line'].decode('latin-1'),
                'request_headers': {key.decode('latin-1'): value.decode('latin-1')
                                    for key, value in observation['headers'].items()},
                'body_sha256': hashlib.sha256(observation['body']).hexdigest(),
                'body_bytes': len(observation['body']),
            }
            self.capture_bytes(f'{tag}-request-{index:02d}-wire.json',
                               json.dumps(frame, ensure_ascii=False, indent=1).encode())
            self.capture_bytes(f'{tag}-request-{index:02d}-body.bin', observation['body'])
        for receipt in sorted(model.receipts.iterdir()):
            self.capture_bytes(f'{tag}-receipt-{receipt.name}', receipt.read_bytes())

    def capture_state(self, tag):
        # Called after the driver subprocess exited, the peer closed and this
        # class's per-call DB connections finished: copy the physical database
        # plus any WAL/SHM sidecars that still exist, the synthetic TLS
        # credential originals and a complete workspace effect inventory,
        # before TemporaryDirectory cleanup runs. The absent-key branch
        # returns before any of these capture-only file reads.
        if self.evidence_root() is None:
            return
        for suffix, label in (('', '-database'), ('-wal', '-wal'), ('-shm', '-shm')):
            source = Path(str(self.database) + suffix)
            if source.exists():
                self.capture_bytes(f'{tag}{label}', source.read_bytes())
        effects = []
        if self.workspaces.exists():
            for path in sorted(self.workspaces.rglob('*')):
                relative = str(path.relative_to(self.workspaces))
                if path.is_file():
                    data = path.read_bytes()
                    effects.append({'path': relative, 'type': 'file', 'bytes': len(data),
                                    'mode': oct(path.stat().st_mode & 0o777),
                                    'sha256': hashlib.sha256(data).hexdigest()})
                    self.capture_bytes(f'{tag}-effect-' + relative.replace('/', '_'), data)
                else:
                    effects.append({'path': relative, 'type': 'dir',
                                    'mode': oct(path.stat().st_mode & 0o777)})
        self.capture_bytes(f'{tag}-effects.json',
                           json.dumps(effects, ensure_ascii=False, indent=1).encode())
        for credential in sorted(Path(self.certificates).iterdir()):
            self.capture_bytes(f'{tag}-tls-{credential.name}', credential.read_bytes())

    def invoke(self, peer=None, model=None, evidence=None, **options):
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
        if evidence is not None and self.evidence_root() is not None:
            # G1-3 FINAL v12/v14 6.4/3: the original driver stdout/stderr (the
            # UTF-8 text this subprocess call captured) and the exact frozen
            # configuration file are preserved before any parse or assertion;
            # with the evidence key absent this block is skipped before its
            # capture-only reads.
            self.capture_bytes(f'{evidence}-driver-stdout.txt', result.stdout.encode('utf-8'))
            self.capture_bytes(f'{evidence}-driver-stderr.txt', result.stderr.encode('utf-8'))
            self.capture_bytes(f'{evidence}-configuration.json', path.read_bytes())
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

    def feedback(self, ordinal):
        row = self.rows('SELECT result FROM model_tool_feedback ORDER BY rowid LIMIT 1 OFFSET ?',
                        (ordinal - 1,))[0]
        return json.loads(row['result'])

    def correction_common(self, result, model, expected_requests):
        """Shared same-pump completion assertions for the four L1-L4 classes."""
        self.assertNotIn('threw', result, result)
        report = result['reports'][0]
        self.assertEqual(report['stop_reason'], 'quiescent', result)
        self.assertEqual(len(model.actions), expected_requests)
        self.assertEqual(len(report['calls']), expected_requests)
        self.assertEqual(result['credential_reads'], expected_requests)
        self.assertEqual(result['evidence_reads'], expected_requests)
        self.assertEqual(report['network_started_calls'], expected_requests)
        self.assertEqual(report['provider_calls'], 0)
        self.assertTrue(report['network_count_complete'])
        self.assertEqual([row['status'] for row in self.rows('SELECT status FROM tasks')], ['ACCEPTED'])
        self.assertEqual(self.rows('SELECT status FROM experience_jobs')[0]['status'], 'DONE')
        self.assertEqual(self.rows('SELECT count(*) AS n FROM model_tool_feedback')[0]['n'], expected_requests)
        self.assertEqual(self.rows('SELECT count(*) AS n FROM live_send_claims')[0]['n'], expected_requests)
        self.assertEqual(self.rows('SELECT count(*) AS n FROM usage_reconciliations')[0]['n'], expected_requests)
        self.assertTrue(all(row['cost_micro_usd'] is None for row in self.rows('SELECT cost_micro_usd FROM call_attempts')))
        self.assertEqual(result['budget']['spent_cost'], expected_requests * 17)
        self.assertTrue(result['accounting']['ready'], result)
        self.assert_private_safe(result)

    # G1-3 FINAL v18 F3: shared bound-prefix observation for the L7/L8 sibling
    # cases. Exact slots are compared with no filtering of taskless rows, no
    # sorting and no first-seen inference; runtime roles come from the original
    # driver calls (a request body carries no trusted runtime-role field), and
    # every prefix request body must decode into the same captured contract.

    def assert_correction_sibling_prefix(self, model, peer, report, nodes, role_nodes, actions):
        count = len(actions)
        self.assertEqual(len(model.contracts[:count]), count)
        first = model.contracts[0]
        self.assertIsNone(first['task'])
        self.assertEqual(first['binding']['task_id'], '')
        self.assertEqual((report['calls'][0]['role'], report['calls'][0]['node_id'],
                          report['calls'][0]['task_id']), ('dispatcher', '', ''))
        self.assertEqual(model.actions[0], 'submit_plan')
        self.assertEqual([contract['task']['node_id'] for contract in model.contracts[1:count]], nodes)
        for position in range(1, count):
            contract = model.contracts[position]
            self.assertEqual(contract['task']['task_id'], contract['binding']['task_id'])
            self.assertEqual(contract['task']['task_id'], report['calls'][position]['task_id'])
            self.assertIn(contract['task']['node_id'], ('alpha', 'beta'))
        self.assertEqual([(call['role'], call['node_id']) for call in report['calls'][:count]],
                         role_nodes)
        self.assertEqual(model.actions[:count], actions)
        for position in range(count):
            body = json.loads(peer.requests[position]['body'])
            wire = json.loads(json.loads(body['input'][0]['content'])['task_contract'])
            self.assertEqual(wire, model.contracts[position])

    def test_model_refusal_records_model_layer_feedback_without_dispatch(self):
        compat = self.directory / 'compat-profile.json'
        compat.write_text(json.dumps({
            'format': 'wls-responses-compat-v1', 'reference': 'openai-responses-2026-11-08',
            'model_aliases': {'o3': ['o3-2025-04-16']}}))
        body = (SOURCE / 'tests/fixtures/responses/refusal-response.json').read_bytes()

        def refusal(peer, connection):
            peer.request(connection)
            connection.sendall(tls.headers(len(body)) + body)

        with tls.Peer(self.contexts['server'], refusal) as peer:
            result = self.invoke(peer, action='refusal', compat_profile=str(compat))
            self.assertEqual(len(peer.requests), 1)  # no automatic retry
        feedback = result['refusal_feedback']
        self.assertEqual(feedback['result']['failure_layer'], 'model')
        self.assertEqual(feedback['result']['model_error_code'], 'model_refusal')
        self.assertTrue(feedback['result']['runtime_error'])
        self.assertEqual(feedback['result']['observation_complete'], False)
        self.assertEqual(feedback['response']['error_code'], 'model_refusal')
        self.assertEqual(feedback['response']['tool_calls'], [])
        self.assertEqual(feedback['response']['responses_output']['output'][1]['content'][0]['type'],
                         'refusal')
        self.assertEqual(result['ops'], '0')
        self.assertEqual(result['refusal_attempt']['accepted'], 0)
        self.assertEqual(result['transport_summary']['status'], 'completed')
        self.assertEqual(result['transport_summary']['reason'], 'model_refusal')
        self.assertEqual(result['transport_summary']['response_usable'], False)

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

    # G1-3 controlled local-TLS same-pump correction regressions (plan §7 L1-L4).
    # The same bytes are behavioral RED on the exact baseline (the batch stops at
    # the first ordinary Worker failure) and GREEN on the correction candidate.

    def test_l1_text_only_round_corrects_within_the_same_pump(self):
        model = ModelPeer(self.directory, 'correction_text')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True)
        self.correction_common(result, model, 8)
        self.assertEqual(model.actions, ['submit_plan', 'issue_work_order', 'text_only', 'fs_write',
            'submit', 'check_submission', 'accept_candidate', 'report_experience'])
        third = self.feedback(3)
        self.assertEqual(third['format'], 'wls-correction-feedback-v1')
        self.assertEqual(third['reason'], 'missing_tool_call')
        self.assertEqual(third['failure_layer'], 'model_schema')
        self.assertTrue(third['correction_eligible'])
        self.assertEqual(third['effect_count'], 0)
        self.assertFalse(third.get('runtime_error', False))
        self.assertEqual((self.workspaces / 'alpha' / 'result.txt').read_bytes(), b'text corrected candidate\n')
        # The corrective wire observed the failed text round as data only: no
        # invented native function call or orphan output for the text response.
        items = json.loads(peer.requests[3]['body'])['input']
        user_items = [item for item in items if item.get('role') == 'user']
        self.assertEqual(len(user_items), 2)  # task contract plus one observation
        observation = [item for item in user_items if 'synthetic text-only round' in item['content']]
        self.assertEqual(len(observation), 1)
        self.assertIn('missing_tool_call', observation[0]['content'])
        self.assertFalse([item for item in items if item.get('type') in ('function_call', 'function_call_output')])

    def test_l2_invalid_arguments_settle_and_second_wire_carries_the_original_pair(self):
        model = ModelPeer(self.directory, 'correction_schema')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True)
        self.correction_common(result, model, 8)
        self.assertEqual(model.actions, ['submit_plan', 'issue_work_order', 'fs_write', 'fs_write',
            'submit', 'check_submission', 'accept_candidate', 'report_experience'])
        third = self.feedback(3)
        self.assertEqual(third['reason'], 'invalid_arguments')
        self.assertEqual(third['failure_layer'], 'model_schema')
        self.assertTrue(third['correction_eligible'])
        self.assertEqual(third['effect_count'], 0)
        self.assertNotIn('operation', third)
        # The schema-invalid round dispatched nothing; exactly one real write.
        self.assertEqual([row['status'] for row in
                          self.rows("SELECT status FROM ops WHERE tool_id='fs_write' ORDER BY rowid")],
                         ['SUCCEEDED'])
        self.assertEqual((self.workspaces / 'alpha' / 'result.txt').read_bytes(), b'args corrected candidate\n')
        # The second actual wire carries the original bad parameters verbatim and
        # their ordinary tool failure, paired through the original call identity.
        items = json.loads(peer.requests[3]['body'])['input']
        calls = [item for item in items if item.get('type') == 'function_call'
                 and json.loads(item['arguments']) == {'path': 'result.txt'}]
        self.assertEqual(len(calls), 1)
        outputs = [item for item in items if item.get('type') == 'function_call_output'
                   and item['call_id'] == calls[0]['call_id']]
        self.assertEqual(len(outputs), 1)
        self.assertIn('invalid_arguments', outputs[0]['output'])
        sources = json.loads(items[0]['content'])['message_sources']
        self.assertTrue(any(source.get('source', {}).get('original_tool_call_id') == 'fixture-call-3'
                            for source in sources))

    def test_l3_missing_leaf_read_corrects_with_real_content(self):
        (self.workspaces / 'alpha').mkdir(mode=0o700)
        (self.workspaces / 'alpha' / 'present.txt').write_bytes(b'original present bytes\n')
        model = ModelPeer(self.directory, 'correction_missing')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True)
        self.correction_common(result, model, 9)
        self.assertEqual(model.actions, ['submit_plan', 'issue_work_order', 'fs_read', 'fs_read', 'fs_write',
            'submit', 'check_submission', 'accept_candidate', 'report_experience'])
        third = self.feedback(3)
        self.assertEqual(third['reason'], 'not_found')
        self.assertEqual(third['failure_layer'], 'tool_result')
        self.assertTrue(third['correction_eligible'])
        self.assertNotIn('content', third)  # the missing leaf is never fabricated
        self.assertEqual(self.rows("SELECT count(*) AS n FROM ops WHERE tool_id='fs_read'")[0]['n'], 0)
        self.assertEqual((self.workspaces / 'alpha' / 'result.txt').read_bytes(), b'read corrected candidate\n')
        # Both reads returned on later wires: the persisted not_found failure and
        # the real present bytes, each paired with its original call identity.
        items = json.loads(peer.requests[4]['body'])['input']
        outputs = [item['output'] for item in items if item.get('type') == 'function_call_output']
        self.assertEqual(len(outputs), 2)
        self.assertTrue(any('not_found' in output for output in outputs), outputs)
        self.assertTrue(any('original present bytes' in output for output in outputs), outputs)
        call_ids = {item['call_id'] for item in items if item.get('type') == 'function_call'}
        for item in items:
            if item.get('type') == 'function_call_output':
                self.assertIn(item['call_id'], call_ids)

    def test_l4_settled_precondition_failure_corrects_with_cas(self):
        (self.workspaces / 'alpha').mkdir(mode=0o700)
        (self.workspaces / 'alpha' / 'result.txt').write_bytes(b'original candidate\n')
        model = ModelPeer(self.directory, 'correction_precondition')
        with tls.Peer(self.contexts['server'], model) as peer:
            result = self.invoke(peer, model, reconcile=True)
        self.correction_common(result, model, 8)
        self.assertEqual(model.actions, ['submit_plan', 'issue_work_order', 'fs_write', 'fs_write',
            'submit', 'check_submission', 'accept_candidate', 'report_experience'])
        third = self.feedback(3)
        self.assertEqual(third['operation']['status'], 'FAILED')
        self.assertEqual(third['execution_result']['observed_outcome'], 'FAILED')
        self.assertEqual(third['execution_result']['evidence']['failure_kind'], 'content_precondition')
        self.assertEqual(third['correction_feedback']['reason'], 'content_precondition')
        self.assertTrue(third['correction_feedback']['correction_eligible'])
        # The settled FAILED operation stays immutable; the corrective CAS write
        # is a distinct dispatch, never a replay of the failed key.
        writes = self.rows("SELECT op_id,status FROM ops WHERE tool_id='fs_write' ORDER BY rowid")
        self.assertEqual([row['status'] for row in writes], ['FAILED', 'SUCCEEDED'])
        self.assertNotEqual(writes[0]['op_id'], writes[1]['op_id'])
        self.assertEqual((self.workspaces / 'alpha' / 'result.txt').read_bytes(), b'cas corrected candidate\n')

    def test_zero_tool_receipt_binding_mismatch_stops_without_next_send(self):
        for variant in ('missing', 'wrong_request', 'wrong_digest', 'wrong_id'):
            with self.subTest(variant=variant):
                # Each subcase owns its original request and database.
                directory = self.directory / variant
                directory.mkdir(mode=0o700)
                self.database = directory / 'runtime.sqlite'
                self.workspaces = directory / 'workspaces'
                self.workspaces.mkdir(mode=0o700)
                model = ModelPeer(directory, 'correction_text', text_receipt=variant)
                with tls.Peer(self.contexts['server'], model) as peer:
                    result = self.invoke(peer, model, reconcile=True)
                    self.assertEqual(len(peer.requests), 3)
                self.assertEqual(result['reports'][0]['stop_reason'], 'needs_accounting', result)
                self.assertEqual(result['credential_reads'], 3)
                self.assertEqual(result['task_count'], 1)
                self.assertEqual(self.rows('SELECT count(*) AS n FROM model_tool_feedback')[0]['n'], 3)
                self.assertTrue(self.feedback(3)['runtime_error'])
                self.assertEqual(self.rows('SELECT count(*) AS n FROM usage_reconciliations')[0]['n'], 2)
                self.assertFalse((self.workspaces / 'alpha' / 'result.txt').exists())
                self.assert_private_safe(result)

    def test_correction_gates_stop_without_a_next_credential_or_send(self):
        # The per-task Worker cap, a zero frozen same-call retry cap and a tight
        # frozen cost budget all stop after the original failed round on either
        # build; none issues a fourth credential, send or business effect.
        cases = [{'worker_calls': 1}, {'schema_retry_cap': 0}, {'cost_budget': 140}]
        for options in cases:
            with self.subTest(options=options):
                directory = self.directory / '-'.join(f'{key}={value}' for key, value in sorted(options.items()))
                directory.mkdir(mode=0o700)
                self.database = directory / 'runtime.sqlite'
                self.workspaces = directory / 'workspaces'
                self.workspaces.mkdir(mode=0o700)
                model = ModelPeer(directory, 'correction_text')
                with tls.Peer(self.contexts['server'], model) as peer:
                    result = self.invoke(peer, model, reconcile=True, **options)
                    self.assertEqual(len(peer.requests), 3)
                self.assertNotIn('threw', result, result)
                self.assertEqual(result['credential_reads'], 3)
                self.assertEqual(result['budget']['attempts'], 3)
                self.assertEqual(len(self.rows('SELECT status FROM tasks')), 1)
                self.assertNotEqual(self.rows('SELECT status FROM tasks')[0]['status'], 'ACCEPTED')
                self.assertEqual(self.rows('SELECT count(*) AS n FROM workflow_submissions')[0]['n'], 0)
                self.assertEqual(self.rows('SELECT count(*) AS n FROM model_tool_feedback')[0]['n'], 3)
                self.assertFalse((self.workspaces / 'alpha' / 'result.txt').exists())
                # The failed Worker claim keeps its original pending events.
                self.assertGreater(
                    self.rows("SELECT count(*) AS n FROM event_inbox WHERE status<>'DONE'")[0]['n'], 0)
                self.assert_private_safe(result)

    # G1-3 v8 C1/C2 live regressions: correction admissions accumulate across
    # completed claims for one Worker inside one live pump, and the bounded
    # correction_limit report halts the whole batch without rewriting durable
    # feedback. The same bytes are behavioral RED on the frozen v6 candidate
    # (the workflow counter restarts per claim block; exhaustion is silent)
    # and GREEN only on the C1/C2 repair.

    def test_l5_cross_claim_correction_accumulation_reports_correction_limit(self):
        model = ModelPeer(self.directory, 'correction_cross_claim')
        try:
            with tls.Peer(self.contexts['server'], model) as peer:
                result = self.invoke(peer, model, reconcile=True, schema_retry_cap=1,
                                     evidence='l5')
                self.capture_wire('l5', peer, model)
                self.assertEqual(len(peer.requests), 5)
            report = result['reports'][0]
            self.assertEqual(report['stop_reason'], 'correction_limit', result)
            self.assertTrue(any(item['code'] == 'correction_limit' and item.get('node_id') == 'alpha'
                                for item in report['blocked']), result)
            self.assertEqual(model.actions, ['submit_plan', 'issue_work_order', 'text_only', 'fs_write', 'text_only'])
            self.assertEqual(result['credential_reads'], 5)
            self.assertEqual(result['evidence_reads'], 5)
            self.assertEqual(report['network_started_calls'], 5)
            self.assertEqual(result['budget']['spent_cost'], 5 * 17)
            self.assertTrue(result['accounting']['ready'], result)
            # The middle success completed its original claim; the third failure
            # ran on a fresh claim of the identical frozen binding and config, and
            # that ordinary success must not have refreshed any admission quota.
            workers = self.rows("SELECT frozen_binding,config_snapshot_sha256 FROM call_attempts"
                                " WHERE role='worker' ORDER BY rowid")
            self.assertEqual(len(workers), 3)
            self.assertEqual(json.loads(workers[0]['frozen_binding']), json.loads(workers[2]['frozen_binding']))
            self.assertEqual({row['config_snapshot_sha256'] for row in workers},
                             {workers[0]['config_snapshot_sha256']})
            final = self.feedback(5)
            self.assertEqual(final['format'], 'wls-correction-feedback-v1')
            self.assertEqual(final['reason'], 'missing_tool_call')
            self.assertEqual(final['failure_layer'], 'model_schema')
            self.assertTrue(final['correction_eligible'])
            self.assertEqual(final['effect_count'], 0)
            self.assertFalse(final.get('runtime_error', False))
            self.assertEqual((self.workspaces / 'alpha' / 'corrected.txt').read_bytes(), b'cross-claim corrected\n')
            self.assertFalse((self.workspaces / 'alpha' / 'extra.txt').exists())
            self.assertEqual(self.rows("SELECT count(*) AS n FROM workflow_submissions")[0]['n'], 0)
            self.assertGreater(self.rows("SELECT count(*) AS n FROM event_inbox WHERE status<>'DONE'")[0]['n'], 0)
            self.assert_private_safe(result)
        finally:
            self.capture_state('l5')

    def test_l6_consecutive_failures_report_correction_limit_exactly(self):
        for cap, requests, failures in ((2, 5, 3), (0, 3, 1)):
            with self.subTest(schema_retry_cap=cap):
                directory = self.directory / f'cap={cap}'
                directory.mkdir(mode=0o700)
                self.database = directory / 'runtime.sqlite'
                self.workspaces = directory / 'workspaces'
                self.workspaces.mkdir(mode=0o700)
                model = ModelPeer(directory, 'correction_exhaust')
                try:
                    with tls.Peer(self.contexts['server'], model) as peer:
                        result = self.invoke(peer, model, reconcile=True, schema_retry_cap=cap,
                                             evidence=f'l6-cap{cap}')
                        self.capture_wire(f'l6-cap{cap}', peer, model)
                        self.assertEqual(len(peer.requests), requests)
                    report = result['reports'][0]
                    self.assertEqual(report['stop_reason'], 'correction_limit', result)
                    self.assertTrue(any(item['code'] == 'correction_limit' for item in report['blocked']), result)
                    self.assertEqual(result['credential_reads'], requests)
                    self.assertEqual(result['budget']['attempts'], requests)
                    self.assertEqual(model.actions.count('text_only'), failures)
                    final = self.feedback(requests)
                    self.assertEqual(final['format'], 'wls-correction-feedback-v1')
                    self.assertTrue(final['correction_eligible'])
                    self.assertEqual(final['effect_count'], 0)
                    self.assertFalse(final.get('runtime_error', False))
                    self.assertFalse((self.workspaces / 'alpha' / 'never.txt').exists())
                    self.assertGreater(self.rows("SELECT count(*) AS n FROM event_inbox WHERE status<>'DONE'")[0]['n'], 0)
                    self.assert_private_safe(result)
                finally:
                    self.capture_state(f'l6-cap{cap}')

    def test_l7_post_failure_worker_cap_halts_the_whole_live_batch(self):
        # G1-3 FINAL v18 F1/F3: both Worker tasks hold a legal per-task call
        # cap of two (report max_worker_calls_per_task). alpha succeeds once
        # and still owns a runnable pending read with budget, events, binding
        # and deadline all available; beta owns the two ordinary failures that
        # exhaust beta's own cap while the schema cap still has a free
        # admission, so the whole batch halts at exactly six calls without the
        # dedicated exhaustion code. The same bytes must still take a real
        # business RED on the frozen v6 candidate, which lets alpha consume
        # its pending read as a seventh send.
        model = ModelPeer(self.directory, 'correction_worker_cap_sibling')
        try:
            with tls.Peer(self.contexts['server'], model) as peer:
                result = self.invoke(peer, model, reconcile=True, schema_retry_cap=2,
                                     worker_calls=2, evidence='l7')
                self.capture_wire('l7', peer, model)
                # G1-3 FINAL v18 F1: the report's per-task Worker capacity is
                # exactly two before any request-count statement is made.
                self.assertEqual(result['reports'][0]['max_worker_calls_per_task'], 2)
                # G1-3 FINAL v18 F3: the exact bound prefix is observed before
                # the request-count assertion, so a later count failure carries
                # this role attribution with it.
                self.assert_correction_sibling_prefix(
                    model, peer, result['reports'][0],
                    nodes=['alpha', 'alpha', 'beta', 'beta', 'beta'],
                    role_nodes=[('dispatcher', ''), ('team_lead', 'alpha'), ('worker', 'alpha'),
                                ('team_lead', 'beta'), ('worker', 'beta'), ('worker', 'beta')],
                    actions=['submit_plan', 'issue_work_order', 'fs_write', 'issue_work_order',
                             'text_only', 'text_only'])
                self.assertEqual(len(peer.requests), 6)
            report = result['reports'][0]
            self.assertNotIn('threw', result, result)
            self.assertEqual(model.actions, ['submit_plan', 'issue_work_order', 'fs_write',
                                             'issue_work_order', 'text_only', 'text_only'])
            # The real host guard reason stays: beta's worker cap ended the
            # corrective continuation while the schema cap still had a free
            # admission, so the dedicated exhaustion code must not be invented.
            self.assertTrue(any(item['code'] == 'worker_limit' and item.get('node_id') == 'beta'
                                for item in report['blocked']), result)
            self.assertFalse(any(item['code'] == 'correction_limit' for item in report['blocked']), result)
            # The whole live batch stopped after beta's host cap: the still
            # runnable alpha sibling gained no further credential or send.
            self.assertEqual(result['credential_reads'], 6)
            self.assertEqual(result['evidence_reads'], 6)
            self.assertEqual(report['network_started_calls'], 6)
            self.assertEqual(result['budget']['attempts'], 6)
            self.assertEqual(result['budget']['spent_cost'], 6 * 17)
            self.assertTrue(result['accounting']['ready'], result)
            self.assertEqual((self.workspaces / 'alpha' / 'result.txt').read_bytes(), b'sibling progress\n')
            self.assertFalse((self.workspaces / 'beta' / 'result.txt').exists())
            final = self.feedback(6)
            self.assertEqual(final['format'], 'wls-correction-feedback-v1')
            self.assertEqual(final['reason'], 'missing_tool_call')
            self.assertEqual(final['failure_layer'], 'model_schema')
            self.assertTrue(final['correction_eligible'])
            self.assertEqual(final['effect_count'], 0)
            self.assertFalse(final.get('runtime_error', False))
            # alpha's post-write feedback event stays pending: after the halt
            # no role gains a new capture, credential, reservation or send.
            self.assertGreater(self.rows("SELECT count(*) AS n FROM event_inbox WHERE status<>'DONE'")[0]['n'], 0)
            self.assert_private_safe(result)
        finally:
            self.capture_state('l7')

    def test_l8_correction_limit_halts_every_live_role_not_just_the_worker(self):
        # G1-3 FINAL v18 F1/F2: the configuration keeps the worker_calls key
        # absent, so each Worker task holds the driver's default per-task
        # capacity of sixteen and alpha's finite script offers exactly one
        # write plus fifteen same-path reads. beta's first ordinary failure
        # under the frozen schema cap of zero must halt every live role at
        # exactly five calls; any later over-send on an old candidate meets a
        # real settled native read slot, never a fixture IndexError.
        model = ModelPeer(self.directory, 'correction_siblings')
        try:
            with tls.Peer(self.contexts['server'], model) as peer:
                result = self.invoke(peer, model, reconcile=True, schema_retry_cap=0,
                                     evidence='l8')
                self.capture_wire('l8', peer, model)
                # G1-3 FINAL v18 F1: no worker_calls key is configured, so the
                # report keeps the driver's default per-task capacity sixteen.
                self.assertEqual(result['reports'][0]['max_worker_calls_per_task'], 16)
                # G1-3 FINAL v18 F3: the exact bound prefix is observed before
                # the request-count assertion, so a later count failure carries
                # this role attribution with it.
                self.assert_correction_sibling_prefix(
                    model, peer, result['reports'][0],
                    nodes=['alpha', 'alpha', 'beta', 'beta'],
                    role_nodes=[('dispatcher', ''), ('team_lead', 'alpha'), ('worker', 'alpha'),
                                ('team_lead', 'beta'), ('worker', 'beta')],
                    actions=['submit_plan', 'issue_work_order', 'fs_write', 'issue_work_order',
                             'text_only'])
                self.assertEqual(len(peer.requests), 5)
            report = result['reports'][0]
            self.assertEqual(report['stop_reason'], 'correction_limit', result)
            self.assertTrue(any(item['code'] == 'correction_limit' and item.get('node_id') == 'beta'
                                for item in report['blocked']), result)
            self.assertEqual(model.actions, ['submit_plan', 'issue_work_order', 'fs_write',
                                             'issue_work_order', 'text_only'])
            self.assertEqual(result['credential_reads'], 5)
            self.assertEqual(report['network_started_calls'], 5)
            self.assertEqual((self.workspaces / 'alpha' / 'result.txt').read_bytes(), b'sibling progress\n')
            self.assertFalse((self.workspaces / 'beta' / 'result.txt').exists())
            # alpha's post-write feedback event stays pending: after exhaustion no
            # role gains a new capture, credential, reservation, send or dispatch.
            self.assertGreater(self.rows("SELECT count(*) AS n FROM event_inbox WHERE status<>'DONE'")[0]['n'], 0)
            self.assertTrue(result['accounting']['ready'], result)
            self.assert_private_safe(result)
        finally:
            self.capture_state('l8')

    def test_unclassified_live_failures_stay_stopped(self):
        # Transport/protocol failure, two declared calls in one response and an
        # undeclared tool name never enter the four ordinary correction branches.
        for mode in ('server_error', 'two_tool_calls', 'undeclared_tool'):
            with self.subTest(mode=mode):
                directory = self.directory / mode
                directory.mkdir(mode=0o700)
                self.database = directory / 'runtime.sqlite'
                self.workspaces = directory / 'workspaces'
                self.workspaces.mkdir(mode=0o700)
                model = ModelPeer(directory, mode)
                with tls.Peer(self.contexts['server'], model) as peer:
                    result = self.invoke(peer, model, reconcile=True)
                    self.assertEqual(len(peer.requests), 3)
                self.assertNotIn('threw', result, result)
                self.assertIn(result['reports'][0]['stop_reason'], ('error', 'needs_accounting'), result)
                self.assertEqual(result['credential_reads'], 3)
                self.assertEqual(len(self.rows('SELECT status FROM tasks')), 1)
                self.assertNotEqual(self.rows('SELECT status FROM tasks')[0]['status'], 'ACCEPTED')
                self.assertEqual(self.rows('SELECT count(*) AS n FROM workflow_submissions')[0]['n'], 0)
                self.assertTrue(self.feedback(3)['runtime_error'])
                for path in ('result.txt', 'left.txt', 'right.txt'):
                    self.assertFalse((self.workspaces / 'alpha' / path).exists())
                self.assert_private_safe(result)


if __name__ == '__main__':
    unittest.main(verbosity=2)
