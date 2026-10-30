#!/usr/bin/env python3
"""Request-scoped recovery through real processes/SQLite; only throwaway data."""
from contextlib import closing
import importlib.util
import json
import sqlite3
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('r2_helpers', Path(__file__).with_name('r1_structured_cli_smoke.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)

class RecoveryCli(unittest.TestCase):
    setUp = h.StructuredCli.setUp
    configure = h.StructuredCli.configure
    write = h.StructuredCli.write
    call = h.StructuredCli.call
    rows = h.StructuredCli.rows
    read_script = h.StructuredCli.read_script
    work_file = h.StructuredCli.work_file

    def admit(self, key):
        return self.call('submit', '--db', self.db, '--key', key,
            '--text-file', h.FIXTURES / 's13_request.txt',
            '--constraints-file', h.FIXTURES / 's13_constraints.json',
            '--config', self.config, '--functions-dir', h.SOURCE / 'config/examples/functions')['request']['request_id']

    def pump(self, request):
        work = self.root / request
        work.mkdir(mode=0o700, exist_ok=True)
        path = work / 'r1.txt'; path.write_text('original evidence'); path.chmod(0o600)
        mapping = self.write('map.json', {'alpha': str(work)})
        return self.call('pump-fake', '--db', self.db, '--request', request,
            '--script', self.write('script.json', self.read_script(1)),
            '--workspaces-file', mapping, '--worker-calls', 1, '--max-calls', 64)

    def snapshot(self, request=None):
        # Include child-only tables (feedback, operation output, ownership, etc.),
        # not just the request row. All identifier strings originate in sqlite_master.
        result = {}
        with closing(sqlite3.connect(self.db.as_uri() + '?mode=ro', uri=True)) as conn:
            names = [r[0] for r in conn.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")]
            joins = {'attempt_id': 'call_attempts', 'op_id': 'ops', 'team_id': 'teams',
                     'task_id': 'tasks', 'experience_id': 'experience_jobs', 'delivery_id': 'delivery_intents'}
            for name in names:
                columns = {r[1] for r in conn.execute(f'PRAGMA table_info("{name}")')}
                if request is None:
                    result[name] = sorted(conn.execute(f'SELECT * FROM "{name}"').fetchall(), key=repr)
                elif 'request_id' in columns:
                    result[name] = conn.execute(f'SELECT * FROM "{name}" WHERE request_id=? ORDER BY rowid', (request,)).fetchall()
                else:
                    for key, parent in joins.items():
                        if key in columns and parent in names:
                            parent_cols = {r[1] for r in conn.execute(f'PRAGMA table_info("{parent}")')}
                            if key in parent_cols and 'request_id' in parent_cols:
                                result[name] = conn.execute(f'SELECT c.* FROM "{name}" c JOIN "{parent}" p USING("{key}") WHERE p.request_id=? ORDER BY c.rowid', (request,)).fetchall()
                                break
        return result

    def setup_pair(self):
        a, b = self.admit('a'), self.admit('b')
        self.pump(a); self.pump(b)
        return a, b

    def test_successful_cancel_does_not_fence_sibling(self):
        a, b = self.setup_pair(); before = self.snapshot(b)
        self.call('cancel', '--db', self.db, '--request', a)
        self.assertEqual(self.snapshot(b), before)
        self.assertEqual(self.rows('SELECT status FROM requests WHERE request_id=?', (a,))[0]['status'], 'CANCELLED')

    def test_pump_preserves_sibling_and_idle_epoch(self):
        a, b = self.setup_pair(); before = self.snapshot(b)
        self.pump(a)
        self.assertEqual(self.snapshot(b), before)
        old_epoch = self.rows('SELECT dispatcher_epoch FROM requests WHERE request_id=?', (b,))[0]['dispatcher_epoch']
        recoveries = self.rows("SELECT * FROM events WHERE request_id=? AND type='recover' ORDER BY seq", (b,))
        leads = self.rows("SELECT attempt_id FROM call_attempts WHERE request_id=? AND role='team_lead'", (b,))
        other = self.snapshot(a)
        result = self.pump(b)
        self.assertEqual(len(result['calls']), 1, result)
        self.assertEqual(self.snapshot(a), other)
        self.assertEqual(self.rows('SELECT dispatcher_epoch FROM requests WHERE request_id=?', (b,))[0]['dispatcher_epoch'], old_epoch)
        self.assertEqual(self.rows("SELECT * FROM events WHERE request_id=? AND type='recover' ORDER BY seq", (b,)), recoveries)
        self.assertEqual(self.rows("SELECT attempt_id FROM call_attempts WHERE request_id=? AND role='team_lead'", (b,)), leads)

    def test_admission_and_idempotent_replay_do_not_recover_existing(self):
        _, b = self.setup_pair(); before = self.snapshot(b)
        c = self.admit('c')
        self.assertEqual(self.snapshot(b), before)
        all_before = self.snapshot()
        self.assertEqual(self.admit('c'), c)
        self.assertEqual(self.snapshot(), all_before)

    def test_missing_request_changes_no_durable_rows(self):
        self.setup_pair(); before = self.snapshot()
        self.call('cancel', '--db', self.db, '--request', 'req_not_present', error='invalid_input')
        self.assertEqual(self.snapshot(), before)

    def test_wait_and_wake_are_targeted(self):
        a, b = self.setup_pair()
        task = self.rows('SELECT task_id FROM tasks WHERE request_id=?', (a,))[0]['task_id']
        before = self.snapshot(b)
        waiting = self.call('wait', '--db', self.db, '--request', a, '--task', task, '--reason', 'explicit wait')
        self.assertEqual(self.snapshot(b), before)
        self.call('wake', '--db', self.db, '--request', a, '--task', task, '--wait-generation', waiting['generation'])
        self.assertEqual(self.snapshot(b), before)

    def test_terminal_cleanup_lease_is_not_changed_by_sibling(self):
        a, b = self.setup_pair()
        self.call('cancel', '--db', self.db, '--request', b)
        self.assertTrue(self.rows('SELECT * FROM cleanup_jobs WHERE request_id=?', (b,)))
        before = self.snapshot(b)
        self.pump(a); self.admit('new-after-terminal')
        self.assertEqual(self.snapshot(b), before)

if __name__ == '__main__':
    if h.os.geteuid() == 0:
        raise SystemExit('CLI recovery regressions require an ordinary user')
    unittest.main()
