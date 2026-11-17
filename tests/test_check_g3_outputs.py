"""Synthetic original-record captures; no models, Store mutation, or workspace reads."""
import base64
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / 'scripts'))
import check_g3_outputs as g


def file(path, content):
    return {'path': path, 'size_bytes': len(content), 'sha256': hashlib.sha256(content).hexdigest(),
            'content_base64': base64.b64encode(content).decode()}


def seal(row):
    row['record_sha256'] = g.e.original_digest({k: g.e.strict_json(row[k]) for k in ('body', 'original_binding', 'artifact_snapshot')})


def fixture(task):
    c = dict(format='wls-g3-artifact-capture-v1', task_id=task, condition='multi_agent', request_id='req',
             requirements_revision=2 if task == 'partial_amend' else 1, plan_revision=1,
             source_snapshot_sha256='a' * 64, submissions=[], questions=[], deliveries=[])
    contents = {'tool_roundtrip': [dict(result=b'alpha\nbeta\n')], 'partial_amend': [dict(left=b'left-v1\n', right=b'right-v1\n'), dict(left=b'left-v2\n', right=b'right-v1\n')],
                'rejected_rework': [dict(result=b'draft\n'), dict(result=b'final\n')], 'exhausted_rework': [dict(result=b'draft\n')], 'delivery_receipt': [dict(result=b'final\n')]}
    for i, content in enumerate(contents[task]):
        binding = dict(request_id='req', task_id='task', team_id='team', requirements_revision=i + 1 if task == 'partial_amend' else 1,
                       plan_revision=1, task_revision=i + 1, instruction_revision=i + 1, execution_epoch=1)
        body = dict(binding, submission_id=f's{i}', summary='Original fixture', deliverables=[{'path': '/work/' + n + '.txt', 'description': n} for n in content])
        row = dict(submission_id=f's{i}', request_id='req', task_id='task', team_id='team', status='SUBMITTED',
                   body=json.dumps(body), original_binding=json.dumps(binding), artifact_snapshot=json.dumps([file('/work/' + n + '.txt', v) for n, v in content.items()]))
        row['status'] = 'SUPERSEDED' if i == 0 and task in ('partial_amend', 'rejected_rework', 'exhausted_rework') else 'SUBMITTED'
        seal(row)
        c['submissions'].append(row)
    for key in ('original', 'current'):
        c[key + '_constraint_catalog'] = json.dumps(dict(request_id='req', requirements_revision=1, constraints=[dict(constraint_id='c1', source_revision=1, text='Preserve original constraints.')]))
    if task == 'exhausted_rework':
        fact = dict(body=dict(question_id='q', task_id='task', text='Owner decision needed'), original_binding=binding)
        c['questions'] = [dict(question_id='q', request_id='req', task_id='task', team_id='team', holder='user', status='OPEN',
                               original_sha256=g.e.original_digest(fact), **{k: json.dumps(v) for k, v in fact.items()})]
    if task == 'delivery_receipt':
        accepted = [dict(task_id='task', acceptance_id='acc', record_sha256='b' * 64)]
        package = [dict(name='artifact-task-1.bin', **{k: v for k, v in file('unused', b'final\n').items() if k != 'path'})]
        for name in ('requirements', 'plan', 'usage', 'questions', 'acceptance', 'experience', 'side-effects', 'delivery'):
            payload = json.dumps([{'acceptance': accepted[0]}]).encode() if name == 'acceptance' else b'{}'
            package.append(dict(name=name + '.json', **{k: v for k, v in file('unused', payload).items() if k != 'path'}))
        artifact = dict(name='artifact-task-1.bin', source_path='/work/result.txt', submission_id='s0', task_id='task', acceptance_id='acc', sha256=package[0]['sha256'], size_bytes=6)
        manifest = dict(request_id='req', delivery_id='d', requirements_revision=1, plan_revision=1, artifacts=[artifact], files=[{k: f[k] for k in ('name', 'sha256', 'size_bytes')} for f in package])
        digest = g.e.original_digest(manifest)
        receipt = dict(request_id='req', requirements_revision=1, plan_revision=1, delivery_id='d', manifest_sha256=digest, receipt_id='receipt')
        c['deliveries'] = [dict(receipt=json.dumps(receipt), status='CONFIRMED', acceptance_snapshot=json.dumps(accepted),
                               frozen_plan=json.dumps(dict(delivery_id='d', manifest=manifest, manifest_sha256=digest, files=package)), **{k: v for k, v in receipt.items() if k != 'receipt_id'})]
    return c


class G3Outputs(unittest.TestCase):
    def setUp(self):
        temp = tempfile.TemporaryDirectory()
        self.addCleanup(temp.cleanup)
        self.path = Path(temp.name) / 'capture.json'

    def result(self, capture):
        self.path.write_text(json.dumps(capture), encoding='utf-8')
        return g.check_outputs(capture['task_id'], self.path)

    def test_all_five_positive_and_each_artifact_negative(self):
        for task in ('tool_roundtrip', 'partial_amend', 'rejected_rework', 'exhausted_rework', 'delivery_receipt'):
            good = fixture(task)
            good['condition'] = 'single_agent_plain' if task == 'tool_roundtrip' else 'multi_agent'
            report = self.result(good)
            self.assertEqual((report['artifact_status'], report['business_acceptance']), ('pass', 'not_assessed'), task)
            for i, row in enumerate(good['submissions']):
                for j, original in enumerate(json.loads(row['artifact_snapshot'])):
                    bad = fixture(task)
                    entries = json.loads(row['artifact_snapshot'])
                    entries[j] = file(original['path'], b'wrong\n')
                    bad['submissions'][i]['artifact_snapshot'] = json.dumps(entries)
                    seal(bad['submissions'][i])
                    self.assertEqual(self.result(bad)['artifact_status'], 'fail', (task, i, j))

    def test_semantics_and_corruption(self):
        for task, key, value in [('exhausted_rework', 'holder', 'team_lead'), ('delivery_receipt', 'status', 'STORED')]:
            bad = fixture(task)
            bad['questions' if task == 'exhausted_rework' else 'deliveries'][0][key] = value
            self.assertEqual(self.result(bad)['artifact_status'], 'fail')
        bad = fixture('rejected_rework')
        bad['current_constraint_catalog'] = bad['current_constraint_catalog'].replace('Preserve', 'Weaken')
        self.assertEqual(self.result(bad)['artifact_status'], 'fail')
        bad = fixture('rejected_rework')
        row = bad['submissions'][-1]
        row.update(task_id='other', body=row['body'].replace('"task"', '"other"'), original_binding=row['original_binding'].replace('"task"', '"other"'))
        seal(row)
        self.assertEqual(self.result(bad)['checks']['same_task_correction'], 'fail')
        original = file('/work/result.txt', b'alpha\nbeta\n')
        snapshots = [[dict(original, **{k: v})] for k, v in [('content_base64', '!'), ('sha256', '0' * 64), ('size_bytes', True), ('size_bytes', 65537), ('path', '../result.txt')]]
        snapshots += [[original] * 2, [original] * 9, [file(f'/work/{i}', b'x' * 65536) for i in range(5)]]
        for entries in snapshots:
            bad = fixture('tool_roundtrip')
            bad['submissions'][0]['artifact_snapshot'] = json.dumps(entries)
            seal(bad['submissions'][0])
            self.assertEqual(self.result(bad)['checks']['capture_integrity'], 'fail')
        for key, value in [('request_id', 'other'), ('requirements_revision', True), ('requirements_revision', 0), ('plan_revision', 2), ('submissions', [fixture('tool_roundtrip')['submissions'][0]] * 9)]:
            bad = fixture('tool_roundtrip')
            bad[key] = value
            self.assertNotEqual(self.result(bad)['artifact_status'], 'pass')

    def test_bounded_input_missing_and_cli(self):
        raws = [json.dumps(fixture('tool_roundtrip')), '{"a":1,"a":2}', '{"a":NaN}', '[' * 66 + '0' + ']' * 66, '[0' + ',0' * 100000 + ']', ' ' * (g.e.MAX_JSON + 1)]
        for i, raw in enumerate(raws):
            self.path.write_text(raw)
            process = subprocess.run([sys.executable, str(ROOT / 'scripts/check_g3_outputs.py'), 'tool_roundtrip', str(self.path)], capture_output=True)
            self.assertEqual((process.returncode, json.loads(process.stdout)['artifact_status']), (1, 'fail') if i else (0, 'pass'))
        self.path.unlink()
        self.assertEqual(g.check_outputs('tool_roundtrip', self.path)['artifact_status'], 'missing')
        self.path.symlink_to(self.path.parent)
        self.assertEqual(g.check_outputs('tool_roundtrip', self.path)['artifact_status'], 'fail')


if __name__ == '__main__':
    unittest.main()
