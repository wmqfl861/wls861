#!/usr/bin/env python3
"""Opt-in Worker windows on real CLI/SQLite/private files, with no paid calls."""
import base64
import copy
import importlib.util
import json
import os
from pathlib import Path
import sqlite3
import unittest

spec = importlib.util.spec_from_file_location('structured_helpers', Path(__file__).with_name('r1_structured_cli_smoke.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)

class WindowCli(unittest.TestCase):
    # Share fixture operations, not the earlier release's test methods.
    setUp = h.StructuredCli.setUp
    write = h.StructuredCli.write
    work_file = h.StructuredCli.work_file
    call = h.StructuredCli.call
    rows = h.StructuredCli.rows
    read_script = h.StructuredCli.read_script
    pump = h.StructuredCli.pump
    envelopes = h.StructuredCli.envelopes

    def configure(self, choice='structured-v2', policy=True):
        h.StructuredCli.configure(self, choice)
        if policy:
            self.config.write_text(self.config.read_text().replace('context:\n',
                'context:\n  worker_history: {mode: window-v1, recent_attempts: 4, inline_bytes: 32768}\n'))

    def admit(self):
        return self.call('submit', '--db', self.db, '--key', 'window-test',
            '--text-file', h.FIXTURES / 's13_request.txt', '--constraints-file', h.FIXTURES / 's13_constraints.json',
            '--config', self.config, '--functions-dir', h.SOURCE / 'config/examples/functions',
            '--input-budget', 8000000, '--output-budget', 800000, '--max-attempts', 500)['request']['request_id']

    def control_script(self, name, arguments):
        script = self.read_script(1)
        script['nodes']['alpha'][-1]['tool_calls'] = [{'call_id': 'history-1', 'name': name, 'arguments': arguments}]
        return script

    def feedback(self):
        return self.rows('SELECT f.*,a.task_id FROM model_tool_feedback f JOIN call_attempts a USING(attempt_id) ORDER BY a.rowid')

    def read_args(self, row, **kwargs):
        result = {'attempt_id': row['attempt_id'], 'record_sha256': row['record_sha256'],
                  'section': 'result', 'offset': 0, 'max_bytes': 4096}
        result.update(kwargs)
        return result

    def test_32_large_reads_survive_reopen_without_new_lead(self):
        self.work_file('x' * 61100)
        request = self.admit()
        first = self.pump(request, self.read_script(16), 16)
        self.assertEqual(len(first['calls']), 18, first)
        before = self.rows("SELECT * FROM model_tool_feedback ORDER BY rowid")
        leads = len(self.rows("SELECT * FROM call_attempts WHERE role='team_lead'"))
        self.configure('framed-v1', policy=False)  # Cannot alter frozen selection.
        second = self.pump(request, self.read_script(16), 16)
        self.assertEqual(len(second['calls']), 16, second)
        self.assertEqual(len(self.envelopes('worker')), 32)
        self.assertEqual(len(self.rows("SELECT * FROM call_attempts WHERE role='team_lead'")), leads)
        self.assertEqual(self.rows("SELECT * FROM model_tool_feedback ORDER BY rowid")[:len(before)], before)
        for index, envelope in enumerate(self.envelopes('worker')):
            context = envelope['context']['input']
            window = json.loads(context['messages'][0]['content'])
            self.assertEqual(window['total_count'], index)
            self.assertEqual(window['omitted_count'], max(0, index - 4))
            self.assertLessEqual(len(context['messages']), 5)
            self.assertIn('只读写明确指定的工作目录', context['task_contract'])
            self.assertEqual(json.loads(context['task_contract'])['worker_history_policy']['mode'], 'window-v1')
            self.assertLess(len(h.canonical(context)), 100000)
        original_worker_results = [json.loads(r['result']) for r in self.feedback() if r['task_id']]
        self.assertEqual(sum(isinstance(r,dict) and r.get('content') == 'x' * 61100 for r in original_worker_results),32)

    def test_windowed_out_result_is_retrieved_by_model_not_current_file(self):
        self.work_file('原证据😀' * 10000)
        request = self.admit()
        self.pump(request, self.read_script(6), 6)
        original = self.rows("SELECT f.* FROM model_tool_feedback f JOIN call_attempts a USING(attempt_id) WHERE a.role='worker' ORDER BY a.rowid")[0]
        self.work_file('replacement')
        read = self.pump(request, self.control_script('history_read', self.read_args(original)), 1)
        chunk = read['calls'][-1]['result']
        self.assertEqual(base64.b64decode(chunk['content_base64']), h.canonical(json.loads(original['result']))[:4096])
        self.assertEqual(chunk['original_binding'], json.loads(original['original_binding']))
        self.pump(request, self.read_script(1), 1)
        turns = self.envelopes('worker')[-1]['context']['input']['messages']
        results = [json.loads(m['content']) for m in turns if m['role']=='tool']
        self.assertIn(chunk, results)  # A readback must be visible, not recursively hidden behind its own reference.

    def test_model_index_has_fixed_scope_and_bounded_page(self):
        self.work_file('small')
        request = self.admit(); self.pump(request, self.read_script(6), 6)
        high = json.loads(self.envelopes('worker')[-1]['context']['input']['messages'][0]['content'])['through_sequence']
        result = self.pump(request, self.control_script('history_page', {'after_sequence':0, 'through_sequence':high, 'limit':4}),1)
        page = result['calls'][-1]['result']
        self.assertEqual(len(page['items']),4)
        self.assertTrue(page['has_more'])
        self.assertEqual(page['through_sequence'],high)
        self.assertTrue(all(item['role']=='worker' for item in page['items']))
        self.assertTrue(all(item['sequence'] <= high for item in page['items']))

    def test_invalid_or_foreign_reads_are_recorded_not_authorized(self):
        self.work_file('small'); request=self.admit(); self.pump(request,self.read_script(1),1)
        # TeamLead evidence belongs to a different scope despite sharing request/task.
        foreign=self.rows("SELECT f.* FROM model_tool_feedback f JOIN call_attempts a USING(attempt_id) WHERE a.role='team_lead'")[0]
        result=self.pump(request,self.control_script('history_read',self.read_args(foreign)),1)
        self.assertTrue(any(b['code']=='unauthorized' for b in result['blocked']), result)
        last=json.loads(self.feedback()[-1]['result'])
        self.assertTrue(last['runtime_error']); self.assertEqual(last['error_code'],'unauthorized')
        own=self.rows("SELECT f.* FROM model_tool_feedback f JOIN call_attempts a USING(attempt_id) WHERE a.role='worker' ORDER BY a.rowid")[0]
        for args,expected in [(self.read_args(own,record_sha256='0'*64),'conflict'),
                              (self.read_args(own,max_bytes=4097),'invalid_input'),
                              (self.read_args(own,offset=-1),'invalid_input'),
                              (dict(self.read_args(own),task_id='forged'),'invalid_input')]:
            result=self.pump(request,self.control_script('history_read',args),1)
            self.assertTrue(any(b['code']==expected for b in result['blocked']),result)

    def test_rate_budget_is_persistent_across_reopen(self):
        self.config.write_text(self.config.read_text().replace('max_queries_per_minute: 60','max_queries_per_minute: 2'))
        self.work_file('small'); request=self.admit(); self.pump(request,self.read_script(1),1)
        script=self.control_script('history_page',{'after_sequence':0,'through_sequence':0,'limit':1})
        for _ in range(2):
            result=self.pump(request,script,1)
            self.assertEqual(result['calls'][-1]['result']['format'],'wls-history-index-v1')
        result=self.pump(request,script,1)
        self.assertTrue(any(b['code']=='quota' for b in result['blocked']),result)
        self.assertEqual(json.loads(self.feedback()[-1]['result'])['error_code'],'quota')

    def test_wait_does_not_disappear_when_history_is_windowed(self):
        self.work_file('small'); request=self.admit(); self.pump(request,self.read_script(6),6)
        task=self.rows('SELECT task_id FROM tasks')[0]['task_id']
        self.call('wait','--db',self.db,'--request',request,'--task',task,'--reason','explicit user wait')
        before=self.rows('SELECT attempt_id FROM call_attempts ORDER BY rowid')
        result=self.pump(request,self.read_script(1),1)
        self.assertEqual(result['calls'],[])
        self.assertEqual(self.rows('SELECT attempt_id FROM call_attempts ORDER BY rowid'),before)

    def test_failed_readback_consumes_persistent_rate_budget(self):
        self.config.write_text(self.config.read_text().replace('max_queries_per_minute: 60','max_queries_per_minute: 2'))
        self.work_file('original'); request = self.admit()
        self.pump(request, self.read_script(1), 1)
        own = self.rows("SELECT f.* FROM model_tool_feedback f JOIN call_attempts a USING(attempt_id) WHERE a.role='worker' ORDER BY a.rowid")[0]
        before = len(self.envelopes('worker'))
        rejected = self.pump(request, self.control_script('history_read', self.read_args(own, record_sha256='0'*64)), 1)
        self.assertTrue(any(item['code'] == 'conflict' for item in rejected['blocked']), rejected)
        valid = self.pump(request, self.control_script('history_read', self.read_args(own)), 1)
        self.assertEqual(valid['calls'][-1]['result']['format'], 'wls-history-bytes-v1')
        limited = self.pump(request, self.control_script('history_read', self.read_args(own)), 1)
        self.assertTrue(any(item['code'] == 'quota' for item in limited['blocked']), limited)
        self.assertEqual(len(self.envelopes('worker')), before + 3)
        results = [json.loads(row['result']) for row in self.feedback()[-3:]]
        self.assertEqual(results[0]['error_code'], 'conflict')
        self.assertEqual(results[1]['format'], 'wls-history-bytes-v1')
        self.assertEqual(results[2]['error_code'], 'quota')

    def test_legacy_mode_and_malformed_window_do_not_silently_migrate(self):
        for source in ['  worker_history: {mode: window-v1, recent_attempts: 9, inline_bytes: 32768}',
                       '  worker_history: {mode: window-v1, recent_attempts: 8, inline_bytes: 65536}',
                       '  worker_history: {mode: auto, recent_attempts: 4, inline_bytes: 32768}',
                       '  worker_history: {mode: window-v1, recent_attempts: 4, inline_bytes: 1024}',
                       '  worker_history: {mode: window-v1, recent_attempts: 4, inline_bytes: 32768, ignore_constraints: true}']:
            self.configure('structured-v2',False)
            self.config.write_text(self.config.read_text().replace('context:\n','context:\n'+source+'\n'))
            self.call('config','check','--config',self.config,'--functions-dir',h.SOURCE/'config/examples/functions',error='invalid_input')
        self.configure('framed-v1',True)
        self.call('config','check','--config',self.config,'--functions-dir',h.SOURCE/'config/examples/functions',error='invalid_input')
        self.configure('structured-v2',True)
        checked=self.call('config','check','--config',self.config,'--functions-dir',h.SOURCE/'config/examples/functions')
        self.assertIn('"history_window_integrated": true',json.dumps(checked))

    def test_compatibility_runner_also_uses_window(self):
        self.work_file('x'*61100);request=self.admit()
        plan=self.read_script(16)
        script=plan['default']+plan['nodes']['alpha']
        result=self.call('run-fake','--db',self.db,'--request',request,'--workspace',self.work,
                        '--worker-calls',16,'--script',self.write('single.json',script))
        self.assertEqual(len(result['calls']),18,result)
        self.assertEqual(len(self.envelopes('worker')),16)

if __name__=='__main__':
    if os.geteuid()==0: raise SystemExit('ordinary user required')
    unittest.main(verbosity=2)
