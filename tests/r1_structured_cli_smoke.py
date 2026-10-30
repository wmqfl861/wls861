#!/usr/bin/env python3
"""Frozen structured requests through the real CLI, SQLite and private files."""
from __future__ import annotations
from contextlib import closing
import hashlib
import json
import os
from pathlib import Path
import sqlite3
import subprocess
import sys
import tempfile
import unittest

BINARY, SOURCE = (Path(p).resolve() for p in sys.argv[1:3])
FIXTURES = SOURCE / 'tests/fixtures'
sys.argv = [sys.argv[0]]


def canonical(value):
    return json.dumps(value, sort_keys=True, separators=(',', ':'), ensure_ascii=False).encode('utf-8')


class StructuredCli(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory(prefix='wls-r12-')
        self.addCleanup(temp.cleanup)
        self.root = Path(temp.name)
        self.db = self.root / 'control.db'
        self.work = self.root / 'alpha'
        self.work.mkdir(mode=0o700)
        self.mapping = self.write('workspaces.json', {'alpha': str(self.work)})
        self.config = self.root / 'system.yaml'
        self.configure('structured-v2')

    def configure(self, choice):
        source = (SOURCE / 'config/examples/system.yaml').read_text()
        marker = '  request_format: framed-v1\n'
        self.assertIn(marker, source)
        source = source.replace(marker, '' if choice is None else f'  request_format: {choice}\n')
        self.config.write_text(source)

    def write(self, name, value):
        path = self.root / name
        path.write_text(json.dumps(value, ensure_ascii=False))
        return path

    def work_file(self, content):
        path = self.work / 'r1.txt'
        # This fixture deliberately runs under the caller's permissive umask.
        # Only its own file gets private modes; production checks are unchanged.
        path.write_text(content)
        path.chmod(0o600)
        self.assertEqual(path.stat().st_mode & 0o777, 0o600)
        return path

    def call(self, *args, error=None):
        result = subprocess.run([str(BINARY), *map(str, args), '--source-root', str(SOURCE)],
                                cwd=self.root, text=True, capture_output=True, timeout=50)
        if error:
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual(json.loads(result.stderr)['error'], error, result.stderr)
            return json.loads(result.stderr)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def admit(self, stem='s13'):
        return self.call('submit', '--db', self.db, '--key', 'structured-test',
            '--text-file', FIXTURES / f'{stem}_request.txt', '--constraints-file', FIXTURES / f'{stem}_constraints.json',
            '--config', self.config, '--functions-dir', SOURCE / 'config/examples/functions')['request']['request_id']

    def rows(self, query, parameters=()):
        with closing(sqlite3.connect(self.db.as_uri() + '?mode=ro', uri=True)) as conn:
            conn.row_factory = sqlite3.Row
            return [dict(row) for row in conn.execute(query, parameters)]

    def read_script(self, count=2):
        script = json.loads((FIXTURES / 's13_fake_initial.json').read_text())
        script['default'][0]['tool_calls'][0]['arguments']['nodes'] = script['default'][0]['tool_calls'][0]['arguments']['nodes'][:1]
        lead, worker = script['nodes']['alpha'][:2]
        worker['tool_calls'] = [{'call_id': 'same-provider-id', 'name': 'fs_read',
                                  'arguments': {'path': 'r1.txt', 'max_bytes': 65536}}]
        steps = [lead]
        for index in range(1, count + 1):
            item = json.loads(json.dumps(worker))
            item['role_call_index'] = index
            steps.append(item)
        script['nodes'] = {'alpha': steps}
        return script

    def pump(self, request, script, workers=2):
        return self.call('pump-fake', '--db', self.db, '--request', request,
            '--script', self.write('script.json', script), '--workspaces-file', self.mapping,
            '--worker-calls', workers, '--max-calls', 64)

    def envelopes(self, role=None):
        query = 'SELECT request_envelope FROM call_attempts'
        if role:
            query += ' WHERE role=?'
        return [json.loads(row['request_envelope']) for row in self.rows(query + ' ORDER BY rowid', (role,) if role else ())]

    def test_all_roles_receive_fields_and_exact_native_turn(self):
        self.work_file('原始观察😀\n')
        request = self.admit()
        result = self.pump(request, self.read_script())
        self.assertEqual(len(result['calls']), 4, result)
        self.assertEqual(result['real_model_calls'], 0)
        for envelope in self.envelopes():
            context = envelope['context']
            request_bytes = {k: v for k, v in envelope.items() if k not in ('logical_request_sha256', 'wire_request_sha256')}
            self.assertEqual(envelope['logical_request_sha256'], hashlib.sha256(canonical(request_bytes)).hexdigest())
            self.assertIsNone(envelope['wire_request_sha256'])
            self.assertEqual(context['format'], 'wls-compiled-context-v2')
            self.assertIsNone(context['wire_sha256'])
            self.assertNotIn('dynamic_suffix', context)
            self.assertEqual(hashlib.sha256(canonical(context['input'])).hexdigest(), context['logical_input_sha256'])
            self.assertEqual(hashlib.sha256(canonical(context['input']['prefix'])).hexdigest(), context['prefix_sha256'])
            self.assertIn('只读写明确指定的工作目录', context['input']['task_contract'])
            self.assertTrue(all(isinstance(t['parameters'], dict) for t in context['input']['prefix']['tools']))
        messages = self.envelopes('worker')[-1]['context']['input']['messages']
        self.assertEqual([m['role'] for m in messages], ['assistant', 'tool'])
        self.assertEqual(messages[0]['tool_calls'][0]['call_id'], messages[1]['tool_call_id'])
        self.assertEqual(messages[0]['source']['original_tool_call_id'], 'same-provider-id')
        self.assertEqual(json.loads(messages[1]['content'])['content'], '原始观察😀\n')

    def test_reopen_keeps_original_bytes_binding_and_frozen_choice(self):
        self.work_file('old bytes')
        request = self.admit()
        self.pump(request, self.read_script())
        old = self.rows("SELECT * FROM call_attempts WHERE role='worker' ORDER BY rowid")
        lead_count = len(self.rows("SELECT * FROM call_attempts WHERE role='team_lead'"))
        self.work_file('new bytes')
        self.configure('framed-v1')
        # Retain the cross-epoch frozen-history check with an explicit takeover.
        self.call('init', '--db', self.db)
        self.pump(request, self.read_script(1), workers=1)
        envelopes = self.envelopes('worker')
        context = envelopes[-1]['context']
        self.assertEqual(context['format'], 'wls-compiled-context-v2')
        messages = context['input']['messages']
        self.assertEqual(len(messages), 4)
        self.assertEqual(json.loads(messages[1]['content'])['content'], 'old bytes')
        self.assertNotEqual(messages[0]['tool_calls'][0]['call_id'], messages[2]['tool_calls'][0]['call_id'])
        self.assertEqual(messages[0]['source']['frozen_binding'], json.loads(old[0]['frozen_binding']))
        self.assertGreater(envelopes[-1]['binding']['execution_epoch'], messages[0]['source']['frozen_binding']['execution_epoch'])
        self.assertEqual(len(self.rows("SELECT * FROM call_attempts WHERE role='team_lead'")), lead_count)
        self.assertEqual(self.rows("SELECT * FROM call_attempts WHERE role='worker' ORDER BY rowid")[:2], old)

    def test_acceptance_and_experience_use_structured_captured_contract(self):
        request = self.admit('s14')
        script = json.loads((FIXTURES / 's15_fake_complete.json').read_text())
        result = self.pump(request, script)
        view = self.call('show', '--db', self.db, '--request', request)
        self.assertEqual(view['tasks'][0]['status'], 'ACCEPTED', result)
        actions = []
        for envelope in self.envelopes():
            self.assertEqual(envelope['context']['format'], 'wls-compiled-context-v2')
            contract = json.loads(envelope['context']['input']['task_contract'])
            action = contract.get('workflow_action')
            if action:
                actions.append(action['kind'])
        self.assertIn('submission', actions)
        self.assertIn('acceptance', actions)
        self.assertIn('experience', actions)

    def test_old_snapshot_without_selection_stays_legacy(self):
        self.configure(None)
        self.work_file('old request')
        request = self.admit()
        self.configure('structured-v2')
        self.pump(request, self.read_script())
        self.assertTrue(all(e['context']['format'] == 'wls-compiled-context-v1' for e in self.envelopes()))

    def test_config_discloses_selection_and_rejects_unknown(self):
        checked = self.call('config', 'check', '--config', self.config,
            '--functions-dir', SOURCE / 'config/examples/functions')
        self.assertIn('structured-v2', json.dumps(checked))
        self.assertIn('"history_window_integrated": false', json.dumps(checked))
        self.configure('unknown-v99')
        self.call('config', 'check', '--config', self.config,
            '--functions-dir', SOURCE / 'config/examples/functions', error='invalid_input')

    def test_capacity_still_blocks_without_silent_truncation(self):
        self.work_file('x' * 61100)
        request = self.admit()
        result = self.pump(request, self.read_script(16), workers=16)
        self.assertTrue(any(b['code'] == 'quota' for b in result['blocked']), result)
        workers = self.envelopes('worker')
        self.assertGreater(len(workers), 1)
        self.assertLess(len(workers), 16)
        for index, envelope in enumerate(workers):
            messages = envelope['context']['input']['messages']
            self.assertEqual(len(messages), 2 * index)
            for message in messages:
                if message['role'] == 'tool':
                    self.assertEqual(json.loads(message['content'])['content'], 'x' * 61100)
        saved = self.rows("SELECT attempt_id FROM call_attempts WHERE role='worker'")
        self.pump(request, self.read_script(1), workers=1)
        self.assertEqual(self.rows("SELECT attempt_id FROM call_attempts WHERE role='worker'"), saved)


if __name__ == '__main__':
    if os.getuid() == 0:
        raise SystemExit('Run structured integration tests as an ordinary user')
    unittest.main(verbosity=2)
