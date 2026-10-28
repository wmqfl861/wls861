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
import math
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
LOGICAL_AGENT_CONTRACT = 'single-agent-role-broker-v1'
DEFAULT_LOGICAL_AGENT_POLICY = {'mode': LOGICAL_AGENT_CONTRACT, 'max_attempts': 128, 'max_history_bytes': 524288}
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


class OriginalFloat(float):
    """A JSON number with its original spelling and normal numeric behavior."""
    def __new__(cls, spelling):
        value = super().__new__(cls, spelling)
        require(math.isfinite(value), 'nonfinite JSON')
        value.spelling = spelling
        return value


def original_canonical(value):
    """Rebuild C++ Store facts with their captured numeric tokens.

    Plan and report fingerprints retain canonical()/digest() semantics.
    Original Store JSON is already canonical; no Boost number formatting is
    inferred from a rounded Python value when reconstructing its aggregates.
    """
    if isinstance(value, OriginalFloat):
        return value.spelling.encode('ascii')
    if isinstance(value, dict):
        return b'{' + b','.join(canonical(key) + b':' + original_canonical(value[key])
                                for key in sorted(value)) + b'}'
    if isinstance(value, list):
        return b'[' + b','.join(original_canonical(item) for item in value) + b']'
    return canonical(value)


def original_digest(value):
    return hashlib.sha256(original_canonical(value)).hexdigest()


def original_logical_request_sha256(raw, envelope):
    """Hash Store's canonical values without changing nested number spellings.

    The caller has already checked the complete document with strict_json.
    Python and Boost.JSON serialize doubles differently, so only top-level
    self-referential fields are removed; every other original value is kept.
    """
    decoder, fields = json.JSONDecoder(), {}

    def whitespace(index):
        while index < len(raw) and raw[index] in ' \t\r\n':
            index += 1
        return index

    position = whitespace(0)
    require(raw[position:position + 1] == '{', 'original request must be an object')
    position = whitespace(position + 1)
    while raw[position:position + 1] != '}':
        key, end = decoder.raw_decode(raw, position)
        require(isinstance(key, str) and key not in fields, 'invalid original request key')
        position = whitespace(end)
        require(raw[position:position + 1] == ':', 'invalid original request member')
        start = whitespace(position + 1)
        _, end = decoder.raw_decode(raw, start)
        fields[key] = raw[start:end]
        position = whitespace(end)
        if raw[position:position + 1] == '}':
            break
        require(raw[position:position + 1] == ',', 'invalid original request separator')
        position = whitespace(position + 1)
    require(whitespace(position + 1) == len(raw) and set(fields) == set(envelope),
            'original request fields or trailing bytes differ')
    retained = [canonical(key) + b':' + fields[key].encode() for key in sorted(fields)
                if key not in ('logical_request_sha256', 'wire_request_sha256')]
    return hashlib.sha256(b'{' + b','.join(retained) + b'}').hexdigest()


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate JSON key')
            result[key] = value
        return result
    try:
        result = json.loads(data, object_pairs_hook=pairs, parse_float=OriginalFloat,
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


def common_lock(db, request_id, model_ref, reasoning, *, logical_agent_comparison=False):
    request = one(db, 'SELECT * FROM requests WHERE request_id=?', (request_id,))
    snapshot = strict_json(request['config_snapshot'])
    profile = snapshot['system']['models'][model_ref]
    runtime = one(db, 'SELECT * FROM request_runtime WHERE request_id=?', (request_id,))
    require(hashlib.sha256(request['config_snapshot'].encode()).hexdigest() == runtime['config_snapshot_sha256'],
            'original frozen configuration fingerprint mismatch')
    budget = one(db, 'SELECT * FROM request_budgets WHERE request_id=?', (request_id,))
    workflow_policy = {key: snapshot['system'][key] for key in ('amendment', 'limits', 'control', 'context')}
    if logical_agent_comparison:
        # Only the explicitly versioned experimental topology differs. All
        # other context, permission, environment and budget facts still lock.
        workflow_policy['context'] = {key: value for key, value in workflow_policy['context'].items()
                                      if key != 'logical_agent'}
    return {'model_ref': model_ref, 'profile_sha256': digest(profile), 'reasoning': reasoning,
            'environment_sha256': environment_digest(snapshot),
            'workflow_policy_sha256': digest(workflow_policy),
            'constraints_sha256': digest(rows(db, 'SELECT constraint_id,source_revision,text FROM request_constraints WHERE request_id=? AND requirements_revision=1 ORDER BY constraint_id', (request_id,))),
            'tools': sorted(strict_json(runtime['tool_allowlist'])),
            'budget': {key: budget[key] for key in ('max_attempts', 'input_limit', 'output_limit', 'cost_limit')},
            'max_wall_ms': snapshot['system']['budget_defaults']['max_wall_minutes'] * 60000,
            'max_paid_retries': 0}


def validate_logical_agent_policy(policy):
    require(isinstance(policy, dict) and set(policy) == {'mode', 'max_attempts', 'max_history_bytes'},
            'invalid logical Agent policy shape')
    require(policy['mode'] == LOGICAL_AGENT_CONTRACT, 'unknown logical Agent policy')
    require(type(policy['max_attempts']) is int and 1 <= policy['max_attempts'] <= 128,
            'logical Agent attempt capacity is invalid')
    require(type(policy['max_history_bytes']) is int and 4096 <= policy['max_history_bytes'] <= 524288,
            'logical Agent history capacity is invalid')


def make_plan(db, request_id, model_ref='fake', reasoning=None, evidence_mode='synthetic',
              decision_evidence=None, identity_contract=None):
    require(evidence_mode in ('synthetic', 'provider'), 'unknown evidence mode')
    require(identity_contract in (None, LOGICAL_AGENT_CONTRACT), 'unknown logical identity contract')
    decision_evidence = decision_evidence or ('model-origin-v1' if evidence_mode == 'provider' or identity_contract else 'structural-v1')
    require(decision_evidence in ('structural-v1', 'model-origin-v1'), 'unknown decision evidence contract')
    require(evidence_mode != 'provider' or decision_evidence == 'model-origin-v1',
            'new provider plans require model-origin-v1 decision evidence')
    require(not identity_contract or decision_evidence == 'model-origin-v1',
            'logical Agent comparison requires original model-decision provenance')
    budget = one(db, 'SELECT attempts FROM request_budgets WHERE request_id=?', (request_id,))
    require(budget['attempts'] == 0, 'plan seed must be admitted before any model attempt')
    plan = {'schema_version': 1, 'benchmark_id': 'r3-small-tasks-v1', 'evidence_mode': evidence_mode,
            'conditions': list(CONDITIONS), 'repetitions': 1,
            'common': common_lock(db, request_id, model_ref, reasoning or {'mode': 'provider_default'},
                                  logical_agent_comparison=bool(identity_contract)),
            'tasks': [{'task_id': name, 'material': material,
                       'material_sha256': hashlib.sha256(material.encode()).hexdigest(), 'checks': checks}
                      for name, material, checks in TASKS]}
    if decision_evidence == 'model-origin-v1':
        plan.update(schema_version=2, benchmark_id='r3-small-tasks-v2', decision_evidence=decision_evidence)
    if identity_contract:
        request = one(db, 'SELECT config_snapshot FROM requests WHERE request_id=?', (request_id,))
        context = strict_json(request['config_snapshot'])['system']['context']
        require(context.get('request_format') == 'structured-v2', 'logical Agent comparison requires a structured-v2 seed')
        policy = context.get('logical_agent', DEFAULT_LOGICAL_AGENT_POLICY)
        validate_logical_agent_policy(policy)
        plan.update(schema_version=3, benchmark_id='r3-small-tasks-v3', identity_contract=identity_contract,
                    single_agent_policy=policy)
    plan['plan_sha256'] = digest(plan)
    return plan


def validate_plan(plan):
    fields = {'schema_version', 'benchmark_id', 'evidence_mode',
              'conditions', 'repetitions', 'common', 'tasks', 'plan_sha256'}
    require(isinstance(plan, dict), 'plan must be an object')
    version = plan.get('schema_version')
    if version in (2, 3):
        fields.add('decision_evidence')
        require(plan.get('decision_evidence') == 'model-origin-v1', 'unknown decision evidence contract')
    if version == 3:
        fields.update(('identity_contract', 'single_agent_policy'))
        require(plan.get('identity_contract') == LOGICAL_AGENT_CONTRACT, 'unknown logical identity contract')
        validate_logical_agent_policy(plan.get('single_agent_policy'))
    require(set(plan) == fields, 'unexpected plan shape')
    require(type(version) is int and version in (1, 2, 3) and
            plan['benchmark_id'] == f'r3-small-tasks-v{version}', 'unknown evaluation contract')
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


def captured_contract(envelope):
    """Read the original compiled input, never today's task or workspace."""
    context = envelope['context']
    if context['format'] == 'wls-compiled-context-v2':
        source = context['input']
        require(original_digest(source) == context['logical_input_sha256'], 'original logical input fingerprint mismatch')
        require(original_digest(source['prefix']) == context['prefix_sha256'], 'original context prefix fingerprint mismatch')
        return strict_json(source['task_contract'])
    require(context['format'] == 'wls-compiled-context-v1', 'unknown captured context format')
    raw = context['dynamic_suffix'].encode()
    header, separator, remaining = raw.partition(b'\n')
    match = re.fullmatch(rb'task_contract (0|[1-9][0-9]{0,19})', header)
    require(bool(separator and match), 'original task contract frame is missing')
    size = int(match[1])
    require(size < len(remaining) and remaining[size:size + 1] == b'\n', 'original task contract frame is truncated')
    return strict_json(remaining[:size])


def logical_agent_marker(logical_id, prefix, chain):
    return {'contract': LOGICAL_AGENT_CONTRACT, 'logical_agent_id': logical_id,
            'global_step': len(prefix) + 1, 'history_count': len(prefix),
            'history_sha256': original_digest(prefix), 'previous_chain_sha256': chain}


def logical_agent_messages(history):
    """Independent reconstruction of the native messages actually sent onward."""
    messages = []
    for record in history:
        response, feedback = record['response'], record['feedback']
        runtime_error = isinstance(feedback['result'], dict) and feedback['result'].get('runtime_error') is True
        paired = (record['accepted'] == 1 and response['status'] == 'completed' and
                  len(response['tool_calls']) == 1 and not runtime_error)
        if not paired:
            messages.append({'role': 'user', 'content': original_canonical(record).decode(), 'tool_calls': [], 'tool_call_id': '',
                             'source': {'kind': 'unapplied_or_missing_observation', 'not_execution_authority': True}})
            continue
        tool = response['tool_calls'][0]
        call_id = 'history_' + digest({'format': 'wls-history-tool-id-v1', 'attempt_id': response['attempt_id'],
                                       'call_id': tool['call_id']})
        source = {key: value for key, value in record.items() if key not in ('request_envelope', 'response', 'feedback')}
        source.update(record_sha256=feedback['record_sha256'], original_tool_call_id=tool['call_id'],
                      kind='recorded_tool_turn', not_execution_authority=True)
        messages.append({'role': 'assistant', 'content': response['text'],
                         'tool_calls': [{'call_id': call_id, 'name': tool['name'], 'arguments': tool['arguments']}],
                         'tool_call_id': '', 'source': source})
        messages.append({'role': 'tool', 'content': original_canonical(feedback['result']).decode(),
                         'tool_calls': [], 'tool_call_id': call_id, 'source': source})
    return messages


def logical_agent_evidence(db, request, attempts, condition, policy):
    snapshot = strict_json(request['config_snapshot'])
    configured = snapshot['system']['context'].get('logical_agent')
    if condition == 'multi_agent':
        require(configured is None, 'multi-Agent condition uses the single-Agent shared-history policy')
        for attempt in attempts:
            envelope = strict_json(attempt['request_envelope'])
            require('logical_agent_execution' not in captured_contract(envelope),
                    'multi-Agent condition contains a shared logical execution marker')
            if envelope['context']['format'] == 'wls-compiled-context-v2':
                for message in envelope['context']['input']['messages']:
                    source = message['source'] or {}
                    if source.get('kind') == 'unapplied_or_missing_observation':
                        source = strict_json(message['content'])
                    if source.get('kind') == 'recorded_tool_turn' or 'attempt_id' in source:
                        require(all(source[key] == attempt[key] for key in ('role', 'principal_id', 'task_id', 'team_id')),
                                'multi-Agent native history crossed its original authority identity')
                    if message['role'] in ('assistant', 'tool'):
                        require(source.get('kind') == 'recorded_tool_turn', 'multi-Agent native history has no original actor provenance')
        return {'contract': 'role-isolated-v1', 'status': 'linked' if attempts else 'incomplete',
                'shared_request_history': False}
    require(configured == policy, 'single-Agent frozen policy differs from the precommitted plan')
    validate_logical_agent_policy(configured)
    require(snapshot['system']['context'].get('request_format') == 'structured-v2', 'single-Agent input is not structured-v2')
    require(len(attempts) <= policy['max_attempts'], 'single-Agent history exceeds its frozen attempt capacity')
    resolutions = snapshot['role_resolutions']
    selected = resolutions['dispatcher']
    for role in ('team_lead', 'worker'):
        require(all(resolutions[role][key] == selected[key] for key in ('model_ref', 'reasoning')),
                'single-Agent authority roles changed model or reasoning')
    config_hash = one(db, 'SELECT config_snapshot_sha256 FROM request_runtime WHERE request_id=?',
                      (request['request_id'],))['config_snapshot_sha256']
    require(hashlib.sha256(request['config_snapshot'].encode()).hexdigest() == config_hash,
            'single-Agent frozen configuration fingerprint mismatch')
    seed = digest({'format': 'wls-single-agent-chain-v1', 'request_id': request['request_id'],
                   'config_snapshot_sha256': config_hash, 'policy': policy})
    logical_id, chain, history = 'logical_' + seed, seed, []
    tables = {row['name'] for row in rows(db, "SELECT name FROM sqlite_master WHERE type='table'")}
    for attempt in attempts:
        envelope = strict_json(attempt['request_envelope'])
        require(envelope['context']['format'] == 'wls-compiled-context-v2', 'single-Agent original context is not structured')
        contract = captured_contract(envelope)
        marker = logical_agent_marker(logical_id, history, chain)
        require(contract.get('logical_agent_execution') == marker, 'single-Agent execution chain is missing, reordered or forked')
        require(envelope['context']['input']['messages'] == logical_agent_messages(history),
                'single-Agent actual model input omitted or changed the shared original history')
        require(len(original_canonical(history)) <= policy['max_history_bytes'], 'single-Agent shared history byte capacity exceeded')
        require(envelope['logical_request_sha256'] == original_logical_request_sha256(attempt['request_envelope'], envelope),
                'single-Agent original request fingerprint mismatch')
        if 'live_attempt_execution' in tables:
            live = rows(db, 'SELECT logical_request_sha256 FROM live_attempt_execution WHERE attempt_id=?', (attempt['attempt_id'],))
            if live:
                require(live[0]['logical_request_sha256'] == envelope['logical_request_sha256'],
                        'single-Agent transcript differs from the request admitted for original live transport')
        require(attempt['settlement_sha256'] is not None and attempt['response'] is not None,
                'single-Agent original turn remains unsettled')
        feedback = one(db, 'SELECT * FROM model_tool_feedback WHERE attempt_id=?', (attempt['attempt_id'],))
        for key in ('original_binding', 'response', 'result'):
            feedback[key] = strict_json(feedback[key])
        response, binding = strict_json(attempt['response']), strict_json(attempt['frozen_binding'])
        require(feedback['original_binding'] == binding and feedback['response'] == response,
                'single-Agent immutable feedback changed its original response or binding')
        require(original_digest({key: feedback[key] for key in ('attempt_id', 'original_binding', 'response', 'result')}) == feedback['record_sha256'],
                'single-Agent immutable feedback fingerprint mismatch')
        require(response['call_id'] == attempt['call_id'] and response['attempt_id'] == attempt['attempt_id'] and
                response['binding'] == binding and response['role'] == attempt['role'] and
                response['config_snapshot_id'] == attempt['config_snapshot_sha256'] and
                response['role_call_index'] == envelope['role_call_index'], 'single-Agent original response identity mismatch')
        record = {key: attempt[key] for key in ('attempt_id', 'call_id', 'request_id', 'task_id', 'team_id', 'role',
                    'principal_id', 'config_snapshot_sha256', 'model_ref', 'reservation_sha256', 'status', 'accepted',
                    'settlement_sha256')}
        record.update(frozen_binding=binding, role_call_index=envelope['role_call_index'], response=response,
                      usage=strict_json(attempt['usage']), logical_agent_execution=marker,
                      history_sequence=len(history) + 1, feedback=feedback, feedback_missing=False)
        require(record['request_id'] == request['request_id'] and record['config_snapshot_sha256'] == config_hash,
                'single-Agent history crossed its request or configuration')
        chain = original_digest({'format': 'wls-single-agent-link-v1', 'previous_sha256': chain, 'fact': record})
        history.append(record)
    return {'contract': LOGICAL_AGENT_CONTRACT, 'status': 'linked' if attempts else 'incomplete',
            'logical_agent_id': logical_id, 'logical_agent_count': 1 if attempts else 0,
            'global_steps': len(history), 'final_chain_sha256': chain, 'shared_request_history': True,
            'authority_roles_are_separate_agents': False, 'private_history_in_report': False}


def model_applications(db, request_id, attempts):
    """Check original response -> captured target -> feedback -> business event.

    This is a consistency check over trusted host records, not a signature or
    proof that a provider used the advertised model. Diagnostic file reads
    intentionally have feedback but no business operation row.
    """
    applications, missing = [], []
    tables = {row['name'] for row in rows(db, "SELECT name FROM sqlite_master WHERE type='table'")}
    recognized = {'submit_plan', 'submit_amendment_plan', 'resolve_amendment', 'issue_work_order',
                  'submit', 'check_submission', 'accept_candidate', 'change_task', 'escalate_task',
                  'fs_read', 'fs_write', 'fs_patch', 'fs_mkdir', 'note'}
    for ordinal, attempt in enumerate(attempts):
        if attempt['accepted'] != 1 or attempt['response'] is None:
            continue
        response, envelope = strict_json(attempt['response']), strict_json(attempt['request_envelope'])
        if response['status'] != 'completed' or len(response['tool_calls']) != 1:
            continue
        tool = response['tool_calls'][0]
        if tool['name'] not in recognized:
            continue
        binding = strict_json(attempt['frozen_binding'])
        require(envelope['binding'] == binding, 'original attempt binding mismatch')
        for key in ('call_id', 'attempt_id', 'role', 'role_call_index', 'binding', 'config_snapshot_id'):
            require(response[key] == envelope[key], 'model response changed captured identity')
        require(response['role'] == attempt['role'] and binding['request_id'] == request_id,
                'model response changed original role or request')
        require(response['config_snapshot_id'] == attempt['config_snapshot_sha256'], 'model response changed frozen config')
        transport_identity = {}
        if 'live_attempt_execution' in tables:
            live = rows(db, 'SELECT * FROM live_attempt_execution WHERE attempt_id=?', (attempt['attempt_id'],))
            if live:
                claim = one(db, 'SELECT * FROM live_send_claims WHERE attempt_id=?', (attempt['attempt_id'],))
                observed = one(db, 'SELECT * FROM live_transport_observations WHERE attempt_id=?', (attempt['attempt_id'],))
                summary = strict_json(observed['safe_summary'])
                require(strict_json(observed['response']) == response and summary['decoded_response_sha256'] == original_digest(response),
                        'model decision differs from the original transport response')
                require(observed['claim_id'] == claim['claim_id'] and strict_json(claim['frozen_binding']) == binding,
                        'model decision changed original live send identity')
                require(summary['record_sha256'] == observed['record_sha256'] and
                        original_digest({key: value for key, value in summary.items() if key != 'record_sha256'}) == observed['record_sha256'],
                        'original transport fingerprint mismatch')
                require(summary['response_usable'] and summary['transfer_complete'] and summary['network_started'],
                        'model application has no complete original transport')
                for key in ('encoded_record_sha256', 'request_body_sha256', 'profile_sha256',
                            'logical_request_sha256', 'transport_options_sha256'):
                    require(summary[key] == live[0][key], 'model application changed admitted live request')
                for key in ('headers', 'body'):
                    raw = bytes.fromhex(observed['raw_' + key + '_hex'])
                    require(hashlib.sha256(raw).hexdigest() == summary['raw_' + key + '_sha256'],
                            'original transport bytes differ from their fingerprint')
                require(original_digest(strict_json(observed['protocol_evidence'])) == summary['protocol_evidence_sha256'],
                        'original protocol evidence fingerprint mismatch')
                transport_identity = {'transport_sha256': observed['record_sha256'], 'send_claim_id': claim['claim_id'],
                                      'execution_kind': live[0]['execution_kind']}
        contract = captured_contract(envelope)
        require(contract['binding'] == binding and type(contract['captured_state_version']) is int,
                'captured workflow input changed original binding')
        feedback = rows(db, 'SELECT * FROM model_tool_feedback WHERE attempt_id=?', (attempt['attempt_id'],))
        if not feedback:
            missing.append({'attempt_id': attempt['attempt_id'], 'reason': 'missing_original_feedback'})
            continue
        feedback = feedback[0]
        original, recorded, result = (strict_json(feedback[key]) for key in ('original_binding', 'response', 'result'))
        require(original == binding and recorded == response, 'feedback changed original response or binding')
        require(original_digest({'attempt_id': attempt['attempt_id'], 'original_binding': original,
                        'response': recorded, 'result': result}) == feedback['record_sha256'],
                'original feedback fingerprint mismatch')
        if not isinstance(result, dict) or result.get('runtime_error'):
            continue
        action = contract.get('workflow_action')
        name, arguments = tool['name'], tool['arguments']
        application = {'attempt_id': attempt['attempt_id'], 'role': attempt['role'], 'tool_name': name,
                       'tool_call_id': tool['call_id'], 'ordinal': ordinal, 'binding': binding,
                       'arguments': arguments, 'result': result, 'action': action,
                       'feedback_sha256': feedback['record_sha256'], 'event_id': None,
                       'target_sha256': digest(action), **transport_identity}
        command_id = attempt['attempt_id'] + ':' + tool['call_id']
        if name == 'fs_read':
            require(attempt['role'] == 'worker' and action is None, 'diagnostic belongs to another model role')
            require(result.get('path') == arguments['path'].removeprefix('/work/'), 'diagnostic result changed original path')
            applications.append(application)
            continue
        if name in ('fs_write', 'fs_patch', 'fs_mkdir', 'note'):
            require(attempt['role'] == 'worker' and action is None, 'business tool belongs to another model role')
            operation = result.get('operation', result)
            if operation.get('status') != 'SUCCEEDED' or 'op_id' not in operation:
                continue
            stored = one(db, 'SELECT * FROM ops WHERE op_id=? AND request_id=?', (operation['op_id'], request_id))
            require(stored['tool_id'] == name and stored['business_idempotency_key'] == attempt['attempt_id'] and
                    stored['args_sha256'] == original_digest(arguments), 'operation did not originate from the model tool call')
            for key, value in binding.items():
                if key in stored:
                    require(stored[key] == value, 'operation changed the original model binding')
            intent = one(db, 'SELECT arguments FROM operation_intents WHERE op_id=?', (stored['op_id'],))
            require(strict_json(intent['arguments']) == arguments, 'original operation arguments differ')
            if 'execution_result' in result:
                execution = one(db, 'SELECT * FROM operation_results WHERE op_id=?', (stored['op_id'],))
                require(execution['record_sha256'] == result['execution_result']['record_sha256'] and
                        strict_json(execution['original_binding']) == binding and execution['observed_outcome'] == 'SUCCEEDED',
                        'operation result differs from original model feedback')
            application['operation_id'] = stored['op_id']
            applications.append(application)
            continue
        if name in ('submit_plan', 'submit_amendment_plan', 'resolve_amendment'):
            require(attempt['role'] == 'dispatcher', 'plan decision belongs to another model role')
            if name == 'submit_plan':
                require(action is None and binding['plan_revision'] == 0, 'initial model plan cannot supply host AMEND decisions')
                event_type, suffix = 'independent_plan', ':independent-plan'
            else:
                require(action is not None and action['kind'] == 'amendment', 'AMEND decision lacks its original captured target')
                publish = name == 'submit_amendment_plan'
                require(action['target']['plan_required'] is publish, 'AMEND response changed its captured phase')
                event_type, suffix = ('independent_plan', ':amendment-plan') if publish else ('independent_dispositions', ':amendment')
        else:
            event_type, suffix = {'issue_work_order': ('instruction', ''), 'submit': ('submit_work', ''),
                'check_submission': ('check_submission', ''), 'accept_candidate': ('accept_candidate', ''),
                'change_task': ('change_task', ''), 'escalate_task': ('escalate_task', '')}[name]
        events = rows(db, 'SELECT * FROM events WHERE request_id=? AND command_id=? AND type=?',
                      (request_id, command_id + suffix, event_type))
        if not events:
            missing.append({'attempt_id': attempt['attempt_id'], 'reason': 'missing_original_application_event'})
            continue
        event = events[0]
        payload = strict_json(event['payload'])
        require(original_digest(payload) == event['command_sha256'], 'application event fingerprint mismatch')
        require(strict_json(event['result']) == result, 'application event result differs from original feedback')
        actor = payload['actor']
        require(actor['role'] == attempt['role'] and actor['principal_id'] == attempt['principal_id'] and
                actor['request_id'] == request_id and actor['task_id'] == (attempt['task_id'] or '') and
                actor['team_id'] == (attempt['team_id'] or ''), 'application event changed original actor')
        for key in ('dispatcher_epoch', 'execution_epoch'):
            require(actor[key] == binding[key], 'application event changed original actor epoch')
        application.update(event_id=event['event_id'], event_seq=event['seq'], command_sha256=event['command_sha256'])
        if name in ('submit_plan', 'submit_amendment_plan', 'resolve_amendment'):
            require(payload['expected_request_version'] == contract['captured_state_version'] and
                    payload['based_on_requirements_revision'] == binding['requirements_revision'],
                    'plan application changed captured request versions')
            decisions = {} if name == 'submit_plan' else arguments['dispositions']
            require(payload['dispositions'] == decisions, 'host dispositions are not original model decisions')
            if name != 'resolve_amendment':
                expected = dict(arguments if name == 'submit_plan' else arguments['plan'],
                                plan_revision=binding['plan_revision'] + 1,
                                based_on_requirements_revision=binding['requirements_revision'])
                require(payload['body'] == expected, 'applied plan differs from original model plan')
            else:
                require(payload['target_plan_revision'] == binding['plan_revision'], 'AMEND changed captured plan revision')
            if name != 'submit_plan':
                target = action['target']
                require(set(decisions) <= set(target['pending_node_ids']), 'AMEND decision names an uncaptured node')
                guards = [{key: item[key] for key in ('node_id', 'task_id', 'state_version', 'team_id', 'execution_epoch')}
                          for item in target['tasks'] if name == 'submit_amendment_plan' or item['node_id'] in decisions]
                require(payload['captured_tasks'] == guards, 'AMEND application changed captured task guards')
            application['plan_revision'] = result['plan_revision']
        else:
            require(payload['binding'] == binding, 'business application changed original model binding')
            if name in ('issue_work_order', 'submit'):
                require(action is None and payload['expected_task_version'] == contract['captured_state_version'],
                        'model application changed captured task version')
                require(attempt['role'] == ('team_lead' if name == 'issue_work_order' else 'worker'),
                        'model application belongs to another role')
                expected = dict(arguments)
                if name == 'issue_work_order':
                    expected.update(binding, instruction_revision=binding['instruction_revision'] + 1)
                require(payload['body'] == expected, 'business body differs from original model arguments')
            else:
                expected_action = {'check_submission': ('team_lead', 'submission'), 'accept_candidate': ('dispatcher', 'acceptance'),
                                   'change_task': ('dispatcher', 'rejection'), 'escalate_task': ('dispatcher', 'rejection')}
                role, kind = expected_action[name]
                require(attempt['role'] == role and action is not None and action['kind'] == kind,
                        'model decision lacks its original captured target')
                target = action['target']
                expected_task = contract['captured_state_version'] if name == 'check_submission' else action['expected_task_version']
                require(payload['submission_id'] == target['submission_id'] and
                        payload['expected_submission_version'] == target['state_version'] and
                        payload['expected_task_version'] == expected_task and result['submission_id'] == target['submission_id'],
                        'model decision applied to another target or version')
                body_key = {'check_submission': 'check_body', 'accept_candidate': 'decision',
                            'change_task': 'correction', 'escalate_task': 'body'}[name]
                expected = {key: value for key, value in arguments.items() if name != 'check_submission' or key != 'work_order'}
                require(payload[body_key] == expected, 'applied decision differs from original model arguments')
                if name == 'check_submission':
                    require(payload['rework_order'] == arguments.get('work_order'), 'applied rework order differs from model decision')
                application['submission_id'] = target['submission_id']
        if name == 'submit':
            application['submission_id'] = result['submission_id']
        if name == 'accept_candidate':
            application['acceptance_id'] = result['acceptance']['acceptance_id']
            acceptance = one(db, 'SELECT * FROM candidate_acceptances WHERE acceptance_id=? AND request_id=?',
                             (application['acceptance_id'], request_id))
            require(acceptance['submission_id'] == application['submission_id'] and
                    strict_json(acceptance['captured_binding']) == binding and strict_json(acceptance['decision']) == arguments and
                    acceptance['record_sha256'] == result['acceptance']['record_sha256'], 'acceptance fact differs from original model decision')
            application['acceptance_status'] = acceptance['status']
        applications.append(application)
    public = [{key: app[key] for key in ('attempt_id', 'role', 'tool_name', 'tool_call_id', 'feedback_sha256',
               'event_id', 'target_sha256', 'operation_id', 'submission_id', 'acceptance_id',
               'transport_sha256', 'send_claim_id', 'execution_kind') if key in app}
              for app in applications]
    return applications, {'contract': 'model-origin-v1', 'applications': public, 'missing': missing,
                          'evidence_sha256': digest({'applications': public, 'missing': missing}),
                          'source_authenticity': 'trusted_host_records_not_provider_signatures'}


def model_origin_checks(db, request, task, applications, evidence, *, allow_recovery_projection=False):
    """Require the model's actual application to be the business fact scored."""
    by_name = {name: [app for app in applications if app['tool_name'] == name]
               for name in {app['tool_name'] for app in applications}}
    submitted = {app['submission_id']: app for app in by_name.get('submit', [])}
    checked = {app['submission_id']: app for app in by_name.get('check_submission', []) if app['arguments']['passed']}
    accepted = {app['submission_id']: app for app in by_name.get('accept_candidate', []) if app['arguments']['passed']}
    rejected = {app['submission_id']: app for app in by_name.get('accept_candidate', []) if not app['arguments']['passed']}
    plans = by_name.get('submit_plan', []) + by_name.get('submit_amendment_plan', [])
    orders = by_name.get('issue_work_order', [])
    tasks = {row['task_id']: row for row in rows(db, 'SELECT * FROM tasks WHERE request_id=?', (request['request_id'],))}
    originals = {}
    recovery_witnesses = {}
    original_orders = {}
    prior_recoveries = {}

    def order_reaches_submission(order, app):
        original, target = order['result']['binding'], app['binding']
        if order['ordinal'] >= app['ordinal']:
            return False
        if original == target:
            return True
        # v1/v2 retain their original exact-binding interpretation. v3 can
        # follow the Store's explicitly recorded epoch-only recovery lease;
        # it cannot infer a semantic decision from today's task state.
        if not allow_recovery_projection:
            return False
        epochs = ('dispatcher_epoch', 'execution_epoch')
        if set(original) != set(target) or any(original[key] != target[key] for key in original if key not in epochs):
            return False
        if any(target[key] <= original[key] for key in epochs):
            return False
        key = (original['task_id'], original['instruction_revision'])
        if key not in original_orders:
            original_orders[key] = one(db, 'SELECT * FROM instructions WHERE request_id=? AND task_id=? AND instruction_revision=?',
                                      (request['request_id'], *key))
        instruction = original_orders[key]
        semantic = ('request_id', 'task_id', 'team_id', 'plan_revision', 'task_revision',
                    'instruction_revision', 'dispatcher_epoch', 'execution_epoch')
        require(all(instruction[name] == original[name] for name in semantic) and
                instruction['based_on_requirements_revision'] == original['requirements_revision'] and
                strict_json(instruction['body']) == dict(order['arguments'], **original),
                'recovered instruction differs from its original model work order')
        interval = (order['event_seq'], app['event_seq'])
        if interval not in prior_recoveries:
            prior_recoveries[interval] = rows(db,
                "SELECT * FROM events WHERE request_id=? AND type='recover' AND seq>? AND seq<? ORDER BY seq",
                (request['request_id'], *interval))
        recoveries = prior_recoveries[interval]
        if not recoveries:
            return False
        body_sha256 = hashlib.sha256(instruction['body'].encode()).hexdigest()
        projected = {name: original[name] for name in epochs}
        for recovery in recoveries:
            payload, result = strict_json(recovery['payload']), strict_json(recovery['result'])
            require(payload == {'type': 'recover'} and original_digest(payload) == recovery['command_sha256'],
                    'original instruction recovery event fingerprint mismatch')
            entries = [item for item in result.get('rebound_instructions', [])
                       if item['task_id'] == original['task_id'] and item['instruction_revision'] == original['instruction_revision']]
            if not entries:
                return False
            require(len(entries) == 1, 'original instruction recovery contains ambiguous projections')
            entry = entries[0]
            # Store 410's previous_* fields reference the immutable original
            # instruction, not a previous projection. Each actual recover
            # advances current dispatcher/team epochs by one. Check the whole
            # interval without inventing an intermediate binding record.
            require(entry['body_sha256'] == body_sha256 and
                    result['dispatcher_epoch'] == entry['dispatcher_epoch'] and
                    all(entry['previous_' + name] == original[name] and entry[name] == projected[name] + 1 for name in epochs),
                    'original instruction recovery does not prove the submitted epoch projection')
            projected = {name: entry[name] for name in epochs}
            recovery_witnesses[(order['attempt_id'], app['attempt_id'], recovery['seq'])] = {
                'order_attempt_id': order['attempt_id'], 'submission_attempt_id': app['attempt_id'],
                'recovery_event_id': recovery['event_id'], 'recovery_event_seq': recovery['seq'],
                'instruction_body_sha256': body_sha256, **projected}
        require(all(projected[name] == target[name] for name in epochs),
                'original instruction recovery does not prove the submitted epoch projection')
        return True

    def current(binding):
        return (binding['requirements_revision'] == request['requirements_revision'] and
                binding['plan_revision'] == request['plan_revision'])

    def current_producer(binding):
        task = tasks.get(binding['task_id'])
        return (current(binding) and task is not None and
                task['based_on_requirements_revision'] == binding['requirements_revision'] and
                all(task[key] == binding[key] for key in ('plan_revision', 'task_revision', 'instruction_revision')))

    def original_submission(submission_id):
        if submission_id not in originals:
            originals[submission_id] = one(db, 'SELECT * FROM workflow_submissions WHERE submission_id=? AND request_id=?',
                                          (submission_id, request['request_id']))
        return originals[submission_id]

    def produced(submission_id):
        app = submitted.get(submission_id)
        if app is None:
            return False
        original = original_submission(submission_id)
        binding, body, artifacts = (strict_json(original[key]) for key in ('original_binding', 'body', 'artifact_snapshot'))
        require(binding == app['binding'] and original['record_sha256'] == app['result']['record_sha256'] and
                original_digest({'body': body, 'original_binding': binding, 'artifact_snapshot': artifacts}) == original['record_sha256'],
                'submission fact differs from original model application')
        if not any(order_reaches_submission(order, app) for order in orders):
            return False
        return any(plan['plan_revision'] == binding['plan_revision'] and plan['ordinal'] < app['ordinal'] and
                   (any(item['task_id'] == binding['task_id'] for item in plan['result']['tasks']) or
                    any(resolution['plan_revision'] == plan['plan_revision'] and
                        plan['ordinal'] < resolution['ordinal'] < app['ordinal'] and
                        any(item['task_id'] == binding['task_id'] for item in resolution['result']['tasks'])
                        for resolution in by_name.get('resolve_amendment', []))) for plan in plans)

    def accepted_chain(submission_id):
        return (submission_id in accepted and submission_id in checked and produced(submission_id) and
                accepted[submission_id]['acceptance_status'] == 'ACCEPTED' and
                current_producer(submitted[submission_id]['binding']) and
                tasks[submitted[submission_id]['binding']['task_id']]['status'] == 'ACCEPTED' and
                accepted[submission_id]['result']['original_binding'] == submitted[submission_id]['binding'] and
                submitted[submission_id]['ordinal'] < checked[submission_id]['ordinal'] < accepted[submission_id]['ordinal'])

    checks = {'model_application_records_complete': not evidence['missing']}
    name = task['task_id']
    if name == 'tool_roundtrip':
        expected = hashlib.sha256(b'alpha\nbeta\n').hexdigest()
        progress, roundtrips = {}, {}
        # Keep one earliest witness for each exact original binding. A bounded
        # linear scan also avoids a cubic cross-product over repeated reads.
        for app in applications:
            key = canonical(app['binding'])
            state = progress.setdefault(key, {})
            if app['tool_name'] == 'fs_read':
                value = app['result']
                if value.get('offset') != 0 or value.get('eof') is not True:
                    continue
                content = base64.b64decode(value['content_base64'], validate=True)
                require(len(content) == value['bytes_returned'] == value['size_bytes'] and
                        hashlib.sha256(content).hexdigest() == value['sha256'], 'original diagnostic bytes differ from their fingerprint')
                if value['sha256'] == expected:
                    if value['path'] == 'seed.txt':
                        state.setdefault('seed', app['ordinal'])
                    elif value['path'] == 'result.txt' and 'write' in state:
                        roundtrips.setdefault(key, app['ordinal'])
            elif (app['tool_name'] == 'fs_write' and app['arguments'].get('path') == 'result.txt' and
                  hashlib.sha256(app['arguments']['content'].encode()).hexdigest() == expected and 'seed' in state):
                state.setdefault('write', app['ordinal'])
        checks['model_tool_roundtrip'] = bool(roundtrips)
        checks['model_submission_source'] = any(
            canonical(app['binding']) in roundtrips and current_producer(app['binding']) and
            submission_id in checked and produced(submission_id) and
            original_submission(submission_id)['status'] == 'CANDIDATE_READY' and
            roundtrips[canonical(app['binding'])] < app['ordinal'] < checked[submission_id]['ordinal'] and
            artifact_matches(original_submission(submission_id), task['checks']['artifact_sha256'], logical_paths=True)
            for submission_id, app in submitted.items())
    elif name == 'partial_amend':
        decisions = []
        for app in by_name.get('submit_amendment_plan', []) + by_name.get('resolve_amendment', []):
            if app['binding']['requirements_revision'] != request['requirements_revision'] or app['plan_revision'] != request['plan_revision']:
                continue
            changed = {item['node_id']: item for item in app['result']['tasks']}
            for node, decision in app['arguments']['dispositions'].items():
                target = changed[node]
                original = one(db, 'SELECT * FROM amend_dispositions WHERE request_id=? AND task_id=? AND based_on_requirements_revision=?',
                               (request['request_id'], target['task_id'], request['requirements_revision']))
                require(original['disposition'] == decision['disposition'] and original['rationale'] == decision['rationale'] and
                        original['target_plan_revision'] == app['plan_revision'] and original['target_task_revision'] == target['task_revision'],
                        'AMEND fact differs from original model decision')
                if any(accepted_chain(sid) and accepted[sid]['result']['task_id'] == target['task_id'] and
                       accepted[sid]['result']['original_binding']['requirements_revision'] == request['requirements_revision'] and
                       accepted[sid]['result']['original_binding']['plan_revision'] == request['plan_revision'] for sid in accepted):
                    decisions.append((target['task_id'], decision['disposition']))
        checks['model_amend_update_and_keep'] = any(a[1] == 'update' and b[1] == 'keep' and a[0] != b[0] for a in decisions for b in decisions)
    elif name == 'rejected_rework':
        checks['model_rejection_correction_acceptance'] = any(
            correction['submission_id'] in rejected and produced(correction['submission_id']) and
            current(rejected[correction['submission_id']]['result']['original_binding']) and
            current(correction['result']['target_binding']) and
            rejected[correction['submission_id']]['ordinal'] < correction['ordinal'] < accepted[sid]['ordinal'] and
            correction['result']['target_binding']['task_id'] == accepted[sid]['result']['task_id'] and
            correction['result']['target_binding']['task_revision'] == accepted[sid]['result']['original_binding']['task_revision'] and
            accepted_chain(sid)
            for correction in by_name.get('change_task', []) for sid in accepted)
    elif name == 'exhausted_rework':
        questions = rows(db, "SELECT q.question_id FROM questions q JOIN question_runtime r USING(question_id) WHERE q.request_id=? AND q.holder='user' AND r.status='OPEN' AND json_extract(r.original_binding,'$.requirements_revision')=? AND json_extract(r.original_binding,'$.plan_revision')=?",
                         (request['request_id'], request['requirements_revision'], request['plan_revision']))
        question_ids = {item['question_id'] for item in questions}
        maximum = strict_json(request['config_snapshot'])['system']['limits']['team_internal_rework_max']
        checks['model_exhaustion_escalation'] = any(
            app['result']['question_id'] in question_ids and current(app['binding']) and
            current(app['action']['target']['original_binding']) and app['action']['target']['rework_remaining'] == 0 and
            app['action']['target']['team_rework_count'] >= maximum and produced(app['submission_id']) and
            (app['submission_id'] in rejected or any(check['submission_id'] == app['submission_id'] and
                 not check['arguments']['passed'] for check in by_name.get('check_submission', [])))
            for app in by_name.get('escalate_task', []))
    elif name == 'delivery_receipt':
        deliveries = rows(db, "SELECT acceptance_snapshot FROM delivery_runtime WHERE request_id=? AND status='CONFIRMED' AND requirements_revision=? AND plan_revision=?",
                          (request['request_id'], request['requirements_revision'], request['plan_revision']))
        ids = {app['acceptance_id'] for sid, app in accepted.items() if accepted_chain(sid)}
        checks['model_accepted_delivery_source'] = any(
            bool(strict_json(delivery['acceptance_snapshot'])) and all(item['acceptance_id'] in ids for item in strict_json(delivery['acceptance_snapshot']))
            for delivery in deliveries)
    if allow_recovery_projection:
        witnesses = list(recovery_witnesses.values())
        evidence['recovery_projections'] = {'contract': 'original-instruction-epoch-rebind-v1',
                                          'witnesses': witnesses, 'witnesses_sha256': digest(witnesses)}
    return checks


def artifact_matches(submission, expected, *, logical_paths):
    found = {}
    for item in strict_json(submission['artifact_snapshot']):
        payload = base64.b64decode(item['content_base64'], validate=True)
        require(len(payload) == item['size_bytes'] and hashlib.sha256(payload).hexdigest() == item['sha256'],
                'stored submission byte mismatch')
        path = item['path'].removeprefix('/work/') if logical_paths else item['path']
        found[path] = item['sha256']
    return all(found.get(path) == value for path, value in expected.items())


def evidence_checks(db, request, task, applications=None):
    request_id = request['request_id']
    tasks = rows(db, 'SELECT * FROM tasks WHERE request_id=?', (request_id,))
    operations = rows(db, 'SELECT op_id,tool_id,status FROM ops WHERE request_id=?', (request_id,))
    results = {}
    for kind, target in task['checks'].items():
        if kind == 'successful_tools':
            observed = {x['tool_id'] for x in operations if x['status'] == 'SUCCEEDED'}
            if applications is not None:
                observed = {app['tool_name'] for app in applications}
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
            if applications is not None:
                model_submissions = {app['submission_id']: app for app in applications if app['tool_name'] == 'submit'}
                results[kind] = any(submission['submission_id'] in model_submissions and
                    strict_json(submission['original_binding']) == model_submissions[submission['submission_id']]['binding'] and
                    strict_json(submission['original_binding'])['requirements_revision'] == request['requirements_revision'] and
                    strict_json(submission['original_binding'])['plan_revision'] == request['plan_revision'] and
                    artifact_matches(submission, target, logical_paths=True) for submission in captured)
                continue
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


def comparable_live_settings(settings):
    # Provider parsing treats these capabilities as sets, then emits them in
    # canonical order. The frozen JSON may retain a different valid order.
    provider = settings['provider_profile']
    capabilities = provider['capabilities']
    return {**settings, 'provider_profile': {**provider, 'capabilities': {
        **capabilities, 'reasoning_modes': sorted(capabilities['reasoning_modes']),
        'reasoning_efforts': sorted(capabilities['reasoning_efforts'])}}}


def original_execution_kind(db, snapshot, config_hash, attempt, envelope, tables):
    profile = snapshot['system']['models'][attempt['model_ref']]
    require(attempt['config_snapshot_sha256'] == config_hash and envelope['config_snapshot_id'] == config_hash,
            'original attempt changed its frozen configuration')
    require(envelope['context']['adapter_version'] == profile['adapter_version'] and
            envelope['context']['protocol_version'] == profile['protocol'],
            'original attempt changed its frozen adapter or protocol')
    live = rows(db, 'SELECT * FROM live_attempt_execution WHERE attempt_id=?', (attempt['attempt_id'],)) \
        if 'live_attempt_execution' in tables else []
    if profile['adapter'] == 'fake':
        require(not live, 'original live execution conflicts with the frozen synthetic adapter')
        return 'synthetic_fake'
    require(profile['adapter'] == 'openai-responses' and len(live) == 1,
            'original live execution is missing or has an unsupported adapter')
    live = live[0]
    settings = strict_json(live['settings'])
    require(hashlib.sha256(live['settings'].encode()).hexdigest() == live['settings_sha256'],
            'original live execution settings fingerprint mismatch')
    original = profile['responses']
    kind = original['execution_kind']
    require(kind in ('local_tls', 'provider') and settings['execution_kind'] == kind and live['execution_kind'] == kind,
            'original live execution source differs from the frozen settings')
    require(digest(comparable_live_settings(settings)) == digest(comparable_live_settings(original)) and
            original_digest(settings['provider_profile']) == live['profile_sha256'],
            'original live execution settings differ from the frozen provider profile')
    logical_hash = original_logical_request_sha256(attempt['request_envelope'], envelope)
    require(envelope['logical_request_sha256'] == logical_hash == live['logical_request_sha256'],
            'transcript differs from the request admitted for original live transport')
    return kind


def inspect_run(plan, task, condition, run, base_directory):
    require(set(run) == {'task_id', 'condition', 'database', 'request_id', 'elapsed_ms', 'plan_sha256'}, 'unexpected run fields')
    require(run['plan_sha256'] == plan['plan_sha256'], 'run references another plan')
    require(type(run['elapsed_ms']) is int and 0 <= run['elapsed_ms'] <= 2**63 - 1, 'invalid host elapsed time')
    with database(base_directory / run['database']) as db:
        request = one(db, 'SELECT * FROM requests WHERE request_id=?', (run['request_id'],))
        original = one(db, 'SELECT text FROM request_texts WHERE request_id=? AND requirements_revision=1', (run['request_id'],))
        require(original['text'] == task['material'], 'original task materials differ')
        common = plan['common']
        require(common_lock(db, run['request_id'], common['model_ref'], common['reasoning'],
                            logical_agent_comparison=plan['schema_version'] == 3) == common,
                'model/environment/tools/budget conditions differ')
        snapshot = strict_json(request['config_snapshot'])
        config_hash = hashlib.sha256(request['config_snapshot'].encode()).hexdigest()
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
            kind = original_execution_kind(db, snapshot, config_hash, attempt, envelope, tables)
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
        identity_evidence = None
        if plan['schema_version'] == 3:
            identity_evidence = logical_agent_evidence(db, request, attempts, condition, plan['single_agent_policy'])
            condition_valid = bool(attempts) and identity_evidence['status'] == 'linked' and (
                condition == 'single_agent' or len(roles) > 1)
        applications = None
        decision_evidence = {'contract': 'structural-v1', 'status': 'not_checked',
                             'model_decision_origin_established': False}
        if plan['schema_version'] in (2, 3):
            applications, decision_evidence = model_applications(db, run['request_id'], attempts)
        checks = evidence_checks(db, request, task, applications)
        if applications is not None:
            origin_checks = model_origin_checks(db, request, task, applications, decision_evidence,
                                                allow_recovery_projection=plan['schema_version'] == 3)
            checks.update(origin_checks)
            decision_evidence['status'] = 'linked' if all(origin_checks.values()) else 'incomplete'
            decision_evidence['model_decision_origin_established'] = all(origin_checks.values())
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
                'decision_evidence': decision_evidence,
                **({'identity_evidence': identity_evidence} if identity_evidence is not None else {}),
                'pass_scope': 'model_application_and_structural_checks' if applications is not None else 'original_structural_checks_only',
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
    report = {'schema_version': plan['schema_version'], 'plan_sha256': plan['plan_sha256'], 'evidence_mode': plan['evidence_mode'],
              'rows': output, 'counts': dict(counts), 'expected_cells': len(TASKS) * 2,
              'observed_cells': len(indexed), 'paired_cells_complete': len(indexed) == len(TASKS) * 2,
              'general_quality_or_cost_advantage_established': False,
              'decision_evidence_contract': plan.get('decision_evidence', 'structural-v1'),
              'provider_quality_acceptance_established': False,
              'cache_definition': 'provider-reported nullable cache token dimensions; not inferred savings',
              'provenance': 'trusted local SQLite snapshot; fingerprints are consistency checks, not signatures'}
    if plan['schema_version'] == 3:
        report['identity_contract'] = plan['identity_contract']
        report['comparison_scope'] = 'shared_request_history_vs_role_isolated_history_with_original_authority_brokers'
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
    seed.add_argument('--decision-evidence', choices=('structural-v1', 'model-origin-v1'))
    seed.add_argument('--identity-contract', choices=(LOGICAL_AGENT_CONTRACT,))
    report = sub.add_parser('report', help='read all supplied evidence; retain every missing or invalid cell')
    report.add_argument('--plan', required=True)
    report.add_argument('--runs', required=True)
    args = parser.parse_args()
    try:
        if args.command == 'plan':
            with database(args.db) as db:
                result = make_plan(db, args.request, args.model_ref, strict_json(args.reasoning), args.evidence_mode,
                                   args.decision_evidence, args.identity_contract)
        else:
            result = build_report(read_json(args.plan), read_json(args.runs), Path(args.runs).absolute().parent)
        print(canonical(result).decode())
        return 0
    except (InvalidEvidence, sqlite3.Error, KeyError, TypeError, ValueError, OSError) as error:
        print(json.dumps({'error': 'invalid_evidence', 'message': str(error)[:256]}), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
