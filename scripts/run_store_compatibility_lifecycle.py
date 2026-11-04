#!/usr/bin/env python3
"""Bounded lifecycle runner for the N5 strict compatibility matrix (evidence tool).

Manually invoked evidence tool, deliberately NOT registered as a CTest suite.
Each matrix row uses one fresh fixture directory under the evidence root on the
data disk; evidence records, copies and process journals are written OUTSIDE the
fixture directories.  Every product child runs once per row; failures are
recorded, never retried to green.  A test-only holder subprocess is the only
fixture writer inside its window (wal_autocheckpoint=0, bounded pipe handshake,
phase and handshake sequence numbers, monotonic anchors); it is an experiment
participant and never a second production Store writer.  The old 410 ELF is
verified before and after every use and is never modified.

Correction 2026-11-04: the C4 controlled-kill experiment is NOT executed by
this runner.  The historical attempt only ever signalled the strace tracer; no
old-ELF tracee identity or syscall rendezvous was ever established, so row_c4
refuses up front with a structured NOT_RUN and _trace_old rejects its kill mode
before any effect.  See docs/reports/n5-compatibility-strict-20261104-v3.md.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import signal
import socket
import subprocess
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                'tests'))
import store_compatibility_observer as obs  # noqa: E402

OLD_ELF_SHA256 = 'b048a449ecdc22cd53ccf2813804bd36c720a26870732f03ebe9c066ed5d407e'
OLD_ELF_BYTES = 4733672
STRACE_EVENTS = ('openat,open,close,read,pread64,preadv,preadv2,pwrite64,pwritev,'
                 'mmap,munmap,ftruncate,unlink,unlinkat,rename,renameat,renameat2,'
                 'chmod,fchmod,fchmodat,utime,utimensat,flock,fcntl,exit,exit_group')
ROW_TIMEOUT_S = 25
HANDSHAKE_TIMEOUT_S = 15

HOLDER_SOURCE = r'''#!/usr/bin/env python3
"""Test-only fixture holder: one SQLite connection, bounded handshake protocol."""
import fcntl, json, os, socket, sqlite3, sys, time

sock_path_fd = int(sys.argv[1])
db = sys.argv[2]
journal_path = sys.argv[3]
sock = socket.socket(fileno=sock_path_fd)
journal = open(journal_path, 'a', buffering=1)

def log(event, data):
    record = {'mono_ns': time.monotonic_ns(), 'event': event}
    record.update(data)
    journal.write(json.dumps(record, ensure_ascii=False) + '\n')

def send(payload):
    sock.sendall((json.dumps(payload) + '\n').encode())

buffer = b''

def recv():
    global buffer
    while b'\n' not in buffer:
        chunk = sock.recv(65536)
        if not chunk:
            raise EOFError('runner closed the holder socket')
        buffer += chunk
    line, _, buffer = buffer.partition(b'\n')
    return json.loads(line)

con = None
flock_fd = None
seq = 0

def reply(request, ok=True, **extra):
    global seq
    seq += 1
    payload = {'ok': ok, 'req_seq': request.get('seq'), 'holder_seq': seq,
               'mono_ns': time.monotonic_ns()}
    payload.update(extra)
    send(payload)

log('holder_start', {'pid': os.getpid(), 'db': db})
while True:
    try:
        request = recv()
    except EOFError:
        log('socket_eof', {})
        break
    command = request.get('command')
    log('request', {'command': command, 'req_seq': request.get('seq')})
    try:
        if command == 'HELLO':
            maps = [line for line in open('/proc/self/maps') if 'libsqlite3' in line]
            stat_fields = open('/proc/self/stat').read().split()
            reply(request, pid=os.getpid(), start_ticks=int(stat_fields[21]),
                  cmdline=open('/proc/self/cmdline', 'rb').read().decode(errors='replace').split('\0'),
                  open_fds=sorted(os.listdir('/proc/self/fd')),
                  sqlite_version=sqlite3.sqlite_version,
                  libsqlite3_maps=maps)
        elif command == 'OPEN':
            con = sqlite3.connect(db, isolation_level=None, timeout=10)
            con.execute('PRAGMA wal_autocheckpoint=0')
            mode = con.execute('PRAGMA journal_mode').fetchone()[0]
            reply(request, journal_mode=mode, wal_autocheckpoint=0,
                  wal_bytes=os.path.getsize(db + '-wal') if os.path.exists(db + '-wal') else 0)
        elif command in ('WRITE', 'RESUME'):
            token = request['token']
            con.execute('BEGIN IMMEDIATE')
            con.execute('CREATE TABLE IF NOT EXISTS compat_controlled_facts('
                        'id INTEGER PRIMARY KEY, token TEXT NOT NULL, phase TEXT NOT NULL, '
                        'mono_ns INTEGER NOT NULL)')
            con.execute('INSERT INTO compat_controlled_facts(token, phase, mono_ns) '
                        'VALUES (?, ?, ?)', (token, command, time.monotonic_ns()))
            con.execute('COMMIT')
            reply(request, committed_token=token,
                  wal_bytes=os.path.getsize(db + '-wal') if os.path.exists(db + '-wal') else 0,
                  in_transaction=con.in_transaction)
        elif command == 'QUIESCE':
            count = con.execute('SELECT count(*) FROM sqlite_schema').fetchone()[0]
            reply(request, settle_schema_objects=count, in_transaction=con.in_transaction)
        elif command == 'FLOCK':
            flock_fd = os.open(request['path'], os.O_RDWR | os.O_CREAT | os.O_CLOEXEC
                               | os.O_NOFOLLOW, 0o600)
            fcntl.flock(flock_fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            reply(request, flock_held=True)
        elif command == 'STATUS':
            reply(request, connection_open=con is not None,
                  in_transaction=bool(con.in_transaction) if con else None,
                  wal_autocheckpoint=con.execute('PRAGMA wal_autocheckpoint').fetchone()[0],
                  flock_held=flock_fd is not None,
                  wal_bytes=os.path.getsize(db + '-wal') if os.path.exists(db + '-wal') else 0)
        elif command == 'EXIT':
            close_result = 'ok'
            if con is not None:
                try:
                    con.close()
                except Exception as failure:
                    close_result = f'[{type(failure).__name__}] {failure}'
            if flock_fd is not None:
                os.close(flock_fd)
            reply(request, sqlite_close=close_result)
            log('holder_exit', {'sqlite_close': close_result})
            break
        else:
            reply(request, ok=False, error='unknown command')
    except Exception as failure:
        log('command_error', {'command': command,
                              'error': f'[{type(failure).__name__}] {failure}'})
        reply(request, ok=False, error=f'[{type(failure).__name__}] {failure}')
journal.close()
'''


def utc():
    import datetime
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as handle:
        while block := handle.read(1024 * 1024):
            digest.update(block)
    return digest.hexdigest()


class Evidence:
    """Anti-overwrite evidence writer bound to one run id."""

    def __init__(self, root, run_id):
        self.root = root
        self.run_id = run_id
        self.records = os.path.join(root, 'records')
        self.fixtures = os.path.join(root, 'fixtures')
        self.tmp = os.path.join(root, 'tmp')
        for path in (self.records, self.fixtures, self.tmp):
            os.makedirs(path, mode=0o700, exist_ok=True)

    def row_dir(self, row):
        path = os.path.join(self.records, f'{row}-{self.run_id}')
        os.makedirs(path, mode=0o700, exist_ok=False)
        os.makedirs(os.path.join(path, 'copies'), mode=0o700)
        return path

    def fixture_dir(self, name):
        path = os.path.join(self.fixtures, f'{name}-{self.run_id}')
        os.makedirs(path, mode=0o700, exist_ok=False)
        return path

    def json(self, path, payload):
        obs.write_json(path, payload)


class Identity:
    def __init__(self, evidence, args):
        self.evidence = evidence
        self.args = args
        self.started_utc = utc()
        self.environment = self._environment()

    def _environment(self):
        executable = os.path.realpath(sys.executable)
        libsqlite = []
        with open('/proc/self/maps', encoding='utf-8') as handle:
            for line in handle:
                if 'libsqlite3' in line and line.rstrip().endswith('.so.0'):
                    candidate = line.rstrip().split()[-1]
                    if candidate not in [item[0] for item in libsqlite]:
                        libsqlite.append(candidate)
        environment = {
            'utc': self.started_utc, 'uid': os.geteuid(), 'umask': '0077',
            'hostname': os.uname().nodename, 'kernel': os.uname().release,
            'python_executable': sys.executable, 'python_realpath': executable,
            'python_version': sys.version,
            'python_file_sha256': file_sha256(executable),
            'sqlite_runtime': __import__('sqlite3').sqlite_version,
            'libsqlite3_paths': [{'path': item, 'sha256': file_sha256(item)}
                                 for item in libsqlite],
            'mount_records': obs.mount_records(self.evidence.root),
            'df_bytes': subprocess.run(['df', '-B1', self.evidence.root], capture_output=True,
                                       text=True).stdout.strip().splitlines()[-1],
            'cpu_count': os.cpu_count(),
            'tmpdir': self.evidence.tmp,
            'argv': vars(self.args),
        }
        return environment

    def binary(self, path, expected_sha=None):
        payload = {'path': os.path.realpath(path), 'sha256': file_sha256(path),
                   'bytes': os.path.getsize(path),
                   'elf_magic': open(path, 'rb').read(4) == b'\x7fELF',
                   'mode': oct(os.stat(path).st_mode & 0o777)}
        try:
            ldd = subprocess.run(['ldd', path], capture_output=True, text=True, timeout=15)
            payload['ldd_exit'] = ldd.returncode
            payload['ldd_libsqlite3'] = [line.strip() for line in ldd.stdout.splitlines()
                                         if 'libsqlite3' in line]
        except Exception as failure:
            payload['ldd_error'] = f'[{type(failure).__name__}] {failure}'
        payload['sha_matches_plan'] = (expected_sha is None or
                                       payload['sha256'] == expected_sha)
        return payload


class Runner:
    def __init__(self, evidence, identity, args):
        self.evidence = evidence
        self.identity = identity
        self.args = args
        self.holder_path = os.path.join(evidence.root, 'holder',
                                        f'holder-{args.run_id}.py')
        os.makedirs(os.path.dirname(self.holder_path), mode=0o700, exist_ok=True)
        with open(self.holder_path, 'xb') as handle:
            handle.write(HOLDER_SOURCE.encode())
        self.holder_sha256 = file_sha256(self.holder_path)
        self.rows = {}

    # ------------------------------------------------------------ CLI plumbing
    def verify_old_elf(self):
        info = self.identity.binary(self.args.old_elf, OLD_ELF_SHA256)
        if not (info['sha_matches_plan'] and info['bytes'] == OLD_ELF_BYTES
                and info['elf_magic']):
            raise SystemExit(f'old 410 ELF identity mismatch: {info}')
        return info

    def run_cli(self, binary, argv, cwd, tag, timeout=ROW_TIMEOUT_S, env_extra=None):
        started = time.monotonic_ns()
        environment = dict(os.environ)
        environment['TMPDIR'] = self.evidence.tmp
        environment.setdefault('PYTHONDONTWRITEBYTECODE', '1')
        environment.update(env_extra or {})
        child = subprocess.Popen([str(binary), *argv], cwd=str(cwd), env=environment,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        start_ticks = 'UNKNOWN'
        try:
            with open(f'/proc/{child.pid}/stat', encoding='utf-8') as handle:
                start_ticks = int(handle.read().split(')')[-1].split()[19])
        except (OSError, ValueError, IndexError):
            pass
        try:
            stdout, stderr = child.communicate(timeout=timeout)
            timed_out = False
        except subprocess.TimeoutExpired:
            child.kill()
            stdout, stderr = child.communicate()
            timed_out = True
        ended = time.monotonic_ns()
        record = {'tag': tag, 'argv': [str(binary), *argv], 'cwd': str(cwd),
                  'pid': child.pid, 'start_ticks': start_ticks,
                  'started_mono_ns': started, 'ended_mono_ns': ended,
                  'timed_out': timed_out, 'exit_code': child.returncode,
                  'stdout_bytes': len(stdout), 'stderr_bytes': len(stderr),
                  'stdout_sha256': hashlib.sha256(stdout).hexdigest(),
                  'stderr_sha256': hashlib.sha256(stderr).hexdigest()}
        if len(stdout) <= 65536:
            record['stdout_text'] = stdout.decode('utf-8', 'replace')
        if len(stderr) <= 65536:
            record['stderr_text'] = stderr.decode('utf-8', 'replace')
        return record, stdout, stderr

    def error_classification(self, stderr):
        try:
            return json.loads(stderr.decode('utf-8')).get('error')
        except (ValueError, UnicodeDecodeError):
            return None

    # ------------------------------------------------------------ holder client
    def holder_start(self, database, journal_path):
        parent, child_end = socket.socketpair()
        holder = subprocess.Popen([sys.executable, self.holder_path,
                                   str(child_end.fileno()), str(database), str(journal_path)],
                                  pass_fds=(child_end.fileno(),), cwd=self.evidence.tmp,
                                  env={**os.environ, 'TMPDIR': self.evidence.tmp,
                                       'PYTHONDONTWRITEBYTECODE': '1'},
                                  stdin=subprocess.DEVNULL)
        child_end.close()
        client = HolderClient(parent, holder)
        hello = client.command('HELLO')
        return client, hello

    # ------------------------------------------------------------ fixture builds
    def build_static_411(self, name):
        fixture = self.evidence.fixture_dir(name)
        database = os.path.join(fixture, 'fixture.sqlite')
        workspace = os.path.join(fixture, 'workspace')
        os.makedirs(workspace, mode=0o700)
        source = self.args.source
        fixtures = os.path.join(source, 'tests', 'fixtures')
        steps = []
        steps.append(self.run_cli(self.args.current_binary,
                                  ['init', '--db', database, '--source-root', source],
                                  fixture, f'{name}-init')[0])
        for key in ('compat-a', 'compat-b'):
            steps.append(self.run_cli(
                self.args.current_binary,
                ['submit', '--db', database, '--key', key,
                 '--text-file', os.path.join(fixtures, 's12_request.txt'),
                 '--constraints-file', os.path.join(fixtures, 's12_constraints.json'),
                 '--source-root', source], fixture, f'{name}-submit-{key}')[0])
        request = json.loads(steps[-1]['stdout_text'])['request']['request_id']
        steps.append(self.run_cli(
            self.args.current_binary,
            ['run-fake', '--db', database, '--request', request,
             '--script', os.path.join(fixtures, 's12_fake_workspace.json'),
             '--workspace', workspace, '--worker-calls', '3', '--source-root', source],
            fixture, f'{name}-run-fake')[0])
        return fixture, database, steps, request

    def row_dir_records_path(self, name):
        return os.path.join(self.evidence.records, f'{name}-{self.evidence.run_id}')

    def build_populated_410(self, name):
        fixture = self.evidence.fixture_dir(name)
        database = os.path.join(fixture, 'fixture.sqlite')
        workspace = os.path.join(fixture, 'workspace')
        os.makedirs(workspace, mode=0o700)
        old_fixtures = os.path.join(self.args.old_source, 'tests', 'fixtures')
        steps = [self.run_cli(self.args.old_elf,
                              ['init', '--db', database, '--source-root', self.args.old_source],
                              fixture, f'{name}-old-init')[0]]
        for key in ('old-a', 'old-b'):
            steps.append(self.run_cli(
                self.args.old_elf,
                ['submit', '--db', database, '--key', key,
                 '--text-file', os.path.join(old_fixtures, 's12_request.txt'),
                 '--constraints-file', os.path.join(old_fixtures, 's12_constraints.json'),
                 '--source-root', self.args.old_source], fixture, f'{name}-old-submit-{key}')[0])
        request = json.loads(steps[-1]['stdout_text'])['request']['request_id']
        steps.append(self.run_cli(
            self.args.old_elf,
            ['run-fake', '--db', database, '--request', request,
             '--script', os.path.join(old_fixtures, 's12_fake_workspace.json'),
             '--workspace', workspace, '--worker-calls', '3',
             '--source-root', self.args.old_source], fixture, f'{name}-old-run-fake')[0])
        return fixture, database, steps, request

    def strict_window(self, row_path, database, tag):
        copies = os.path.join(row_path, 'copies', tag)
        stability = obs.stability_check(database, self.evidence.root, rounds=2)
        record = {'tag': tag, 'stability_ok': stability['stable'],
                  'snapshot_sha256': [obs.sha256_json(item) for item in stability['observations']]}
        if not stability['stable']:
            record['verdict'] = 'WINDOW_UNSTABLE'
            record['comparison'] = stability['comparisons']
            return record, None, None
        copy = obs.frozen_copy(database, copies, self.evidence.root)
        dump = obs.logical_dump(os.path.join(copies, os.path.basename(database)),
                                self.evidence.root)
        record.update({'copy_readback_all_match': all(v['matches_original']
                                                      for v in copy['readback'].values()),
                       'copy_members': sorted(copy['readback']),
                       'logical_sha256': dump.get('logical_sha256'),
                       'logical_close': dump.get('close'),
                       'sqlite_error': dump.get('sqlite_error'),
                       'user_version': dump.get('pragmas', {}).get('user_version'),
                       'pre_open_members': dump.get('pre_open_members')})
        record['verdict'] = 'OK' if (record['copy_readback_all_match'] and
                                     dump.get('logical_sha256') and
                                     not dump.get('sqlite_error')) else 'DEGRADED'
        self.evidence.json(os.path.join(row_path, f'{tag}-copy.json'),
                           {'copy': {key: copy[key] for key in
                                     ('original_before', 'original_after', 'readback',
                                      'copy_metadata')}})
        self.evidence.json(os.path.join(row_path, f'{tag}-dump.json'), dump)
        return record, copy, dump

    def raw_between(self, before_copy, after_copy):
        return obs.compare_snapshots(before_copy['original_before'],
                                     after_copy['original_after'])


class HolderClient:
    def __init__(self, sock, process):
        self.sock = sock
        self.process = process
        self.seq = 0
        self.buffer = b''

    def command(self, command, **extra):
        self.seq += 1
        payload = {'command': command, 'seq': self.seq, **extra}
        self.sock.sendall((json.dumps(payload) + '\n').encode())
        while b'\n' not in self.buffer:
            self.sock.settimeout(HANDSHAKE_TIMEOUT_S)
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError('holder closed the socket')
            self.buffer += chunk
        line, _, self.buffer = self.buffer.partition(b'\n')
        reply = json.loads(line)
        if not reply.get('ok'):
            raise RuntimeError(f'holder refused {command}: {reply}')
        return reply

    def close(self):
        try:
            return self.command('EXIT')
        finally:
            self.process.wait(timeout=15)
            self.sock.close()


def verdict_for_row(row_record):
    """Single-row status per the FINAL plan; failures stay failures."""
    if row_record.get('not_run_reason'):
        return 'NOT_RUN'
    if row_record.get('blocked_reason'):
        return 'BLOCKED'
    return row_record.get('expected_status', 'RECORDED')


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence-root', required=True)
    parser.add_argument('--run-id', required=True)
    parser.add_argument('--old-elf', required=True)
    parser.add_argument('--old-source', required=True)
    parser.add_argument('--source', required=True)
    parser.add_argument('--current-binary', required=True)
    parser.add_argument('--rows', required=True,
                        help='comma list among c0,c1,c2a,c2b,c3a,c3b,c4receipt,c4,c5,c7a,c7b,c6')
    arguments = parser.parse_args(argv)
    if os.geteuid() == 0:
        raise SystemExit('refusing to run the lifecycle runner as root')
    os.umask(0o077)
    evidence = Evidence(arguments.evidence_root, arguments.run_id)
    identity = Identity(evidence, arguments)
    runner = Runner(evidence, identity, arguments)
    manifest = {
        'run_id': arguments.run_id, 'started_at': identity.started_utc,
        'environment': identity.environment,
        'old_elf': runner.verify_old_elf(),
        'current_binary': identity.binary(arguments.current_binary),
        'tools': {
            'runner': {'path': os.path.abspath(__file__),
                       'sha256': file_sha256(os.path.abspath(__file__))},
            'observer': {'path': os.path.abspath(obs.__file__),
                         'sha256': file_sha256(os.path.abspath(obs.__file__))},
            'holder': {'path': runner.holder_path, 'sha256': runner.holder_sha256}},
    }
    evidence.json(os.path.join(evidence.records, f'manifest-{arguments.run_id}.json'), manifest)
    rows = [item.strip() for item in arguments.rows.split(',') if item.strip()]
    results = {}
    for row in rows:
        handler = ROW_HANDLERS.get(row)
        if handler is None:
            results[row] = {'status': 'NOT_RUN', 'not_run_reason': f'unknown row {row}'}
            continue
        row_path = evidence.row_dir(row)
        started = time.monotonic_ns()
        try:
            record = handler(runner, row_path)
        except Exception as failure:
            record = {'status': 'ERROR', 'error': f'[{type(failure).__name__}] {failure}'}
        record['elapsed_s'] = round((time.monotonic_ns() - started) / 1e9, 6)
        record['row'] = row
        evidence.json(os.path.join(row_path, 'row.json'), record)
        results[row] = record
        runner.verify_old_elf()
    summary = {'run_id': arguments.run_id, 'ended_at': utc(), 'rows': {
        row: {'status': record.get('status', verdict_for_row(record)),
              'detail': record.get('verdicts', record.get('not_run_reason',
                                                          record.get('error', '')))}
        for row, record in results.items()}}
    evidence.json(os.path.join(evidence.records, f'summary-{arguments.run_id}.json'), summary)
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0


# --------------------------------------------------------------------- rows
def _per_file(raw, name):
    return (raw or {}).get('per_file', {}).get(name, {})


def _raw_field(raw, key):
    return (raw or {}).get(key)


def _all_raw_equal(raw):
    if not raw:
        return None
    return all(entry.get('raw_equal') or entry.get('status') == 'added'
               for entry in raw['per_file'].values())


def row_c0(runner, row_path):
    fixture, database, steps, request = runner.build_static_411('c0')
    client, hello = runner.holder_start(database, os.path.join(row_path, 'holder.journal'))
    record = {'fixture': fixture, 'construction': steps, 'request': request,
              'holder_hello': hello, 'phases': {}}
    record['phases']['PREPARED'] = {'fixture_built': True}
    record['phases']['WAL_READY'] = {'open': client.command('OPEN'),
                                     'write': client.command('WRITE', token='c0-fact')}
    record['phases']['WRITER_QUIESCED'] = client.command('QUIESCE')
    before, copy_b, dump_b = runner.strict_window(row_path, database, 'before')
    record['phases']['BEFORE'] = before
    record['phases']['NO_PRODUCT'] = {'argv': None, 'note': 'C0 runs no product operation'}
    after, copy_a, dump_a = runner.strict_window(row_path, database, 'after')
    record['phases']['AFTER'] = after
    comparison = runner.raw_between(copy_b, copy_a) if copy_b and copy_a else None
    logical = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
    record['verdicts'] = {
        'observer_non_perturbing': bool(comparison and comparison['strict_equal']),
        'logical_equal': bool(logical and logical['logical_equal']),
        'comparison': comparison, 'logical': logical}
    record['phases']['HOLDER_CLOSE'] = client.close()
    record['phases']['POST_CLOSE_MEMBERS'] = sorted(
        name for name in os.listdir(fixture) if name.startswith('fixture.sqlite'))
    record['status'] = ('PASS' if record['verdicts']['observer_non_perturbing'] and
                        record['verdicts']['logical_equal'] and
                        before['verdict'] == 'OK' and after['verdict'] == 'OK' else 'FAIL')
    return record


def row_c1(runner, row_path):
    record = {'cases': {}}
    for case, command in (('c1a-pending', 'pending'), ('c1b-init', 'init')):
        fixture, database, steps, request = runner.build_static_411(f'c1-{command}')
        before, copy_b, dump_b = runner.strict_window(row_path, database, f'{case}-before')
        runner.verify_old_elf()
        step, stdout, stderr = runner.run_cli(
            runner.args.old_elf, [command, '--db', database,
                                  '--source-root', runner.args.old_source],
            fixture, f'c1-{command}')
        runner.verify_old_elf()
        after, copy_a, dump_a = runner.strict_window(row_path, database, f'{case}-after')
        comparison = runner.raw_between(copy_b, copy_a) if copy_b and copy_a else None
        logical = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
        record['cases'][case] = {
            'fixture': fixture, 'steps': steps, 'product': step,
            'error_classification': runner.error_classification(stderr),
            'before': before, 'after': after,
            'raw': comparison, 'logical': logical,
            'main_equal': bool(_per_file(comparison, 'fixture.sqlite').get('raw_equal')),
            'business_equal': bool(logical and logical['logical_equal'])}
    cases = record['cases']
    record['verdicts'] = {}
    for case in cases:
        entry = cases[case]
        record['verdicts'][case] = {
            'refused_unsupported': entry['error_classification'] == 'unsupported',
            'main_equal': entry['main_equal'],
            'business_equal': entry['business_equal'],
            'sidecar_added': _raw_field(entry['raw'], 'added'),
            'sidecar_removed': _raw_field(entry['raw'], 'removed')}
    strict = all(
        entry['error_classification'] == 'unsupported' and entry['main_equal']
        and entry['business_equal'] and not _raw_field(entry['raw'], 'added')
        and not _raw_field(entry['raw'], 'removed')
        and all(item.get('raw_equal') for item in
                (entry['raw'] or {}).get('per_file', {}).values())
        for entry in cases.values())
    record['status'] = 'STRICT_PASS' if strict else 'STRICT_FAIL'
    return record


def _online_row(runner, row_path, name, command, *, flock=False, note=None):
    fixture, database, steps, request = runner.build_static_411(name)
    client, hello = runner.holder_start(database, os.path.join(row_path, 'holder.journal'))
    record = {'fixture': fixture, 'construction': steps, 'request': request,
              'holder_hello': hello, 'phases': {}, 'note': note}
    record['phases']['PREPARED'] = {'fixture_built': True}
    record['phases']['WAL_READY'] = {'open': client.command('OPEN'),
                                     'write': client.command('WRITE', token=f'{name}-fact')}
    record['phases']['WRITER_QUIESCED'] = client.command('QUIESCE')
    if flock:
        record['phases']['FLOCK'] = client.command('FLOCK', path=database + '.writer-lock')
    before, copy_b, dump_b = runner.strict_window(row_path, database, 'before')
    record['phases']['BEFORE'] = before
    runner.verify_old_elf()
    step, stdout, stderr = runner.run_cli(
        runner.args.old_elf, [command, '--db', database,
                              '--source-root', runner.args.old_source], fixture, name)
    runner.verify_old_elf()
    record['phases']['OLD_OPEN'] = step
    record['error_classification'] = runner.error_classification(stderr)
    after, copy_a, dump_a = runner.strict_window(row_path, database, 'after')
    record['phases']['AFTER'] = after
    comparison = runner.raw_between(copy_b, copy_a) if copy_b and copy_a else None
    logical = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
    record['phases']['WRITER_RESUME'] = client.command('RESUME', token=f'{name}-post-old')
    record['phases']['POST_WRITE'] = client.command('STATUS')
    record['phases']['HOLDER_CLOSE'] = client.close()
    record['raw'], record['logical'] = comparison, logical
    record['verdicts'] = {
        'error': record['error_classification'],
        'main': _per_file(comparison, 'fixture.sqlite'),
        'wal': _per_file(comparison, 'fixture.sqlite-wal'),
        'shm': _per_file(comparison, 'fixture.sqlite-shm'),
        'writer_lock': _per_file(comparison, 'fixture.sqlite.writer-lock'),
        'added': _raw_field(comparison, 'added'),
        'removed': _raw_field(comparison, 'removed'),
        'business_product_equal': bool(logical and logical['logical_equal'])}
    return record


def row_c2a(runner, row_path):
    record = _online_row(runner, row_path, 'c2a', 'pending',
                         note='C2a: non-empty stable online WAL (main header 411), old410 pending')
    verdicts = record['verdicts']
    strict_members = all(entry.get('raw_equal')
                         for key, entry in verdicts.items()
                         if key in ('main', 'wal', 'shm', 'writer_lock') and entry)
    record['status'] = ('STRICT_PASS' if verdicts['error'] == 'unsupported' and
                        strict_members and not verdicts['added'] and
                        not verdicts['removed'] and
                        verdicts['business_product_equal'] else 'STRICT_FAIL')
    return record


def row_c2b(runner, row_path):
    fixture, database, receipt, client, hello = _state2_fixture(runner, 'c2b')
    record = {'fixture': fixture, 'state2_construction': receipt,
              'holder_hello': hello, 'phases': {}}
    if receipt.get('construction_status') != 'CONSTRUCTED':
        client.close()
        record.update({'status': 'NOT_RUN',
                       'not_run_reason': 'state-2 fixture could not be constructed stably; '
                                         'returned to the plan author per FINAL plan'})
        return record
    record['phases']['WAL_READY'] = receipt['post_migration_write']
    record['phases']['WRITER_QUIESCED'] = client.command('QUIESCE')
    before, copy_b, dump_b = runner.strict_window(row_path, database, 'before')
    record['phases']['BEFORE'] = before
    runner.verify_old_elf()
    step, stdout, stderr = runner.run_cli(
        runner.args.old_elf, ['pending', '--db', database,
                              '--source-root', runner.args.old_source], fixture, 'c2b')
    runner.verify_old_elf()
    record['phases']['OLD_OPEN'] = step
    record['error_classification'] = runner.error_classification(stderr)
    after, copy_a, dump_a = runner.strict_window(row_path, database, 'after')
    record['phases']['AFTER'] = after
    comparison = runner.raw_between(copy_b, copy_a) if copy_b and copy_a else None
    logical = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
    record['phases']['WRITER_RESUME'] = client.command('RESUME', token='c2b-post-old')
    record['phases']['HOLDER_CLOSE'] = client.close()
    record['raw'], record['logical'] = comparison, logical
    record['verdicts'] = {
        'error': record['error_classification'],
        'main': _per_file(comparison, 'fixture.sqlite'),
        'wal': _per_file(comparison, 'fixture.sqlite-wal'),
        'shm': _per_file(comparison, 'fixture.sqlite-shm'),
        'writer_lock': _per_file(comparison, 'fixture.sqlite.writer-lock'),
        'added': _raw_field(comparison, 'added'),
        'removed': _raw_field(comparison, 'removed'),
        'business_product_equal': bool(logical and logical['logical_equal'])}
    verdicts = record['verdicts']
    strict_members = all(entry.get('raw_equal')
                         for key, entry in verdicts.items()
                         if key in ('main', 'wal', 'shm', 'writer_lock') and entry)
    record['status'] = ('STRICT_PASS' if verdicts['error'] == 'unsupported' and
                        strict_members and not verdicts['added'] and
                        not verdicts['removed'] and
                        verdicts['business_product_equal'] else 'STRICT_FAIL')
    return record


def _state2_fixture(runner, name):
    """Populated 410 whose committed 411 upgrade exists only in WAL (keeper held).

    The keeper connection stays open across the product migration so the last
    close never checkpoints; the fixture is returned with the keeper live.
    """
    fixture, database, steps, request = runner.build_populated_410(name)
    journal_dir = runner.row_dir_records_path(name)
    os.makedirs(journal_dir, mode=0o700, exist_ok=True)
    client, hello = runner.holder_start(database, os.path.join(journal_dir, 'keeper.journal'))
    opened = client.command('OPEN')
    pre = obs.snapshot(database, runner.evidence.root)
    main_before_sha = pre['members']['fixture.sqlite']['sha256']
    main_before_version = int.from_bytes(open(database, 'rb').read(64)[60:64], 'big')
    migration_step, _, _ = runner.run_cli(
        runner.args.current_binary,
        ['init', '--db', database, '--source-root', runner.args.source], fixture,
        f'{name}-current-init-migration')
    post = obs.snapshot(database, runner.evidence.root)
    main_after_version = int.from_bytes(open(database, 'rb').read(64)[60:64], 'big')
    write = client.command('WRITE', token=f'{name}-state2-fact')
    quiesce = client.command('QUIESCE')
    copies = os.path.join(journal_dir, 'construction-copy')
    obs.frozen_copy(database, copies, runner.evidence.root)
    dump = obs.logical_dump(os.path.join(copies, 'fixture.sqlite'), runner.evidence.root)
    copy_version = dump.get('pragmas', {}).get('user_version')
    receipt = {
        'populated_410_steps': steps, 'request': request,
        'keeper_hello': hello, 'keeper_open': opened,
        'keeper_was_open_during_migration': True,
        'migration_connection_was_last': False,
        'migration_step': migration_step, 'migration_exit': migration_step['exit_code'],
        'main_sha_before': main_before_sha,
        'main_sha_after': post['members']['fixture.sqlite']['sha256'],
        'main_user_version_before': main_before_version,
        'main_user_version_after': main_after_version,
        'wal_bytes_after': post['members'].get('fixture.sqlite-wal', {}).get('bytes', 0),
        'main_changed': main_before_sha != post['members']['fixture.sqlite']['sha256'],
        'copy_user_version': copy_version,
        'post_migration_write': write, 'quiesce': quiesce,
        'copy_user_version_is_411': copy_version == [{'k': 'int', 'v': 411}],
    }
    receipt['construction_status'] = (
        'CONSTRUCTED' if migration_step['exit_code'] == 0 and main_after_version == 410
        and not receipt['main_changed'] and receipt['copy_user_version_is_411'] and
        receipt['wal_bytes_after'] > 32 else 'CONSTRUCTION_FAILED')
    runner.evidence.json(os.path.join(journal_dir, 'state2-receipt.json'), receipt)
    return fixture, database, receipt, client, hello


def row_c3a(runner, row_path):
    record = _online_row(runner, row_path, 'c3a', 'init', flock=True,
                         note='C3a: real Store lifecycle flock held; Conflict expected before '
                              'any user_version execution; recorded as its own single column')
    verdicts = record['verdicts']
    strict_members = all(entry.get('raw_equal')
                         for key, entry in verdicts.items()
                         if key in ('main', 'wal', 'shm', 'writer_lock') and entry)
    record['status'] = ('STRICT_PASS' if verdicts['error'] == 'conflict' and
                        strict_members and not verdicts['added'] and
                        not verdicts['removed'] and
                        verdicts['business_product_equal'] else 'STRICT_FAIL')
    return record


def row_c3b(runner, row_path):
    record = _online_row(runner, row_path, 'c3b', 'init', flock=False,
                         note='C3b: no Store flock; the real old-init path (not the Conflict '
                              'path) is recorded as its own single column')
    verdicts = record['verdicts']
    strict_members = all(entry.get('raw_equal')
                         for key, entry in verdicts.items()
                         if key in ('main', 'wal', 'shm', 'writer_lock') and entry)
    record['status'] = ('STRICT_PASS' if verdicts['error'] == 'unsupported' and
                        strict_members and not verdicts['added'] and
                        not verdicts['removed'] and
                        verdicts['business_product_equal'] else 'STRICT_FAIL')
    return record


def _trace_old(runner, row_path, name, command, database, fixture, kill=False):
    """Run the old ELF under strace for normal, uninterrupted observation only.

    This helper starts, streams and waits for the strace TRACER process: the
    returned pid/start_ticks/exit_code describe that tracer, never the old-ELF
    tracee, whose identity is not obtained.  kill=True is rejected before any
    Popen, signal, fixture or trace-file effect: a controlled termination at a
    tracee syscall stop was never established (the historical attempt only ever
    signalled the tracer), so this runner must not attempt it.  A communicate
    timeout only cleans up the tracer; that is tracer cleanup, not product exit
    or reap proof.
    """
    if kill:
        raise ValueError('_trace_old kill mode is removed: no proven old-ELF '
                         'tracee identity or syscall-stop rendezvous exists; '
                         'controlled kill of the traced product is not '
                         'supported by this runner')
    trace_path = os.path.join(row_path, f'{name}.strace')
    argv = ['strace', '-ttt', '-yy', '-s', '256', '-e', f'trace={STRACE_EVENTS}',
            '-o', '/dev/stdout', str(runner.args.old_elf), command, '--db', str(database),
            '--source-root', str(runner.args.old_source)]
    environment = dict(os.environ)
    environment['TMPDIR'] = runner.evidence.tmp
    started = time.monotonic_ns()
    child = subprocess.Popen(argv, cwd=fixture, env=environment,
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
    start_ticks = 'UNKNOWN'
    try:
        with open(f'/proc/{child.pid}/stat', encoding='utf-8') as handle:
            start_ticks = int(handle.read().split(')')[-1].split()[19])
    except (OSError, ValueError, IndexError):
        pass
    rendezvous = {'shm_open_seen_mono_ns': None, 'pattern': 'fixture.sqlite-shm'}
    import select as _select
    stream_fd = child.stdout.fileno()
    os.set_blocking(stream_fd, False)
    collected = bytearray()
    deadline = time.monotonic() + ROW_TIMEOUT_S
    while time.monotonic() < deadline:
        ready, _, _ = _select.select([child.stdout], [], [], 0.002)
        if ready:
            try:
                chunk = os.read(stream_fd, 65536)
            except BlockingIOError:
                chunk = b''
            if chunk:
                collected.extend(chunk)
                if (rendezvous['shm_open_seen_mono_ns'] is None and
                        b'fixture.sqlite-shm' in bytes(collected)):
                    rendezvous['shm_open_seen_mono_ns'] = time.monotonic_ns()
        if child.poll() is not None:
            break
    os.set_blocking(stream_fd, True)
    try:
        remaining, stderr = child.communicate(timeout=15)
    except subprocess.TimeoutExpired:
        child.kill()  # tracer-only stream cleanup; not product exit/reap proof
        remaining, stderr = child.communicate()
    if remaining:
        collected.extend(remaining)
    ended = time.monotonic_ns()
    with open(trace_path, 'xb') as handle:
        handle.write(bytes(collected))
    return {'tag': name, 'argv': argv, 'pid': child.pid, 'start_ticks': start_ticks,
            'child_role': 'strace tracer; the old-ELF tracee identity is never '
                          'obtained by this helper',
            'started_mono_ns': started, 'ended_mono_ns': ended,
            'exit_code': child.returncode, 'timed_out': False,
            'trace_file': trace_path, 'trace_bytes': len(collected),
            'trace_sha256': hashlib.sha256(bytes(collected)).hexdigest(),
            'rendezvous': rendezvous,
            'stderr_sha256': hashlib.sha256(stderr).hexdigest(),
            'stderr_text': stderr.decode('utf-8', 'replace')[:4096],
            'error_classification': runner.error_classification(stderr)}


def row_c4receipt(runner, row_path):
    """Mechanism receipt: traced vs untraced normal refusal on isomorphic fixtures."""
    cases = {}
    for label, traced in (('traced', True), ('untraced', False)):
        name = f'c4receipt-{label}'
        fixture, database, steps, request = runner.build_static_411(name)
        client, hello = runner.holder_start(database, os.path.join(row_path, f'{label}.journal'))
        client.command('OPEN')
        client.command('WRITE', token=f'{name}-fact')
        client.command('QUIESCE')
        before, copy_b, dump_b = runner.strict_window(row_path, database, f'{label}-before')
        runner.verify_old_elf()
        if traced:
            step = _trace_old(runner, row_path, label, 'init', database, fixture, kill=False)
            error = step['error_classification']
        else:
            step, _, stderr = runner.run_cli(
                runner.args.old_elf, ['init', '--db', database,
                                      '--source-root', runner.args.old_source],
                fixture, label)
            error = runner.error_classification(stderr)
        runner.verify_old_elf()
        after, copy_a, dump_a = runner.strict_window(row_path, database, f'{label}-after')
        comparison = runner.raw_between(copy_b, copy_a) if copy_b and copy_a else None
        logical = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
        client.close()
        cases[label] = {
            'step': step, 'before': before, 'after': after, 'raw': comparison,
            'logical': logical,
            'classification': {
                'exit_code': step.get('exit_code'), 'error': error,
                'added': _raw_field(comparison, 'added'),
                'removed': _raw_field(comparison, 'removed'),
                'changed': sorted(item for item, entry in
                                  (comparison or {}).get('per_file', {}).items()
                                  if not entry.get('raw_equal')),
                'main_equal': bool(_per_file(comparison, 'fixture.sqlite').get('raw_equal')),
                'business_equal': bool(logical and logical['logical_equal'])}}
    left, right = cases['traced']['classification'], cases['untraced']['classification']
    equivalent = all(left[key] == right[key] for key in
                     ('exit_code', 'error', 'added', 'removed', 'changed', 'main_equal',
                      'business_equal'))
    trace_visible = bool(cases['traced']['step'].get('rendezvous', {}).get(
        'shm_open_seen_mono_ns'))
    record = {'cases': cases,
              'mechanism': {'trace_shm_open_visible': trace_visible,
                            'traced_untraced_equivalent': equivalent},
              'scope': 'normal-comparison receipt only: one traced and one untraced '
                       'uninterrupted refusal on isomorphic fixtures plus SHM-text '
                       'visibility in the streamed trace; it verifies no kill '
                       'rendezvous, identifies no tracee, and must never be used as '
                       'a gate for controlled-kill execution; equivalence of normal '
                       'runs does not imply non-perturbation under interruption',
              'note': 'single run each; no retry to green; any difference limits C4 '
                      'comparability and is reported to the plan author'}
    record['status'] = 'PASS' if equivalent and trace_visible else 'FAIL'
    return record


def row_c4(runner, row_path):
    """Controlled-kill coverage row: permanently NOT_RUN from this correction on.

    The historical r4 attempt signalled, state-confirmed and reaped only the
    strace tracer; no old-ELF tracee PID, start ticks, syscall-stop rendezvous,
    exit or reap was ever established.  This handler therefore refuses before
    any fixture construction, holder start, old-ELF/strace launch, signal or
    observer database access, and never reads an old receipt to re-enable
    execution.  No PID/exit/reap/SHM-stop fact is fabricated here; the corrected
    adjudication of the historical attempt lives in the v3 report.
    """
    return {'status': 'NOT_RUN',
            'not_run_reason': 'C4 controlled-kill execution is disabled: no proven '
                              'actual tracee identity or syscall rendezvous exists; '
                              'the historical attempt only ever signalled the strace '
                              'tracer',
            'tracee_identity': 'UNKNOWN',
            'product_crash_position_and_exit': 'NOT_ESTABLISHED',
            'product_reap': 'NOT_ESTABLISHED'}


def row_c5(runner, row_path):
    fixture, database, steps, request = runner.build_static_411('c5')
    client, hello = runner.holder_start(database, os.path.join(row_path, 'holder.journal'))
    record = {'fixture': fixture, 'construction': steps, 'request': request,
              'holder_hello': hello, 'phases': {}, 'note':
              'coexistence diagnostic: one authorized commit inside the old program '
              'window; this row never claims raw strict equality; the stopped child is '
              'the direct old-ELF process (no tracer involved), and only '
              'process-lifetime overlap is claimed'}
    record['coverage'] = {
        'supported': 'old-program process-lifetime overlap: one authorized holder '
                     'commit was provably committed while the direct old-ELF child '
                     'was SIGSTOP-confirmed, before SIGCONT and its exit',
        'not_established': ['which SQLite/DB-access stage the old process had '
                            'reached when stopped (no FD or stage evidence)',
                            'simultaneous SQLite access by both sides',
                            'any raw trace for this row']}
    record['phases']['WAL_READY'] = {'open': client.command('OPEN'),
                                     'write': client.command('WRITE', token='c5-fact')}
    record['phases']['WRITER_QUIESCED'] = client.command('QUIESCE')
    before, copy_b, dump_b = runner.strict_window(row_path, database, 'before')
    record['phases']['BEFORE'] = before
    runner.verify_old_elf()
    started = time.monotonic_ns()
    child = subprocess.Popen([str(runner.args.old_elf), 'pending', '--db', str(database),
                              '--source-root', str(runner.args.old_source)], cwd=fixture,
                             env={**os.environ, 'TMPDIR': runner.evidence.tmp},
                             stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                             stderr=subprocess.PIPE)
    start_ticks = 'UNKNOWN'
    try:
        with open(f'/proc/{child.pid}/stat', encoding='utf-8') as handle:
            start_ticks = int(handle.read().split(')')[-1].split()[19])
    except (OSError, ValueError, IndexError):
        pass
    stop_sent = time.monotonic_ns()
    stopped_confirmed = False
    try:
        os.kill(child.pid, signal.SIGSTOP)
    except OSError as failure:
        record['phases']['OLD_OPEN'] = {'stop_error': str(failure)}
        child.communicate(timeout=ROW_TIMEOUT_S)
        record.update({'status': 'RECORDED_DEVIATION', 'not_run_reason':
                       'SIGSTOP rendezvous failed; single-shot row'})
        client.close()
        return record
    for _ in range(5000):
        try:
            state = open(f'/proc/{child.pid}/stat',
                         encoding='utf-8').read().split(')')[1].split()[0]
            if state == 'T':
                stopped_confirmed = True
                break
        except (OSError, IndexError):
            break
    commit = client.command('WRITE', token='c5-concurrent-commit')
    cont_sent = time.monotonic_ns()
    try:
        os.kill(child.pid, signal.SIGCONT)
    except OSError as failure:
        record['cont_error'] = str(failure)
    stdout, stderr = child.communicate(timeout=ROW_TIMEOUT_S)
    exited = time.monotonic_ns()
    runner.verify_old_elf()
    record['phases']['OLD_OPEN'] = {
        'tag': 'c5-concurrent',
        'argv': [str(runner.args.old_elf), 'pending', '--db', str(database),
                 '--source-root', str(runner.args.old_source)],
        'pid': child.pid, 'start_ticks': start_ticks,
        'started_mono_ns': started, 'stop_sent_mono_ns': stop_sent,
        'stopped_state_confirmed': stopped_confirmed,
        'commit_mono_ns': commit['mono_ns'], 'cont_sent_mono_ns': cont_sent,
        'ended_mono_ns': exited,
        'commit_within_child_window': started < commit['mono_ns'] < exited,
        'exit_code': child.returncode,
        'stdout_sha256': hashlib.sha256(stdout).hexdigest(),
        'stderr_sha256': hashlib.sha256(stderr).hexdigest(),
        'stderr_text': stderr.decode('utf-8', 'replace')[:4096],
        'error_classification': runner.error_classification(stderr)}
    after, copy_a, dump_a = runner.strict_window(row_path, database, 'after')
    record['phases']['AFTER'] = after
    record['phases']['HOLDER_CLOSE'] = client.close()
    record['raw'] = runner.raw_between(copy_b, copy_a) if copy_b and copy_a else None
    record['logical'] = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
    differences = record['logical']['differences'] if record['logical'] else []
    product_changes = [item for item in differences
                       if item.get('table') != 'compat_controlled_facts' and
                       not item.get('pragma')]
    controlled_changes = [item for item in differences
                          if item.get('table') == 'compat_controlled_facts']
    record['verdicts'] = {
        'commit_within_child_window':
            record['phases']['OLD_OPEN']['commit_within_child_window'],
        'old_refusal':
            record['phases']['OLD_OPEN']['error_classification'] == 'unsupported',
        'business_product_rows_unchanged': not product_changes,
        'authorized_controlled_delta': controlled_changes,
        'shared_file_changes_retained':
            (record['raw'] or {}).get('per_file', {}),
        'raw_equality_claimed': False}
    record['status'] = ('RECORDED_OK' if all((
        record['verdicts']['commit_within_child_window'],
        record['verdicts']['old_refusal'],
        record['verdicts']['business_product_rows_unchanged'])) else 'RECORDED_DEVIATION')
    return record


def row_c7a(runner, row_path):
    fixture, database, steps, request = runner.build_populated_410('c7a')
    before, copy_b, dump_b = runner.strict_window(row_path, database, 'before')
    runner.verify_old_elf()
    step, _, stderr = runner.run_cli(
        runner.args.old_elf, ['pending', '--db', database,
                              '--source-root', runner.args.old_source], fixture,
        'c7a-old-pending')
    runner.verify_old_elf()
    after, copy_a, dump_a = runner.strict_window(row_path, database, 'after')
    logical = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
    tables = dump_b.get('tables', {}) if dump_b else {}
    record = {'fixture': fixture, 'construction_steps': steps, 'request': request,
              'product': step, 'error_classification': runner.error_classification(stderr),
              'before': before, 'after': after, 'logical': logical,
              'populated_tables': {name: info['row_count']
                                   for name, info in tables.items()},
              'user_version_before': dump_b.get('pragmas', {}).get('user_version')
              if dump_b else None}
    record['status'] = ('SUPPORT_CONTROL_PASS' if step['exit_code'] == 0 and
                        logical and logical['logical_equal'] and
                        any(count for count in record['populated_tables'].values()) else
                        'SUPPORT_CONTROL_FAIL')
    return record


def row_c7b(runner, row_path):
    fixture, database, steps, request = runner.build_static_411('c7b')
    before, copy_b, dump_b = runner.strict_window(row_path, database, 'before')
    step, _, stderr = runner.run_cli(
        runner.args.current_binary, ['pending', '--db', database,
                                     '--source-root', runner.args.source], fixture,
        'c7b-current-pending')
    after, copy_a, dump_a = runner.strict_window(row_path, database, 'after')
    logical = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
    tables = dump_b.get('tables', {}) if dump_b else {}
    record = {'fixture': fixture, 'construction_steps': steps, 'request': request,
              'product': step, 'error_classification': runner.error_classification(stderr),
              'before': before, 'after': after, 'logical': logical,
              'populated_tables': {name: info['row_count']
                                   for name, info in tables.items()},
              'user_version_before': dump_b.get('pragmas', {}).get('user_version')
              if dump_b else None}
    record['status'] = ('SUPPORT_CONTROL_PASS' if step['exit_code'] == 0 and
                        logical and logical['logical_equal'] and
                        any(count for count in record['populated_tables'].values()) else
                        'SUPPORT_CONTROL_FAIL')
    return record


def row_c6(runner, row_path):
    """Existing nine-method suite only, with its own 20s CLI bound unchanged."""
    environment = dict(os.environ)
    environment.update({'TMPDIR': runner.evidence.tmp, 'PYTHONDONTWRITEBYTECODE': '1',
                        'PYTHONPATH': os.path.join(runner.args.source, 'tests')})
    started = time.monotonic_ns()
    completed = subprocess.run(
        [sys.executable, os.path.join(runner.args.source, 'tests',
                                      'store_open_compatibility_tests.py'),
         str(runner.args.current_binary), str(runner.args.source)],
        cwd=runner.args.source, env=environment, stdin=subprocess.DEVNULL,
        capture_output=True, timeout=180)
    ended = time.monotonic_ns()
    stdout_text = completed.stdout.decode('utf-8', 'replace')
    stderr_text = completed.stderr.decode('utf-8', 'replace')
    # unittest writes results to stderr; parse the combined captured bytes once.
    combined = stdout_text + stderr_text
    methods = sorted(set(re.findall(r'(test_\w+)', combined)))
    record = {'suite': 'tests/store_open_compatibility_tests.py',
              'argv': [sys.executable, 'tests/store_open_compatibility_tests.py',
                       str(runner.args.current_binary), str(runner.args.source)],
              'started_mono_ns': started, 'ended_mono_ns': ended,
              'exit_code': completed.returncode,
              'stdout_sha256': hashlib.sha256(completed.stdout).hexdigest(),
              'stderr_sha256': hashlib.sha256(completed.stderr).hexdigest(),
              'method_names_seen': methods, 'method_count': len(methods),
              'ok_lines': [line for line in combined.splitlines()
                           if line.startswith('OK') or 'Ran ' in line or 'FAILED' in line]}
    with open(os.path.join(row_path, 'suite-stdout.txt'), 'xb') as handle:
        handle.write(completed.stdout)
    with open(os.path.join(row_path, 'suite-stderr.txt'), 'xb') as handle:
        handle.write(completed.stderr)
    record['stdout_text'] = stdout_text
    record['stderr_text'] = stderr_text
    record['status'] = 'SUITE_PASS' if completed.returncode == 0 and len(methods) == 9 \
        else 'SUITE_FAIL'
    return record


ROW_HANDLERS = {
    'c0': row_c0, 'c1': row_c1, 'c2a': row_c2a, 'c2b': row_c2b, 'c3a': row_c3a,
    'c3b': row_c3b, 'c4receipt': row_c4receipt, 'c4': row_c4, 'c5': row_c5,
    'c7a': row_c7a, 'c7b': row_c7b, 'c6': row_c6,
}


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
