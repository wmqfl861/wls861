#!/usr/bin/env python3
"""Compare original same-job image sources without selecting a preferred value.

Offline only: read saved API/log/archive captures; never query GitHub, execute
artifact code, update old evidence or change a prior audit's result. A mismatch
remains UNRESOLVED (exit 1). Invalid source binding exits 2. Matching observations
exit 0 but are not cryptographic host/image attestation.
"""
import argparse
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import re
import stat
import zipfile


SCHEMA = 'wls-windows-image-source-comparison-v1'
ENVIRONMENT_MEMBER = 'native-image-environment.json'
STAMP = r'\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d(?:\.\d+)?Z'
MACHINE_ENVIRONMENT_KEY = r'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'


def require(condition, message):
    if not condition:
        raise ValueError(message)


def timestamp(value):
    result = datetime.datetime.fromisoformat(value.replace('Z', '+00:00'))
    require(result.tzinfo is not None, 'capture time lacks an explicit timezone')
    return result


def windows_identity(*, path=None, fd=None):
    """Comparable native observations; Python path/fd ctime sources can differ."""
    import ctypes
    from ctypes import wintypes
    import msvcrt

    class BasicInfo(ctypes.Structure):
        _fields_ = [('CreationTime', ctypes.c_int64), ('LastAccessTime', ctypes.c_int64),
                    ('LastWriteTime', ctypes.c_int64), ('ChangeTime', ctypes.c_int64),
                    ('FileAttributes', wintypes.DWORD)]

    class IdentityInfo(ctypes.Structure):
        _fields_ = [('VolumeSerialNumber', ctypes.c_uint64), ('FileId', ctypes.c_ubyte * 16)]

    class StandardInfo(ctypes.Structure):
        _fields_ = [('AllocationSize', ctypes.c_int64), ('EndOfFile', ctypes.c_int64),
                    ('NumberOfLinks', wintypes.DWORD), ('DeletePending', ctypes.c_ubyte),
                    ('Directory', ctypes.c_ubyte)]

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, wintypes.LPVOID,
                                  wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
    kernel.CreateFileW.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.GetFileType.argtypes = [wintypes.HANDLE]
    kernel.GetFileType.restype = wintypes.DWORD
    kernel.GetFileInformationByHandleEx.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
    kernel.GetFileInformationByHandleEx.restype = wintypes.BOOL
    require((path is None) != (fd is None), 'exactly one native identity source is required')
    owned = fd is None
    if owned:
        # Metadata only; observe the path object itself, including reparse points.
        # Share writes/deletes so a concurrent change is observed and rejected,
        # rather than turning this offline comparison into a writer lease.
        handle = kernel.CreateFileW(str(path), 0x80, 7, None, 3, 0x00200000 | 0x02000000, None)
        if handle == ctypes.c_void_p(-1).value:
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        handle = msvcrt.get_osfhandle(fd)
    try:
        require(kernel.GetFileType(handle) == 1, 'native evidence input is not a disk file')
        basic, identity, standard = BasicInfo(), IdentityInfo(), StandardInfo()
        for information_class, value in ((0, basic), (18, identity), (1, standard)):
            if not kernel.GetFileInformationByHandleEx(handle, information_class, ctypes.byref(value), ctypes.sizeof(value)):
                raise ctypes.WinError(ctypes.get_last_error())
        require(not basic.FileAttributes & (0x10 | 0x400) and not standard.Directory and
                not standard.DeletePending and standard.NumberOfLinks == 1,
                'native evidence input must be a regular single-link non-reparse file')
        # LastAccessTime is intentionally excluded: the read itself can update it.
        # ChangeTime is compared across path and descriptor observations, including
        # the pre-open window in which size and LastWriteTime can be restored.
        return (identity.VolumeSerialNumber, bytes(identity.FileId), standard.EndOfFile,
                basic.CreationTime, basic.LastWriteTime, basic.ChangeTime,
                basic.FileAttributes, standard.NumberOfLinks)
    finally:
        if owned:
            kernel.CloseHandle(handle)


def read_file(path, maximum=32 * 1024 * 1024):
    path = Path(path)
    before = path.lstat()
    require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and before.st_size <= maximum,
            'evidence input must be a bounded regular single-link file')
    native_before = None
    if os.name == 'nt':
        native_before = windows_identity(path=path)
        # The initial lstat is a type-before-open preflight. Start the bound
        # Python observation after native ChangeTime/ID have been captured.
        before = path.lstat()
        require(stat.S_ISREG(before.st_mode) and before.st_nlink == 1 and before.st_size <= maximum,
                'evidence input must be a bounded regular single-link file')
    flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
    comparable = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns,
                                value.st_mode, value.st_nlink)
    with os.fdopen(os.open(path, flags), 'rb') as stream:
        opened = os.fstat(stream.fileno())
        require(stat.S_ISREG(opened.st_mode) and opened.st_nlink == 1 and opened.st_size <= maximum,
                'opened evidence input is not a bounded regular file')
        native_opened = windows_identity(fd=stream.fileno()) if native_before is not None else None
        if native_before is not None:
            require(native_before == native_opened and comparable(before) == comparable(opened),
                    'evidence input changed while reading')
        raw = stream.read(maximum + 1)
        after = os.fstat(stream.fileno())
        native_after = windows_identity(fd=stream.fileno()) if native_before is not None else None
    final = path.lstat()
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size, value.st_mtime_ns,
                              value.st_ctime_ns, value.st_mode, value.st_nlink)
    stable = identity(before) == identity(opened) == identity(after) == identity(final)
    if native_before is not None:
        stable = (identity(before) == identity(final) and identity(opened) == identity(after) and
                  comparable(before) == comparable(opened) and
                  native_before == native_opened == native_after == windows_identity(path=path))
    require(stable and len(raw) == before.st_size, 'evidence input changed while reading')
    return raw, dict(path=str(path.absolute()), bytes=len(raw), sha256=hashlib.sha256(raw).hexdigest())


def captured(reference, endpoint):
    raw, file_identity = read_file(reference['data'])
    receipt_raw, receipt_identity = read_file(reference['receipt'], 1024 * 1024)
    receipt = json.loads(receipt_raw)
    require(receipt.get('http_method') == 'GET' and receipt.get('remote_mutation') is False and
            receipt.get('exit_code') == 0 and receipt.get('timed_out') is False and
            receipt.get('launch_error_type') is None, 'capture was not a completed read-only GET')
    argv = receipt.get('argv', [])
    require(argv[:2] == ['gh', 'api'] and argv[-1] == endpoint, 'capture endpoint does not match its evidence role')
    saved = receipt.get('files', {}).get(Path(reference['data']).name)
    require(saved == {key: file_identity[key] for key in ('bytes', 'sha256')}, 'capture receipt hash or size differs')
    started, ended = receipt['started_at_utc'], receipt['ended_at_utc']
    require(timestamp(started) <= timestamp(ended), 'capture receipt time order is invalid')
    return raw, dict(**file_identity, capture_started_at_utc=started, capture_ended_at_utc=ended,
                    source_endpoint=endpoint, capture_receipt=receipt_identity,
                    trust='saved GitHub API GET and local capture receipt; not independent signed host attestation')


def header_observation(text):
    lines = text.splitlines()
    groups, active = [], None
    for line_number, line in enumerate(lines, 1):
        match = re.fullmatch(r'(' + STAMP + r') (.*)', line.lstrip('\ufeff'))
        if not match:
            continue
        observed_at, body = match.groups()
        if body == '##[group]Runner Image':
            require(active is None, 'nested Runner Image header')
            active = dict(start_line=line_number, emitted_at_utc=observed_at, fields={}, original_lines=[line])
        elif active is not None:
            active['original_lines'].append(line)
            if body == '##[endgroup]':
                active['end_line'] = line_number
                groups.append(active)
                active = None
            elif ': ' in body:
                key, value = body.split(': ', 1)
                require(key not in active['fields'], 'duplicate Runner Image header field')
                active['fields'][key] = value
    require(active is None and len(groups) == 1, 'exactly one complete Runner Image group is required')
    result = groups[0]
    require(result['fields'].get('Image') and result['fields'].get('Version'), 'Runner Image fields are incomplete')
    return result


def machine_observation(environment):
    """Validate an optional new capture without inventing it for historical jobs."""
    observed = environment.get('machine_image_environment')
    if observed is None:
        return dict(status='NOT_COLLECTED', values=None, field_comparison=None,
                    causal_explanation=None, trust='historical capture has no machine registry observation')
    require(isinstance(observed, dict) and set(observed) == {'schema', 'captured_at_utc', 'source', 'key', 'view',
                                                          'values', 'field_status', 'status', 'reason'} and
            observed.get('schema') == 'wls-native-machine-image-environment-v1' and
            observed.get('source') == 'read-only Windows Registry HKEY_LOCAL_MACHINE' and
            observed.get('key') == MACHINE_ENVIRONMENT_KEY and observed.get('view') == '64-bit',
            'machine image source schema or explicit registry scope differs')
    require(observed.get('reason') in {None, 'NATIVE_WINDOWS_REQUIRED', 'REGISTRY_API_UNAVAILABLE',
            'MACHINE_ENVIRONMENT_KEY_MISSING', 'MACHINE_ENVIRONMENT_KEY_DENIED', 'MACHINE_ENVIRONMENT_KEY_UNAVAILABLE'},
            'unknown machine image observation reason')
    require(timestamp(observed['captured_at_utc']) >= timestamp(environment['captured_at_utc']),
            'machine image observation predates its containing capture')
    names = {'ImageOS', 'ImageVersion'}
    values, states = observed.get('values', {}), observed.get('field_status', {})
    require(set(values) == names and set(states) == names, 'machine image observation is not exactly allowlisted')
    supported = {'NOT_CHECKED', 'MISSING', 'DENIED', 'QUERY_FAILED', 'UNSUPPORTED_VALUE', 'OBSERVED'}
    for name in names:
        require(states[name] in supported, 'unknown machine image observation state')
        if states[name] == 'OBSERVED':
            require(isinstance(values[name], str) and 0 < len(values[name]) <= 256 and
                    all(ord(item) >= 32 and ord(item) != 127 for item in values[name]),
                    'machine image observation claims missing or unsupported bytes')
        else:
            require(values[name] is None, 'unobserved machine image field cannot carry a value')
    count = sum(value == 'OBSERVED' for value in states.values())
    status = 'OBSERVED' if count == 2 else 'PARTIAL' if count else 'NOT_CHECKED'
    require(observed.get('status') == status, 'machine image observation status differs from its fields')
    process = environment['image_environment']
    return dict(status=status, observation=observed,
                field_comparison={name: ('MATCH' if process.get(name) == values[name] else 'CONFLICT')
                                  if states[name] == 'OBSERVED' else 'NOT_CHECKED' for name in sorted(names)},
                causal_explanation=None,
                trust='same-job captured named machine registry values; not signed host or image attestation')


def compare(inputs):
    require(isinstance(inputs, dict), 'provenance inputs must be an object')
    require(inputs.get('schema') == 'wls-windows-image-source-input-v1', 'unknown provenance input schema')
    repository = inputs['repository']
    require(re.fullmatch(r'[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+', repository), 'invalid repository')
    run_id, job_id, artifact_id = (inputs[key] for key in ('run_id', 'job_id', 'artifact_id'))
    require(all(type(value) is int and value > 0 for value in (run_id, job_id, artifact_id)), 'invalid API IDs')
    require(type(inputs.get('attempt')) is int and inputs['attempt'] > 0, 'invalid run attempt')
    prefix = 'repos/' + repository + '/actions/'
    run_raw, run_source = captured(inputs['run'], prefix + f'runs/{run_id}/attempts/{inputs["attempt"]}')
    run = json.loads(run_raw)
    require(run.get('id') == run_id and run.get('run_attempt') == inputs['attempt'], 'run/attempt binding differs')
    jobs_raw, jobs_source = captured(inputs['jobs'], prefix + f'runs/{run_id}/attempts/{inputs["attempt"]}/jobs?per_page=100')
    jobs = json.loads(jobs_raw)
    matches = [value for value in jobs.get('jobs', []) if value.get('id') == job_id]
    require(len(matches) == 1, 'numeric job ID is absent or ambiguous')
    job = matches[0]
    require(job.get('run_id') == run_id and job.get('run_attempt') == inputs['attempt'] and
            job.get('head_sha') == run.get('head_sha'), 'job/run/head binding differs')
    log_raw, log_source = captured(inputs['log'], prefix + f'jobs/{job_id}/logs')
    log = log_raw.decode('utf-8-sig').replace('\r\n', '\n')
    require('Complete job name: ' + job['name'] + '\n' in log, 'job name does not match original log')
    artifacts_raw, artifacts_source = captured(inputs['artifacts'], prefix + f'runs/{run_id}/artifacts?per_page=100')
    artifacts = json.loads(artifacts_raw)
    selected = [value for value in artifacts.get('artifacts', []) if value.get('id') == artifact_id]
    require(len(selected) == 1, 'artifact ID is absent or ambiguous')
    artifact = selected[0]
    require(artifact.get('workflow_run', {}).get('id') == run_id and
            artifact.get('workflow_run', {}).get('head_sha') == run.get('head_sha'), 'artifact belongs to another run/head')
    require(f'https://github.com/{repository}/actions/runs/{run_id}/artifacts/{artifact_id}' in log,
            'same-job log does not record the artifact upload')
    archive_raw, archive_source = captured(inputs['archive'], prefix + f'artifacts/{artifact_id}/zip')
    require(artifact.get('digest') == 'sha256:' + archive_source['sha256'] and
            artifact.get('size_in_bytes') == len(archive_raw), 'artifact archive differs from API digest/size')
    with zipfile.ZipFile(io.BytesIO(archive_raw)) as archive:
        members = [entry for entry in archive.infolist() if entry.filename == ENVIRONMENT_MEMBER]
        require(len(members) == 1 and members[0].file_size <= 65536 and
                not members[0].is_dir() and not members[0].flag_bits & 1, 'bounded original environment member missing')
        environment_raw = archive.read(members[0])
    environment = json.loads(environment_raw)
    require(environment.get('schema') == 'wls-native-image-environment-v1', 'unknown environment schema')
    context = environment.get('github_context', {})
    checkout = environment.get('actual_checkout_sha', '')
    require(re.fullmatch(r'[0-9a-f]{40}', checkout) and context.get('GITHUB_SHA') == checkout and
            context.get('GITHUB_RUN_ID') == str(run_id) and
            context.get('GITHUB_RUN_ATTEMPT') == str(inputs['attempt']) and
            context.get('GITHUB_EVENT_NAME') == run.get('event') and
            context.get('GITHUB_JOB') == inputs['workflow_job_key'], 'environment run/attempt/job/checkout binding differs')
    checkout_lines = re.findall(r'(?m)^' + STAMP + r' \[command\].* log -1 --format=%H\n' +
                                STAMP + r' ([0-9a-f]{40})$', log)
    require(checkout_lines == [checkout], 'actual checkout is not uniquely confirmed by original checkout log')
    # Match the original structured capture printed by the same job, avoiding
    # a workflow-job-key -> numeric-job inference from names alone.
    printed = []
    for line in log.splitlines():
        match = re.fullmatch(STAMP + r' (\{.*\})', line)
        if match:
            value = json.loads(match.group(1))
            if value.get('schema') == 'wls-native-image-environment-v1':
                printed.append(value)
    require(printed == [environment], 'artifact environment does not equal the original same-job printed capture')
    require(timestamp(job['started_at']) <= timestamp(environment['captured_at_utc']) <= timestamp(job['completed_at']),
            'environment capture is outside the actual job interval')
    header = header_observation(log)
    checkout_line = next(index for index, line in enumerate(log.splitlines(), 1)
                         if '[command]' in line and line.endswith(' log -1 --format=%H'))
    require(header['end_line'] < checkout_line, 'Runner Image header was not recorded before checkout')
    require(timestamp(job['started_at']) <= timestamp(header['emitted_at_utc']) <= timestamp(environment['captured_at_utc']),
            'header and environment source chronology differs')
    values = environment.get('image_environment', {})
    require(set(values) == {'ImageOS', 'ImageVersion'} and all(isinstance(value, str) and value for value in values.values()),
            'image environment fields are incomplete')
    version_match = header['fields']['Version'] == values['ImageVersion']
    machine = machine_observation(environment)
    if machine['status'] != 'NOT_COLLECTED':
        require(timestamp(machine['observation']['captured_at_utc']) <= timestamp(job['completed_at']),
                'machine image observation is outside the actual job interval')
    machine_conflict = any(value == 'CONFLICT' for value in (machine['field_comparison'] or {}).values())
    sources_consistent = version_match and not machine_conflict
    unresolved_reason = None
    if not version_match:
        unresolved_reason = 'original Runner Image Version and inherited ImageVersion disagree; no causal evidence was supplied'
    elif machine_conflict:
        unresolved_reason = 'named machine registry and inherited process image fields disagree; no causal evidence was supplied'
    environment_source = dict(archive=archive_source, member=ENVIRONMENT_MEMBER, bytes=len(environment_raw),
        sha256=hashlib.sha256(environment_raw).hexdigest(), captured_at_utc=environment['captured_at_utc'],
        trust='workflow inherited process variables; archive/API hash and same-job printed capture agree; values remain workflow-controlled')
    return dict(schema=SCHEMA, recorded_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        repository=repository, run_id=run_id, job_id=job_id, attempt=inputs['attempt'],
        workflow_job_key=context['GITHUB_JOB'], event=run['event'], branch_head_sha=run['head_sha'],
        actual_checkout_sha=checkout, artifact_id=artifact_id,
        binding_status='MATCHED_SAVED_SAME_JOB_EVIDENCE',
        runner_header=dict(**header, source=log_source,
            trust='runner initialization log before checkout; metadata observation, not an independent host-image attestation'),
        process_environment=dict(values=values, source=environment_source),
        machine_registry_environment=machine,
        api_sources=dict(run=run_source, jobs=jobs_source, artifacts=artifacts_source),
        image_version_fields_match=version_match,
        exact_runner_image_identity='CONSISTENT_OBSERVATIONS_NOT_HOST_ATTESTATION' if sources_consistent else 'UNRESOLVED',
        explanation='matching observed fields only; missing channels remain unobserved' if sources_consistent else None,
        causal_explanation_evidence=[],
        unresolved_reason=unresolved_reason,
        preferred_source=None, prior_audit_rewritten=False, artifact_code_executed=False,
        remote_requests=0, windows11_acceptance='NOT_RUN', exit_code=0 if sources_consistent else 1)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, required=True,
                        help='explicit saved API/log/archive references and their capture receipts; never a deployment SID/build policy')
    parser.add_argument('--output', type=Path, required=True,
                        help='fresh private JSON receipt; refuses an existing destination')
    arguments = parser.parse_args()
    raw, input_identity = read_file(arguments.inputs, 1024 * 1024)
    try:
        result = compare(json.loads(raw))
    except (ValueError, KeyError, TypeError, AttributeError, OSError, RuntimeError, EOFError, zipfile.BadZipFile) as error:
        result = dict(schema=SCHEMA, recorded_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
            binding_status='REJECTED', exact_runner_image_identity='UNRESOLVED', error=str(error),
            preferred_source=None, prior_audit_rewritten=False, artifact_code_executed=False, exit_code=2)
    result['input_manifest'] = input_identity
    fd = os.open(arguments.output, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    with os.fdopen(fd, 'w', encoding='utf-8', newline='\n') as output:
        json.dump(result, output, ensure_ascii=False, indent=2)
        output.write('\n')
    print(json.dumps({key: result[key] for key in ('binding_status', 'exact_runner_image_identity', 'exit_code')}, sort_keys=True))
    return result['exit_code']


if __name__ == '__main__':
    raise SystemExit(main())
