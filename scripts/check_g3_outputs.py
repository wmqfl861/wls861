"""G3 artifact oracle: TASK_ID CAPTURE.json; exit 0 only for artifact pass.
Export sealed Store rows; bind capture SHA externally. Provenance/acceptance are not assessed.
Capture v1: format=wls-g3-artifact-capture-v1, task_id, condition, request_id,
requirements_revision, plan_revision, source_snapshot_sha256; submissions,
questions (joined runtime rows), deliveries (runtime rows plus receipt body).
Rework also needs original_constraint_catalog/current_constraint_catalog strings:
{request_id, requirements_revision, constraints:[{constraint_id,source_revision,text}]}.
Keep JSON columns as original strings; expected bytes are fixed below."""
import argparse
import base64
import hashlib
import json
import re
import evaluate_r3 as e

CASES = {
    'tool_roundtrip': ({}, {'result.txt': b'alpha\nbeta\n'}),
    'partial_amend': ({'left.txt': b'left-v1\n', 'right.txt': b'right-v1\n'},
                      {'left.txt': b'left-v2\n', 'right.txt': b'right-v1\n'}),
    'rejected_rework': ({'result.txt': b'draft\n'}, {'result.txt': b'final\n'}),
    'exhausted_rework': ({'result.txt': b'draft\n'}, {}),
    'delivery_receipt': ({}, {'result.txt': b'final\n'}),
}
VERSIONS = ('requirements_revision', 'plan_revision')


def check_outputs(task, path):
    report = {'task_id': task, 'capture_sha256': None, 'checks': {}, 'business_acceptance': 'not_assessed'}
    total = 0

    def check(name, value):
        report['checks'][name] = 'missing' if value is None else 'pass' if value else 'fail'

    def bound(binding, current=False):
        e.require(binding['request_id'] == capture['request_id'], 'foreign request')
        for key in VERSIONS:
            e.require(type(binding[key]) is int and 1 <= binding[key] <= capture[key], 'invalid version')
            e.require(not current or binding[key] == capture[key], 'stale version')
        e.require(all(type(v) is int and v >= 0 for k, v in binding.items()
                      if k.endswith(('_revision', '_epoch'))), 'invalid binding integer')

    def files(items, field='path', maximum=8):
        nonlocal total
        e.require(isinstance(items, list) and len(items) <= maximum, 'file count exceeds bound')
        found = {}
        for item in items:
            name = item[field].removeprefix('/work/') if field == 'path' else item[field]
            e.require(isinstance(name, str) and name and not name.startswith('/') and
                      all(p not in ('', '.', '..') for p in name.split('/')) and name not in found, 'invalid/duplicate path')
            e.require(type(item['size_bytes']) is int and 0 <= item['size_bytes'] <= 65536, 'invalid file size')
            payload = base64.b64decode(item['content_base64'], validate=True)
            total += len(payload)
            e.require(total <= 262144 and len(payload) == item['size_bytes'] and
                      base64.b64encode(payload).decode() == item['content_base64'] and
                      hashlib.sha256(payload).hexdigest() == item['sha256'], 'artifact bytes/size/hash exceed or disagree')
            found[name] = payload
        return found

    try:
        with e.regular_path(path).open('rb') as stream:
            raw = stream.read(e.MAX_JSON + 1)
        e.require(len(raw) <= e.MAX_JSON, 'JSON input exceeds 8 MiB')
        report['capture_sha256'] = hashlib.sha256(raw).hexdigest()
        capture = e.strict_json(raw)
        e.require(capture['format'] == 'wls-g3-artifact-capture-v1' and capture['task_id'] == task and
                  task in CASES and capture['condition'] in e.PLAIN_CONDITIONS, 'unsupported capture identity')
        e.require(isinstance(capture['request_id'], str) and capture['request_id'] and
                  re.fullmatch('[0-9a-f]{64}', capture['source_snapshot_sha256']), 'missing source identity')
        bound(capture, True)
        for kind, key in (('submissions', 'submission_id'), ('questions', 'question_id'), ('deliveries', 'delivery_id')):
            rows = capture[kind]
            e.require(isinstance(rows, list) and len(rows) <= 8, 'record count exceeds bound')
            e.require(all(isinstance(r[key], str) and r[key] for r in rows) and
                      len({r[key] for r in rows}) == len(rows), 'missing/duplicate record identity')
        submissions = []
        for row in capture['submissions']:
            fact = {k: e.strict_json(row[k]) for k in ('body', 'original_binding', 'artifact_snapshot')}
            binding, body = fact['original_binding'], fact['body']
            bound(binding)
            e.require(e.original_digest(fact) == row['record_sha256'] and
                      all(row[k] == binding[k] for k in ('request_id', 'task_id', 'team_id')) and
                      all(body[k] == v for k, v in binding.items()) and body['submission_id'] == row['submission_id'], 'submission binding/hash mismatch')
            e.require(row['status'] in ('SUBMITTED', 'CANDIDATE_READY', 'RETURNED_FOR_REWORK', 'BLOCKED', 'SUPERSEDED'), 'invalid submission status')
            captured = files(fact['artifact_snapshot'])
            submissions.append((row, binding, captured))
        before, expected = CASES[task]
        current = [(r, b, f) for r, b, f in submissions if all(b[k] == capture[k] for k in VERSIONS)]
        for name, wanted in before.items():
            originals = [f[name] for r, b, f in submissions if name in f and
                         (b['requirements_revision'] == 1 if task == 'partial_amend' else r['status'] in ('RETURNED_FOR_REWORK', 'BLOCKED', 'SUPERSEDED'))]
            check('original.' + name, wanted in originals if originals else None)
        for name, wanted in expected.items():
            available = [f[name] for r, b, f in current if name in f and r['status'] in ('SUBMITTED', 'CANDIDATE_READY')]
            if task == 'partial_amend' and name == 'right.txt':
                available = [f[name] for r, b, f in submissions if name in f]
            check('final.' + name, all(v == wanted for v in available) if available else None)
        if task == 'partial_amend':
            check('amended_version', capture['requirements_revision'] >= 2)
        if task in ('rejected_rework', 'exhausted_rework'):
            catalogs = [e.strict_json(capture[k + '_constraint_catalog']) for k in ('original', 'current')]
            for catalog, revision in zip(catalogs, (1, capture['requirements_revision'])):
                e.require(catalog['request_id'] == capture['request_id'] and type(catalog['requirements_revision']) is int and
                          catalog['requirements_revision'] == revision and isinstance(catalog['constraints'], list), 'constraint source mismatch')
            check('original_constraints', catalogs[0]['constraints'] == catalogs[1]['constraints'])
        if task == 'rejected_rework':
            check('same_task_correction', any(a['task_id'] == b['task_id'] and a['team_id'] == b['team_id'] and
                  a['task_revision'] < b['task_revision'] and af.get('result.txt') == b'draft\n' and bf.get('result.txt') == b'final\n'
                  for ar, a, af in submissions if ar['status'] in ('RETURNED_FOR_REWORK', 'BLOCKED', 'SUPERSEDED')
                  for br, b, bf in current if br['status'] in ('SUBMITTED', 'CANDIDATE_READY')) if current else None)
        questions = []
        for row in capture['questions']:
            fact = {k: e.strict_json(row[k]) for k in ('body', 'original_binding')}
            bound(fact['original_binding'])
            e.require(e.original_digest(fact) == row['original_sha256'] and row['request_id'] == capture['request_id'] and
                      all(row[k] == fact['original_binding'][k] for k in ('task_id', 'team_id')) and
                      fact['body']['task_id'] == row['task_id'] and fact['body']['question_id'] == row['question_id'], 'question binding/hash mismatch')
            questions.append((row, fact['original_binding']))
        if task == 'exhausted_rework':
            check('open_owner_question', any(q['status'] == 'OPEN' and q['holder'] == 'user' and qb == b
                  and f.get('result.txt') == b'draft\n' for q, qb in questions for _, b, f in current) if questions else None)
            check('no_successful_delivery', not any(d['status'] == 'CONFIRMED' for d in capture['deliveries']))
        for row in capture['deliveries']:
            bound(row, True)
            plan, accepted = (e.strict_json(row[k]) for k in ('frozen_plan', 'acceptance_snapshot'))
            manifest = plan['manifest']
            bound(manifest, True)
            e.require(row['delivery_id'] == plan['delivery_id'] == manifest['delivery_id'] and
                      e.original_digest(manifest) == row['manifest_sha256'] == plan['manifest_sha256'], 'delivery manifest mismatch')
            package = files(plan['files'], 'name', 16)
            e.require(manifest['files'] == [{k: f[k] for k in ('name', 'sha256', 'size_bytes')} for f in plan['files']], 'package inventory mismatch')
            refs = {(a['task_id'], a['acceptance_id']) for a in accepted}
            accepted_rows = e.strict_json(package['acceptance.json'])
            e.require(accepted == [{k: a['acceptance'][k] for k in ('task_id', 'acceptance_id', 'record_sha256')}
                                  for a in accepted_rows], 'accepted snapshot differs from package')
            artifacts = manifest['artifacts']
            e.require(0 < len(refs) == len(accepted) <= 8 and 0 < len(artifacts) <= 8 and len({a['name'] for a in artifacts}) == len(artifacts) and
                      {(a['task_id'], a['acceptance_id']) for a in artifacts} == refs, 'invalid accepted inventory')
            e.require(all(any(a['source_path'].removeprefix('/work/') == name and package[a['name']] == value
                              for a in artifacts) for name, value in expected.items()), 'delivery lacks expected original bytes')
            for item in artifacts:
                matches = [f for r, b, f in current if r['submission_id'] == item['submission_id'] and r['task_id'] == item['task_id']]
                e.require((item['task_id'], item['acceptance_id']) in refs and len(matches) == 1 and
                          package[item['name']] == matches[0][item['source_path'].removeprefix('/work/')] and
                          type(item['size_bytes']) is int and len(package[item['name']]) == item['size_bytes'] and
                          hashlib.sha256(package[item['name']]).hexdigest() == item['sha256'], 'delivery differs from accepted original')
            if row['status'] == 'CONFIRMED':
                receipt = e.strict_json(row['receipt'])
                bound(receipt, True)
                e.require(receipt['receipt_id'] and receipt['delivery_id'] == row['delivery_id'] and
                          receipt['manifest_sha256'] == row['manifest_sha256'], 'receipt mismatch')
        if task == 'delivery_receipt':
            check('confirmed_original_delivery', any(d['status'] == 'CONFIRMED' for d in capture['deliveries']) if capture['deliveries'] else None)
        check('capture_integrity', True)
    except FileNotFoundError:
        check('capture_integrity', None)
    except (OSError, ValueError, KeyError, TypeError, AttributeError, OverflowError) as error:
        check('capture_integrity', False)
        report['reason'] = str(error)
    states = report['checks'].values()
    report['artifact_status'] = 'fail' if 'fail' in states else 'missing' if 'missing' in states else 'pass'
    return report


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('task_id', choices=CASES)
    parser.add_argument('capture')
    args = parser.parse_args()
    result = check_outputs(args.task_id, args.capture)
    print(json.dumps(result, ensure_ascii=False, indent=2))
    raise SystemExit(0 if result['artifact_status'] == 'pass' else 1)
