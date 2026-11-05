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
# Bounded per-step waits during owned-resource cleanup only; never raises any
# product or row deadline above its original bound.
_CLEANUP_WAIT_S = 5.0

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
try:
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
                    finally:
                        con = None
                if flock_fd is not None:
                    os.close(flock_fd)
                    flock_fd = None
                reply(request, sqlite_close=close_result)
                log('holder_exit', {'sqlite_close': close_result})
                break
            else:
                reply(request, ok=False, error='unknown command')
        except Exception as failure:
            log('command_error', {'command': command,
                                  'error': f'[{type(failure).__name__}] {failure}'})
            reply(request, ok=False, error=f'[{type(failure).__name__}] {failure}')
finally:
    # Minimal owned cleanup: connection, flock, journal and socket each once;
    # an EXIT reply already recorded keeps its actual close error.
    exit_close = 'not_run'
    if con is not None:
        try:
            con.close()
            exit_close = 'ok'
        except Exception as failure:
            exit_close = f'[{type(failure).__name__}] {failure}'
    if flock_fd is not None:
        try:
            os.close(flock_fd)
        except OSError:
            pass
    try:
        journal.close()
    except OSError:
        pass
    try:
        sock.close()
    except OSError:
        pass
    if exit_close not in ('ok', 'not_run'):
        log('final_close_error', {'sqlite_close': exit_close})
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
                    if candidate not in libsqlite:
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
        # Owned-holder registry: holder_start registers a live client only after
        # its HELLO succeeds; close_owned_holders is the idempotent backstop.
        self.owned_holders = []
        self.stop_reason = None

    def close_owned_holders(self):
        """Idempotent backstop close of every holder this runner still owns."""
        facts = []
        while self.owned_holders:
            client = self.owned_holders.pop()
            try:
                fact = client.close()
            except Exception as failure:
                fact = {'close_error': f'[{type(failure).__name__}] {failure}',
                        'pid': getattr(getattr(client, 'process', None), 'pid', None)}
            facts.append(fact)
            effective = fact.get('first_close', fact) if isinstance(fact, dict) else {}
            if effective.get('cleanup_complete') is not True:
                self.stop_reason = self.stop_reason or (
                    f'owned holder cleanup incomplete: {json.dumps(fact, default=str)[:200]}')
        return facts

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
            cleanup_complete = True
        except subprocess.TimeoutExpired:
            timed_out = True
            child.kill()
            try:
                stdout, stderr = child.communicate(timeout=_CLEANUP_WAIT_S)
                cleanup_complete = True
            except subprocess.TimeoutExpired as drain_failure:
                cleanup_complete = False
                stdout = getattr(drain_failure, 'stdout', None) or b''
                stderr = getattr(drain_failure, 'stderr', None) or b''
        ended = time.monotonic_ns()
        record = {'tag': tag, 'argv': [str(binary), *argv], 'cwd': str(cwd),
                  'pid': child.pid, 'start_ticks': start_ticks,
                  'started_mono_ns': started, 'ended_mono_ns': ended,
                  'timed_out': timed_out, 'exit_code': child.returncode,
                  'cleanup_complete': cleanup_complete,
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
        """Start one holder; own every endpoint until HELLO succeeds.

        socket.timeout/TimeoutError during HELLO is a startup failure: after a
        failed Popen both local ends are closed; after a successful Popen the
        parent closes its child_end copy immediately, and a failed HELLO first
        closes the still-held parent socket (the holder sees EOF) and then does
        a bounded wait/terminate/kill/reap of that holder.  The handshake is
        never retried and no further business command is sent.
        """
        parent = child_end = None
        holder = None
        try:
            parent, child_end = socket.socketpair()
            try:
                holder = subprocess.Popen(
                    [sys.executable, self.holder_path,
                     str(child_end.fileno()), str(database), str(journal_path)],
                    pass_fds=(child_end.fileno(),), cwd=self.evidence.tmp,
                    env={**os.environ, 'TMPDIR': self.evidence.tmp,
                         'PYTHONDONTWRITEBYTECODE': '1'},
                    stdin=subprocess.DEVNULL)
            finally:
                # The parent's copy of the child endpoint closes as soon as the
                # Popen outcome is known; a failed Popen means the child never
                # inherited it and the outer finally closes the other end too.
                if child_end is not None:
                    try:
                        child_end.close()
                    except OSError:
                        pass
                    child_end = None
            client = HolderClient(parent, holder)
            try:
                hello = client.command('HELLO')
            except BaseException as failure:
                try:
                    parent.close()  # holder sees EOF first, then bounded reap below
                except OSError:
                    pass
                parent = None
                failure.holder_reap = _bounded_reap(holder, paused=False)
                raise
            parent = None  # ownership transferred to the live client
            holder = None
            self.owned_holders.append(client)
            return client, hello
        finally:
            for endpoint in (parent, child_end):
                if endpoint is not None:
                    try:
                        endpoint.close()
                    except OSError:
                        pass

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
        """One strict observation window; OK requires complete evidence end to end.

        Stability (>=2 complete rounds), a complete member copy with independent
        readback, a complete logical dump with quick_check ok and a real close
        are all prerequisites of verdict OK.  Expected observer refusals are
        saved as failed observations and return DEGRADED/WINDOW_UNSTABLE instead
        of raising past a row's holder cleanup.
        """
        copies = os.path.join(row_path, 'copies', tag)
        record = {'tag': tag}
        try:
            stability = obs.stability_check(database, self.evidence.root, rounds=2)
        except obs.ObservationError as failure:
            record.update({'verdict': 'DEGRADED', 'stability_error': failure.as_dict()})
            self.evidence.json(os.path.join(row_path, f'{tag}-window-error.json'), record)
            return record, None, None
        record['stability_ok'] = stability['stable']
        record['observation_complete'] = stability.get('observation_complete')
        record['incomplete_reasons'] = stability.get('incomplete_reasons')
        record['snapshot_sha256'] = [obs.sha256_json(item)
                                     for item in stability['observations']]
        if not stability['stable']:
            record['verdict'] = 'WINDOW_UNSTABLE'
            record['comparison'] = stability['comparisons']
            return record, None, None
        try:
            copy = obs.frozen_copy(database, copies, self.evidence.root)
        except obs.ObservationError as failure:
            record.update({'verdict': 'DEGRADED', 'copy_error': failure.as_dict()})
            self.evidence.json(os.path.join(row_path, f'{tag}-window-error.json'), record)
            return record, None, None
        try:
            dump = obs.logical_dump(os.path.join(copies, os.path.basename(database)),
                                    self.evidence.root)
        except obs.ObservationError as failure:
            record.update({'verdict': 'DEGRADED', 'dump_error': failure.as_dict()})
            self.evidence.json(os.path.join(row_path, f'{tag}-window-error.json'), record)
            return record, None, None
        completeness = obs.logical_dump_completeness(dump)
        record.update({'copy_readback_all_match': all(entry['matches_original']
                                                      for entry in copy['readback'].values()),
                       'copy_members': sorted(copy['readback']),
                       'member_bytes': {name: entry['size']
                                        for name, entry in copy['readback'].items()},
                       'copy_source_complete': copy.get('source_complete'),
                       'logical_sha256': dump.get('logical_sha256'),
                       'logical_close': dump.get('close'),
                       'sqlite_error': dump.get('sqlite_error'),
                       'user_version': dump.get('pragmas', {}).get('user_version'),
                       'pre_open_members': dump.get('pre_open_members'),
                       'dump_completeness': completeness})
        record['verdict'] = ('OK' if (record['copy_readback_all_match'] and
                                      copy.get('source_complete') and
                                      completeness['observation_complete'] and
                                      not dump.get('sqlite_error') and
                                      copy['readback']) else 'DEGRADED')
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
        self._closed = False
        self._close_record = None

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
        """Idempotent close: EXIT handshake, then bounded exit/reap of the holder.

        The holder's SQLite close reply and its actual process exit are recorded
        as two separate facts.  Every branch closes the local socket; a failing
        EXIT or wait escalates to bounded terminate/kill of our own holder, and a
        forced end never claims a graceful SQLite close.
        """
        if self._closed:
            return {'idempotent_second_close': True, 'first_close': self._close_record,
                    'cleanup_complete': (self._close_record or {}).get('cleanup_complete'),
                    'pid': getattr(self.process, 'pid', None)}
        record = {'pid': getattr(self.process, 'pid', None)}
        try:
            record['exit_reply'] = self.command('EXIT')
            record['sqlite_close_reported'] = record['exit_reply'].get('sqlite_close')
        except Exception as failure:  # EOF, timeout, refused EXIT, socket errors
            record['exit_error'] = f'[{type(failure).__name__}] {failure}'
        try:
            self.sock.close()
            record['socket_closed'] = True
        except Exception as failure:
            record['socket_close_error'] = f'[{type(failure).__name__}] {failure}'
        forced = False
        if self.process.returncode is None:
            reap = _bounded_reap(self.process, paused=False)
            record.update(reap)
            forced = any('terminate' in action or 'kill' in action
                         for action in reap.get('actions', []))
        else:
            record['actions'] = ['already_exited']
        record['process_exit_code'] = self.process.returncode
        record['reaped'] = self.process.returncode is not None
        record['cleanup_complete'] = (record['reaped'] and
                                      record.get('socket_closed') is True and
                                      not record.get('reap_timeout'))
        if forced:
            record['graceful_sqlite_close'] = 'not_established_forced_termination'
        self._closed = True
        self._close_record = record
        return record


def verdict_for_row(row_record):
    """Single-row status per the FINAL plan; failures stay failures."""
    if row_record.get('not_run_reason'):
        return 'NOT_RUN'
    if row_record.get('blocked_reason'):
        return 'BLOCKED'
    return row_record.get('expected_status', 'RECORDED')


# ------------------------------------------------- owned-process and gate helpers
def _bounded_reap(process, *, paused=False):
    """Bounded wait -> terminate -> bounded wait -> kill -> bounded wait.

    Only ever applied to processes this module itself created and still owns; a
    paused process is killed directly because SIGTERM stays pending while stopped.
    """
    record = {'actions': []}
    if process is None:
        record['absent'] = True
        record['reaped'] = True
        record['cleanup_complete'] = True
        return record
    record['pid'] = getattr(process, 'pid', None)
    if process.returncode is None:
        if paused:
            # A stopped process cannot act on SIGTERM and does not exit on its
            # own; the owner kills it directly and then waits, bounded.
            try:
                process.kill()
                record['actions'].append('kill(paused)')
            except Exception as failure:
                record['signal_error'] = str(failure)
            try:
                process.wait(timeout=_CLEANUP_WAIT_S)
            except subprocess.TimeoutExpired:
                try:
                    process.kill()
                    record['actions'].append('kill(escalated)')
                    process.wait(timeout=_CLEANUP_WAIT_S)
                except Exception as failure:
                    record['wait_error'] = str(failure)
                    record['reap_timeout'] = True
            except Exception as failure:
                record['wait_error'] = str(failure)
                record['reap_timeout'] = True
        else:
            try:
                process.wait(timeout=_CLEANUP_WAIT_S)
                record['actions'].append('wait(grace)')
            except subprocess.TimeoutExpired:
                try:
                    process.terminate()
                    record['actions'].append('terminate')
                except Exception as failure:
                    record['signal_error'] = str(failure)
                try:
                    process.wait(timeout=_CLEANUP_WAIT_S)
                except subprocess.TimeoutExpired:
                    try:
                        process.kill()
                        record['actions'].append('kill(escalated)')
                        process.wait(timeout=_CLEANUP_WAIT_S)
                    except Exception as failure:
                        record['wait_error'] = str(failure)
                        record['reap_timeout'] = True
                except Exception as failure:
                    record['wait_error'] = str(failure)
                    record['reap_timeout'] = True
    record['exit_code'] = process.returncode
    record['reaped'] = process.returncode is not None and not record.get('reap_timeout')
    record['cleanup_complete'] = record['reaped']
    return record


def _confirm_stopped(pid, budget_s=_CLEANUP_WAIT_S):
    """Bounded /proc state poll; True only when the owned child is really in T."""
    deadline = time.monotonic() + budget_s
    while time.monotonic() < deadline:
        try:
            with open(f'/proc/{pid}/stat', encoding='utf-8') as handle:
                state = handle.read().split(')')[1].split()[0]
            if state == 'T':
                return True
            if state in ('Z', 'X'):
                return False
        except (OSError, IndexError):
            return False
        time.sleep(0.002)
    return False


def _cleanup_owned_child(child, *, paused=False):
    """Reap and drain one child this row owns; every wait/drain is bounded."""
    facts = {'actions': []}
    if child is None:
        facts['child_absent'] = True
        facts['reaped'] = True
        facts['cleanup_complete'] = True
        return facts
    facts['pid'] = getattr(child, 'pid', None)
    reap = _bounded_reap(child, paused=paused)
    facts['actions'].extend(reap.get('actions', []))
    if reap.get('signal_error'):
        facts['signal_error'] = reap['signal_error']
    if reap.get('wait_error'):
        facts['wait_error'] = reap['wait_error']
    facts['reaped'] = reap['reaped']
    drained = True
    try:
        extra_out, _extra_err = child.communicate(timeout=_CLEANUP_WAIT_S)
        facts['drained'] = True
        facts['drain_bytes'] = len(extra_out or b'')
    except subprocess.TimeoutExpired:
        drained = False
        facts['drain_timeout'] = True
    except Exception as failure:
        drained = False
        facts['drain_error'] = str(failure)
    facts['returncode'] = getattr(child, 'returncode', None)
    facts['cleanup_complete'] = bool(reap['reaped'] and drained)
    return facts


def _window_ok(window):
    return isinstance(window, dict) and window.get('verdict') == 'OK'


def _raw_comparison_complete(comparison):
    return isinstance(comparison, dict) and comparison.get('comparable') is True


def _logical_comparable(logical):
    return isinstance(logical, dict) and logical.get('comparable') is True


def _logical_equal(logical):
    return _logical_comparable(logical) and logical.get('logical_equal') is True


def _step_terminal_observed(step):
    return (isinstance(step, dict) and step.get('exit_code') is not None and
            not step.get('timed_out') and step.get('cleanup_complete', True) is True)


def _c0_gate(comparison, logical, before, after):
    """C0 PASS needs two valid windows, a complete raw comparison that is equal,
    and comparable logic that is equal."""
    reasons = []
    if not (_window_ok(before) and _window_ok(after)):
        reasons.append('window_not_ok')
    if not _raw_comparison_complete(comparison):
        reasons.append('raw_comparison_incomplete')
    elif comparison.get('strict_equal') is not True:
        reasons.append('raw_not_strict_equal')
    if not _logical_comparable(logical):
        reasons.append('logical_comparison_incomplete')
    elif logical.get('logical_equal') is not True:
        reasons.append('logical_not_equal')
    return reasons


def _c1_strict_gate(entry):
    """C1a/C1b STRICT_PASS needs complete evidence, main included, and an
    actually observed terminal state for the subprocess."""
    reasons = []
    before, after = entry.get('before'), entry.get('after')
    if not (_window_ok(before) and _window_ok(after)):
        reasons.append('window_not_ok')
    comparison = entry.get('raw')
    if not _raw_comparison_complete(comparison):
        reasons.append('raw_comparison_incomplete')
    per_file = comparison.get('per_file', {}) if isinstance(comparison, dict) else {}
    main_entry = per_file.get('fixture.sqlite')
    if not (isinstance(main_entry, dict) and main_entry.get('evidence_complete') and
            main_entry.get('raw_equal') is True):
        reasons.append('main_missing_or_incomplete')
    for name, member in per_file.items():
        if member.get('status') in ('added', 'removed'):
            continue
        if not (member.get('evidence_complete') and member.get('raw_equal') is True):
            reasons.append(f'member_incomplete:{name}')
    if isinstance(comparison, dict) and (comparison.get('added') or comparison.get('removed')):
        reasons.append('members_added_or_removed')
    logical = entry.get('logical')
    if not _logical_comparable(logical):
        reasons.append('logical_comparison_incomplete')
    elif logical.get('logical_equal') is not True:
        reasons.append('logical_not_equal')
    if not _step_terminal_observed(entry.get('product')):
        reasons.append('product_terminal_not_observed')
    if entry.get('error_classification') != 'unsupported':
        reasons.append('error_not_unsupported')
    return reasons


def _online_strict_gate(record, expected_error, require_writer_lock=False):
    """C2/C3 rows: two valid windows, complete comparable comparisons, main plus
    non-empty WAL/SHM actually present and equal, and every actually-present
    prefix member complete; empty dicts never satisfy a member requirement."""
    reasons = []
    phases = record.get('phases') or {}
    for label, window in (('before', phases.get('BEFORE')), ('after', phases.get('AFTER'))):
        if not _window_ok(window):
            reasons.append(f'{label}_window_not_ok')
    comparison = record.get('raw')
    if not _raw_comparison_complete(comparison):
        reasons.append('raw_comparison_incomplete')
    logical = record.get('logical')
    if not _logical_comparable(logical):
        reasons.append('logical_comparison_incomplete')
    elif logical.get('logical_equal') is not True:
        reasons.append('logical_not_equal')
    if record.get('error_classification') != expected_error:
        reasons.append('error_classification_mismatch')
    per_file = comparison.get('per_file', {}) if isinstance(comparison, dict) else {}
    member_bytes = record.get('member_bytes') or {}
    if not (isinstance(per_file.get('fixture.sqlite'), dict) and
            per_file['fixture.sqlite'].get('evidence_complete') and
            per_file['fixture.sqlite'].get('raw_equal') is True):
        reasons.append('main_missing_or_incomplete')
    for member, label in (('fixture.sqlite-wal', 'wal'), ('fixture.sqlite-shm', 'shm')):
        entry = per_file.get(member)
        if not (isinstance(entry, dict) and entry.get('evidence_complete') and
                entry.get('raw_equal') is True):
            reasons.append(f'{label}_missing_or_incomplete')
        if not isinstance(member_bytes.get(member), int) or member_bytes.get(member) <= 0:
            reasons.append(f'{label}_empty_or_absent')
    if require_writer_lock:
        entry = per_file.get('fixture.sqlite.writer-lock')
        if not (isinstance(entry, dict) and entry.get('evidence_complete') and
                entry.get('raw_equal') is True):
            reasons.append('writer_lock_missing_or_incomplete')
    for name, entry in per_file.items():
        if entry.get('status') in ('added', 'removed'):
            continue
        if not (entry.get('evidence_complete') and entry.get('raw_equal') is True):
            reasons.append(f'member_incomplete:{name}')
    if isinstance(comparison, dict) and (comparison.get('added') or comparison.get('removed')):
        reasons.append('members_added_or_removed')
    return reasons


def _c4receipt_evaluation(cases, trace_visible):
    """C4receipt: keep the literal classification equivalence, add independent
    pass eligibility; equal classifications never outrank missing evidence and
    two identical failures can never PASS."""
    reasons = []
    for label in ('traced', 'untraced'):
        case = cases[label]
        if not (_window_ok(case.get('before')) and _window_ok(case.get('after'))):
            reasons.append(f'{label}_window_incomplete')
        if not _raw_comparison_complete(case.get('raw')):
            reasons.append(f'{label}_raw_comparison_incomplete')
        if not _logical_comparable(case.get('logical')):
            reasons.append(f'{label}_logical_comparison_incomplete')
        step = case.get('step') or {}
        if step.get('timed_out'):
            reasons.append(f'{label}_timeout')
        if step.get('exit_code') is None:
            reasons.append(f'{label}_exit_not_observed')
        if label == 'traced':
            if step.get('cleanup_complete') is not True:
                reasons.append('traced_cleanup_incomplete')
            if step.get('stream_complete') is not True:
                reasons.append('traced_stream_incomplete')
    left, right = cases['traced']['classification'], cases['untraced']['classification']
    equivalent = all(left[key] == right[key] for key in
                     ('exit_code', 'error', 'added', 'removed', 'changed', 'main_equal',
                      'business_equal'))
    pass_eligible = not reasons
    status = ('PASS' if pass_eligible and equivalent and trace_visible else
              'INCOMPLETE' if not pass_eligible else 'FAIL')
    return {'status': status, 'pass_eligible': pass_eligible,
            'not_pass_reasons': reasons,
            'mechanism': {'trace_shm_open_visible': bool(trace_visible),
                          'traced_untraced_equivalent': equivalent}}


def _c5_business_gate(before, after, logical):
    """C5 business projection needs both windows OK and comparable logic; a
    missing dump can never derive 'business rows unchanged' from differences=[]."""
    reasons = []
    if not (_window_ok(before) and _window_ok(after)):
        reasons.append('window_not_ok')
    if not _logical_comparable(logical):
        reasons.append('logical_not_comparable')
    return reasons


def _c7_support_gate(product_exit_code, before, after, logical, populated_tables):
    reasons = []
    if product_exit_code != 0:
        reasons.append('product_exit_nonzero')
    if not (_window_ok(before) and _window_ok(after)):
        reasons.append('window_not_ok')
    if not _logical_equal(logical):
        reasons.append('logical_not_comparable_or_not_equal')
    if not any(populated_tables.values()):
        reasons.append('fixture_not_populated')
    return reasons


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
        if runner.stop_reason:
            record = {'status': 'NOT_RUN',
                      'not_run_reason': ('previous row left unfinished owned resources; '
                                         'refusing to continue with locks or stopped '
                                         f'processes: {runner.stop_reason}')}
            record['elapsed_s'] = 0
            record['row'] = row
            evidence.json(os.path.join(row_path, 'row.json'), record)
            results[row] = record
            continue
        started = time.monotonic_ns()
        record = None
        try:
            try:
                record = handler(runner, row_path)
            finally:
                # Idempotent backstop: a row that already closed its holder sees
                # no-op facts; an unfinished cleanup keeps later rows NOT_RUN.
                cleanup_facts = runner.close_owned_holders()
                if isinstance(record, dict):
                    record.setdefault('owned_holder_cleanup', cleanup_facts)
                try:
                    evidence.json(os.path.join(row_path, 'owned-cleanup.json'),
                                  {'row': row, 'holders': cleanup_facts,
                                   'stop_reason': runner.stop_reason})
                except Exception:
                    pass
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
    gate_reasons = _c0_gate(comparison, logical, before, after)
    record['verdicts'] = {
        'observer_non_perturbing': bool(
            comparison and comparison.get('comparable') and comparison['strict_equal']),
        'logical_equal': _logical_equal(logical),
        'gate_reasons': gate_reasons,
        'comparison': comparison, 'logical': logical}
    record['phases']['HOLDER_CLOSE'] = client.close()
    record['phases']['POST_CLOSE_MEMBERS'] = sorted(
        name for name in os.listdir(fixture) if name.startswith('fixture.sqlite'))
    record['status'] = 'PASS' if not gate_reasons else 'FAIL'
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
            'business_equal': _logical_equal(logical)}
    cases = record['cases']
    record['verdicts'] = {}
    for case in cases:
        entry = cases[case]
        record['verdicts'][case] = {
            'refused_unsupported': entry['error_classification'] == 'unsupported',
            'main_equal': entry['main_equal'],
            'business_equal': entry['business_equal'],
            'gate_reasons': _c1_strict_gate(entry),
            'sidecar_added': _raw_field(entry['raw'], 'added'),
            'sidecar_removed': _raw_field(entry['raw'], 'removed')}
    strict = all(not entry['gate_reasons'] for entry in record['verdicts'].values())
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
    record['member_bytes'] = dict(before.get('member_bytes') or {})
    for member, size in (after.get('member_bytes') or {}).items():
        record['member_bytes'][member] = max(record['member_bytes'].get(member, 0), size)
    record['verdicts'] = {
        'error': record['error_classification'],
        'main': _per_file(comparison, 'fixture.sqlite'),
        'wal': _per_file(comparison, 'fixture.sqlite-wal'),
        'shm': _per_file(comparison, 'fixture.sqlite-shm'),
        'writer_lock': _per_file(comparison, 'fixture.sqlite.writer-lock'),
        'added': _raw_field(comparison, 'added'),
        'removed': _raw_field(comparison, 'removed'),
        'business_product_equal': _logical_equal(logical)}
    return record


def row_c2a(runner, row_path):
    record = _online_row(runner, row_path, 'c2a', 'pending',
                         note='C2a: non-empty stable online WAL (main header 411), old410 pending')
    record['gate_reasons'] = _online_strict_gate(record, 'unsupported')
    record['status'] = 'STRICT_PASS' if not record['gate_reasons'] else 'STRICT_FAIL'
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
    record['member_bytes'] = dict(record['phases']['BEFORE'].get('member_bytes') or {})
    for member, size in (record['phases']['AFTER'].get('member_bytes') or {}).items():
        record['member_bytes'][member] = max(record['member_bytes'].get(member, 0), size)
    record['verdicts'] = {
        'error': record['error_classification'],
        'main': _per_file(comparison, 'fixture.sqlite'),
        'wal': _per_file(comparison, 'fixture.sqlite-wal'),
        'shm': _per_file(comparison, 'fixture.sqlite-shm'),
        'writer_lock': _per_file(comparison, 'fixture.sqlite.writer-lock'),
        'added': _raw_field(comparison, 'added'),
        'removed': _raw_field(comparison, 'removed'),
        'business_product_equal': _logical_equal(logical)}
    record['gate_reasons'] = _online_strict_gate(record, 'unsupported')
    record['status'] = 'STRICT_PASS' if not record['gate_reasons'] else 'STRICT_FAIL'
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
    construction_copy = obs.frozen_copy(database, copies, runner.evidence.root)
    dump = obs.logical_dump(os.path.join(copies, 'fixture.sqlite'), runner.evidence.root)
    copy_version = dump.get('pragmas', {}).get('user_version')
    construction_copy_complete = bool(
        construction_copy.get('source_complete') and construction_copy['readback'] and
        all(entry['matches_original'] for entry in construction_copy['readback'].values()))
    construction_logical = obs.logical_dump_completeness(dump)
    construction_logical_complete = construction_logical['observation_complete']
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
        'construction_copy_complete': construction_copy_complete,
        'construction_logical_complete': construction_logical_complete,
        'construction_logical_reasons': construction_logical['reasons'],
        'copy_user_version_is_411': copy_version == [{'k': 'int', 'v': 411}],
    }
    receipt['construction_status'] = (
        'CONSTRUCTED' if migration_step['exit_code'] == 0 and main_after_version == 410
        and not receipt['main_changed'] and receipt['copy_user_version_is_411'] and
        receipt['wal_bytes_after'] > 32 and construction_copy_complete and
        construction_logical_complete else 'CONSTRUCTION_FAILED')
    runner.evidence.json(os.path.join(journal_dir, 'state2-receipt.json'), receipt)
    return fixture, database, receipt, client, hello


def row_c3a(runner, row_path):
    record = _online_row(runner, row_path, 'c3a', 'init', flock=True,
                         note='C3a: real Store lifecycle flock held; Conflict expected before '
                              'any user_version execution; recorded as its own single column')
    record['gate_reasons'] = _online_strict_gate(record, 'conflict', require_writer_lock=True)
    record['status'] = 'STRICT_PASS' if not record['gate_reasons'] else 'STRICT_FAIL'
    return record


def row_c3b(runner, row_path):
    record = _online_row(runner, row_path, 'c3b', 'init', flock=False,
                         note='C3b: no Store flock; the real old-init path (not the Conflict '
                              'path) is recorded as its own single column')
    record['gate_reasons'] = _online_strict_gate(record, 'unsupported')
    record['status'] = 'STRICT_PASS' if not record['gate_reasons'] else 'STRICT_FAIL'
    return record


def _trace_old(runner, row_path, name, command, database, fixture, kill=False):
    """Run the old ELF under strace for normal, uninterrupted observation only.

    This helper starts, streams and waits for the strace TRACER process: the
    returned pid/start_ticks/exit_code describe that tracer, never the old-ELF
    tracee, whose identity is not obtained.  kill=True is rejected before any
    Popen, signal, fixture or trace-file effect: a controlled termination at a
    tracee syscall stop was never established (the historical attempt only ever
    signalled the tracer), so this runner must not attempt it.

    The timeout model is explicit and one-way: an exhausted poll window or a
    communicate TimeoutExpired (or a cleanup timeout) sets timed_out=True and it
    is never reset by a later kill/reap success; data returned after the deadline
    never turns the window back into a normal one.  TimeoutExpired.output is kept
    exactly once, failures keep a marked prefix, and the tracer's own pipes are
    closed and the tracer reaped by bounded waits in this helper's finally.  That
    is tracer cleanup, not product exit or reap proof.
    """
    if kill:
        raise ValueError('_trace_old kill mode is removed: no proven old-ELF '
                         'tracee identity or syscall-stop rendezvous exists; '
                         'controlled kill of the traced product is not '
                         'supported by this runner')
    trace_path = os.path.join(row_path, f'{name}.strace')
    stderr_path = os.path.join(row_path, f'{name}.stderr.bin')
    argv = ['strace', '-ttt', '-yy', '-s', '256', '-e', f'trace={STRACE_EVENTS}',
            '-o', '/dev/stdout', str(runner.args.old_elf), command, '--db', str(database),
            '--source-root', str(runner.args.old_source)]
    environment = dict(os.environ)
    environment['TMPDIR'] = runner.evidence.tmp
    started = time.monotonic_ns()
    receipt = {
        'tag': name, 'argv': argv,
        'child_role': 'strace tracer; the old-ELF tracee identity is never '
                      'obtained by this helper',
        'tracee_identity': 'UNKNOWN', 'tracee_exit': 'NOT_ESTABLISHED',
        'tracee_reap': 'NOT_ESTABLISHED',
        'timed_out': False, 'poll_deadline_exceeded': False, 'communicate_timeout': False,
        'actions': [], 'stream_complete': False, 'output_complete': False,
        'cleanup_complete': False, 'tracer_reaped': False, 'evidence_complete': False,
        'start_ticks': 'UNKNOWN',
    }
    rendezvous = {'shm_open_seen_mono_ns': None, 'pattern': 'fixture.sqlite-shm'}
    child = None
    collected = bytearray()
    stderr_bytes = b''
    interrupt = None
    import select as _select
    try:
        child = subprocess.Popen(argv, cwd=fixture, env=environment,
                                 stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE)
        receipt['pid'] = child.pid
        try:
            with open(f'/proc/{child.pid}/stat', encoding='utf-8') as handle:
                receipt['start_ticks'] = int(handle.read().split(')')[-1].split()[19])
        except (OSError, ValueError, IndexError):
            pass  # start_ticks stays UNKNOWN; ownership does not depend on /proc
        stream_fd = child.stdout.fileno()
        os.set_blocking(stream_fd, False)
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
        if child.poll() is None:
            receipt['poll_deadline_exceeded'] = True
            receipt['timed_out'] = True
            receipt['actions'].append('poll_deadline_exhausted')
        os.set_blocking(stream_fd, True)
        try:
            remaining, stderr_bytes = child.communicate(timeout=HANDSHAKE_TIMEOUT_S)
            if remaining:
                collected.extend(remaining)
            receipt['stream_complete'] = True
        except subprocess.TimeoutExpired as failure:
            receipt['communicate_timeout'] = True
            receipt['timed_out'] = True
            partial = getattr(failure, 'output', None) or b''
            if partial:
                collected.extend(partial)
                receipt['communicate_partial_bytes'] = len(partial)
            try:
                child.kill()  # tracer-only stream cleanup; not product exit/reap proof
                receipt['actions'].append('kill(tracer_stream_cleanup)')
            except Exception as kill_failure:
                receipt['kill_error'] = str(kill_failure)
            try:
                rest, stderr_bytes = child.communicate(timeout=_CLEANUP_WAIT_S)
                if rest:
                    collected.extend(rest)
                receipt['stream_complete'] = True
            except subprocess.TimeoutExpired:
                receipt['cleanup_timeout'] = 'drain'
                receipt['output_prefix_only'] = True
        receipt['ended_mono_ns'] = time.monotonic_ns()
    except (KeyboardInterrupt, SystemExit) as failure:
        interrupt = failure
        receipt['helper_interrupt'] = f'[{type(failure).__name__}] {failure}'
    except Exception as failure:
        receipt['helper_error'] = f'[{type(failure).__name__}] {failure}'
    finally:
        if child is not None:
            if child.returncode is None and 'kill(tracer_stream_cleanup)' not in receipt['actions']:
                try:
                    child.kill()
                    receipt['actions'].append('kill(exception_cleanup)')
                except Exception as kill_failure:
                    receipt['kill_error'] = receipt.get('kill_error', str(kill_failure))
            if child.returncode is None:
                try:
                    child.wait(timeout=_CLEANUP_WAIT_S)
                except subprocess.TimeoutExpired:
                    receipt['cleanup_timeout'] = receipt.get('cleanup_timeout', 'reap')
                except Exception as wait_failure:
                    receipt['wait_error'] = str(wait_failure)
            receipt['exit_code'] = child.returncode
            receipt['tracer_reaped'] = child.returncode is not None
            for stream in (child.stdout, child.stderr):
                if stream is not None:
                    try:
                        stream.close()
                    except OSError:
                        pass
        receipt['rendezvous'] = rendezvous
        receipt['trace_file'] = trace_path
        receipt['trace_bytes'] = len(collected)
        receipt['trace_sha256'] = hashlib.sha256(bytes(collected)).hexdigest()
        receipt['stderr_file'] = stderr_path
        receipt['stderr_bytes'] = len(stderr_bytes)
        receipt['stderr_sha256'] = hashlib.sha256(stderr_bytes).hexdigest()
        receipt['stderr_text'] = stderr_bytes.decode('utf-8', 'replace')[:4096]
        receipt['error_classification'] = runner.error_classification(stderr_bytes)
        receipt['started_mono_ns'] = started
        try:
            with open(trace_path, 'xb') as handle:
                handle.write(bytes(collected))
            with open(stderr_path, 'xb') as handle:
                handle.write(stderr_bytes)
            receipt['evidence_complete'] = True
        except Exception as save_failure:
            receipt['evidence_error'] = f'[{type(save_failure).__name__}] {save_failure}'
        receipt['output_complete'] = bool(receipt['stream_complete'] and
                                          receipt['evidence_complete'])
        receipt['cleanup_complete'] = bool(receipt['tracer_reaped'] and
                                           receipt['stream_complete'] and
                                           not receipt.get('cleanup_timeout'))
    if interrupt is not None:
        raise interrupt
    return receipt


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
    trace_visible = bool(cases['traced']['step'].get('rendezvous', {}).get(
        'shm_open_seen_mono_ns'))
    evaluation = _c4receipt_evaluation(cases, trace_visible)
    record = {'cases': cases,
              'mechanism': evaluation['mechanism'],
              'pass_eligible': evaluation['pass_eligible'],
              'not_pass_reasons': evaluation['not_pass_reasons'],
              'scope': 'normal-comparison receipt only: one traced and one untraced '
                       'uninterrupted refusal on isomorphic fixtures plus SHM-text '
                       'visibility in the streamed trace; it verifies no kill '
                       'rendezvous, identifies no tracee, and must never be used as '
                       'a gate for controlled-kill execution; equivalence of normal '
                       'runs does not imply non-perturbation under interruption',
              'note': 'single run each; no retry to green; any difference limits C4 '
                      'comparability and is reported to the plan author'}
    record['status'] = evaluation['status']
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
    """C5 coexistence diagnostic with owned resources under one finally.

    A partial record and the ownership state exist before any holder or child is
    created; every exception path from holder_start through HELLO/OPEN/WRITE/
    QUIESCE, the BEFORE window, old-ELF verification, Popen, SIGSTOP, state
    confirmation, the concurrent WRITE, SIGCONT, communicate, the AFTER window and
    close enters the same cleanup.  The first exception and the phases that
    happened are preserved; later cleanup errors are appended, never overwrite it.
    Only processes this row itself created are signalled, always through their own
    Popen handle; every cleanup wait/drain is bounded.  The row's normal exit and
    any partial stdout/stderr are saved as raw files with completeness marks.
    """
    record = {'phases': {}, 'note':
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
    client = None
    child = None
    child_facts = {'child_role': 'direct old-ELF child; no tracer involved',
                   'pid': None, 'start_ticks': 'UNKNOWN',
                   'stop_sent': False, 'stopped_confirmed': False,
                   'cont_sent': False, 'returncode': 'UNKNOWN', 'timed_out': False}
    phases_completed = []
    phase = 'entry'
    stdout = stderr = None
    output_complete = False
    first_error = None
    interrupt = None
    copy_b = dump_b = None
    commit = None
    try:
        phase = 'fixture'
        fixture, database, steps, request = runner.build_static_411('c5')
        record.update({'fixture': fixture, 'construction': steps, 'request': request})
        phases_completed.append(phase)
        phase = 'holder_start'
        client, hello = runner.holder_start(database, os.path.join(row_path, 'holder.journal'))
        record['holder_hello'] = hello
        phases_completed.append(phase)
        phase = 'wal_ready'
        record['phases']['WAL_READY'] = {'open': client.command('OPEN'),
                                         'write': client.command('WRITE', token='c5-fact')}
        phases_completed.append(phase)
        phase = 'quiesce'
        record['phases']['WRITER_QUIESCED'] = client.command('QUIESCE')
        phases_completed.append(phase)
        phase = 'before_window'
        before, copy_b, dump_b = runner.strict_window(row_path, database, 'before')
        record['phases']['BEFORE'] = before
        phases_completed.append(phase)
        phase = 'old_elf_verified'
        runner.verify_old_elf()
        phases_completed.append(phase)
        phase = 'child_spawn'
        started = time.monotonic_ns()
        child = subprocess.Popen(
            [str(runner.args.old_elf), 'pending', '--db', str(database),
             '--source-root', str(runner.args.old_source)], cwd=fixture,
            env={**os.environ, 'TMPDIR': runner.evidence.tmp},
            stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        child_facts['pid'] = child.pid
        try:
            with open(f'/proc/{child.pid}/stat', encoding='utf-8') as handle:
                child_facts['start_ticks'] = int(handle.read().split(')')[-1].split()[19])
        except (OSError, ValueError, IndexError):
            pass  # UNKNOWN start_ticks; Popen ownership still drives cleanup
        phases_completed.append(phase)
        phase = 'sigstop'
        stop_send_error = None
        try:
            child.send_signal(signal.SIGSTOP)
            child_facts['stop_sent'] = True
        except OSError as failure:
            stop_send_error = str(failure)
        stop_sent = time.monotonic_ns()
        phases_completed.append(phase)
        phase = 'stop_confirm'
        stopped_confirmed = (_confirm_stopped(child.pid)
                             if stop_send_error is None else False)
        child_facts['stopped_confirmed'] = stopped_confirmed
        phases_completed.append(phase)
        if not stopped_confirmed:
            record['deviation'] = ('SIGSTOP rendezvous failed; the concurrent WRITE is '
                                   'not executed and the child is reclaimed')
            if stop_send_error is not None:
                record['phases']['OLD_OPEN'] = {'stop_error': stop_send_error}
            record['status'] = 'RECORDED_DEVIATION'
            record['verdicts'] = {'commit_within_child_window': None,
                                  'old_refusal': None,
                                  'business_product_rows_unchanged': None,
                                  'authorized_controlled_delta': None,
                                  'shared_file_changes_retained': None,
                                  'raw_equality_claimed': False,
                                  'gate_reasons': ['stop_state_not_confirmed']}
            return record
        phase = 'concurrent_write'
        commit = client.command('WRITE', token='c5-concurrent-commit')
        record['phases']['CONCURRENT_COMMIT'] = commit
        phases_completed.append(phase)
        phase = 'sigcont'
        try:
            child.send_signal(signal.SIGCONT)
            child_facts['cont_sent'] = True
        except OSError as failure:
            record['cont_error'] = str(failure)
        cont_sent = time.monotonic_ns()
        phases_completed.append(phase)
        phase = 'child_exit'
        try:
            stdout, stderr = child.communicate(timeout=ROW_TIMEOUT_S)
            output_complete = True
        except subprocess.TimeoutExpired as failure:
            child_facts['timed_out'] = True
            partial = getattr(failure, 'output', None)
            stdout = partial if partial is not None else b''
            stderr = getattr(failure, 'stderr', None)
            stderr = stderr if stderr is not None else b''
            record['child_communicate_timeout'] = True
            try:
                child.kill()
            except Exception as kill_failure:
                record['child_kill_error'] = str(kill_failure)
            try:
                more_out, more_err = child.communicate(timeout=_CLEANUP_WAIT_S)
                stdout = (stdout or b'') + (more_out or b'')
                stderr = (stderr or b'') + (more_err or b'')
                output_complete = True
            except subprocess.TimeoutExpired:
                record['child_drain_timeout'] = True
        exited = time.monotonic_ns()
        child_facts['returncode'] = child.returncode
        record['phases']['OLD_OPEN'] = {
            'tag': 'c5-concurrent',
            'argv': [str(runner.args.old_elf), 'pending', '--db', str(database),
                     '--source-root', str(runner.args.old_source)],
            'pid': child.pid, 'start_ticks': child_facts['start_ticks'],
            'started_mono_ns': started, 'stop_sent_mono_ns': stop_sent,
            'stopped_state_confirmed': stopped_confirmed,
            'commit_mono_ns': commit['mono_ns'], 'cont_sent_mono_ns': cont_sent,
            'ended_mono_ns': exited,
            'commit_within_child_window': started < commit['mono_ns'] < exited,
            'exit_code': child.returncode, 'timed_out': child_facts['timed_out'],
            'stdout_sha256': hashlib.sha256(stdout).hexdigest(),
            'stderr_sha256': hashlib.sha256(stderr).hexdigest(),
            'stderr_text': stderr.decode('utf-8', 'replace')[:4096],
            'error_classification': runner.error_classification(stderr)}
        phases_completed.append(phase)
        phase = 'old_elf_reverified'
        runner.verify_old_elf()
        phases_completed.append(phase)
        phase = 'after_window'
        after, copy_a, dump_a = runner.strict_window(row_path, database, 'after')
        record['phases']['AFTER'] = after
        phases_completed.append(phase)
        phase = 'holder_close'
        record['phases']['HOLDER_CLOSE'] = client.close()
        phases_completed.append(phase)
        phase = 'comparison'
        record['raw'] = runner.raw_between(copy_b, copy_a) if copy_b and copy_a else None
        record['logical'] = obs.diff_logical(dump_b, dump_a) if dump_b and dump_a else None
        gate_reasons = _c5_business_gate(before, after, record['logical'])
        differences = (record['logical']['differences']
                       if record['logical'] and not gate_reasons else None)
        if differences is None:
            product_changes = None
            controlled_changes = None
        else:
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
            'business_product_rows_unchanged': (None if product_changes is None
                                                else not product_changes),
            'authorized_controlled_delta': controlled_changes,
            'shared_file_changes_retained':
                (record['raw'] or {}).get('per_file', {}),
            'raw_equality_claimed': False,
            'gate_reasons': gate_reasons}
        record['status'] = ('RECORDED_OK' if all((
            record['verdicts']['commit_within_child_window'],
            record['verdicts']['old_refusal'],
            record['verdicts']['business_product_rows_unchanged'])) else
            'RECORDED_DEVIATION')
        return record
    except (KeyboardInterrupt, SystemExit) as failure:
        interrupt = failure
        first_error = failure
    except Exception as failure:
        first_error = failure
    finally:
        cleanup_errors = []
        child_cleanup = None
        try:
            paused_for_cleanup = bool(child_facts['stopped_confirmed'] and
                                      not child_facts['cont_sent'] and
                                      child is not None and child.returncode is None)
            child_cleanup = _cleanup_owned_child(child, paused=paused_for_cleanup)
            record['owned_child_cleanup'] = child_cleanup
            if child_cleanup.get('cleanup_complete') is not True:
                runner.stop_reason = runner.stop_reason or (
                    f'c5 child cleanup incomplete: pid={child_facts.get("pid")}')
        except Exception as failure:
            cleanup_errors.append(f'child: [{type(failure).__name__}] {failure}')
        holder_close = None
        try:
            if client is not None and not getattr(client, '_closed', False):
                holder_close = client.close()
                record['phases']['HOLDER_CLOSE'] = holder_close
            elif client is not None:
                holder_close = {'already_closed_by_row': True}
        except Exception as failure:
            cleanup_errors.append(f'holder: [{type(failure).__name__}] {failure}')
        record['phases_completed'] = phases_completed
        record['child_facts'] = child_facts
        output_files = {}
        for label, payload in (('stdout', stdout), ('stderr', stderr)):
            if payload is None:
                continue
            path = os.path.join(row_path, f'c5-child-{label}.bin')
            try:
                os.makedirs(row_path, exist_ok=True)
                with open(path, 'xb') as handle:
                    handle.write(payload)
                output_files[label] = {'path': path, 'bytes': len(payload),
                                       'sha256': hashlib.sha256(payload).hexdigest(),
                                       'complete': output_complete,
                                       'incomplete_reason': None if output_complete else
                                       'communicate window incomplete'}
            except Exception as failure:
                output_files[label] = {'path': path, 'bytes': len(payload),
                                       'complete': False,
                                       'save_error': f'[{type(failure).__name__}] {failure}'}
        if output_files:
            record['child_output_files'] = output_files
        if cleanup_errors:
            record['cleanup_errors'] = cleanup_errors
        if first_error is not None and not interrupt:
            record['status'] = 'ERROR'
            record['error'] = f'[{type(first_error).__name__}] {first_error}'
            record['error_phase'] = phase
        try:
            os.makedirs(row_path, exist_ok=True)
            obs.write_json(os.path.join(row_path, 'c5-owned-cleanup.json'),
                           {'phases_completed': phases_completed, 'error_phase': phase,
                            'error': (f'[{type(first_error).__name__}] {first_error}'
                                      if first_error is not None else None),
                            'child_facts': child_facts,
                            'owned_child_cleanup': child_cleanup,
                            'holder_close': holder_close,
                            'cleanup_errors': cleanup_errors})
        except Exception as failure:
            record['owned_cleanup_evidence_error'] = str(failure)
    if interrupt is not None:
        raise interrupt
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
    record['gate_reasons'] = _c7_support_gate(
        step['exit_code'], before, after, logical, record['populated_tables'])
    record['status'] = ('SUPPORT_CONTROL_PASS' if not record['gate_reasons'] else
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
    record['gate_reasons'] = _c7_support_gate(
        step['exit_code'], before, after, logical, record['populated_tables'])
    record['status'] = ('SUPPORT_CONTROL_PASS' if not record['gate_reasons'] else
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
