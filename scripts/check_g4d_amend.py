"""Validate retained G4-D facts; the self-test fixture is synthetic, never native evidence."""
import base64
import copy
import hashlib
import json
from pathlib import Path

FIXTURE = Path(__file__).resolve().parents[1] / 'tests/fixtures/g4'
EXPECTED = json.loads((FIXTURE / 'amend-expected.json').read_text())
if not __debug__: raise RuntimeError('oracle requires assertions enabled')


def check_operations(data):
    assert len(data['ops']) == len(data['operation_results'])
    for op in data['ops']:
        record, = [row for row in data['operation_results'] if row['op_id'] == op['op_id']]
        process, proof = record['result'], record['evidence']
        assert op['tool_id'] == 'run' and op['status'] == record['observed_outcome'] and record['accepted']
        assert op['status'] in ('SUCCEEDED', 'CANCELLED') and op['started_at']
        assert process['spawned'] and process['launcher_exec_confirmed'] and process['all_descendants_exited']
        assert not process['strong_isolation'] and not process['output_truncated'] and data['removed'][op['op_id']]
        assert (process['status'] == 'CANCELLED') if op['status'] == 'CANCELLED' else process['exit_code'] == 0
        assert process['evidence'] == proof and proof['proof_format'] == 'wls-process-proof-standard-cgroup-v1'
        assert proof['isolation_verified'] and proof['network'] == 'none' and proof['resource_enforcement'] == 'cgroup-v2'
        assert proof['descendants_proof'] == 'cgroup.events populated=0' and proof['workspace_observation']['complete']
        assert proof['operation_id'] == op['op_id'] and all(op[key] == value for key, value in record['original_binding'].items())
        feedback, = [row for row in data['model_tool_feedback'] if row['result'].get('operation', {}).get('op_id') == op['op_id']]
        attempt, = [row for row in data['attempts'] if row['attempt_id'] == feedback['attempt_id']]
        assert attempt['frozen_binding'] == feedback['original_binding'] == record['original_binding']
        assert attempt['response'] == feedback['response'] and attempt['settlement_sha256']
        outcome = feedback['result']
        assert outcome['executed_now'] and not outcome['replayed'] and outcome['execution_result']['record_sha256'] == record['record_sha256']
        assert outcome['execution_result']['result'] == process and all(outcome['operation'][key] == value for key, value in op.items())
        assert outcome['operation']['arguments'] == feedback['response']['tool_calls'][0]['arguments']
        claim, = [event for event in data['events'] if event['type'] == 'start_tool_op' and event['payload']['op_id'] == op['op_id']]
        assert claim['result']['dispatch_claimed'] and claim['result']['operation']['status'] == 'RUNNING'
        assert claim['command_id'] == attempt['attempt_id'] + ':' + feedback['response']['tool_calls'][0]['call_id'] + ':start'
        for stream in ('stdout', 'stderr'):
            output, = [row for row in data['outputs'] if row['op_id'] == op['op_id'] and row['stream'] == stream]
            raw = bytes.fromhex(output['bytes_hex'])
            assert not record[stream + '_truncated'] and len(raw) == record[stream + '_bytes']
            assert hashlib.sha256(raw).hexdigest() == record[stream + '_sha256'] and raw == base64.b64decode(process[stream + '_base64'], validate=True)


def check_cancelled(before, active):
    check_operations(before)
    stable, changed = before['ops']
    assert [op['task_id'] for op in before['ops']] == [next(task['task_id'] for task in before['tasks'] if task['node_id'] == node) for node in ('alpha', 'beta')]
    assert [op['status'] for op in before['ops']] == ['SUCCEEDED', 'CANCELLED']
    assert [op['op_id'] for op in active['ops']] == [stable['op_id'], changed['op_id']]
    assert [op['status'] for op in active['ops']] == ['SUCCEEDED', 'RUNNING']
    assert active['ready'] == 'READY\n' and active['signal'] == 2 and active['parent_pid'] > 0
    assert len(active['claims']) == 2 and active['stable'] == EXPECTED['alpha']
    assert all(dict(event, payload=json.loads(event['payload']), result=json.loads(event['result'])) in before['events'] for event in active['claims'])
    assert before['files']['alpha'] == EXPECTED['alpha'] and before['files']['beta'] == {'ready.txt': 'READY\n'}
    assert before['cli']['returncode'] == 130 and before['cli']['summary']['request_id'] == before['request'][0]['request_id']
    assert before['request'][0]['requirements_revision'] == 1 and not before['workflow_submissions']


def check_resumed(before, after, active):
    check_cancelled(before, active)
    check_operations(after)
    old, current = before['request'][0], after['request'][0]
    assert all(old[key] == current[key] for key in ('request_id', 'created_at', 'config_snapshot_sha256', 'budget'))
    assert before['cli']['argv'] == after['cli']['argv'] and before['frozen'] == after['frozen']
    for table in ('ops', 'operation_results', 'outputs', 'model_tool_feedback', 'attempts', 'events', 'instructions', 'workspace_bindings', 'live_send_claims'):
        assert after[table][:len(before[table])] == before[table], table
    assert len(after['ops']) == 3 and after['ops'][2]['task_id'] == before['ops'][1]['task_id']
    assert after['ops'][2]['status'] == 'SUCCEEDED' and after['ops'][2]['requirements_revision'] == 2
    assert after['files']['alpha'] == before['files']['alpha'] == EXPECTED['alpha']
    assert after['files']['beta'] == dict(EXPECTED['beta'], **{'ready.txt': 'READY\n'})
    amendment, = [event for event in after['events'] if event['type'] == 'amend']
    assert amendment['request_id'] == current['request_id'] and amendment not in before['events']
    assert amendment['payload']['text'] == (FIXTURE / 'amend-text.txt').read_text() and amendment['result']['requirements_revision'] == 2
    assert amendment['payload']['constraint_catalog'] == json.loads((FIXTURE / 'amend-amended-constraints.json').read_text())
    decisions, = [row for row in after['model_tool_feedback'] if row['response']['tool_calls'][0]['name'] == 'submit_amendment_plan']
    assert decisions['response']['tool_calls'][0]['arguments'] == json.loads((FIXTURE / 'amend-resume.json').read_text())['amendment']
    assert len(after['tasks']) == len(after['amend_dispositions']) == len(after['workflow_submissions']) == 2
    assert {task['node_id'] for task in after['tasks']} == set(EXPECTED)
    for task in after['tasks']:
        previous, = [row for row in before['tasks'] if row['task_id'] == task['task_id'] and row['node_id'] == task['node_id']]
        disposition, = [row for row in after['amend_dispositions'] if row['task_id'] == task['task_id']]
        assert disposition['disposition'] == ('keep' if task['node_id'] == 'alpha' else 'update')
        assert disposition['based_on_requirements_revision'] == disposition['target_task_revision'] == disposition['target_plan_revision'] == 2
        assert task['based_on_requirements_revision'] == task['plan_revision'] == task['task_revision'] == task['instruction_revision'] == 2
        assert all(task['applied_' + key] == task[key] for key in ('plan_revision', 'task_revision', 'instruction_revision'))
        order, = [row for row in after['instructions'] if row['task_id'] == task['task_id'] and row['instruction_revision'] == 2]
        ack, = [event for event in after['events'] if event['type'] == 'ack' and event['payload']['binding']['task_id'] == task['task_id'] and event['payload']['binding']['requirements_revision'] == 2]
        assert order['based_on_requirements_revision'] == 2 and ack['seq'] > amendment['seq'] and task['status'] == 'ACCEPTED'
        submission, = [row for row in after['workflow_submissions'] if row['task_id'] == task['task_id']]
        assert submission['original_binding'] == ack['payload']['binding'] and submission['status'] == 'CANDIDATE_READY'
        artifacts = submission['artifact_snapshot']
        expected = EXPECTED[task['node_id']]
        assert len(artifacts) == len(expected) and {item['path'] for item in artifacts} == {'/work/' + name for name in expected}
        for artifact in artifacts:
            raw = base64.b64decode(artifact['content_base64'], validate=True)
            assert raw == expected[Path(artifact['path']).name].encode() and artifact['size_bytes'] == len(raw)
            assert artifact['sha256'] == hashlib.sha256(raw).hexdigest()
        check, = [row for row in after['team_lead_checks'] if row['submission_id'] == submission['submission_id']]
        accepted, = [row for row in after['candidate_acceptances'] if row['submission_id'] == submission['submission_id']]
        assert check['status'] == 'CANDIDATE_READY' and check['check_body']['passed'] and accepted['status'] == 'ACCEPTED' and accepted['decision']['passed']
    delivery, = after['delivery_runtime']
    receipt, = after['delivery_receipts']
    assert delivery['status'] == 'CONFIRMED' and delivery['delivery_id'] == receipt['delivery_id'] and receipt['request_id'] == current['request_id']
    assert delivery['requirements_revision'] == current['requirements_revision'] == current['plan_revision'] == 2
    assert len(after['experience_jobs']) == 2 and all(job['status'] == 'DONE' for job in after['experience_jobs'])
    assert current['status'] == after['cli']['summary']['status'] == 'DELIVERED' and after['cli']['returncode'] == 0
    assert after['cli']['summary']['request_id'] == current['request_id'] and after['cli']['summary']['accounting']['ready']
    assert 0 < after['calls'] == len(after['attempts']) == len(after['model_tool_feedback']) == len(after['live_send_claims']) <= 24 and after['elapsed_s'] <= 90


if __name__ == '__main__':
    fixture = json.loads((FIXTURE / 'amend-oracle-synthetic.json').read_text())
    assert fixture['kind'] == 'synthetic oracle fixture, not native evidence'
    check_resumed(fixture['before'], fixture['after'], fixture['active'])
    for negative in fixture['negatives']:
        altered = copy.deepcopy(fixture)
        target = altered
        for key in negative['path'][:-1]:
            target = target[key]
        target[negative['path'][-1]] = negative['value']
        try:
            check_resumed(altered['before'], altered['after'], altered['active'])
        except (AssertionError, KeyError, ValueError):
            continue
        raise AssertionError('oracle accepted ' + negative['name'])
    print(f"PASS synthetic oracle positive and {len(fixture['negatives'])} negatives; native NOT_RUN")
