#!/usr/bin/env python3
"""R2 rejection routing on real CLI/SQLite; all decisions remain fixed FakeModel fixtures."""
import copy
import importlib.util
import json
import hashlib
import os
from pathlib import Path
import unittest

spec = importlib.util.spec_from_file_location('acceptance_helpers', Path(__file__).with_name('s15_cli_smoke.py'))
h = importlib.util.module_from_spec(spec)
spec.loader.exec_module(h)

class ReworkCli(unittest.TestCase):
    setUp = h.AcceptanceCliSmoke.setUp
    invoke = h.AcceptanceCliSmoke.invoke
    rows = h.AcceptanceCliSmoke.rows
    json_file = h.AcceptanceCliSmoke.json_file
    fixture = h.AcceptanceCliSmoke.fixture
    admit = h.AcceptanceCliSmoke.admit
    view = h.AcceptanceCliSmoke.view
    pump = h.AcceptanceCliSmoke.pump

    def step(self, role, name, args, index=1):
        return {'role':role, 'role_call_index':index, 'status':'completed',
                'tool_calls':[{'call_id':f'{role}-{name}-{index}','name':name,'arguments':args}],
                'usage':{'input_tokens':100,'output_tokens':20,'cache_read_tokens':0,
                         'cache_write_tokens':0,'cost_micro_usd':0}}

    def rejected(self, *, maximum=3, max_attempts=64, structured=False):
        config=(h.SOURCE/'config/examples/system.yaml').read_text().replace(
            '  team_internal_rework_max: 3', f'  team_internal_rework_max: {maximum}')
        if structured:
            config=config.replace('request_format: framed-v1','request_format: structured-v2')
        config_path=self.directory/'system.yaml'; config_path.write_text(config)
        request=self.invoke('submit','--db',self.database,'--key','reject-request',
            '--text-file',h.FIXTURES/'s14_request.txt','--constraints-file',h.FIXTURES/'s14_constraints.json',
            '--config',config_path,'--functions-dir',h.SOURCE/'config/examples/functions',
            '--max-attempts',max_attempts)['request']['request_id']
        script=self.fixture()
        script['default'][1]=self.step('dispatcher','accept_candidate',
            {'passed':False,'notes':'The original result needs a bounded correction.'},2)
        script['nodes']['alpha']=[s for s in script['nodes']['alpha']
            if s['tool_calls'][0]['name']!='report_experience']
        first=self.pump(request,self.json_file('reject.json',script))
        self.assertEqual(self.view(request)['tasks'][0]['status'],'BLOCKED',first)
        return request

    def test_rejected_candidate_has_captured_dispatcher_successor(self):
        request=self.rejected()
        before=self.view(request)['tasks'][0]
        originals=self.rows('SELECT body,artifact_snapshot,record_sha256 FROM workflow_submissions')
        script={'default':[self.step('dispatcher','change_task',
            {'rationale':'Acceptance rejected the current candidate.','correction':'Reinspect and correct the result without weakening its criterion.'})]}
        result=self.pump(request,self.json_file('correct.json',script))
        actions=[c for c in result['calls'] if c['role']=='dispatcher']
        self.assertEqual(len(actions),1,result)
        after=self.view(request)['tasks'][0]
        self.assertEqual(after['status'],'READY',result)
        self.assertEqual(after['task_revision'],before['task_revision']+1)
        self.assertEqual(after['brief'],before['brief'])
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':1}])
        self.assertEqual(self.rows('SELECT body,artifact_snapshot,record_sha256 FROM workflow_submissions'),originals)
        self.assertEqual(self.rows('SELECT status FROM candidate_acceptances'),[{'status':'REJECTED'}])

    def correction_script(self, *, finish=False, reject_again=False):
        original=self.fixture()
        result={'default':[self.step('dispatcher','change_task',
            {'rationale':'Current evidence failed acceptance.','correction':'Produce a corrected result under the original obligations.'})]}
        if finish:
            result['nodes']=copy.deepcopy(original['nodes'])
            writer=result['nodes']['alpha'][1]['tool_calls'][0]['arguments']
            writer['content']='alpha candidate version 2\n'
            writer['expected_sha256']=hashlib.sha256((self.workspace/'result.txt').read_bytes()).hexdigest()
            result['default'].append(copy.deepcopy(original['default'][1]))
            if reject_again:
                result['default'][-1]=self.step('dispatcher','accept_candidate',{'passed':False,'notes':'Still fails.'},2)
                result['nodes']['alpha']=result['nodes']['alpha'][:-1]
        return self.json_file('correct-full.json',result)

    def escalate(self, request):
        script={'default':[self.step('dispatcher','escalate_task',
            {'text':'The fixed rework allowance or requirement needs an owner decision.',
             'context':'No implicit approval or additional attempts.',
             'options':[{'id':'amend','label':'Send a separate AMEND or cancellation.'}]})]}
        result=self.pump(request,self.json_file('escalate.json',script))
        self.assertEqual(len(result['calls']),1,result)
        question=self.invoke('questions','--db',self.database,'--request',request)['questions'][0]
        self.assertEqual(question['holder'],'user')
        self.assertEqual(question['asked_by'],'dispatcher')
        self.assertEqual(question['status'],'OPEN')
        return question

    def answer(self, request, question):
        return self.invoke('answer','--db',self.database,'--request',request,
            '--question',question['question_id'],'--expected-version',question['state_version'],
            '--text-file',h.FIXTURES/'s14_answer.txt')

    def test_full_rejection_correction_resubmission_acceptance(self):
        request=self.rejected(structured=True)
        old=self.rows('SELECT submission_id,original_binding,body,artifact_snapshot,record_sha256 FROM workflow_submissions')[0]
        instructions=self.rows('SELECT * FROM instructions')
        result=self.pump(request,self.correction_script(finish=True))
        self.assertEqual(len(result['calls']),7,result)
        task=self.view(request)['tasks'][0]
        self.assertEqual(task['status'],'ACCEPTED',result)
        self.assertEqual(task['task_revision'],2)
        self.assertEqual(task['instruction_revision'],2)
        self.assertEqual(task['applied_task_revision'],2)
        self.assertEqual(task['applied_instruction_revision'],2)
        self.assertEqual(self.rows('SELECT * FROM instructions')[:len(instructions)],instructions)
        self.assertEqual(self.rows('SELECT submission_id,original_binding,body,artifact_snapshot,record_sha256 FROM workflow_submissions WHERE submission_id=?',(old['submission_id'],))[0],old)
        self.assertEqual([r['status'] for r in self.rows('SELECT status FROM workflow_submissions ORDER BY rowid')],['RETURNED_FOR_REWORK','CANDIDATE_READY'])
        self.assertEqual([r['status'] for r in self.rows('SELECT status FROM candidate_acceptances ORDER BY rowid')],['REJECTED','ACCEPTED'])
        self.assertEqual((self.workspace/'result.txt').read_text(),'alpha candidate version 2\n')
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':1}])
        # A new TeamLead sees the rejection and correction, not just a new epoch.
        envelopes=self.rows("SELECT request_envelope FROM call_attempts WHERE role='team_lead' ORDER BY rowid")
        self.assertIn('Produce a corrected result',envelopes[2]['request_envelope'])
        self.assertEqual(len(self.rows('SELECT * FROM model_tool_feedback')),len(self.rows('SELECT * FROM call_attempts')))
        self.assertFalse(result['business_delivered'])

    def test_shared_allowance_survives_reopen_and_second_rejection(self):
        request=self.rejected(maximum=1)
        result=self.pump(request,self.correction_script(finish=True,reject_again=True))
        self.assertEqual(self.view(request)['tasks'][0]['status'],'BLOCKED',result)
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':1}])
        q=self.escalate(request)
        self.answer(request,q)
        # A model cannot regain eligibility by changing script/config or opening another writer.
        result=self.pump(request,self.correction_script())
        self.assertEqual(len(result['calls']),0,result)
        self.assertEqual(self.view(request)['tasks'][0]['status'],'BLOCKED')
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':1}])
        self.assertEqual(len(self.rows('SELECT * FROM questions')),1)

    def test_zero_allowance_user_answer_does_not_accept_or_reset(self):
        request=self.rejected(maximum=0)
        q=self.escalate(request)
        before=self.rows('SELECT body,artifact_snapshot,record_sha256 FROM workflow_submissions')
        self.invoke('wake','--db',self.database,'--request',request,'--task',q['task_id'],
            '--wait-generation',q['wait_generation'],error='not_ready')
        self.answer(request,q)
        result=self.pump(request,self.json_file('empty.json',{'default':[]}))
        self.assertEqual(len(result['calls']),0,result)
        self.assertEqual(self.view(request)['tasks'][0]['status'],'BLOCKED')
        self.assertEqual(self.rows('SELECT body,artifact_snapshot,record_sha256 FROM workflow_submissions'),before)
        self.assertEqual(self.rows('SELECT status FROM candidate_acceptances'),[{'status':'REJECTED'}])
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':0}])
        self.assertEqual(len(self.rows('SELECT * FROM questions')),1)

    def test_answer_with_allowance_allows_new_captured_correction(self):
        request=self.rejected()
        q=self.escalate(request)
        waiting=self.pump(request,self.correction_script())
        self.assertEqual(len(waiting['calls']),0,waiting)
        self.answer(request,q)
        result=self.pump(request,self.correction_script(finish=True))
        self.assertEqual(self.view(request)['tasks'][0]['status'],'ACCEPTED',result)
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':1}])
        self.assertEqual(len(self.rows('SELECT * FROM questions')),1)

    def test_budget_exhaustion_retains_rejected_candidate_and_pending_event(self):
        request=self.rejected(max_attempts=6)
        before=self.rows('SELECT * FROM call_attempts')
        result=self.pump(request,self.correction_script())
        self.assertEqual(len(result['calls']),0,result)
        self.assertTrue(any(x['code']=='quota' for x in result['blocked']),result)
        self.assertEqual(self.rows('SELECT * FROM call_attempts'),before)
        self.assertEqual(self.view(request)['tasks'][0]['status'],'BLOCKED')
        self.assertTrue(self.rows("SELECT * FROM event_inbox WHERE role='dispatcher' AND status='PENDING'"))

    def test_model_cannot_choose_target_or_replace_obligations(self):
        request=self.rejected()
        original=self.view(request)['tasks'][0]
        for field in ('task_id','submission_id','brief','rework_limit','task_revision'):
            args={'rationale':'correct','correction':'unchanged obligations',field:'forged'}
            result=self.pump(request,self.json_file('forged.json',{'default':[self.step('dispatcher','change_task',args)]}))
            self.assertTrue(any(x['code']=='invalid_input' for x in result['blocked']),result)
            task=self.view(request)['tasks'][0]
            self.assertEqual(task['status'],'BLOCKED')
            self.assertEqual(task['brief'],original['brief'])
            self.assertEqual(task['task_revision'],original['task_revision'])
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':0}])

    def test_amendment_captures_new_disposition_and_rejects_old_correction_tool(self):
        request=self.rejected()
        original=self.rows('SELECT submission_id,original_binding,body,artifact_snapshot,record_sha256 FROM workflow_submissions')
        workspace_bytes=(self.workspace/'result.txt').read_bytes()
        self.invoke('amend','--db',self.database,'--request',request,
            '--text-file',h.FIXTURES/'s13_amendment.txt','--constraints-file',h.FIXTURES/'s13_amended_constraints.json')
        task_query=('SELECT task_id,node_id,task_revision,plan_revision,based_on_requirements_revision,'
                    'instruction_revision,status,brief FROM tasks WHERE request_id=? ORDER BY task_id')
        tasks=self.rows(task_query,(request,))
        plans=self.rows('SELECT * FROM plans WHERE request_id=? ORDER BY plan_revision',(request,))
        notice_query=("SELECT i.inbox_id,i.subject_task_id,i.event_requirements_revision FROM event_inbox i "
                      "JOIN events e ON e.seq=i.event_seq WHERE i.request_id=? AND i.role='dispatcher' "
                      "AND i.status='PENDING' AND e.type='amend' ORDER BY i.inbox_id")
        notices=self.rows(notice_query,(request,))
        self.assertTrue(notices)
        budget=self.rows('SELECT spent_input,spent_output FROM request_budgets WHERE request_id=?',(request,))[0]
        attempts=len(self.rows('SELECT attempt_id FROM call_attempts WHERE request_id=?',(request,)))
        result=self.pump(request,self.correction_script())
        # The new AMEND target may call its model. An old rejection correction
        # is still unavailable and cannot substitute for a current disposition.
        self.assertEqual(len(result['calls']),1,result)
        call=result['calls'][0]
        self.assertEqual(call['role'],'dispatcher')
        self.assertEqual(call['workflow_action']['kind'],'amendment')
        self.assertIs(call['workflow_action']['target']['plan_required'],True)
        self.assertEqual(call['error']['code'],'invalid_input',call)
        attempt=self.rows('SELECT input_tokens,output_tokens,response,settlement_sha256 FROM call_attempts WHERE attempt_id=?',
                          (call['attempt_id'],))[0]
        self.assertEqual((attempt['input_tokens'],attempt['output_tokens']),(100,20))
        self.assertIsNotNone(attempt['settlement_sha256'])
        self.assertEqual(json.loads(attempt['response'])['tool_calls'][0]['name'],'change_task')
        self.assertEqual(len(self.rows('SELECT attempt_id FROM call_attempts WHERE request_id=?',(request,))),attempts+1)
        spent=self.rows('SELECT spent_input,spent_output FROM request_budgets WHERE request_id=?',(request,))[0]
        self.assertEqual((spent['spent_input']-budget['spent_input'],spent['spent_output']-budget['spent_output']),(100,20))
        self.assertEqual(self.rows(task_query,(request,)),tasks)
        self.assertEqual(self.rows('SELECT * FROM plans WHERE request_id=? ORDER BY plan_revision',(request,)),plans)
        self.assertEqual(self.rows('SELECT * FROM amend_dispositions WHERE request_id=?',(request,)),[])
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':0}])
        self.assertEqual(self.rows('SELECT status FROM workflow_submissions'),[{'status':'SUPERSEDED'}])
        self.assertEqual(self.rows('SELECT submission_id,original_binding,body,artifact_snapshot,record_sha256 FROM workflow_submissions'),original)
        self.assertEqual(self.rows(notice_query,(request,)),notices)
        self.assertEqual((self.workspace/'result.txt').read_bytes(),workspace_bytes)

    def test_same_pump_rejects_corrects_and_accepts_without_reopen(self):
        request=self.admit()
        script=self.fixture()
        accept=copy.deepcopy(script['default'][1]); accept['role_call_index']=4
        script['default'][1]=self.step('dispatcher','accept_candidate',{'passed':False,'notes':'Correct result.'},2)
        script['default'] += [self.step('dispatcher','change_task',
            {'rationale':'Original result rejected.','correction':'Save corrected bytes.'},3),accept]
        first=copy.deepcopy(script['nodes']['alpha'][:-1])
        second=copy.deepcopy(script['nodes']['alpha'])
        for item in second:
            item['role_call_index'] += 2
            args=item['tool_calls'][0]['arguments']
            if item['tool_calls'][0]['name']=='fs_write':
                args['content']='alpha corrected in same pump\n'
                args['expected_sha256']=hashlib.sha256(b'alpha candidate version 1\n').hexdigest()
        script['nodes']['alpha']=first+second
        result=self.invoke('pump-fake','--db',self.database,'--request',request,
            '--script',self.json_file('one-pump.json',script),'--workspaces-file',self.mapping,
            '--worker-calls',4,'--max-calls',16)
        self.assertEqual(len(result['calls']),13,result)
        self.assertEqual(self.view(request)['tasks'][0]['status'],'ACCEPTED',result)
        self.assertEqual((self.workspace/'result.txt').read_text(),'alpha corrected in same pump\n')
        self.assertEqual(self.rows('SELECT rework_count FROM team_workflow_state'),[{'rework_count':1}])

    def test_escalated_alpha_does_not_block_independent_beta(self):
        request=self.admit()
        beta=self.directory/'beta'; beta.mkdir(mode=0o700)
        mapping=self.json_file('two-workspaces.json',{'alpha':str(self.workspace),'beta':str(beta)})
        script=self.fixture()
        node=copy.deepcopy(script['default'][0]['tool_calls'][0]['arguments']['nodes'][0]);node['node_id']='beta'
        script['default'][0]['tool_calls'][0]['arguments']['nodes'].append(node)
        accept=copy.deepcopy(script['default'][1]);accept['role_call_index']=4
        script['default'][1]=self.step('dispatcher','accept_candidate',{'passed':False,'notes':'Only alpha rejected.'},2)
        script['default'] += [self.step('dispatcher','escalate_task',{'text':'Owner decision for alpha only.'},3),accept]
        script['nodes']['beta']=copy.deepcopy(script['nodes']['alpha'])
        script['nodes']['alpha']=script['nodes']['alpha'][:-1]
        result=self.invoke('pump-fake','--db',self.database,'--request',request,
            '--script',self.json_file('siblings.json',script),'--workspaces-file',mapping,
            '--worker-calls',2,'--max-calls',20)
        tasks={x['node_id']:x for x in self.view(request)['tasks']}
        self.assertEqual(tasks['alpha']['status'],'BLOCKED',result)
        self.assertEqual(tasks['beta']['status'],'ACCEPTED',result)
        questions=self.invoke('questions','--db',self.database,'--request',request)['questions']
        self.assertEqual(len(questions),1)
        self.assertEqual(questions[0]['task_id'],tasks['alpha']['task_id'])
        self.assertEqual(questions[0]['holder'],'user')
        self.assertEqual(self.rows('SELECT status FROM worker_waits WHERE task_id=?',(tasks['alpha']['task_id'],)),[{'status':'WAITING'}])

if __name__=='__main__':
    if os.geteuid()==0:
        raise SystemExit('R2 integration must run as an ordinary user')
    unittest.main(verbosity=2)
