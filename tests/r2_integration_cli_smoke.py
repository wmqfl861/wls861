#!/usr/bin/env python3
"""Combined rejection/recovery guards on real disposable CLI and SQLite data."""
from contextlib import closing
import importlib.util
from pathlib import Path
import sqlite3
import unittest

spec = importlib.util.spec_from_file_location('rework_helpers', Path(__file__).with_name('r2_rework_cli_smoke.py'))
r = importlib.util.module_from_spec(spec)
spec.loader.exec_module(r)


class IntegratedRecovery(unittest.TestCase):
    # Reuse fixtures, not inherited tests: every count below is an independent case.
    setUp = r.ReworkCli.setUp
    invoke = r.ReworkCli.invoke
    rows = r.ReworkCli.rows
    json_file = r.ReworkCli.json_file
    fixture = r.ReworkCli.fixture
    admit = r.ReworkCli.admit
    view = r.ReworkCli.view
    pump = r.ReworkCli.pump
    step = r.ReworkCli.step
    rejected = r.ReworkCli.rejected
    correction_script = r.ReworkCli.correction_script
    escalate = r.ReworkCli.escalate
    answer = r.ReworkCli.answer

    def snapshot(self, request):
        result = {}
        with closing(sqlite3.connect(self.database.as_uri() + '?mode=ro', uri=True)) as db:
            names = [x[0] for x in db.execute("SELECT name FROM sqlite_master WHERE type='table' AND name NOT LIKE 'sqlite_%' ORDER BY name")]
            parents = {'attempt_id': 'call_attempts', 'op_id': 'ops', 'team_id': 'teams',
                       'task_id': 'tasks', 'experience_id': 'experience_jobs', 'delivery_id': 'delivery_intents'}
            for name in names:
                cols = {x[1] for x in db.execute(f'PRAGMA table_info("{name}")')}
                if 'request_id' in cols:
                    result[name] = db.execute(f'SELECT * FROM "{name}" WHERE request_id=? ORDER BY rowid', (request,)).fetchall()
                else:
                    for key, parent in parents.items():
                        if key in cols and parent in names:
                            pcols = {x[1] for x in db.execute(f'PRAGMA table_info("{parent}")')}
                            if key in pcols and 'request_id' in pcols:
                                result[name] = db.execute(f'SELECT c.* FROM "{name}" c JOIN "{parent}" p USING("{key}") WHERE p.request_id=? ORDER BY c.rowid', (request,)).fetchall()
                                break
        self.assertIn('call_attempts', result)
        self.assertIn('model_tool_feedback', result)
        self.assertIn('operation_results', result)
        self.assertIn('event_inbox', result)
        return result

    def sibling(self, *, waiting=False, terminal=False):
        request = self.admit('sibling')
        work = self.directory / 'sibling-work'; work.mkdir(mode=0o700)
        mapping = self.json_file('sibling-map.json', {'alpha': str(work)})
        result = self.invoke('pump-fake', '--db', self.database, '--request', request,
            '--script', r.h.FIXTURES / 's15_fake_complete.json', '--workspaces-file', mapping,
            '--worker-calls', 1, '--max-calls', 3)
        self.assertEqual(len(result['calls']), 3, result)
        if waiting:
            task = self.view(request)['tasks'][0]['task_id']
            self.invoke('wait', '--db', self.database, '--request', request, '--task', task, '--reason', 'independent owner pause')
        if terminal:
            self.invoke('cancel', '--db', self.database, '--request', request)
        return request, work, mapping

    def test_full_corrective_chain_keeps_active_sibling_rows_and_bytes(self):
        request = self.rejected(structured=True)
        sibling, work, _ = self.sibling()
        before, content = self.snapshot(sibling), (work / 'result.txt').read_bytes()
        result = self.pump(request, self.correction_script(finish=True))
        self.assertEqual(len(result['calls']), 7, result)
        self.assertEqual(self.view(request)['tasks'][0]['status'], 'ACCEPTED', result)
        self.assertEqual(self.snapshot(sibling), before)
        self.assertEqual((work / 'result.txt').read_bytes(), content)

    def test_escalation_answer_and_correction_leave_sibling_wait_untouched(self):
        request = self.rejected()
        sibling, _, _ = self.sibling(waiting=True)
        before = self.snapshot(sibling)
        question = self.escalate(request)
        self.assertEqual(self.snapshot(sibling), before)
        self.answer(request, question)
        self.assertEqual(self.snapshot(sibling), before)
        result = self.pump(request, self.correction_script(finish=True))
        self.assertEqual(self.view(request)['tasks'][0]['status'], 'ACCEPTED', result)
        self.assertEqual(self.snapshot(sibling), before)

    def test_exhausted_answer_and_empty_pump_preserve_terminal_cleanup(self):
        request = self.rejected(maximum=0)
        sibling, _, _ = self.sibling(terminal=True)
        before = self.snapshot(sibling)
        self.assertTrue(before['cleanup_jobs'])
        question = self.escalate(request)
        self.answer(request, question)
        result = self.pump(request, self.json_file('empty.json', {'default': []}))
        self.assertEqual(len(result['calls']), 0, result)
        self.assertEqual(self.view(request)['tasks'][0]['status'], 'BLOCKED')
        self.assertEqual(self.snapshot(sibling), before)
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state WHERE team_id=?',
            (self.view(request)['tasks'][0]['team_id'],)), [{'rework_count': 0}])

    def test_later_sibling_resume_preserves_rejected_request_and_reuses_order(self):
        request = self.rejected()
        sibling, _, mapping = self.sibling()
        self.pump(request, self.correction_script())
        before = self.snapshot(request)
        leads = self.rows("SELECT * FROM call_attempts WHERE request_id=? AND role='team_lead'", (sibling,))
        script = {'nodes': {'alpha': [self.step('worker', 'fs_read', {'path': 'result.txt', 'max_bytes': 1024})]}}
        result = self.invoke('pump-fake', '--db', self.database, '--request', sibling,
            '--script', self.json_file('sibling-resume.json', script), '--workspaces-file', mapping,
            '--worker-calls', 1, '--max-calls', 4)
        self.assertEqual(len(result['calls']), 1, result)
        self.assertEqual(self.rows("SELECT * FROM call_attempts WHERE request_id=? AND role='team_lead'", (sibling,)), leads)
        self.assertEqual(self.snapshot(request), before)

    def test_admit_and_exact_replay_preserve_blocked_question_and_all_sibling_rows(self):
        request = self.rejected()
        self.escalate(request)
        sibling, _, _ = self.sibling()
        before_a, before_b = self.snapshot(request), self.snapshot(sibling)
        new = self.admit('new-admission')
        self.assertEqual(self.admit('new-admission'), new)
        self.assertEqual(self.snapshot(request), before_a)
        self.assertEqual(self.snapshot(sibling), before_b)

    def test_invalid_corrective_response_accounts_only_target_attempt(self):
        request = self.rejected()
        sibling, _, _ = self.sibling()
        before = self.snapshot(sibling)
        original = self.view(request)['tasks'][0]
        script = {'default': [self.step('dispatcher', 'change_task', {
            'rationale': 'Reject forged target.', 'correction': 'Preserve criterion.', 'task_id': 'forged'})]}
        result = self.pump(request, self.json_file('invalid-correction.json', script))
        self.assertEqual(len(result['calls']), 1, result)
        self.assertTrue(any(x['code'] == 'invalid_input' for x in result['blocked']), result)
        self.assertEqual(self.view(request)['tasks'][0]['task_revision'], original['task_revision'])
        self.assertEqual(self.snapshot(sibling), before)


if __name__ == '__main__':
    if r.os.geteuid() == 0:
        raise SystemExit('Integrated recovery regressions require an ordinary user')
    unittest.main(verbosity=2)
