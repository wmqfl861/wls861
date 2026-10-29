#!/usr/bin/env python3
"""R2.4 demand recovery using real CLI processes and disposable SQLite."""
from contextlib import closing
import importlib.util
from pathlib import Path
import sqlite3
import unittest

spec = importlib.util.spec_from_file_location('r24_helpers', Path(__file__).with_name('r2_recovery_cli_smoke.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)


class DemandRecoveryCli(unittest.TestCase):
    setUp = h.RecoveryCli.setUp
    configure = h.RecoveryCli.configure
    write = h.RecoveryCli.write
    call = h.RecoveryCli.call
    rows = h.RecoveryCli.rows
    read_script = h.RecoveryCli.read_script
    work_file = h.RecoveryCli.work_file
    admit = h.RecoveryCli.admit
    pump = h.RecoveryCli.pump
    snapshot = h.RecoveryCli.snapshot

    def epoch(self, request):
        return self.rows('SELECT dispatcher_epoch FROM requests WHERE request_id=?', (request,))[0]['dispatcher_epoch']

    def recoveries(self, request):
        return self.rows("SELECT * FROM events WHERE request_id=? AND type IN ('recover','recover_experience','attempts_unknown') ORDER BY seq", (request,))

    def test_empty_batches_preserve_epoch_and_original_request(self):
        request = self.admit('idle')
        epoch = self.epoch(request)
        original = self.rows('SELECT * FROM requests WHERE request_id=?', (request,))
        evidence = self.rows('SELECT * FROM request_texts WHERE request_id=?', (request,))
        for _ in range(3):
            result = self.call('pump-fake', '--db', self.db, '--request', request,
                               '--script', self.write('empty.json', []), '--max-calls', 1)
            self.assertEqual(result['calls'], [])
            self.assertEqual(self.epoch(request), epoch)
            self.assertEqual(self.rows('SELECT * FROM requests WHERE request_id=?', (request,)), original)
            self.assertEqual(self.rows('SELECT * FROM request_texts WHERE request_id=?', (request,)), evidence)
            self.assertEqual(self.recoveries(request), [])

    def test_complete_worker_batches_continue_without_recovery(self):
        request, sibling = self.admit('worker'), self.admit('sibling')
        self.pump(request)
        epoch, recovery = self.epoch(request), self.recoveries(request)
        sibling_before = self.snapshot(sibling)
        instructions = self.rows('SELECT * FROM instructions WHERE request_id=?', (request,))
        with closing(sqlite3.connect(self.db)) as conn:
            original = conn.execute('SELECT * FROM model_tool_feedback ORDER BY rowid').fetchall()
        for _ in range(2):
            result = self.pump(request)
            self.assertEqual(len(result['calls']), 1, result)
            self.assertEqual(self.epoch(request), epoch)
            self.assertEqual(self.recoveries(request), recovery)
            self.assertEqual(self.rows('SELECT * FROM instructions WHERE request_id=?', (request,)), instructions)
            self.assertEqual(self.snapshot(sibling), sibling_before)
            with closing(sqlite3.connect(self.db)) as conn:
                self.assertEqual(conn.execute('SELECT * FROM model_tool_feedback ORDER BY rowid LIMIT ?', (len(original),)).fetchall(), original)

    def test_init_remains_explicit_force_maintenance(self):
        request = self.admit('force')
        epoch = self.epoch(request)
        self.call('init', '--db', self.db)
        self.assertEqual(self.epoch(request), epoch + 1)
        self.assertEqual(len(self.recoveries(request)), 1)


if __name__ == '__main__':
    if h.h.os.getuid() == 0:
        raise SystemExit('R2.4 CLI regressions require an ordinary user')
    unittest.main(verbosity=2)
