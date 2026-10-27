#!/usr/bin/env python3
"""Bounded, offline paired evaluation over original WLS Store evidence.

This reads a trusted local Store in one read transaction. It does not execute
models, resolve credentials, sign receipts, or turn a synthetic run into a
provider result. JSON reports contain identities/counts/hashes, not prompts.
"""
import argparse
import base64
from collections import Counter
from contextlib import closing, contextmanager
import hashlib
import json
import os
from pathlib import Path
import re
import sqlite3
import stat
import sys
import time

MAX_JSON = 8 * 1024 * 1024
MAX_ROWS = 4096
CONDITIONS = ('multi_agent', 'single_agent')
DIMENSIONS = {'input': 'input_tokens', 'output': 'output_tokens', 'cost': 'cost_micro_usd'}
TASKS = (
    ('tool_roundtrip', 'The input seed.txt contains exactly alpha\\nbeta\\n (UTF-8, LF). Read it, write result.txt with the same bytes, read it back, and submit the captured result.',
     {'successful_tools': ['fs_read', 'fs_write'], 'artifact_sha256': {'result.txt': hashlib.sha256(b'alpha\nbeta\n').hexdigest()}}),
    ('partial_amend', 'Complete two independent files. Apply a changed requirement to one task and explicitly keep the other.',
     {'min_requirements_revision': 2, 'min_tasks': 2, 'all_tasks_current': True, 'accepted_amend_update_and_keep': True}),
    ('rejected_rework', 'Produce a file, receive a negative acceptance, correct it under the original constraints and obtain acceptance.',
     {'accepted_rework_same_task': True}),
    ('exhausted_rework', 'Preserve the original constraints when the rework budget is exhausted and raise an explicit question to the owner.',
     {'owner_question': True, 'rework_exhausted': True}),
    ('delivery_receipt', 'Deliver accepted original file bytes and record an exact receipt for the current requirement and plan versions.',
     {'confirmed_delivery': True}),
)


class InvalidEvidence(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise InvalidEvidence(message)


def canonical(value):
    return json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(',', ':'), allow_nan=False).encode()


def digest(value):
    return hashlib.sha256(canonical(value)).hexdigest()


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate JSON key')
            result[key] = value
        return result
    try:
        result = json.loads(data, object_pairs_hook=pairs,
                            parse_constant=lambda _: (_ for _ in ()).throw(InvalidEvidence('nonfinite JSON')))
    except RecursionError as error:
        raise InvalidEvidence('JSON nesting exceeds bound') from error
    pending, count = [(result, 0)], 0
    while pending:
        item, depth = pending.pop()
        count += 1
        require(depth <= 64 and count <= 100000, 'JSON structure exceeds bound')
        if isinstance(item, (dict, list)):
            pending.extend((child, depth + 1) for child in (item.values() if isinstance(item, dict) else item))
    return result


def regular_path(path):
    path = Path(os.path.abspath(path))
    for part in [*reversed(path.parents), path]:
        require(not part.is_symlink(), 'symbolic link input refused')
    require(stat.S_ISREG(path.stat().st_mode), 'input must be a regular file')
    return path


def read_json(path):
    path = regular_path(path)
    with path.open('rb') as stream:
        data = stream.read(MAX_JSON + 1)
    require(len(data) <= MAX_JSON, 'JSON input exceeds 8 MiB')
    return strict_json(data)


@contextmanager
def database(path):
    path = regular_path(path)
    require(path.stat().st_size <= 512 * 1024 * 1024, 'evaluation database exceeds 512 MiB')
    with closing(sqlite3.connect(path.as_uri() + '?mode=ro', uri=True, timeout=2)) as db:
        db.row_factory = sqlite3.Row
        db.execute('PRAGMA query_only=ON')
        db.setlimit(sqlite3.SQLITE_LIMIT_LENGTH, MAX_JSON)
        deadline = time.monotonic() + 10
        db.set_progress_handler(lambda: int(time.monotonic() >= deadline), 10000)
        db.execute('BEGIN')
        require(db.execute('PRAGMA user_version').fetchone()[0] in (408, 409, 410), 'unsupported Store version')
        require(db.execute('PRAGMA quick_check(1)').fetchone()[0] == 'ok', 'database integrity check failed')
        yield db


def rows(db, sql, parameters=()):
    result, encoded_bytes = [], 2
    for row in db.execute(sql, parameters):
        require(len(result) < MAX_ROWS, 'evidence row limit exceeded')
        item = dict(row)
        encoded_bytes += len(canonical(item)) + bool(result)
        require(encoded_bytes <= 32 * 1024 * 1024, 'evidence byte limit exceeded')
        result.append(item)
    return result


def one(db, sql, parameters):
    result = rows(db, sql, parameters)
    require(len(result) == 1, 'exactly one original evidence row is required')
    return result[0]


def environment_digest(snapshot):
    return digest({'sandbox': snapshot['system']['sandbox'], 'environment_dependencies': [
        {'path': item['path'], 'sha256': item['sha256']} for item in snapshot['dependencies']
        if item['kind'] == 'environment_template']})


def common_lock(db, request_id, model_ref, reasoning):
    request = one(db, 'SELECT * FROM requests WHERE request_id=?', (request_id,))
    snapshot = strict_json(request['config_snapshot'])
    profile = snapshot['system']['models'][model_ref]
    runtime = one(db, 'SELECT * FROM request_runtime WHERE request_id=?', (request_id,))
    budget = one(db, 'SELECT * FROM request_budgets WHERE request_id=?', (request_id,))
    return {'model_ref': model_ref, 'profile_sha256': digest(profile), 'reasoning': reasoning,
            'environment_sha256': environment_digest(snapshot),
            'workflow_policy_sha256': digest({key: snapshot['system'][key] for key in ('amendment', 'limits', 'control', 'context')}),
            'constraints_sha256': digest(rows(db, 'SELECT constraint_id,source_revision,text FROM request_constraints WHERE request_id=? AND requirements_revision=1 ORDER BY constraint_id', (request_id,))),
            'tools': sorted(strict_json(runtime['tool_allowlist'])),
            'budget': {key: budget[key] for key in ('max_attempts', 'input_limit', 'output_limit', 'cost_limit')},
            'max_wall_ms': snapshot['system']['budget_defaults']['max_wall_minutes'] * 60000,
            'max_paid_retries': 0}


def make_plan(db, request_id, model_ref='fake', reasoning=None, evidence_mode='synthetic'):
    require(evidence_mode in ('synthetic', 'provider'), 'unknown evidence mode')
    budget = one(db, 'SELECT attempts FROM request_budgets WHERE request_id=?', (request_id,))
    require(budget['attempts'] == 0, 'plan seed must be admitted before any model attempt')
    plan = {'schema_version': 1, 'benchmark_id': 'r3-small-tasks-v1', 'evidence_mode': evidence_mode,
            'conditions': list(CONDITIONS), 'repetitions': 1,
            'common': common_lock(db, request_id, model_ref, reasoning or {'mode': 'provider_default'}),
            'tasks': [{'task_id': name, 'material': material,
                       'material_sha256': hashlib.sha256(material.encode()).hexdigest(), 'checks': checks}
                      for name, material, checks in TASKS]}
    plan['plan_sha256'] = digest(plan)
    return plan


def validate_plan(plan):
    require(isinstance(plan, dict) and set(plan) == {'schema_version', 'benchmark_id', 'evidence_mode',
            'conditions', 'repetitions', 'common', 'tasks', 'plan_sha256'}, 'unexpected plan shape')
    require(plan['schema_version'] == 1 and plan['benchmark_id'] == 'r3-small-tasks-v1', 'unknown evaluation contract')
    require(plan['plan_sha256'] == digest({key: value for key, value in plan.items() if key != 'plan_sha256'}), 'plan fingerprint mismatch')
    require(plan['conditions'] == list(CONDITIONS) and plan['repetitions'] == 1, 'fixed paired conditions required')
    require(plan['evidence_mode'] in ('synthetic', 'provider'), 'unknown evidence mode')
    common = plan['common']
    require(set(common) == {'model_ref', 'profile_sha256', 'reasoning', 'environment_sha256', 'workflow_policy_sha256', 'constraints_sha256', 'tools',
            'budget', 'max_wall_ms', 'max_paid_retries'}, 'unexpected common condition')
    for key in ('profile_sha256', 'environment_sha256', 'workflow_policy_sha256', 'constraints_sha256'):
        require(isinstance(common[key], str) and re.fullmatch('[a-f0-9]{64}', common[key]), 'invalid fingerprint')
    require(common['tools'] == sorted(set(common['tools'])), 'tool set must be unique and ordered')
    require(common['max_paid_retries'] == 0, 'automatic paid retries are not authorized')
    require(type(common['max_wall_ms']) is int and 0 < common['max_wall_ms'] <= 86400000, 'invalid wall bound')
    require(set(common['budget']) == {'max_attempts', 'input_limit', 'output_limit', 'cost_limit'}, 'invalid budget shape')
    for key, value in common['budget'].items():
        require(type(value) is int and 0 <= value <= 2**63 - 1, 'invalid integer budget')
    require(0 < common['budget']['max_attempts'] <= MAX_ROWS, 'attempt limit out of range')
    require(len(plan['tasks']) == len(TASKS), 'all five fixed tasks are required')
    for actual, (name, material, checks) in zip(plan['tasks'], TASKS):
        require(actual == {'task_id': name, 'material': material,
                'material_sha256': hashlib.sha256(material.encode()).hexdigest(), 'checks': checks},
                'fixed task material or rubric was changed')


def evidence_checks(db, request, task):
    request_id = request['request_id']
    tasks = rows(db, 'SELECT * FROM tasks WHERE request_id=?', (request_id,))
    operations = rows(db, 'SELECT op_id,tool_id,status FROM ops WHERE request_id=?', (request_id,))
    results = {}
    for kind, target in task['checks'].items():
        if kind == 'successful_tools':
            observed = {x['tool_id'] for x in operations if x['status'] == 'SUCCEEDED'}
            results[kind] = set(target) <= observed
        elif kind == 'min_requirements_revision':
            results[kind] = request['requirements_revision'] >= target
        elif kind == 'min_tasks':
            results[kind] = len(tasks) >= target
        elif kind == 'all_tasks_current':
            results[kind] = bool(tasks) and all(x['based_on_requirements_revision'] == request['requirements_revision']
                and x['plan_revision'] == request['plan_revision'] and x['status'] in ('ACCEPTED', 'CANCELLED', 'SKIPPED') for x in tasks)
        elif kind == 'accepted_amend_update_and_keep':
            dispositions = rows(db, "SELECT d.task_id,d.disposition FROM amend_dispositions d JOIN tasks t ON t.task_id=d.task_id AND t.request_id=d.request_id WHERE d.request_id=? AND d.based_on_requirements_revision=? AND d.target_plan_revision=? AND d.target_task_revision<=t.task_revision AND t.based_on_requirements_revision=? AND t.plan_revision=? AND t.status='ACCEPTED'", (request_id, request['requirements_revision'], request['plan_revision'], request['requirements_revision'], request['plan_revision']))
            updated = {x['task_id'] for x in dispositions if x['disposition'] == 'update'}
            kept = {x['task_id'] for x in dispositions if x['disposition'] == 'keep'}
            results[kind] = bool(updated and kept and updated.isdisjoint(kept))
        elif kind == 'accepted_rework_same_task':
            corrected = rows(db, "SELECT DISTINCT t.task_id FROM tasks t JOIN candidate_acceptances a ON a.task_id=t.task_id AND a.request_id=t.request_id JOIN workflow_submissions s ON s.submission_id=a.submission_id JOIN team_workflow_state w ON w.team_id=s.team_id WHERE t.request_id=? AND t.status='ACCEPTED' AND a.status='ACCEPTED' AND w.rework_count>=1 AND json_extract(a.producer_binding,'$.requirements_revision')=? AND json_extract(a.producer_binding,'$.plan_revision')=? AND json_extract(a.producer_binding,'$.task_revision')=t.task_revision AND EXISTS (SELECT 1 FROM candidate_acceptances rejected WHERE rejected.request_id=t.request_id AND rejected.task_id=t.task_id AND json_extract(rejected.decision,'$.passed')=0 AND json_extract(rejected.producer_binding,'$.requirements_revision')=? AND json_extract(rejected.producer_binding,'$.plan_revision')=? AND json_extract(rejected.producer_binding,'$.task_revision')<t.task_revision)", (request_id, request['requirements_revision'], request['plan_revision'], request['requirements_revision'], request['plan_revision']))
            results[kind] = bool(corrected)
        elif kind == 'owner_question':
            results[kind] = bool(rows(db, "SELECT q.question_id FROM questions q JOIN question_runtime r USING(question_id) WHERE q.request_id=? AND q.holder='user' AND r.status='OPEN'", (request_id,)))
        elif kind == 'rework_exhausted':
            maximum = strict_json(request['config_snapshot'])['system']['limits']['team_internal_rework_max']
            exhausted = rows(db, "SELECT q.question_id FROM questions q JOIN question_runtime r USING(question_id) JOIN team_workflow_state w USING(team_id) WHERE q.request_id=? AND q.holder='user' AND r.status='OPEN' AND w.rework_count>=? AND json_extract(r.original_binding,'$.requirements_revision')=? AND json_extract(r.original_binding,'$.plan_revision')=? AND EXISTS (SELECT 1 FROM events e WHERE e.request_id=q.request_id AND e.type='escalate_task' AND json_extract(e.result,'$.question_id')=q.question_id)", (request_id, maximum, request['requirements_revision'], request['plan_revision']))
            results[kind] = bool(exhausted)
        elif kind == 'artifact_sha256':
            captured = rows(db, "SELECT s.* FROM workflow_submissions s JOIN tasks t USING(task_id) WHERE s.request_id=? AND s.status='CANDIDATE_READY' AND t.based_on_requirements_revision=? AND t.plan_revision=?", (request_id, request['requirements_revision'], request['plan_revision']))
            found = {}
            for submission in captured:
                binding = strict_json(submission['original_binding'])
                if binding['requirements_revision'] != request['requirements_revision'] or binding['plan_revision'] != request['plan_revision']:
                    continue
                for item in strict_json(submission['artifact_snapshot']):
                    payload = base64.b64decode(item['content_base64'], validate=True)
                    require(len(payload) == item['size_bytes'] and hashlib.sha256(payload).hexdigest() == item['sha256'], 'stored submission byte mismatch')
                    found[item['path']] = item['sha256']
            results[kind] = all(found.get(path) == expected for path, expected in target.items())
        elif kind == 'confirmed_delivery':
            receipts = rows(db, "SELECT d.*,r.receipt_id FROM delivery_runtime d JOIN delivery_receipts r ON r.delivery_id=d.delivery_id AND r.request_id=d.request_id WHERE d.request_id=? AND d.status='CONFIRMED'", (request_id,))
            valid = False
            for receipt in receipts:
                if receipt['requirements_revision'] != request['requirements_revision'] or receipt['plan_revision'] != request['plan_revision']:
                    continue
                frozen = strict_json(receipt['frozen_plan'])
                require(bool(frozen.get('files')), 'delivery has no captured files')
                for item in frozen['files']:
                    payload = base64.b64decode(item['content_base64'], validate=True)
                    require(len(payload) == item['size_bytes'] and hashlib.sha256(payload).hexdigest() == item['sha256'], 'stored delivery byte mismatch')
                valid = True
            results[kind] = valid
    return results


def inspect_run(plan, task, condition, run, base_directory):
    require(set(run) == {'task_id', 'condition', 'database', 'request_id', 'elapsed_ms', 'plan_sha256'}, 'unexpected run fields')
    require(run['plan_sha256'] == plan['plan_sha256'], 'run references another plan')
    require(type(run['elapsed_ms']) is int and 0 <= run['elapsed_ms'] <= 2**63 - 1, 'invalid host elapsed time')
    with database(base_directory / run['database']) as db:
        request = one(db, 'SELECT * FROM requests WHERE request_id=?', (run['request_id'],))
        original = one(db, 'SELECT text FROM request_texts WHERE request_id=? AND requirements_revision=1', (run['request_id'],))
        require(original['text'] == task['material'], 'original task materials differ')
        common = plan['common']
        require(common_lock(db, run['request_id'], common['model_ref'], common['reasoning']) == common, 'model/environment/tools/budget conditions differ')
        attempts = rows(db, 'SELECT * FROM call_attempts WHERE request_id=? ORDER BY rowid', (run['request_id'],))
        budget = one(db, 'SELECT * FROM request_budgets WHERE request_id=?', (run['request_id'],))
        require(budget['attempts'] == len(attempts), 'attempt count does not reconstruct from original ledger')
        require(len(attempts) <= common['budget']['max_attempts'], 'attempt bound exceeded')
        tables = {row[0] for row in db.execute("SELECT name FROM sqlite_master WHERE type='table'")}
        roles = Counter()
        agent_identities = set()
        kinds = Counter()
        transport_observations = []
        reconciliation_disputes = []
        totals = {name: {'known_total': 0, 'unknown_attempts': 0} for name in (*DIMENSIONS.values(), 'cache_read_tokens', 'cache_write_tokens')}
        evidence = []
        for attempt in attempts:
            envelope = strict_json(attempt['request_envelope'])
            reasoning = {'mode': envelope['context']['reasoning_mode']}
            if reasoning['mode'] == 'effort':
                reasoning['effort'] = envelope['context']['reasoning_effort']
            require(attempt['model_ref'] == common['model_ref'] and reasoning == common['reasoning'], 'attempt changed model or reasoning')
            require(envelope['call_id'] == attempt['call_id'] and envelope['attempt_id'] == attempt['attempt_id'], 'attempt identity mismatch')
            roles[attempt['role']] += 1
            agent_identities.add((attempt['role'], attempt['principal_id'], attempt['team_id']))
            kind = 'synthetic_fake'
            if 'live_attempt_execution' in tables:
                live = rows(db, 'SELECT execution_kind FROM live_attempt_execution WHERE attempt_id=?', (attempt['attempt_id'],))
                if live:
                    kind = live[0]['execution_kind']
            kinds[kind] += 1
            require((kind == 'provider') == (plan['evidence_mode'] == 'provider'), 'synthetic/provider evidence cannot be relabelled')
            if kind != 'synthetic_fake':
                observed = rows(db, 'SELECT record_sha256,safe_summary FROM live_transport_observations WHERE attempt_id=?', (attempt['attempt_id'],))
                if observed:
                    summary = strict_json(observed[0]['safe_summary'])
                    transport_observations.append({'attempt_id': attempt['attempt_id'], 'record_sha256': observed[0]['record_sha256'],
                        'network_started': summary['network_started'], 'transfer_complete': summary['transfer_complete']})
            values = {key: attempt[key] for key in totals}
            # Current projected usage supplements, and never rewrites, the
            # original settlement. Unknown dimensions remain nullable.
            if 'live_usage_projection' in tables:
                projection = rows(db, 'SELECT * FROM live_usage_projection WHERE attempt_id=?', (attempt['attempt_id'],))
                if projection:
                    require(projection[0]['settlement_sha256'] == attempt['settlement_sha256'], 'projection changed original settlement identity')
                    if projection[0]['disputed']:
                        reconciliation_disputes.append(attempt['attempt_id'])
                    for key in values:
                        if key in projection[0]:
                            values[key] = projection[0][key]
            for key, value in values.items():
                if value is None:
                    totals[key]['unknown_attempts'] += 1
                else:
                    require(type(value) is int and value >= 0, 'invalid usage amount')
                    totals[key]['known_total'] += value
                    require(totals[key]['known_total'] <= 2**63 - 1, 'usage total overflow')
            evidence.append({'attempt_id': attempt['attempt_id'], 'call_id': attempt['call_id'],
                             'role': attempt['role'], 'status': attempt['status'],
                             'settlement_sha256': attempt['settlement_sha256'], 'source_kind': kind})
        for dimension, key in DIMENSIONS.items():
            require(totals[key]['known_total'] == budget['spent_' + dimension], 'usage projection does not reconstruct budget')
        condition_valid = bool(attempts) and (set(roles) == {'worker'} and len(agent_identities) == 1
            if condition == 'single_agent' else len(roles) > 1)
        checks = evidence_checks(db, request, task)
        checks['condition_roles'] = condition_valid
        checks['no_usage_dispute'] = not reconciliation_disputes
        checks['live_observations_present'] = len(transport_observations) == sum(count for kind, count in kinds.items() if kind != 'synthetic_fake')
        checks['wall_bound'] = run['elapsed_ms'] <= common['max_wall_ms']
        checks['no_unsettled_attempt'] = all(x['settlement_sha256'] is not None for x in attempts)
        checks['no_unknown_operation'] = not rows(db, "SELECT op_id FROM ops WHERE request_id=? AND status IN ('PREPARED','RUNNING','CANCEL_REQUESTED','UNKNOWN')", (run['request_id'],))
        for dimension in DIMENSIONS:
            checks['budget_' + dimension] = budget['spent_' + dimension] + budget['reserved_' + dimension] <= budget[dimension + '_limit']
        return {'status': 'passed' if all(checks.values()) else 'failed', 'checks': checks,
                'request_id': run['request_id'], 'elapsed_ms': run['elapsed_ms'], 'elapsed_source': 'host_run_index',
                'roles': dict(roles), 'agent_identity_count': len(agent_identities),
                'source_kinds': dict(kinds), 'attempts': evidence,
                'transport_observations': transport_observations, 'disputed_attempts': reconciliation_disputes,
                'usage': totals, 'budget': {key: budget[key] for key in budget if key != 'request_id'},
                'request_status': request['status'], 'evidence_sha256': digest({'attempts': attempts, 'budget': budget})}


def build_report(plan, run_index, base_directory=Path('.')):
    validate_plan(plan)
    base_directory = Path(base_directory)
    require(isinstance(run_index, list) and len(run_index) <= len(TASKS) * 2, 'bounded run array required')
    indexed = {}
    identities = set()
    for run in run_index:
        require(isinstance(run, dict), 'run must be an object')
        key = (run.get('task_id'), run.get('condition'))
        require(key[0] in {x[0] for x in TASKS} and key[1] in CONDITIONS, 'unknown run cell')
        require(key not in indexed, 'duplicate run cell')
        try:
            path = regular_path(base_directory / run['database'])
            identity = (path.stat().st_dev, path.stat().st_ino, run['request_id'])
        except (OSError, InvalidEvidence):
            identity = (str(base_directory / run['database']), run['request_id'])
        require(identity not in identities, 'one request cannot be reused for multiple benchmark cells')
        identities.add(identity)
        indexed[key] = run
    output = []
    for task in plan['tasks']:
        for condition in CONDITIONS:
            cell = {'task_id': task['task_id'], 'condition': condition}
            run = indexed.get((task['task_id'], condition))
            if run is None:
                cell.update(status='not_executed', reason='no original run evidence supplied')
            else:
                try:
                    cell.update(inspect_run(plan, task, condition, run, Path(base_directory)))
                except (InvalidEvidence, sqlite3.Error, KeyError, TypeError, ValueError, OSError) as error:
                    cell.update(status='invalid_evidence', reason=str(error)[:256])
            output.append(cell)
    counts = Counter(x['status'] for x in output)
    report = {'schema_version': 1, 'plan_sha256': plan['plan_sha256'], 'evidence_mode': plan['evidence_mode'],
              'rows': output, 'counts': dict(counts), 'expected_cells': len(TASKS) * 2,
              'observed_cells': len(indexed), 'paired_cells_complete': len(indexed) == len(TASKS) * 2,
              'general_quality_or_cost_advantage_established': False,
              'cache_definition': 'provider-reported nullable cache token dimensions; not inferred savings',
              'provenance': 'trusted local SQLite snapshot; fingerprints are consistency checks, not signatures'}
    report['report_sha256'] = digest(report)
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    seed = sub.add_parser('plan', help='freeze the five-task paired plan from an unused admitted request')
    seed.add_argument('--db', required=True)
    seed.add_argument('--request', required=True)
    seed.add_argument('--model-ref', default='fake')
    seed.add_argument('--reasoning', default='{"mode":"provider_default"}')
    seed.add_argument('--evidence-mode', choices=('synthetic', 'provider'), default='synthetic')
    report = sub.add_parser('report', help='read all supplied evidence; retain every missing or invalid cell')
    report.add_argument('--plan', required=True)
    report.add_argument('--runs', required=True)
    args = parser.parse_args()
    try:
        if args.command == 'plan':
            with database(args.db) as db:
                result = make_plan(db, args.request, args.model_ref, strict_json(args.reasoning), args.evidence_mode)
        else:
            result = build_report(read_json(args.plan), read_json(args.runs), Path(args.runs).absolute().parent)
        print(canonical(result).decode())
        return 0
    except (InvalidEvidence, sqlite3.Error, KeyError, TypeError, ValueError, OSError) as error:
        print(json.dumps({'error': 'invalid_evidence', 'message': str(error)[:256]}), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
