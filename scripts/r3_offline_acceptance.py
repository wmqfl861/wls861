#!/usr/bin/env python3
"""Versioned offline preparation and FakeModel execution for R3's ten v3 cells.

This module has no live execution command. The evaluator remains the authority
for original Store provenance, accounting and logical-Agent identity.
"""
from __future__ import annotations

import argparse
import copy
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import time

import evaluate_r3 as evaluation

SOURCE = Path(__file__).resolve().parent.parent
ASSETS = Path(__file__).resolve().parent / 'offline_acceptance/v1'
CONTRACT = 'r3-offline-acceptance-v1'
CELLS = [(task, condition) for task, _, _ in evaluation.TASKS
         for condition in evaluation.CONDITIONS]
MAX_FILE = 8 * 1024 * 1024
BUDGET = {'max_attempts': 64, 'input_limit': 1000000,
          'output_limit': 100000, 'cost_limit': 0}
SEMANTICS = {
    'tool_roundtrip': 'Read seed.txt, write and read back result.txt; the original submitted bytes must be exactly alpha LF beta LF. Inspect the original tool feedback and team check.',
    'partial_amend': 'Two independent tasks: apply the original amendment to alpha and explicitly keep beta. Inspect both current original submissions, checks and acceptances; omitted beta must stay paused before its disposition.',
    'rejected_rework': 'Reject a current candidate, correct that same task within its frozen allowance and accept a new submission under the original obligations. Inspect both original candidates and the correction.',
    'exhausted_rework': 'After the frozen shared allowance is exhausted, preserve the rejected work and open the exact owner question. An answer is not another allowance or acceptance.',
    'delivery_receipt': 'Publish the complete accepted original byte set and acknowledge the exact current package/hash/versions. Inspect the stored original source chain and actual delivered files.',
}


class PackageError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PackageError(message)


def encoded(value):
    return (json.dumps(value, ensure_ascii=False, sort_keys=True, indent=2,
                       allow_nan=False) + '\n').encode('utf-8')


def sha(data):
    return hashlib.sha256(data).hexdigest()


# Public error projection for this layer: reviewed PackageError constants map
# to fixed codes; everything else defers to the evaluator's fixed fallbacks.
PACKAGE_REASON_CODES = {
    'package directory required': 'package_directory_missing',
    'symlink input or output is forbidden': 'symlink_input_refused',
    'regular file required': 'nonregular_input_refused',
    'executable wls binary required': 'runner_binary_unavailable',
    'offline command refused': 'runner_command_refused',
    'offline command timed out; original logs retained': 'runner_timeout',
    'offline runtime boundary changed': 'runner_offline_boundary_changed',
    'offline package requires the original synthetic v3 plan': 'package_plan_invalid',
    'Fake acceptance requires an ordinary user': 'ordinary_user_required',
}


def public_error_code(error):
    """Fixed public code for a package error; never its raw text."""
    if isinstance(error, PackageError):
        return PACKAGE_REASON_CODES.get(str(error), 'invalid_evidence')
    return evaluation.public_reason_code(error)


def public_runner_reason(reason):
    """Finite whitelist for outcome reasons; unknown text is a fixed fallback."""
    if isinstance(reason, str):
        code = PACKAGE_REASON_CODES.get(reason) or evaluation.INVALID_EVIDENCE_REASON_CODES.get(reason)
        if code is not None:
            return code
    return 'runner_failed'


MAX_ERROR_MESSAGE_BYTES = 64 * 1024


class PrivateErrorCollector:
    """Retain full private error originals inside this run's 0700 directory.

    Files are created exclusively with 0600 through write_new. A message over
    the retention bound keeps its true length and hash with a truncated flag
    instead of a fabricated "complete" copy. A failing write raises to the
    caller, which records a fixed not-retained status without echoing paths.
    """

    def __init__(self, directory, limit=64):
        self.directory, self.limit, self.sequence = directory, limit, 0

    def __call__(self, capture):
        self.sequence += 1
        if self.sequence > self.limit:
            raise RuntimeError('private error retention limit reached')
        error = capture['error']
        message_bytes = str(error).encode('utf-8', 'replace')
        record = {'schema_version': 1,
                  'error_id': f'err-{self.sequence:03d}',
                  'captured_at': time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                  'task_id': capture.get('task_id'), 'condition': capture.get('condition'),
                  'context': capture.get('context'),
                  'error_type': type(error).__name__,
                  'errno': getattr(error, 'errno', None),
                  'message': message_bytes[:MAX_ERROR_MESSAGE_BYTES].decode('utf-8', 'replace'),
                  'message_truncated': len(message_bytes) > MAX_ERROR_MESSAGE_BYTES,
                  'original_message_length': len(message_bytes),
                  'original_message_sha256': sha(message_bytes)}
        data = encoded(record)
        write_new(self.directory / (record['error_id'] + '.json'), data)
        return {'private_error_id': record['error_id'], 'private_error_sha256': sha(data)}


RUNNER_STATUS_CODES = ('STARTED', 'COMPLETED', 'FAILED')


def project_runner_outcomes(outcomes):
    """Explicit safe projection of the private runner outcomes for reports.

    Only validated cells, statuses, fixed reason codes and safe hash
    references are projected. The private original file is never modified,
    its raw reason text is never trusted, and an unrecognized status can
    never be projected as COMPLETED.
    """
    require(isinstance(outcomes, list) and len(outcomes) <= len(CELLS), 'bounded runner outcomes required')
    tasks = {name for name, _, _ in evaluation.TASKS}
    projected = []
    for item in outcomes:
        require(isinstance(item, dict), 'runner outcome must be an object')
        task, condition = item.get('task_id'), item.get('condition')
        row = {'task_id': task if task in tasks else 'UNKNOWN',
               'condition': condition if condition in evaluation.CONDITIONS else 'UNKNOWN',
               'runner_status': (item.get('runner_status') if item.get('runner_status') in RUNNER_STATUS_CODES
                                 else 'UNRECOGNIZED')}
        if 'reason' in item:
            row['reason'] = public_runner_reason(item.get('reason'))
        for key in ('reason_detail_retained', 'reason_retention_failed'):
            if key in item:
                require(isinstance(item[key], bool), 'invalid runner outcome flag')
                row[key] = item[key]
        if item.get('private_error_id') is not None or item.get('private_error_sha256') is not None:
            require(re.fullmatch(r'err-\d{3}', str(item.get('private_error_id'))) is not None and
                    re.fullmatch(r'[a-f0-9]{64}', str(item.get('private_error_sha256'))) is not None,
                    'invalid private error reference')
            row['private_error_reference'] = {'error_id': item['private_error_id'],
                                              'error_sha256': item['private_error_sha256']}
        row['outcome_sha256'] = evaluation.digest(item)
        projected.append(row)
    return projected


def safe_path(path):
    """Reject symlinks in every component, including the package root."""
    path = Path(os.path.abspath(path))
    for item in (*reversed(path.parents), path):
        require(not item.is_symlink(), 'symlink input or output is forbidden')
    return path


def read_bytes(path):
    path = safe_path(path)
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, 'rb') as handle:
        info = os.fstat(handle.fileno())
        require(stat.S_ISREG(info.st_mode), 'regular file required')
        require(info.st_size <= MAX_FILE, 'file exceeds offline input bound')
        data = handle.read(MAX_FILE + 1)
        require(len(data) <= MAX_FILE, 'file grew beyond offline input bound')
        return data


def read_json(path):
    return evaluation.strict_json(read_bytes(path))


def mkdir(path):
    path = safe_path(path)
    path.mkdir(mode=0o700)
    return path


def write_new(path, data):
    path = safe_path(path)
    descriptor = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    with os.fdopen(descriptor, 'wb') as handle:
        handle.write(data)


def write_json(path, value):
    write_new(path, encoded(value))


def progress(path, value):
    """Replace only this run's index; command logs and original DB rows persist."""
    safe_path(path)
    temporary = Path(str(path) + '.next')
    write_json(temporary, value)
    os.replace(temporary, path)


def real_template():
    # This is a preparation record, NOT the existing runtime live authorization
    # format. No amount or credential value is prefilled.
    return {
        'contract': CONTRACT, 'execution_authorized': False, 'real_execution': 'NOT_RUN',
        'endpoint': dict.fromkeys(('provider_identity', 'base_url', 'path', 'model_id',
                                  'model_ref', 'adapter', 'adapter_version', 'protocol',
                                  'reasoning', 'verified_at', 'capability_source')),
        'capabilities': dict.fromkeys(('text', 'function_call', 'tool_history', 'streaming',
                                     'max_output_tokens', 'usage', 'cache')),
        'credential': dict.fromkeys(('host_reference', 'owner', 'permission_evidence')),
        'budget': dict.fromkeys(('per_request_input_tokens', 'per_request_output_tokens',
                                'per_request_cache_read_tokens', 'per_request_cache_write_tokens',
                                'per_request_cost_micro_usd', 'per_attempt_input_tokens',
                                'per_attempt_output_tokens', 'per_attempt_cache_read_tokens',
                                'per_attempt_cache_write_tokens', 'per_attempt_cost_micro_usd',
                                'max_calls', 'max_wall_ms', 'max_batches',
                                'max_paid_retries', 'cancellation_rule', 'remaining_allowance')),
        'billing': dict.fromkeys(('source_identity', 'verifier', 'trust_provenance',
                                 'original_wire_binding', 'units', 'unknown_dimension_rule')),
        'environment': dict.fromkeys(('platform', 'tools', 'directory_grants',
                                     'execution_policy', 'capability_evidence')),
        'approval': dict.fromkeys(('call_owner', 'cost_owner', 'scope', 'materials_sha256',
                                  'data_scope', 'approved_at', 'version',
                                  'request_id', 'config_sha256', 'profile_sha256',
                                  'coordinator_execution_card')),
    }


def check_real_template(card):
    """Check preparation completeness without resolving references or authority."""
    template = real_template()
    require(isinstance(card, dict) and set(card) == set(template), 'unknown real preparation fields')
    require(card['contract'] == CONTRACT and card['execution_authorized'] is False and
            card['real_execution'] == 'NOT_RUN', 'offline preparation cannot grant execution authority')
    missing = []
    for section, fields in template.items():
        if not isinstance(fields, dict):
            continue
        value = card[section]
        require(isinstance(value, dict) and set(value) == set(fields), 'unknown preparation section fields: ' + section)
        for key in fields:
            item = value[key]
            if item is None or item == '' or item == [] or item == {}:
                missing.append(section + '.' + key)
            elif section == 'budget' and key not in ('cancellation_rule', 'remaining_allowance'):
                require(type(item) is int and 0 <= item <= 2**63 - 1, 'budget fields require bounded integers')
                require(key not in ('max_calls', 'max_wall_ms', 'max_batches') or item > 0,
                        'execution limits must be positive')
                require(key != 'max_paid_retries' or item == 0, 'automatic paid retries are not authorized')
            else:
                require(isinstance(item, str) and 0 < len(item) <= 4096,
                        'preparation evidence requires a bounded text reference')
    endpoint = card['endpoint']
    if endpoint['base_url'] is not None:
        from urllib.parse import urlsplit
        url = urlsplit(endpoint['base_url'])
        require(url.scheme == 'https' and url.hostname and not url.username and not url.password
                and not url.query and not url.fragment, 'endpoint requires an exact HTTPS URL without embedded credentials')
    if endpoint['path'] is not None:
        require(endpoint['path'].startswith('/') and '?' not in endpoint['path'] and '#' not in endpoint['path'],
                'endpoint path must be explicit and omit query credentials')
    reference = card['credential']['host_reference']
    if reference is not None:
        require(re.fullmatch(r'env:[A-Za-z_][A-Za-z0-9_]*|file:/[^\x00\r\n]+', reference),
                'credential must be a host env:NAME or file:/absolute/path reference')
    for field in ('materials_sha256', 'config_sha256', 'profile_sha256'):
        value = card['approval'][field]
        if value is not None:
            require(re.fullmatch('[a-f0-9]{64}', value), 'approval requires original SHA-256 identities')
    return {'contract': CONTRACT, 'required_fields_complete': not missing,
            'missing_fields': missing, 'independent_verification': 'NOT_RUN',
            'execution_authorized': False, 'credential_resolution': 'NOT_RUN',
            'real_execution': 'NOT_RUN', 'provider_quality': 'NOT_RUN', 'actual_provider_cost': 'NOT_RUN'}


def human_review(plan_hash=None):
    return {
        'contract': CONTRACT, 'plan_sha256': plan_hash, 'review_status': 'NOT_RUN',
        'instructions': 'Inspect original evidence privately; record independent semantic judgments. Synthetic structure does not establish provider quality, cost advantage or coordinator acceptance.',
        'rows': [{'task_id': task, 'condition': condition,
                  'semantic_criterion': SEMANTICS[task], 'semantic_result': 'NOT_RUN',
                  'reviewer': None, 'reviewed_at': None, 'original_evidence_references': [],
                  'notes': None} for task, condition in CELLS],
        'confounders': ['fixed multi-then-single order', 'one repetition per cell',
                        'FakeModel scripted decisions and synthetic usage',
                        'host scheduling and cold/warm filesystem cache'],
        'provider_quality': 'NOT_RUN', 'actual_provider_cost': 'NOT_RUN',
        'coordinator_acceptance': 'PENDING',
    }


def human_review_markdown():
    lines = ['# Offline acceptance human review', '',
             'Status: NOT_RUN. Record reviewer, time and original evidence references for each judgment.',
             'A synthetic structural pass does not establish provider quality, cost advantage or coordinator acceptance.', '',
             '| Task | Condition | Semantic result | Reviewer / time | Original evidence references and notes |',
             '| --- | --- | --- | --- | --- |']
    lines.extend(f'| {task} | {condition} | NOT_RUN | | |' for task, condition in CELLS)
    lines += ['', 'Use passed / failed / invalid_evidence / not_executed for a completed independent judgment; retain every row.', '']
    lines.extend(f'- **{task}**: {SEMANTICS[task]}' for task, _, _ in evaluation.TASKS)
    lines += ['', 'Record fixed execution order, scheduling and cache temperature as confounders.',
              'Real provider quality and actual provider cost: NOT_RUN. Coordinator acceptance: PENDING.', '']
    return '\n'.join(lines).encode('utf-8')


def package_files():
    base = read_bytes(ASSETS / 'system.yaml').decode('utf-8')
    marker = 'context:\n'
    require(base.count(marker) == 1, 'unexpected versioned configuration context')
    single = base.replace(marker, marker + '  logical_agent:\n'
                          '    mode: single-agent-role-broker-v1\n'
                          '    max_attempts: 128\n    max_history_bytes: 524288\n', 1)
    files = {'config/multi_agent.yaml': base.encode(), 'config/single_agent.yaml': single.encode(),
             'functions/general/function.yaml': read_bytes(ASSETS / 'function.yaml'),
             'functions/general/env.yaml': read_bytes(ASSETS / 'env.yaml'),
             'fixtures/model-arguments.json': read_bytes(ASSETS / 'model-arguments.json'),
             'materials/seed.txt': b'alpha\nbeta\n',
             'materials/amendment.txt': b'Change alpha to produce an amended result. Keep the independent beta requirement unchanged.',
             'materials/constraints.json': b'[]\n',
             'real-execution-template.json': encoded(real_template()),
             'human-review-template.json': encoded(human_review()),
             'human-review-template.md': human_review_markdown(),
             'run-order.json': encoded([{'task_id': task, 'condition': condition} for task, condition in CELLS]),
             'protocol.json': encoded({'contract': CONTRACT, 'evaluation': 'r3-small-tasks-v3',
                 'identity_contract': evaluation.LOGICAL_AGENT_CONTRACT,
                 'repetitions': 1, 'evidence_mode': 'synthetic', 'budget': BUDGET,
                 'max_wall_ms': 300000, 'max_paid_retries': 0, 'team_internal_rework_max': 1,
                 'semantic_criteria': SEMANTICS,
                 'failure_rule': 'Keep every failed, invalid and not_executed cell; never replace an original attempt.',
                 'public_report_rule': 'Only IDs, hashes, states and bounded metrics; raw fixtures and databases remain private.'})}
    for role in ('dispatcher', 'team_lead', 'worker'):
        files[f'functions/general/prompts/{role}.md'] = read_bytes(ASSETS / (role + '.md'))
    for task, material, _ in evaluation.TASKS:
        files['materials/' + task + '.txt'] = material.encode('utf-8')
    return files


def prepare(directory):
    files = package_files()
    directory = mkdir(directory)
    for name, data in sorted(files.items()):
        target = directory / name
        for parent in reversed(target.parents):
            if parent != directory and directory in parent.parents and not parent.exists():
                mkdir(parent)
        write_new(target, data)
    manifest = {'contract': CONTRACT, 'files': {name: sha(data) for name, data in sorted(files.items())}}
    manifest['package_sha256'] = evaluation.digest(manifest)
    write_json(directory / 'manifest.json', manifest)
    return dry_run(directory)


def validate_package(directory):
    directory = safe_path(directory)
    require(directory.is_dir(), 'package directory required')
    manifest = read_json(directory / 'manifest.json')
    expected = package_files()
    require(set(manifest) == {'contract', 'files', 'package_sha256'} and manifest['contract'] == CONTRACT,
            'unknown package manifest')
    require(manifest['package_sha256'] == evaluation.digest({key: value for key, value in manifest.items()
                                                            if key != 'package_sha256'}), 'package fingerprint mismatch')
    require(manifest['files'] == {name: sha(data) for name, data in sorted(expected.items())},
            'package differs from its versioned offline materials or configuration')
    for name, data in expected.items():
        require(read_bytes(directory / name) == data, 'changed package member: ' + name)
    return directory, manifest


def dry_run(directory):
    _, manifest = validate_package(directory)
    return {'contract': CONTRACT, 'package_sha256': manifest['package_sha256'],
            'mode': 'dry-run', 'execution_authorized': False,
            'store_opens': 0, 'reservations': 0, 'credential_resolutions': 0,
            'external_calls': 0, 'send_authority_granted': False,
            'counts': {'not_executed': len(CELLS)},
            'rows': [{'task_id': task, 'condition': condition, 'status': 'not_executed'}
                     for task, condition in CELLS],
            'real_execution': 'NOT_RUN', 'provider_quality': 'NOT_RUN', 'actual_provider_cost': 'NOT_RUN'}


def fake_step(role, name, arguments, index=1):
    return {'role': role, 'role_call_index': index, 'status': 'completed',
            'tool_calls': [{'call_id': f'offline-{role}-{name}-{index}', 'name': name,
                            'arguments': copy.deepcopy(arguments)}],
            'usage': {'input_tokens': 100, 'output_tokens': 20, 'cache_read_tokens': 0,
                      'cache_write_tokens': 0, 'cost_micro_usd': 0}}


def fake_phases(directory, task):
    """Fixed synthetic decisions, never imported from the test harness."""
    args = read_json(directory / 'fixtures/model-arguments.json')
    plan = copy.deepcopy(args['submit_plan'])
    if task == 'partial_amend':
        beta = copy.deepcopy(plan['nodes'][0])
        beta.update(node_id='beta', title='Independent beta')
        plan['nodes'].append(beta)

    def workers(node, version=1, *, roundtrip=False, order=True, experience=True, prior=None):
        steps = [fake_step('team_lead', 'issue_work_order', args['issue_work_order'])] if order else []
        if roundtrip:
            steps += [fake_step('worker', 'fs_read', {'path': 'seed.txt'}),
                      fake_step('worker', 'fs_write', {'path': 'result.txt', 'content': 'alpha\nbeta\n'}, 2),
                      fake_step('worker', 'fs_read', {'path': 'result.txt'}, 3)]
            submit_index = 4
        else:
            content = f'{node} candidate version {version}\n'
            write = {'path': 'result.txt', 'content': content}
            if prior is not None:
                write['expected_sha256'] = sha(prior.encode())
            steps += [fake_step('worker', 'fs_write', write)]
            submit_index = 2
        steps += [fake_step('worker', 'submit', args['submit'], submit_index),
                  fake_step('team_lead', 'check_submission', args['check_submission'], 2 if order else 1)]
        if experience:
            steps += [fake_step('team_lead', 'report_experience', args['report_experience'], 3 if order else 2)]
        return steps

    accept = args['accept_candidate']
    reject = {'passed': False, 'notes': 'Synthetic rejection of the original candidate; keep its complete evidence.'}
    initial = {'default': [fake_step('dispatcher', 'submit_plan', plan),
                           fake_step('dispatcher', 'accept_candidate', reject if task in ('rejected_rework', 'exhausted_rework') else accept, 2)],
               'nodes': {'alpha': workers('alpha', roundtrip=task == 'tool_roundtrip',
                                           experience=task not in ('rejected_rework', 'exhausted_rework'))}}
    if task == 'partial_amend':
        # Only planning and original work orders, then a real host AMEND. No
        # first-version work is accepted on behalf of a model.
        initial = {'default': [fake_step('dispatcher', 'submit_plan', plan)],
                   'nodes': {node: [fake_step('team_lead', 'issue_work_order', args['issue_work_order'])]
                             for node in ('alpha', 'beta')}}
        update = {'default': [fake_step('dispatcher', 'submit_amendment_plan', {
            'plan': plan, 'dispositions': {'alpha': {'disposition': 'update', 'rationale': 'Apply the original AMEND to alpha.'}}})],
                  'nodes': {'alpha': workers('alpha', 2), 'beta': []}}
        keep = {'default': [fake_step('dispatcher', 'resolve_amendment', {'dispositions': {
            'beta': {'disposition': 'keep', 'rationale': 'The original AMEND explicitly keeps beta unchanged.'}}})],
                'nodes': {'alpha': [], 'beta': workers('beta')}}
        # Pending amendment decisions have priority over candidate acceptance.
        # Finish both dispositions before starting the acceptance-only batch.
        acceptances = {'default': [fake_step('dispatcher', 'accept_candidate', accept, index)
                                   for index in (1, 2)],
                       'nodes': {node: [fake_step('team_lead', 'report_experience', args['report_experience'])]
                                 for node in ('alpha', 'beta')}}
        return [('initial', initial), ('amend', None), ('update', update), ('keep', keep), ('acceptances', acceptances)]
    if task not in ('rejected_rework', 'exhausted_rework'):
        return [('initial', initial)]
    correction = {'default': [fake_step('dispatcher', 'change_task', {
        'rationale': 'Correct the same original rejected task.',
        'correction': 'Produce a corrected result under all original constraints.'}),
        fake_step('dispatcher', 'accept_candidate', reject if task == 'exhausted_rework' else accept, 2)],
        'nodes': {'alpha': workers('alpha', 2, experience=task != 'exhausted_rework', prior='alpha candidate version 1\n')}}
    phases = [('initial', initial), ('correction', correction)]
    if task == 'exhausted_rework':
        phases.append(('escalation', {'default': [fake_step('dispatcher', 'escalate_task', {
            'text': 'How should the original rejected task proceed after its frozen allowance is exhausted?',
            'context': 'No implicit acceptance or additional allowance.'})]}))
    return phases


class FakeRunner:
    def __init__(self, binary, directory):
        self.binary = safe_path(binary)
        require(self.binary.is_file() and os.access(self.binary, os.X_OK), 'executable wls binary required')
        self.directory = directory
        self.logs = mkdir(directory / 'commands')
        self.sequence = 0

    def invoke(self, *args):
        # The only process entry point is private to run-fake. A closed command
        # list and byte-verified Fake config prevent a live path through options.
        import subprocess
        require(args[0] in {'submit', 'pump-fake', 'pump-single-fake', 'amend',
                            'publish-delivery', 'ack-delivery'}, 'offline command refused')
        command = [str(self.binary), *(str(item) for item in args), '--source-root', str(SOURCE)]
        self.sequence += 1
        prefix = self.logs / f'{self.sequence:03d}'
        write_json(str(prefix) + '.command.json', command)
        start = time.monotonic()
        try:
            result = subprocess.run(command, cwd=self.directory, capture_output=True, timeout=60, check=False)
        except subprocess.TimeoutExpired as error:
            write_new(str(prefix) + '.stdout', error.stdout or b'')
            write_new(str(prefix) + '.stderr', error.stderr or b'')
            write_json(str(prefix) + '.exit.json', {'exit_code': None, 'status': 'TIMEOUT',
                                                   'elapsed_ms': int((time.monotonic() - start) * 1000)})
            raise PackageError('offline command timed out; original logs retained') from error
        write_new(str(prefix) + '.stdout', result.stdout)
        write_new(str(prefix) + '.stderr', result.stderr)
        write_json(str(prefix) + '.exit.json', {'exit_code': result.returncode,
                                               'elapsed_ms': int((time.monotonic() - start) * 1000)})
        require(result.returncode == 0, f'offline command failed with exit {result.returncode}; inspect private command logs')
        return evaluation.strict_json(result.stdout)

    def admit(self, package, cell, task, condition):
        return self.invoke('submit', '--db', cell / 'store.sqlite', '--key', task + '-' + condition,
            '--text-file', package / 'materials' / (task + '.txt'),
            '--config', package / 'config' / (condition + '.yaml'), '--functions-dir', package / 'functions',
            '--max-attempts', BUDGET['max_attempts'], '--input-budget', BUDGET['input_limit'],
            '--output-budget', BUDGET['output_limit'], '--cost-budget-micro-usd', BUDGET['cost_limit'])['request']['request_id']

    def execute(self, package, cell, task, condition, request):
        workspaces = {node: str(mkdir(cell / node)) for node in ('alpha', 'beta')}
        write_json(cell / 'workspaces.json', workspaces)
        write_new(Path(workspaces['alpha']) / 'seed.txt', read_bytes(package / 'materials/seed.txt'))
        db = cell / 'store.sqlite'
        for name, fixture in fake_phases(package, task):
            if name == 'amend':
                self.invoke('amend', '--db', db, '--request', request,
                    '--text-file', package / 'materials/amendment.txt',
                    '--constraints-file', package / 'materials/constraints.json')
                continue
            write_json(cell / (name + '.json'), fixture)
            result = self.invoke('pump-single-fake' if condition == 'single_agent' else 'pump-fake',
                '--db', db, '--request', request, '--script', cell / (name + '.json'),
                '--workspaces-file', cell / 'workspaces.json', '--max-calls', 16, '--worker-calls', 4)
            require(result['real_model_calls'] == 0 and result['environment_checks_executed'] is False,
                    'offline runtime boundary changed')
            write_json(cell / (name + '.result.json'), result)
        if task == 'delivery_receipt':
            delivery_root = mkdir(cell / 'deliveries')
            published = self.invoke('publish-delivery', '--db', db, '--request', request,
                                    '--delivery-dir', delivery_root)
            delivery = published['delivery']
            require(published['publication']['verified'] and published['publication']['durable'] and
                    not published['receipt_created'] and delivery['status'] == 'STORED', 'delivery publication incomplete')
            self.invoke('ack-delivery', '--db', db, '--request', request,
                        '--delivery', delivery['delivery_id'], '--delivery-dir', delivery_root,
                        '--manifest-sha256', delivery['manifest_sha256'],
                        '--requirements-revision', delivery['requirements_revision'],
                        '--plan-revision', delivery['plan_revision'], '--recipient', 'local-user',
                        '--method', 'durable_local', '--key', 'offline-receipt-v1')


def run_fake(directory, binary, *, private_error_sink=None):
    package, manifest = validate_package(directory)
    require(os.geteuid() != 0, 'Fake acceptance requires an ordinary user')
    run = mkdir(package / 'run')  # A run cannot replace an earlier failed sample.
    collector = private_error_sink if private_error_sink is not None \
        else PrivateErrorCollector(mkdir(run / 'errors'))
    runner = FakeRunner(binary, run)
    seed = mkdir(run / 'plan-seed')
    request = runner.admit(package, seed, evaluation.TASKS[0][0], 'multi_agent')
    with evaluation.database(seed / 'store.sqlite') as db:
        plan = evaluation.make_plan(db, request, identity_contract=evaluation.LOGICAL_AGENT_CONTRACT)
    write_json(run / 'plan.json', plan)  # Before every benchmark attempt.
    runs, outcomes = [], []
    write_json(run / 'runs.json', runs)
    write_json(run / 'outcomes.json', outcomes)
    write_json(run / 'identity.json', {'contract': CONTRACT, 'package_sha256': manifest['package_sha256'],
        'binary_sha256': file_hash(runner.binary), 'entrypoint_sha256': file_hash(Path(__file__)),
        'evaluator_sha256': file_hash(SOURCE / 'scripts/evaluate_r3.py'), 'uid': os.geteuid()})
    for task, condition in CELLS:
        cell = mkdir(run / (task + '-' + condition))
        started = time.monotonic()
        outcome = {'task_id': task, 'condition': condition, 'runner_status': 'STARTED'}
        try:
            request = runner.admit(package, cell, task, condition)
            entry = {'task_id': task, 'condition': condition,
                     'database': str((cell / 'store.sqlite').relative_to(run)), 'request_id': request,
                     'elapsed_ms': 0, 'plan_sha256': plan['plan_sha256']}
            runs.append(entry)
            progress(run / 'runs.json', runs)
            runner.execute(package, cell, task, condition, request)
            outcome['runner_status'] = 'COMPLETED'
        except (PackageError, OSError, ValueError) as error:
            # The private outcome keeps its bounded original reason plus a
            # reference to the retained full error original; the public
            # report later projects only fixed reason codes.
            outcome.update(runner_status='FAILED', reason=str(error)[:256],
                           reason_detail_retained=False)
            try:
                retained = collector({'task_id': task, 'condition': condition, 'error': error,
                                      'context': {'stage': 'fake-cell', 'command_sequence': runner.sequence}})
            except BaseException:
                outcome['reason_retention_failed'] = True
            else:
                if isinstance(retained, dict):
                    outcome.update({key: retained[key] for key in
                                    ('private_error_id', 'private_error_sha256') if key in retained})
                    outcome['reason_detail_retained'] = True
        finally:
            if runs and runs[-1]['task_id'] == task and runs[-1]['condition'] == condition:
                runs[-1]['elapsed_ms'] = int((time.monotonic() - started) * 1000)
                progress(run / 'runs.json', runs)
            outcomes.append(outcome)
            progress(run / 'outcomes.json', outcomes)
    result = report(package, private_error_sink=collector)
    write_json(run / 'report.json', result)
    write_json(run / 'human-review.json', human_review(plan['plan_sha256']))
    write_json(run / 'evidence-sha256.json', {str(path.relative_to(run)): file_hash(path)
        for path in sorted(run.rglob('*')) if path.is_file()})
    return result


def file_hash(path):
    path = safe_path(path)
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    digest = hashlib.sha256()
    with os.fdopen(descriptor, 'rb') as handle:
        require(stat.S_ISREG(os.fstat(handle.fileno()).st_mode), 'regular identity input required')
        while block := handle.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


def report(directory, *, private_error_sink=None):
    package, manifest = validate_package(directory)
    run = safe_path(package / 'run')
    plan, index = read_json(run / 'plan.json'), read_json(run / 'runs.json')
    require(plan.get('schema_version') == 3 and plan.get('evidence_mode') == 'synthetic',
            'offline package requires the original synthetic v3 plan')
    result = evaluation.build_report(plan, index, run, private_error_sink=private_error_sink)
    return {'contract': CONTRACT, 'package_sha256': manifest['package_sha256'], 'evaluation': result,
            'runner_outcomes': project_runner_outcomes(read_json(run / 'outcomes.json')),
            'real_execution': 'NOT_RUN', 'provider_quality': 'NOT_RUN', 'actual_provider_cost': 'NOT_RUN',
            'semantic_review': 'NOT_RUN', 'coordinator_acceptance': 'PENDING'}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest='command', required=True)
    for command in ('prepare', 'dry-run', 'run-fake', 'report'):
        child = commands.add_parser(command)
        child.add_argument('--package', required=True, type=Path)
        if command == 'run-fake':
            child.add_argument('--wls', required=True, type=Path)
    child = commands.add_parser('check-real-template')
    child.add_argument('--card', required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        if args.command == 'check-real-template':
            result = check_real_template(read_json(args.card))
        elif args.command == 'run-fake':
            result = run_fake(args.package, args.wls)
        else:
            result = {'prepare': prepare, 'dry-run': dry_run, 'report': report}[args.command](args.package)
        sys.stdout.buffer.write(encoded(result))
        if args.command == 'run-fake':
            return 0 if result['evaluation']['counts'] == {'passed': 10} and all(
                item['runner_status'] == 'COMPLETED' for item in result['runner_outcomes']) else 1
        if args.command == 'check-real-template':
            return 0 if result['required_fields_complete'] else 2
        return 0
    except (PackageError, evaluation.InvalidEvidence, OSError, KeyError, ValueError) as error:
        print(json.dumps({'error': public_error_code(error)}), file=sys.stderr)
        return 2


if __name__ == '__main__':
    raise SystemExit(main())
