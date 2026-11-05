#!/usr/bin/env python3
"""Self-verification of the strict compatibility observer before any product use.

Manually invoked evidence tool, deliberately NOT registered as a CTest suite.
Positive cases prove observer-only stability, clean frozen copies and that a
uniquely committed WAL row is visible through copy semantics.  Negative cases
tamper only their own throwaway fixtures and must be detected with the exact
field and reason, never merely by a nonzero exit code.  Conditional cases
(same-mode fchmod ctime, atime regime control) honestly report
NOT_ESTABLISHED when their trigger condition does not hold instead of
fabricating a hit.  No historical observer, log or product fixture is touched.

The 2026-11-05 mechanism regressions (F-04/F-05/F-06/F-O3) verify evidence
semantics for the observer helpers, the lifecycle runner rows and the holder /
tracer boundaries; they load the lifecycle module by path (never its CLI main),
use only their own throwaway fixtures, own stock children and stubs, and
intercept every product or signal effect before it becomes real.
"""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import shutil
import signal
import sqlite3
import stat
import sys
import time
import types

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import store_compatibility_observer as obs  # noqa: E402

PAGE = 4096
FRAME = 24 + PAGE  # WAL frame header + one page


class Case:
    def __init__(self, name, kind):
        self.name, self.kind = name, kind
        self.record = {'case': name, 'kind': kind, 'status': 'FAIL', 'fields': {},
                       'reason': 'not executed'}

    def finish(self, status, reason, **fields):
        self.record['status'] = status
        self.record['reason'] = reason
        self.record['fields'].update(fields)
        return self.record


def utc():
    import datetime
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


class Lab:
    """Throwaway fixture factory; every case gets its own fresh directory."""

    def __init__(self, root):
        self.root = root
        self.counter = 0

    def fixture(self, case, populated=True):
        self.counter += 1
        directory = os.path.join(self.root, f'fixture-{case}-{self.counter:03d}')
        os.makedirs(directory, mode=0o700)
        database = os.path.join(directory, 'fixture.sqlite')
        connection = sqlite3.connect(database, isolation_level=None)
        connection.execute('PRAGMA journal_mode=WAL')
        connection.execute('PRAGMA wal_autocheckpoint=0')
        if populated:
            connection.execute('CREATE TABLE business(id INTEGER PRIMARY KEY, payload TEXT)')
            connection.execute('BEGIN')
            connection.execute("INSERT INTO business VALUES (1, 'admitted-original-row')")
            connection.execute('COMMIT')
        return directory, database, connection

    def copy_fixture(self, case, directory):
        """Independent negative fixture: a private copy of a fixture family."""
        self.counter += 1
        target = os.path.join(self.root, f'fixture-{case}-{self.counter:03d}')
        os.makedirs(target, mode=0o700)
        for name in sorted(os.listdir(directory)):
            if name.startswith('fixture.sqlite'):
                shutil.copyfile(os.path.join(directory, name), os.path.join(target, name))
        return target, os.path.join(target, 'fixture.sqlite')


def _wal_path(database):
    return database + '-wal'


def _wal_frames(database):
    size = os.path.getsize(_wal_path(database))
    return (size - 32) // FRAME if size >= 32 else 0


# ------------------------------------------------------------------ positives
def case_noop_stability(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        result = obs.stability_check(database, lab.root, rounds=2)
        stable = result['stable'] and all(item['diff_count'] == 0
                                          for item in result['comparisons'])
        return case.finish('PASS' if stable else 'FAIL',
                           'two consecutive observer-only observations identical including times'
                           if stable else 'observations differ without any participant',
                           rounds=2, members=sorted(result['observations'][0]['members']))
    finally:
        connection.close()


def case_clean_copy(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        result = obs.frozen_copy(database, os.path.join(directory, '..', f'copy-{case.name}'), lab.root)
        all_ok = all(entry['matches_original'] for entry in result['readback'].values())
        return case.finish('PASS' if all_ok else 'FAIL',
                           'clean frozen copy readback equals the frozen originals'
                           if all_ok else 'clean copy failed readback',
                           readback={name: entry['matches_original']
                                     for name, entry in result['readback'].items()})
    finally:
        connection.close()


def case_committed_wal_row(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        connection.execute('BEGIN')
        connection.execute("INSERT INTO business VALUES (2, 'wal-only-committed-row')")
        connection.execute('COMMIT')
        copy_dir = os.path.join(directory, '..', f'copy-{case.name}')
        result = obs.frozen_copy(database, copy_dir, lab.root)
        # Read the copy's main bytes BEFORE the logical dump: the dump's explicit
        # close checkpoints the copy WAL into its main file by design.
        main_bytes = open(os.path.join(copy_dir, 'fixture.sqlite'), 'rb').read()
        only_in_wal = b'wal-only-committed-row' not in main_bytes
        dump = obs.logical_dump(os.path.join(copy_dir, 'fixture.sqlite'), lab.root)
        row = dump.get('tables', {}).get('business', {}).get('rows', [])
        visible = any(cell.get('v') == 'wal-only-committed-row' for r in row for cell in r)
        status = 'PASS' if (visible and only_in_wal) else 'FAIL'
        return case.finish(status,
                           'committed WAL row visible via copy semantics and absent from main bytes'
                           if status == 'PASS' else 'committed WAL fact not proven wal-only',
                           visible=visible, absent_from_main=only_in_wal,
                           wal_bytes=result['original_before']['members']['fixture.sqlite-wal']['bytes'])
    finally:
        connection.close()


# ------------------------------------------------------------------ negatives
def _tamper_copier(tamper):
    """Wrap the observer's copy primitive so tampering lands inside the copy."""
    original = obs._copy_stream

    def wrapper(source, destination):
        result = original(source, destination)
        tamper(source, destination)
        return result
    return original, wrapper


def _copy_tamper_case(lab, case, tamper, expect_name_in=None, expect_field=None):
    directory, database, connection = lab.fixture(case.name)
    try:
        original_stream, wrapper = _tamper_copier(tamper)
        obs._copy_stream = wrapper
        try:
            obs.frozen_copy(database, os.path.join(directory, '..', f'copy-{case.name}'), lab.root)
        except obs.ObservationError as failure:
            detail = failure.as_dict()['detail'] or {}
            named = (expect_name_in is None or detail.get('name') in expect_name_in)
            fielded = (expect_field is None or expect_field in detail or
                       detail.get('missing') or detail.get('copy_kind'))
            return case.finish('PASS' if (named and fielded) else 'FAIL',
                               f'detected via {failure.reason}',
                               reason_code=failure.reason, detail=json.loads(
                                   json.dumps(detail, default=str)))
        return case.finish('FAIL', 'tampered copy was not detected')
    finally:
        obs._copy_stream = original_stream
        connection.close()


def case_copy_same_length(lab, case):
    def tamper(source, destination):
        data = bytearray(open(destination, 'rb').read())
        if len(data) > 200:
            data[150] ^= 0xFF
            open(destination, 'wb').write(data)
    return _copy_tamper_case(lab, case, tamper, expect_field='sha256')


def case_copy_truncation(lab, case):
    def tamper(source, destination):
        data = open(destination, 'rb').read()
        if len(data) > 64:
            open(destination, 'wb').write(data[:len(data) - 64])
    return _copy_tamper_case(lab, case, tamper, expect_field='size')


def case_copy_missing_member(lab, case):
    def tamper(source, destination):
        if destination.endswith('-shm'):
            os.unlink(destination)
    return _copy_tamper_case(lab, case, tamper)


def case_copy_wrong_mapping(lab, case):
    def tamper(source, destination):
        if destination.endswith('fixture.sqlite-wal'):
            wal = open(destination, 'rb').read()
            main_path = destination[:-4]
            main = open(main_path, 'rb').read()
            open(destination, 'wb').write(main)
            open(main_path, 'wb').write(wal)
    return _copy_tamper_case(lab, case, tamper,
                             expect_name_in=('fixture.sqlite', 'fixture.sqlite-wal'))


def _observation_diff_case(lab, case, mutate, member, restore_times=False):
    directory, database, connection = lab.fixture(case.name)
    try:
        before = obs.snapshot(database, lab.root)
        target = os.path.join(directory, member)
        saved = os.lstat(target)
        mutate(target)
        if restore_times:
            os.utime(target, ns=(saved.st_atime_ns, saved.st_mtime_ns))
        after = obs.snapshot(database, lab.root)
        comparison = obs.compare_snapshots(before, after)
        entry = comparison['per_file'].get(member, {})
        return case.finish('PASS' if entry.get('content_equal') is False or
                           entry.get('status') in ('added', 'removed') or
                           entry.get('metadata_equal') is False else 'FAIL',
                           f'detected for {member}',
                           member=member, per_file=json.loads(json.dumps(entry, default=str)),
                           added=comparison['added'], removed=comparison['removed'])
    finally:
        connection.close()


def case_wal_same_length_change(lab, case):
    def mutate(target):
        data = bytearray(open(target, 'rb').read())
        if len(data) > 32 + 24 + 260:
            data[32 + 24 + 200] ^= 0xFF
            open(target, 'wb').write(data)
    return _observation_diff_case(lab, case, mutate, 'fixture.sqlite-wal', restore_times=True)


def case_shm_only_change(lab, case):
    def mutate(target):
        data = bytearray(open(target, 'rb').read())
        if len(data) > 400:
            data[300] ^= 0xFF
            open(target, 'wb').write(data)
    return _observation_diff_case(lab, case, mutate, 'fixture.sqlite-shm', restore_times=True)


def case_main_byte_change(lab, case):
    def mutate(target):
        data = bytearray(open(target, 'rb').read())
        if len(data) > 4096:
            data[3000] ^= 0xFF
            open(target, 'wb').write(data)
    return _observation_diff_case(lab, case, mutate, 'fixture.sqlite', restore_times=True)


def case_inode_replacement(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        before = obs.snapshot(database, lab.root)
        data = open(database, 'rb').read()
        temporary = os.path.join(directory, 'replacement.tmp')
        with open(temporary, 'wb') as handle:
            handle.write(data)
        os.replace(temporary, database)
        after = obs.snapshot(database, lab.root)
        entry = obs.compare_snapshots(before, after)['per_file']['fixture.sqlite']
        inode_changed = entry['metadata_changed_fields'].get('ino') is not None
        return case.finish('PASS' if inode_changed and entry['content_equal'] else 'FAIL',
                           'same-bytes inode replacement detected by identity',
                           per_file=json.loads(json.dumps(entry, default=str)))
    finally:
        connection.close()


def case_sidecar_added_removed(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        before = obs.snapshot(database, lab.root)
        journal = database + '-journal'
        with open(journal, 'wb') as handle:
            handle.write(b'\xd9\xd5\x05\xf9\x20\xa1\x63\xd7' + b'\x00' * 100)
        middle = obs.snapshot(database, lab.root)
        os.unlink(journal)
        after = obs.snapshot(database, lab.root)
        first = obs.compare_snapshots(before, middle)
        second = obs.compare_snapshots(middle, after)
        return case.finish('PASS' if (first['added'] == ['fixture.sqlite-journal'] and
                                      second['removed'] == ['fixture.sqlite-journal']) else 'FAIL',
                           'journal sidecar addition and removal both classified',
                           added=first['added'], removed=second['removed'])
    finally:
        connection.close()


def case_permission_change(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        before = obs.snapshot(database, lab.root)
        os.chmod(database, 0o400)
        after = obs.snapshot(database, lab.root)
        entry = obs.compare_snapshots(before, after)['per_file']['fixture.sqlite']
        os.chmod(database, 0o600)
        return case.finish('PASS' if entry['metadata_changed_fields'].get('mode') else 'FAIL',
                           'chmod detected through the mode field',
                           per_file=json.loads(json.dumps(entry, default=str)))
    finally:
        try:
            os.chmod(database, 0o600)
        except OSError:
            pass
        connection.close()


def case_time_changes(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        base = os.lstat(database)
        before = obs.snapshot(database, lab.root)
        os.utime(database, ns=(base.st_atime_ns, base.st_mtime_ns + 10 ** 9))
        middle = obs.snapshot(database, lab.root)
        mtime_entry = obs.compare_snapshots(before, middle)['per_file']['fixture.sqlite']
        current = os.lstat(database)
        os.utime(database, ns=(current.st_atime_ns + 2 * 86400 * 10 ** 9, current.st_mtime_ns))
        after = obs.snapshot(database, lab.root)
        atime_entry = obs.compare_snapshots(middle, after)['per_file']['fixture.sqlite']
        ok = (mtime_entry['metadata_changed_fields'].get('mtime_ns') and
              atime_entry['metadata_changed_fields'].get('atime_ns'))
        return case.finish('PASS' if ok else 'FAIL',
                           'mtime-only and atime-only changes each detected',
                           mtime=json.loads(json.dumps(mtime_entry['metadata_changed_fields'], default=str)),
                           atime=json.loads(json.dumps(atime_entry['metadata_changed_fields'], default=str)))
    finally:
        connection.close()


def case_same_mode_fchmod(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        before = os.lstat(database)
        descriptor = os.open(database, os.O_RDONLY)
        try:
            os.fchmod(descriptor, stat.S_IMODE(before.st_mode))
        finally:
            os.close(descriptor)
        after = os.lstat(database)
        changed = after.st_ctime_ns != before.st_ctime_ns
        if not changed:
            return case.finish('NOT_ESTABLISHED',
                               'same-mode fchmod did not change ctime on this kernel; '
                               'no fabricated hit',
                               before_ctime_ns=before.st_ctime_ns,
                               after_ctime_ns=after.st_ctime_ns)
        snapshot_before = obs.snapshot(database, lab.root)
        descriptor = os.open(database, os.O_RDONLY)
        try:
            os.fchmod(descriptor, stat.S_IMODE(os.lstat(database).st_mode))
        finally:
            os.close(descriptor)
        snapshot_after = obs.snapshot(database, lab.root)
        entry = obs.compare_snapshots(snapshot_before, snapshot_after)['per_file']['fixture.sqlite']
        return case.finish('PASS' if entry['metadata_changed_fields'].get('ctime_ns') else 'FAIL',
                           'same-mode fchmod ctime change detected when it occurs',
                           per_file=json.loads(json.dumps(entry, default=str)))
    finally:
        connection.close()


def case_refused_members(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        os.symlink(database, database + '-alias')
        os.mkfifo(database + '-journal')
        big = database + '-big'
        with open(big, 'wb') as handle:
            block = b'\x00' * (1024 * 1024)
            for _ in range(obs.READ_LIMIT_BYTES // len(block) + 1):
                handle.write(block)
        result = obs.snapshot(database, lab.root)
        members = result['members']
        reasons = {name: entry.get('error', {}).get('reason')
                   for name, entry in members.items()}
        ok = (reasons.get('fixture.sqlite-alias') == 'symlink_member' and
              reasons.get('fixture.sqlite-journal') == 'non_regular_member' and
              reasons.get('fixture.sqlite-big') == 'read_bound_exceeded' and
              all(entry.get('status') == 'refused' for name, entry in members.items()
                  if name != 'fixture.sqlite' and name != 'fixture.sqlite-wal'
                  and name != 'fixture.sqlite-shm'))
        return case.finish('PASS' if ok else 'FAIL',
                           'symlink, non-regular and oversized members refused with reasons',
                           reasons={name: value for name, value in reasons.items()
                                    if value or name.endswith(('-alias', '-journal', '-big'))},
                           restricted_count=len(result['restricted']))
    finally:
        connection.close()


def case_identity_chain(lab, case):
    base = {'dev': 1, 'ino': 2, 'type': 'regular', 'mode': '0600', 'uid': 1000,
            'gid': 1000, 'nlink': 1, 'size': 4096, 'blocks': 8,
            'atime_ns': 1, 'mtime_ns': 2, 'ctime_ns': 3}
    caught = []
    for field, value in (('size', 8192), ('mtime_ns', 99), ('ino', 7)):
        mutated = dict(base)
        mutated[field] = value
        try:
            obs._assert_same_identity(base, mutated, 'during_read')
        except obs.ObservationError as failure:
            caught.append({'field': field, 'reason': failure.reason,
                           'detail': failure.detail})
    ok = len(caught) == 3 and all(item['reason'] == 'identity_changed_during_read'
                                  and item['field'] in item['detail'] for item in caught)
    return case.finish('PASS' if ok else 'FAIL',
                        'read-time identity chain rejects size/mtime/inode changes precisely',
                        caught=caught)


def _corrupt_wal_case(lab, case, corrupt):
    directory, database, connection = lab.fixture(case.name)
    try:
        connection.execute('BEGIN')
        connection.execute("INSERT INTO business VALUES (7, 'must-survive-row')")
        connection.execute('COMMIT')
        frames_at_window = _wal_frames(database)
        baseline_copy = os.path.join(directory, '..', f'copy-{case.name}-baseline')
        obs.frozen_copy(database, baseline_copy, lab.root)
        baseline = obs.logical_dump(os.path.join(baseline_copy, 'fixture.sqlite'), lab.root)
        baseline_visible = any(
            cell.get('v') == 'must-survive-row'
            for row in baseline.get('tables', {}).get('business', {}).get('rows', [])
            for cell in row)
        negative_dir, negative_db = lab.copy_fixture(case.name, directory)
        connection.close()
        corrupt(_wal_path(negative_db))
        tampered_copy = os.path.join(directory, '..', f'copy-{case.name}-tampered')
        obs.frozen_copy(negative_db, tampered_copy, lab.root)
        tampered = obs.logical_dump(os.path.join(tampered_copy, 'fixture.sqlite'), lab.root)
        tampered_visible = any(
            cell.get('v') == 'must-survive-row'
            for row in tampered.get('tables', {}).get('business', {}).get('rows', [])
            for cell in row)
        sqlite_error = tampered.get('sqlite_error')
        detected = baseline_visible and (not tampered_visible or sqlite_error)
        return case.finish('PASS' if detected else 'FAIL',
                           'corruption measurably removed or errored the committed fact'
                           if detected else 'corruption did not affect the committed fact',
                           baseline_visible=baseline_visible,
                           tampered_visible=tampered_visible,
                           sqlite_error=sqlite_error,
                           wal_frames_baseline=frames_at_window,
                           quick_check=tampered.get('quick_check'))
    finally:
        try:
            connection.close()
        except sqlite3.Error:
            pass


def case_wal_truncated_mid_commit(lab, case):
    def corrupt(path):
        size = os.path.getsize(path)
        if size > 32 + FRAME:
            os.truncate(path, size - FRAME)
        else:
            raise RuntimeError('negative fixture WAL too small to truncate')
    return _corrupt_wal_case(lab, case, corrupt)


def case_wal_frame_byte_flip(lab, case):
    def corrupt(path):
        data = bytearray(open(path, 'rb').read())
        if len(data) < 32 + FRAME:
            raise RuntimeError('negative fixture WAL too small to flip')
        data[32 + 24 + 200] ^= 0xFF
        with open(path, 'wb') as handle:
            handle.write(data)
    return _corrupt_wal_case(lab, case, corrupt)


def case_wal_header_flip(lab, case):
    def corrupt(path):
        data = bytearray(open(path, 'rb').read())
        if len(data) < 32:
            raise RuntimeError('negative fixture WAL has no header')
        data[12] ^= 0xFF
        with open(path, 'wb') as handle:
            handle.write(data)
    return _corrupt_wal_case(lab, case, corrupt)


class _FailingCloseConnection:
    """Test seam: a connection whose close() fails after readable results."""

    def execute(self, sql):
        if sql.startswith('PRAGMA'):
            if sql == 'PRAGMA user_version':
                return iter([(411,)])
            if sql == 'PRAGMA journal_mode':
                return iter([('wal',)])
            if sql == 'PRAGMA quick_check':
                return iter([('ok',)])
            return iter([(1,)])
        if 'sqlite_schema' in sql:
            return iter([('table', 'business', 'business', 2,
                          'CREATE TABLE business(id INTEGER PRIMARY KEY, payload TEXT)')])
        return iter([(1, 'row-one')])

    def close(self):
        raise RuntimeError('injected close failure')


def case_close_failure_recorded(lab, case):
    directory, database, connection = lab.fixture(case.name)
    try:
        copy_dir = os.path.join(directory, '..', f'copy-{case.name}')
        obs.frozen_copy(database, copy_dir, lab.root)
        dump = obs.logical_dump(os.path.join(copy_dir, 'fixture.sqlite'), lab.root,
                                connect=lambda _path: _FailingCloseConnection())
        recorded = 'injected close failure' in str(dump.get('close', ''))
        return case.finish('PASS' if recorded else 'FAIL',
                           'connection close failure recorded instead of swallowed',
                           close=dump.get('close'))
    finally:
        connection.close()


def case_atime_control(lab, case):
    regime = obs.mount_records(os.path.join(lab.root, '.'))['options']
    results = []
    for index in range(2):
        directory, database, connection = lab.fixture(case.name, populated=False)
        try:
            current = os.lstat(database)
            os.utime(database, ns=(current.st_mtime_ns - 2 * 86400 * 10 ** 9,
                                   current.st_mtime_ns))
            triggered_initial = os.lstat(database).st_atime_ns < os.lstat(database).st_mtime_ns
            before = os.lstat(database).st_atime_ns
            obs.snapshot(database, lab.root)
            after_observer = os.lstat(database).st_atime_ns
            descriptor = os.open(database, os.O_RDONLY)
            try:
                while os.read(descriptor, 65536):
                    pass
            finally:
                os.close(descriptor)
            after_plain = os.lstat(database).st_atime_ns
            results.append({'fixture': index, 'triggered_initial': triggered_initial,
                            'observer_bumped_atime': after_observer != before,
                            'plain_read_bumped_atime': after_plain != after_observer,
                            'atime_before': before, 'atime_after_observer': after_observer,
                            'atime_after_plain': after_plain})
        finally:
            connection.close()
    ok = all(not item['observer_bumped_atime'] and item['plain_read_bumped_atime']
             for item in results)
    if ok:
        return case.finish('PASS', 'O_NOATIME observation left atime untouched while a plain '
                           'read updated it under the recorded regime', regime=regime,
                           results=results)
    if all(not item['triggered_initial'] or not item['plain_read_bumped_atime']
           for item in results):
        return case.finish('NOT_ESTABLISHED',
                           'atime trigger condition did not hold under this regime; '
                           'no fabricated hit', regime=regime, results=results)
    return case.finish('FAIL', 'observer observation changed atime or control misbehaved',
                       regime=regime, results=results)


# ---------------------------------------------- mechanism regressions (2026-11-05)
# The cases below verify the audit-fix evidence semantics (F-04/F-05/F-06/F-O3).
# They keep using the same Lab/Case entry and their own throwaway fixtures; the
# lifecycle runner is imported as a module by absolute path (never its CLI main).

_LIFECYCLE_CACHE = {}


def _lifecycle_module():
    """Load the sibling lifecycle runner module; record path and file hash."""
    if 'module' not in _LIFECYCLE_CACHE:
        path = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                            'scripts', 'run_store_compatibility_lifecycle.py')
        with open(path, 'rb') as handle:
            digest = hashlib.sha256(handle.read()).hexdigest()
        spec = importlib.util.spec_from_file_location(
            'store_compatibility_lifecycle_under_test', path)
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        _LIFECYCLE_CACHE.update(module=module, path=path, sha256=digest)
    return _LIFECYCLE_CACHE['module']


def _jsonable(value):
    return json.loads(json.dumps(value, default=str))


def _error_dump(label):
    """A dump record whose observation failed before any digest existed."""
    return {'database': f'/synthetic/{label}/fixture.sqlite',
            'sqlite_error': f'[OperationalError] synthetic failure {label}',
            'close': 'ok'}


def case_f04_missing_evidence_gates(lab, case):
    """R1 layers 1-2: real helpers must not turn missing evidence into equality."""
    left, right = _error_dump('a'), _error_dump('b')
    logical = obs.diff_logical(left, right)
    layer1 = {'logical_equal': logical.get('logical_equal'),
              'comparable': logical.get('comparable'),
              'observation_complete_before': logical.get('observation_complete_before'),
              'observation_complete_after': logical.get('observation_complete_after'),
              'reasons_before': logical.get('reasons_before'),
              'reasons_after': logical.get('reasons_after')}
    layer1_ok = (layer1['comparable'] is False and
                 layer1['logical_equal'] is None and
                 layer1['observation_complete_before'] is False and
                 layer1['observation_complete_after'] is False)
    directory, database, connection = lab.fixture(case.name)
    try:
        os.symlink(database, database + '-alias')
        raw = obs.compare_snapshots(obs.snapshot(database, lab.root),
                                    obs.snapshot(database, lab.root))
        alias = raw.get('per_file', {}).get('fixture.sqlite-alias', {})
        layer2 = {'strict_equal': raw.get('strict_equal'),
                  'comparable': raw.get('comparable'),
                  'records_equal': raw.get('records_equal'),
                  'alias_content_equal': alias.get('content_equal'),
                  'alias_evidence_complete': alias.get('evidence_complete')}
        layer2_ok = (layer2['strict_equal'] is False and layer2['comparable'] is False and
                     layer2['records_equal'] is True and
                     layer2['alias_content_equal'] is None and
                     layer2['alias_evidence_complete'] is False)
        ok = layer1_ok and layer2_ok
        return case.finish('PASS' if ok else 'FAIL',
                           'missing hash/refused evidence yields incomparable, never equality'
                           if ok else 'comparator still derives equality from missing evidence',
                           layer1_diff_logical=_jsonable(layer1),
                           layer2_compare_snapshots=_jsonable(layer2))
    finally:
        connection.close()


def case_f04_row_gate_propagation(lab, case):
    """R1 layer 3: real row_c2a must reject DEGRADED windows fed from real failed facts."""
    mod = _lifecycle_module()
    logical = obs.diff_logical(_error_dump('a'), _error_dump('b'))
    directory, database, connection = lab.fixture(case.name)
    try:
        os.symlink(database, database + '-alias')
        raw = obs.compare_snapshots(obs.snapshot(database, lab.root),
                                    obs.snapshot(database, lab.root))
    finally:
        connection.close()
    degraded = {'tag': 'window', 'verdict': 'DEGRADED',
                'copy_error': {'reason': 'synthetic degraded window for gate propagation'}}

    def fake_online_row(runner, row_path, name, command, *, flock=False, note=None):
        return {'fixture': directory, 'request': 'synthetic',
                'phases': {'BEFORE': degraded, 'AFTER': degraded},
                'error_classification': 'unsupported',
                'member_bytes': {},
                'raw': raw, 'logical': logical,
                'verdicts': {
                    'error': 'unsupported',
                    'main': raw.get('per_file', {}).get('fixture.sqlite', {}),
                    'wal': raw.get('per_file', {}).get('fixture.sqlite-wal', {}),
                    'shm': raw.get('per_file', {}).get('fixture.sqlite-shm', {}),
                    'writer_lock': {}, 'added': raw.get('added'),
                    'removed': raw.get('removed'),
                    'business_product_equal': bool(logical and logical.get('logical_equal'))}}

    original = mod._online_row
    mod._online_row = fake_online_row
    try:
        record = mod.row_c2a(object(), None)
    finally:
        mod._online_row = original
    status = record.get('status')
    ok = status == 'STRICT_FAIL'
    return case.finish('PASS' if ok else 'FAIL',
                       'row_c2a refuses degraded windows and incomparable logic'
                       if ok else 'row_c2a still upgrades failed evidence to STRICT_PASS',
                       row_status=status,
                       upstream_logical_equal_literal=logical.get('logical_equal'),
                       upstream_raw_comparable=raw.get('comparable'),
                       gate_reasons=record.get('gate_reasons'))


def case_f04_dump_validity_matrix(lab, case):
    """Each validity condition of a logical dump carries its own explicit reason."""
    checks = []

    def note(name, ok, detail):
        checks.append({'check': name, 'ok': bool(ok), 'detail': _jsonable(detail)})

    def make_dump(tag, payload):
        directory = os.path.join(lab.root, f'fixture-{case.name}-{tag}-{lab.counter:03d}')
        lab.counter += 1
        os.makedirs(directory, mode=0o700)
        database = os.path.join(directory, 'fixture.sqlite')
        connection = sqlite3.connect(database, isolation_level=None)
        connection.execute('CREATE TABLE business(id INTEGER PRIMARY KEY, payload TEXT)')
        connection.execute('BEGIN')
        connection.execute('INSERT INTO business VALUES (1, ?)', (payload,))
        connection.execute('COMMIT')
        connection.close()
        copy_dir = os.path.join(directory, '..', f'copy-{case.name}-{tag}-{lab.counter:03d}')
        obs.frozen_copy(database, copy_dir, lab.root)
        return obs.logical_dump(os.path.join(copy_dir, 'fixture.sqlite'), lab.root)

    dump_a = make_dump('a', 'same-payload')
    dump_b = make_dump('b', 'same-payload')
    dump_c = make_dump('c', 'different-payload')
    same = obs.diff_logical(dump_a, dump_b)
    differ = obs.diff_logical(dump_a, dump_c)
    note('real_equal_pair', same.get('comparable') is True and same.get('logical_equal') is True,
         {key: same.get(key) for key in ('comparable', 'logical_equal')})
    note('real_differing_pair',
         differ.get('comparable') is True and differ.get('logical_equal') is False,
         {key: differ.get(key) for key in ('comparable', 'logical_equal')})
    completeness = getattr(obs, 'logical_dump_completeness', None)
    note('completeness_helper_present', callable(completeness), {})
    if callable(completeness):
        note('real_dump_complete', completeness(dump_a).get('observation_complete') is True,
             completeness(dump_a))
        variants = {
            'no_hash': {'database': '/x/fixture.sqlite', 'sqlite_error': 'e', 'close': 'ok'},
            'short_hash': {**dump_a, 'logical_sha256': 'ab' * 31},
            'nonhex_hash': {**dump_a, 'logical_sha256': 'z' * 64},
            'sqlite_error_with_digest': {**dump_a, 'sqlite_error': '[Err] late failure'},
            'close_failed': {**dump_a, 'close': '[RuntimeError] injected close failure'},
            'quick_check_missing': {key: value for key, value in dump_a.items()
                                    if key != 'quick_check'},
            'quick_check_bad': {**dump_a, 'quick_check': [['row 3 missing from index business_1']]},
            'core_payload_missing': {key: value for key, value in dump_a.items()
                                     if key != 'tables'},
        }
        for name, record in variants.items():
            result = completeness(record)
            note('invalid_' + name,
                 result.get('observation_complete') is False and bool(result.get('reasons')),
                 result)
        mixed = obs.diff_logical(dump_a, variants['sqlite_error_with_digest'])
        note('mixed_side_incomparable',
             mixed.get('comparable') is False and mixed.get('logical_equal') is None and
             bool(mixed.get('reasons_after')),
             {key: mixed.get(key) for key in ('comparable', 'logical_equal', 'reasons_after')})
    ok = all(item['ok'] for item in checks)
    return case.finish('PASS' if ok else 'FAIL',
                       'every validity condition decided with explicit reasons'
                       if ok else 'a validity condition lost its reason',
                       checks=checks)


def case_f04_copy_and_stability_gates(lab, case):
    """Refused members block copies; missing main blocks SQLite opens; main-only is fine."""
    checks = []

    def note(name, ok, detail):
        checks.append({'check': name, 'ok': bool(ok), 'detail': _jsonable(detail)})

    directory, database, connection = lab.fixture(case.name)
    try:
        os.symlink(database, database + '-alias')
        try:
            obs.stability_check(database, lab.root, rounds=1)
            note('insufficient_rounds_refused', False, 'no error raised')
        except obs.ObservationError as failure:
            note('insufficient_rounds_refused', failure.reason == 'insufficient_rounds',
                 failure.as_dict())
        destination = os.path.join(lab.root, f'copy-{case.name}-refused')
        try:
            obs.frozen_copy(database, destination, lab.root)
            note('refused_source_blocks_copy', False, 'no error raised')
        except obs.ObservationError as failure:
            note('refused_source_blocks_copy',
                 failure.reason == 'copy_source_incomplete' and not os.path.lexists(destination),
                 failure.as_dict())
    finally:
        connection.close()
    incomplete_dir = os.path.join(lab.root, f'fixture-{case.name}-nomain')
    os.makedirs(incomplete_dir, mode=0o700)
    with open(os.path.join(incomplete_dir, 'fixture.sqlite-wal'), 'wb') as handle:
        handle.write(b'orphan-wal-bytes-without-main')
    opener_calls = []

    def counting_opener(path):
        opener_calls.append(path)
        return sqlite3.connect(path)

    dump = obs.logical_dump(os.path.join(incomplete_dir, 'fixture.sqlite'), lab.root,
                            connect=counting_opener)
    note('missing_main_never_opened',
         not opener_calls and dump.get('sqlite_opener_calls') == 0 and
         dump.get('close') != 'ok' and not dump.get('logical_sha256'),
         {'opener_calls': len(opener_calls), 'close': dump.get('close'),
          'pre_open_completeness': dump.get('pre_open_completeness')})
    quiet_dir = os.path.join(lab.root, f'fixture-{case.name}-quiet')
    os.makedirs(quiet_dir, mode=0o700)
    quiet_db = os.path.join(quiet_dir, 'fixture.sqlite')
    quiet_connection = sqlite3.connect(quiet_db, isolation_level=None)
    quiet_connection.execute('CREATE TABLE business(id INTEGER PRIMARY KEY)')
    quiet_connection.execute('INSERT INTO business VALUES (1)')
    quiet_connection.close()
    stability = obs.stability_check(quiet_db, lab.root, rounds=2)
    quiet_copy = os.path.join(quiet_dir, '..', f'copy-{case.name}-quiet')
    copy_result = obs.frozen_copy(quiet_db, quiet_copy, lab.root)
    quiet_dump = obs.logical_dump(os.path.join(quiet_copy, 'fixture.sqlite'), lab.root)
    completeness = getattr(obs, 'logical_dump_completeness', lambda _r: {'observation_complete': None})
    note('main_only_family_complete',
         stability.get('stable') is True and
         completeness(quiet_dump).get('observation_complete') is True and
         sorted(copy_result.get('readback', {})) == ['fixture.sqlite'],
         {'stable': stability.get('stable'),
          'members': sorted(copy_result.get('readback', {})),
          'dump_complete': completeness(quiet_dump).get('observation_complete')})
    ok = all(item['ok'] for item in checks)
    return case.finish('PASS' if ok else 'FAIL',
                       'copy/open gates refuse incomplete evidence and accept complete minimal families'
                       if ok else 'a copy/open gate did not behave as specified',
                       checks=checks)


def case_f04_shared_row_gates(lab, case):
    """Window/comparison gates decide row statuses from real comparator facts."""
    mod = _lifecycle_module()
    checks = []

    def note(name, ok, detail):
        checks.append({'check': name, 'ok': bool(ok), 'detail': _jsonable(detail)})

    directory, database, connection = lab.fixture(case.name)
    try:
        before_snapshot = obs.snapshot(database, lab.root)
        after_snapshot = obs.snapshot(database, lab.root)
    finally:
        connection.close()
    raw_equal = obs.compare_snapshots(before_snapshot, after_snapshot)
    copy_dir = os.path.join(lab.root, f'copy-{case.name}-gate')
    obs.frozen_copy(database, copy_dir, lab.root)
    dump_b = obs.logical_dump(os.path.join(copy_dir, 'fixture.sqlite'), lab.root)
    logical_equal = obs.diff_logical(dump_b, dump_b)
    logical_failed = obs.diff_logical(_error_dump('a'), _error_dump('b'))
    ok_window = {'verdict': 'OK', 'member_bytes': {
        name: entry.get('bytes') for name, entry in before_snapshot['members'].items()}}
    degraded_window = {'verdict': 'DEGRADED'}
    member_bytes = {name: entry.get('bytes')
                    for name, entry in before_snapshot['members'].items()}
    base_record = {'phases': {'BEFORE': ok_window, 'AFTER': ok_window},
                   'raw': raw_equal, 'logical': logical_equal,
                   'error_classification': 'unsupported',
                   'member_bytes': member_bytes}
    gate = getattr(mod, '_online_strict_gate', None)
    note('online_gate_helper_present', callable(gate), {})
    if callable(gate):
        reasons = gate(base_record, 'unsupported')
        note('online_gate_valid', not reasons, reasons)
        missing_after = dict(base_record, phases={'BEFORE': ok_window})
        reasons_miss = gate(missing_after, 'unsupported')
        note('online_gate_single_window',
             bool(reasons_miss) and 'after_window_not_ok' in reasons_miss, reasons_miss)
        empty_wal = dict(base_record, member_bytes={**member_bytes, 'fixture.sqlite-wal': 0})
        reasons_wal = gate(empty_wal, 'unsupported')
        note('online_gate_empty_wal', bool(reasons_wal) and
             any('wal' in item for item in reasons_wal), reasons_wal)
        failed_logic = dict(base_record, logical=logical_failed)
        reasons_logic = gate(failed_logic, 'unsupported')
        note('online_gate_failed_logic', bool(reasons_logic), reasons_logic)
        locked = dict(base_record, member_bytes={**member_bytes, 'fixture.sqlite.writer-lock': 0})
        reasons_lock = gate(locked, 'conflict', require_writer_lock=True)
        note('online_gate_requires_lock', bool(reasons_lock) and
             any('writer_lock' in item for item in reasons_lock), reasons_lock)
    for helper, args, expect_reason, name in (
            ('_c0_gate', (raw_equal, logical_failed, ok_window, ok_window),
             'logical_comparison_incomplete', 'c0_gate_failed_logic'),
            ('_c0_gate', (raw_equal, logical_equal, ok_window, degraded_window),
             'window_not_ok', 'c0_gate_degraded_window'),
            ('_c0_gate', (raw_equal, logical_equal, ok_window, ok_window), None, 'c0_gate_valid'),
            ('_c5_business_gate', (ok_window, ok_window, logical_failed),
             'logical_not_comparable', 'c5_gate_failed_logic'),
            ('_c5_business_gate', (ok_window, ok_window, logical_equal), None, 'c5_gate_valid'),
            ('_c7_support_gate', (0, ok_window, degraded_window, logical_equal, {'t': 1}),
             'window_not_ok', 'c7_gate_degraded_window'),
            ('_c7_support_gate', (0, ok_window, ok_window, logical_equal, {'t': 1}),
             None, 'c7_gate_valid')):
        function = getattr(mod, helper, None)
        if function is None:
            note(name + '_present', False, helper)
            continue
        reasons = function(*args)
        note(name, (reasons == [] if expect_reason is None else expect_reason in reasons), reasons)
    ok = all(item['ok'] for item in checks)
    return case.finish('PASS' if ok else 'FAIL',
                       'row gates propagate completeness from real comparator facts'
                       if ok else 'a row gate accepted or rejected the wrong evidence',
                       checks=checks)


_STOCK_CHILD_SCRIPT = (
    "import sys, time\n"
    "sys.stdout.write('ready\\n')\n"
    "sys.stdout.flush()\n"
    "time.sleep(120)\n")


class _ScriptedStubHolder:
    """Stub holder client: scripted failures, records every command and close."""

    def __init__(self, fail_command=None, fail_token=None, fail_with=None, close_error=None):
        self.commands = []
        self.fail_command = fail_command
        self.fail_token = fail_token
        self.fail_with = fail_with or RuntimeError('scripted holder failure')
        self.close_error = close_error
        self.closed = False

    def command(self, command, **extra):
        self.commands.append((command, extra.get('token')))
        if command == self.fail_command or (command == 'WRITE' and
                                            extra.get('token') == self.fail_token):
            raise self.fail_with
        return {'ok': True, 'command': command, 'mono_ns': time.monotonic_ns()}

    def close(self):
        self.closed = True
        if self.close_error is not None:
            raise self.close_error
        return {'stub': True, 'closed': True}


def case_f05_stopped_child_stub_write_failure(lab, case):
    """R2: a stub holder WRITE failure must not strand the stopped stock child."""
    mod = _lifecycle_module()
    real_subprocess = mod.subprocess
    row_dir = os.path.join(lab.root, f'row-{case.name}')
    os.makedirs(row_dir, mode=0o700)
    fixture_dir = os.path.join(lab.root, f'fixture-{case.name}')
    os.makedirs(fixture_dir, mode=0o700)
    spawned = []

    def spawning_popen(argv, **kwargs):
        # Intercept the product argv before any real effect; start and own a real
        # stock Python child instead, completing the readiness handshake here so
        # the row's SIGSTOP always lands on a ready child.
        child = real_subprocess.Popen([sys.executable, '-c', _STOCK_CHILD_SCRIPT],
                                      stdin=real_subprocess.DEVNULL,
                                      stdout=real_subprocess.PIPE,
                                      stderr=real_subprocess.PIPE)
        ready = child.stdout.readline()
        if ready != b'ready\n':
            child.kill()
            raise RuntimeError(f'stock child handshake failed: {ready!r}')
        spawned.append(child)
        return child

    stub = _ScriptedStubHolder(fail_token='c5-concurrent-commit',
                               fail_with=RuntimeError('stub concurrent WRITE failure'))
    runner = mod.Runner.__new__(mod.Runner)
    runner.evidence = types.SimpleNamespace(root=lab.root, tmp=row_dir)
    runner.args = types.SimpleNamespace(old_elf='intercepted-old-elf',
                                        old_source='intercepted-old-source')
    runner.identity = None
    runner.stop_reason = None
    runner.build_static_411 = lambda name: (
        fixture_dir, os.path.join(fixture_dir, 'fixture.sqlite'), [], 'req-selfcheck')
    runner.holder_start = lambda database, journal: (stub, {'ok': True, 'stub': True})
    runner.strict_window = lambda row_path, database, tag: (
        {'tag': tag, 'verdict': 'DEGRADED'}, None, None)
    runner.verify_old_elf = lambda: None

    mod.subprocess = types.SimpleNamespace(Popen=spawning_popen,
                                           DEVNULL=real_subprocess.DEVNULL,
                                           PIPE=real_subprocess.PIPE,
                                           TimeoutExpired=real_subprocess.TimeoutExpired)
    observed = {'raised': None, 'record': None, 'runner_reaped': None}
    child = None
    try:
        try:
            observed['record'] = mod.row_c5(runner, row_dir)
        except BaseException as failure:  # red observation: the row itself raised
            observed['raised'] = f'[{type(failure).__name__}] {failure}'
        child = spawned[0] if spawned else None
        observed['runner_reaped'] = child.poll() is not None if child is not None else None
        observed['returncode'] = child.returncode if child is not None else None
        harness_cleanup_applied = False
        if child is not None and child.poll() is None:
            # Independent bounded fallback; only the child this test created.
            child.kill()
            try:
                child.wait(timeout=5)
            except real_subprocess.TimeoutExpired:
                pass
            harness_cleanup_applied = True
        record = observed['record']
        cleanup = record.get('owned_child_cleanup') if isinstance(record, dict) else None
        ok = (observed['raised'] is None and isinstance(record, dict) and
              'stub concurrent WRITE failure' in str(record.get('error', '')) and
              record.get('error_phase') == 'concurrent_write' and
              observed['runner_reaped'] is True and stub.closed and
              isinstance(cleanup, dict) and cleanup.get('cleanup_complete') is True and
              cleanup.get('reaped') is True)
        return case.finish('PASS' if ok else 'FAIL',
                           'WRITE failure reaps the stopped child and closes the holder'
                           if ok else 'the row stranded the stopped child or the holder',
                           raised=observed['raised'], row_status=record.get('status') if
                           isinstance(record, dict) else None,
                           error=record.get('error') if isinstance(record, dict) else None,
                           error_phase=record.get('error_phase') if
                           isinstance(record, dict) else None,
                           runner_cleanup_complete=observed['runner_reaped'],
                           harness_cleanup_applied=harness_cleanup_applied,
                           holder_close_attempted=stub.closed,
                           owned_child_cleanup=_jsonable(cleanup))
    finally:
        mod.subprocess = real_subprocess
        if child is not None and child.poll() is None:
            child.kill()
            try:
                child.wait(timeout=5)
            except real_subprocess.TimeoutExpired:
                pass


class _FakeChild:
    """Popen-like stand-in: records signals, never touches any real process."""

    def __init__(self, *, communicate_first=None, communicate_rest=(b'out', b'err'),
                 exit_code=0, cont_error=None, timeout_expired=None):
        self.pid = 4194304  # clearly synthetic; /proc reads fail -> start_ticks UNKNOWN
        self.returncode = None
        self.signals = []
        self._communicate_first = communicate_first
        self._communicate_rest = communicate_rest
        self._cont_error = cont_error
        self._exit_code = exit_code
        self._timeout_expired = timeout_expired
        self.stdout = None
        self.stderr = None

    def send_signal(self, sig):
        name = _SIGNAL_NAMES.get(sig, str(sig))
        if self._cont_error is not None and 'CONT' in name:
            raise self._cont_error
        self.signals.append(name)
        if 'KILL' in name:
            self.returncode = -9
        elif name in ('SIGTERM', 'SIGCONT'):
            self.returncode = self._exit_code

    def poll(self):
        return self.returncode

    def wait(self, timeout=None):
        if self.returncode is None:
            raise self._timeout_expired(cmd='fake', timeout=timeout)
        return self.returncode

    def communicate(self, timeout=None):
        if self._communicate_first is not None:
            first = self._communicate_first
            self._communicate_first = None
            raise first
        return self._communicate_rest

    def terminate(self):
        self.send_signal(signal.SIGTERM)

    def kill(self):
        self.send_signal(signal.SIGKILL)


_SIGNAL_NAMES = {signal.SIGSTOP: 'SIGSTOP', signal.SIGCONT: 'SIGCONT',
                 signal.SIGTERM: 'SIGTERM', signal.SIGKILL: 'SIGKILL'}


def case_f05_exception_matrix(lab, case):
    """Injected failures at C5 boundaries preserve the first error and clean up."""
    mod = _lifecycle_module()
    real_subprocess = mod.subprocess
    checks = []

    def note(name, ok, detail):
        checks.append({'check': name, 'ok': bool(ok), 'detail': _jsonable(detail)})

    def make_runner(row_dir, stub, fake_child_factory=None, strict_window=None):
        runner = mod.Runner.__new__(mod.Runner)
        runner.evidence = types.SimpleNamespace(root=lab.root, tmp=row_dir)
        runner.args = types.SimpleNamespace(old_elf='intercepted-old-elf',
                                            old_source='intercepted-old-source')
        runner.identity = None
        runner.stop_reason = None
        runner.build_static_411 = lambda name: (
            row_dir, os.path.join(row_dir, 'fixture.sqlite'), [], 'req')
        runner.holder_start = lambda database, journal: (stub, {'ok': True, 'stub': True})
        runner.strict_window = strict_window or (
            lambda row_path, database, tag: ({'tag': tag, 'verdict': 'DEGRADED'}, None, None))
        runner.verify_old_elf = lambda: None
        return runner

    def fresh_dir(tag):
        path = os.path.join(lab.root, f'row-{case.name}-{tag}')
        os.makedirs(path, mode=0o700)
        return path

    confirm_original = getattr(mod, '_confirm_stopped', None)

    def run_row(runner, row_dir):
        try:
            return mod.row_c5(runner, row_dir), None
        except BaseException as failure:
            return None, failure

    try:
        if callable(getattr(mod, '_confirm_stopped', None)):
            mod._confirm_stopped = lambda pid, budget_s=5.0: True
        # OPEN failure: no child exists yet; holder still closed.
        row_dir = fresh_dir('open-failure')
        stub = _ScriptedStubHolder(fail_command='OPEN')
        record, failure = run_row(make_runner(row_dir, stub), row_dir)
        note('open_failure_preserved',
             isinstance(record, dict) and 'scripted holder failure' in str(record.get('error'))
             and stub.closed and record.get('owned_child_cleanup', {}).get('child_absent'),
             {'raised': str(failure) if failure else None, 'error': record.get('error') if
              isinstance(record, dict) else str(failure)})
        # Popen failure: recorded, holder closed, nothing spawned.
        row_dir = fresh_dir('popen-failure')

        def failing_popen(argv, **kwargs):
            raise OSError('synthetic spawn refusal')

        mod.subprocess = types.SimpleNamespace(Popen=failing_popen,
                                               TimeoutExpired=real_subprocess.TimeoutExpired,
                                               DEVNULL=real_subprocess.DEVNULL,
                                               PIPE=real_subprocess.PIPE)
        stub = _ScriptedStubHolder()
        record, failure = run_row(make_runner(row_dir, stub), row_dir)
        note('popen_failure_preserved',
             isinstance(record, dict) and 'synthetic spawn refusal' in str(record.get('error'))
             and stub.closed,
             {'error': record.get('error') if isinstance(record, dict) else str(failure)})
        # communicate timeout after a confirmed stop: kill, bounded drain, deviation.
        row_dir = fresh_dir('communicate-timeout')
        fake = _FakeChild(timeout_expired=real_subprocess.TimeoutExpired,
                          communicate_first=real_subprocess.TimeoutExpired(
                              cmd=['fake'], timeout=25, output=b'partial\n'))
        mod.subprocess = types.SimpleNamespace(
            Popen=lambda argv, **kwargs: fake, TimeoutExpired=real_subprocess.TimeoutExpired,
            DEVNULL=real_subprocess.DEVNULL, PIPE=real_subprocess.PIPE)
        stub = _ScriptedStubHolder()
        record, failure = run_row(make_runner(row_dir, stub), row_dir)
        old_open = record.get('phases', {}).get('OLD_OPEN') if isinstance(record, dict) else None
        note('communicate_timeout_recorded',
             isinstance(record, dict) and old_open is not None and
             old_open.get('timed_out') is True and 'SIGKILL' in fake.signals and
             record.get('owned_child_cleanup', {}).get('cleanup_complete') is True,
             {'signals': fake.signals, 'timed_out': old_open.get('timed_out') if old_open else None,
              'error': record.get('error') if isinstance(record, dict) else str(failure)})
        # SIGCONT failure is recorded; the row still finishes bounded cleanup.
        row_dir = fresh_dir('cont-failure')
        fake = _FakeChild(timeout_expired=real_subprocess.TimeoutExpired,
                          cont_error=ProcessLookupError('child already gone'),
                          communicate_first=real_subprocess.TimeoutExpired(
                              cmd=['fake'], timeout=25, output=b''))
        mod.subprocess = types.SimpleNamespace(
            Popen=lambda argv, **kwargs: fake, TimeoutExpired=real_subprocess.TimeoutExpired,
            DEVNULL=real_subprocess.DEVNULL, PIPE=real_subprocess.PIPE)
        stub = _ScriptedStubHolder()
        record, failure = run_row(make_runner(row_dir, stub), row_dir)
        note('cont_failure_recorded',
             isinstance(record, dict) and
             'child already gone' in str(record.get('cont_error')), {'cont_error':
             record.get('cont_error') if isinstance(record, dict) else str(failure)})
        # AFTER window raising unexpectedly: ERROR status, holder closed, child done.
        row_dir = fresh_dir('after-raises')

        def raising_window(row_path, database, tag):
            if tag == 'after':
                raise ValueError('synthetic after-window explosion')
            return {'tag': tag, 'verdict': 'DEGRADED'}, None, None

        fake = _FakeChild(timeout_expired=real_subprocess.TimeoutExpired)
        mod.subprocess = types.SimpleNamespace(
            Popen=lambda argv, **kwargs: fake, TimeoutExpired=real_subprocess.TimeoutExpired,
            DEVNULL=real_subprocess.DEVNULL, PIPE=real_subprocess.PIPE)
        stub = _ScriptedStubHolder()
        record, failure = run_row(make_runner(row_dir, stub, strict_window=raising_window), row_dir)
        note('after_explosion_error',
             isinstance(record, dict) and record.get('status') == 'ERROR' and
             'synthetic after-window explosion' in str(record.get('error')) and stub.closed,
             {'status': record.get('status') if isinstance(record, dict) else None,
              'error': record.get('error') if isinstance(record, dict) else str(failure)})
        # WRITE failure plus a failing holder close: first error kept, cleanup appended.
        row_dir = fresh_dir('write-and-close-failure')
        fake = _FakeChild(timeout_expired=real_subprocess.TimeoutExpired)
        mod.subprocess = types.SimpleNamespace(
            Popen=lambda argv, **kwargs: fake, TimeoutExpired=real_subprocess.TimeoutExpired,
            DEVNULL=real_subprocess.DEVNULL, PIPE=real_subprocess.PIPE)
        stub = _ScriptedStubHolder(fail_token='c5-concurrent-commit',
                                   fail_with=RuntimeError('stub concurrent WRITE failure'),
                                   close_error=RuntimeError('stub close exploded'))
        record, failure = run_row(make_runner(row_dir, stub), row_dir)
        cleanup_errors = record.get('cleanup_errors') if isinstance(record, dict) else None
        note('first_error_not_overwritten',
             isinstance(record, dict) and
             'stub concurrent WRITE failure' in str(record.get('error')) and
             cleanup_errors and any('stub close exploded' in str(item) for item in cleanup_errors),
             {'error': record.get('error') if isinstance(record, dict) else str(failure),
              'cleanup_errors': cleanup_errors})
        # KeyboardInterrupt: own cleanup first, then the interrupt is re-raised.
        row_dir = fresh_dir('interrupt')
        stub = _ScriptedStubHolder(fail_command='QUIESCE',
                                   fail_with=KeyboardInterrupt('simulated operator interrupt'))
        record, failure = run_row(make_runner(row_dir, stub), row_dir)
        partial_path = os.path.join(row_dir, 'c5-owned-cleanup.json')
        partial = None
        if os.path.exists(partial_path):
            with open(partial_path, encoding='utf-8') as handle:
                partial = json.load(handle)
        note('interrupt_cleaned_then_reraised',
             isinstance(failure, KeyboardInterrupt) and stub.closed and
             isinstance(partial, dict) and 'phases_completed' in partial,
             {'raised': type(failure).__name__ if failure else None,
              'partial_saved': partial is not None})
    finally:
        mod.subprocess = real_subprocess
        if confirm_original is not None:
            mod._confirm_stopped = confirm_original
        elif hasattr(mod, '_confirm_stopped'):
            del mod._confirm_stopped
    ok = all(item['ok'] for item in checks)
    return case.finish('PASS' if ok else 'FAIL',
                       'every injected C5 boundary failure keeps its first error and cleanup facts'
                       if ok else 'an injected failure lost its error or its cleanup',
                       checks=checks)


def case_f05_holder_integrations(lab, case):
    """Real test-only holder: HELLO, refused command, graceful EXIT close, timeout reap."""
    mod = _lifecycle_module()
    checks = []

    def note(name, ok, detail):
        checks.append({'check': name, 'ok': bool(ok), 'detail': _jsonable(detail)})

    evidence = mod.Evidence(os.path.join(lab.root, f'ev-{case.name}'), 'holdercase')
    runner = mod.Runner(evidence, types.SimpleNamespace(), types.SimpleNamespace(run_id='hc'))
    directory, database, connection = lab.fixture(case.name)
    journal = os.path.join(lab.root, f'{case.name}-holder.journal')
    try:
        client, hello = runner.holder_start(database, journal)
        note('hello_real_pid', isinstance(hello.get('pid'), int) and hello.get('pid') > 1,
             hello.get('pid'))
        client.command('OPEN')
        refused = False
        try:
            client.command('BOGUS')
        except RuntimeError:
            refused = True
        note('unknown_command_refused', refused, {})
        close_record = client.close()
        exit_code = client.process.returncode
        second = client.close()
        note('graceful_close',
             close_record.get('exit_reply', {}).get('sqlite_close') == 'ok' and
             isinstance(exit_code, int) and close_record.get('cleanup_complete') is True,
             close_record)
        note('second_close_idempotent',
             second.get('idempotent_second_close') is True and
             client.process.returncode == exit_code, {})
    finally:
        connection.close()
    # HELLO timeout: a sleeping stub holder that never replies must still be reaped.
    sleep_holder = os.path.join(lab.root, f'{case.name}-sleep-holder.py')
    with open(sleep_holder, 'x') as handle:
        handle.write("import socket, sys, time\n"
                     "sock = socket.socket(fileno=int(sys.argv[1]))\n"
                     "time.sleep(120)\n")
    original_timeout = mod.HANDSHAKE_TIMEOUT_S
    original_holder_path = runner.holder_path
    raised = None
    try:
        mod.HANDSHAKE_TIMEOUT_S = 1
        runner.holder_path = sleep_holder
        try:
            runner.holder_start(database, journal + '.sleep')
        except BaseException as failure:
            raised = failure
    finally:
        mod.HANDSHAKE_TIMEOUT_S = original_timeout
        runner.holder_path = original_holder_path
    reap = getattr(raised, 'holder_reap', None)
    note('hello_timeout_is_startup_failure',
         isinstance(raised, TimeoutError) or type(raised).__name__ == 'socket.timeout', {})
    note('hello_timeout_holder_reaped',
         isinstance(reap, dict) and reap.get('reaped') is True, reap)
    ok = all(item['ok'] for item in checks)
    return case.finish('PASS' if ok else 'FAIL',
                       'real holder lifecycle: graceful close facts and bounded failure reap'
                       if ok else 'a holder integration boundary misbehaved',
                       checks=checks)


def case_f06_trace_timeout_receipt(lab, case):
    """R3: an inert mock tracer's communicate timeout must surface as timed_out."""
    mod = _lifecycle_module()
    real_subprocess = mod.subprocess
    row_dir = os.path.join(lab.root, f'row-{case.name}')
    os.makedirs(row_dir, mode=0o700)
    read_fd, write_fd = os.pipe()

    class FakeTracer:
        def __init__(self):
            self.pid = 4194303
            self.returncode = None
            self.communicated = 0
            self.stdout = os.fdopen(read_fd, 'rb', closefd=False)
            self.stderr = None

        def poll(self):
            return self.returncode

        def kill(self):
            self.returncode = -9

        def wait(self, timeout=None):
            if self.returncode is None:
                raise real_subprocess.TimeoutExpired(cmd='fake', timeout=timeout)
            return self.returncode

        def communicate(self, timeout=None):
            self.communicated += 1
            if self.communicated == 1:
                raise real_subprocess.TimeoutExpired(cmd='fake-tracer', timeout=timeout,
                                                     output=b'partial-trace\n')
            return b'rest\n', b'tracer-stderr-bytes'

    tracer = FakeTracer()
    runner = types.SimpleNamespace(
        args=types.SimpleNamespace(old_elf='intercepted-old-elf', old_source='intercepted-old'),
        evidence=types.SimpleNamespace(tmp=row_dir),
        error_classification=lambda stderr: None)
    original_timeout = mod.ROW_TIMEOUT_S
    mod.subprocess = types.SimpleNamespace(Popen=lambda argv, **kwargs: tracer,
                                           TimeoutExpired=real_subprocess.TimeoutExpired,
                                           DEVNULL=real_subprocess.DEVNULL,
                                           PIPE=real_subprocess.PIPE)
    try:
        mod.ROW_TIMEOUT_S = 0.4
        receipt = mod._trace_old(runner, row_dir, 'r3timeout', 'init', '/x/fixture.sqlite',
                                 '/x/fixture', kill=False)
    finally:
        mod.ROW_TIMEOUT_S = original_timeout
        mod.subprocess = real_subprocess
        os.close(write_fd)
        os.close(read_fd)
    trace_path = os.path.join(row_dir, 'r3timeout.strace')
    stderr_path = os.path.join(row_dir, 'r3timeout.stderr.bin')
    trace_bytes = open(trace_path, 'rb').read() if os.path.exists(trace_path) else None
    stderr_bytes = open(stderr_path, 'rb').read() if os.path.exists(stderr_path) else None
    projection = {key: receipt.get(key) for key in
                  ('timed_out', 'exit_code', 'child_role', 'start_ticks', 'tracer_reaped',
                   'stream_complete', 'output_complete', 'cleanup_complete', 'actions',
                   'tracee_identity', 'tracee_exit', 'tracee_reap', 'communicate_timeout',
                   'poll_deadline_exceeded')}
    ok = (projection['timed_out'] is True and projection['exit_code'] == -9 and
          projection['communicate_timeout'] is True and
          projection['tracer_reaped'] is True and
          projection['tracee_identity'] == 'UNKNOWN' and
          projection['tracee_exit'] == 'NOT_ESTABLISHED' and
          projection['stream_complete'] is True and
          projection['output_complete'] is True and
          projection['cleanup_complete'] is True and
          trace_bytes == b'partial-trace\nrest\n' and stderr_bytes == b'tracer-stderr-bytes')
    return case.finish('PASS' if ok else 'FAIL',
                       'tracer timeout is recorded as timed_out with bounded cleanup'
                       if ok else 'tracer timeout receipt misstates the observation window',
                       receipt_projection=_jsonable(projection),
                       trace_bytes_literal=len(trace_bytes or b''),
                       stderr_file_matches=stderr_bytes == b'tracer-stderr-bytes')


def case_f06_boundary_and_c4_fence(lab, case):
    """Late completion stays timed_out; cleanup failure stays incomplete; C4 stays fenced."""
    mod = _lifecycle_module()
    real_subprocess = mod.subprocess
    checks = []

    def note(name, ok, detail):
        checks.append({'check': name, 'ok': bool(ok), 'detail': _jsonable(detail)})

    def fresh_dir(tag):
        path = os.path.join(lab.root, f'row-{case.name}-{tag}')
        os.makedirs(path, mode=0o700)
        return path

    late_read_fd, late_write_fd = os.pipe()

    class LateTracer:
        """Poll stays None until the deadline; communicate then returns normally."""

        def __init__(self):
            self.pid = 4194302
            self.returncode = None
            self.stdout = os.fdopen(late_read_fd, 'rb', closefd=False)
            self.stderr = None
            self.killed = False

        def poll(self):
            return self.returncode

        def kill(self):
            self.killed = True
            self.returncode = -9

        def wait(self, timeout=None):
            return self.returncode

        def communicate(self, timeout=None):
            self.returncode = 0
            return b'late\n', b'e'

    runner = types.SimpleNamespace(
        args=types.SimpleNamespace(old_elf='x', old_source='y'),
        evidence=types.SimpleNamespace(tmp=fresh_dir('late')),
        error_classification=lambda stderr: None)
    original_timeout = mod.ROW_TIMEOUT_S
    try:
        mod.ROW_TIMEOUT_S = 0.3
        # Late completion: poll window exhausted, communicate fine -> still timed_out.
        late = LateTracer()
        mod.subprocess = types.SimpleNamespace(Popen=lambda argv, **kwargs: late,
                                               TimeoutExpired=real_subprocess.TimeoutExpired,
                                               DEVNULL=real_subprocess.DEVNULL,
                                               PIPE=real_subprocess.PIPE)
        late_dir = fresh_dir('late-run')
        receipt = mod._trace_old(runner, late_dir, 'late', 'init', '/x/db', '/x', kill=False)
        note('late_completion_still_timed_out',
             receipt.get('timed_out') is True and receipt.get('exit_code') == 0 and
             receipt.get('poll_deadline_exceeded') is True and
             not any('kill' in str(item) for item in receipt.get('actions', [])),
             {key: receipt.get(key) for key in ('timed_out', 'exit_code', 'actions')})
        # Cleanup drain failure: prefix evidence only, cleanup_complete false.
        read_fd, write_fd = os.pipe()

        class UndrainableTracer:
            def __init__(self):
                self.pid = 4194301
                self.returncode = None
                self.communicated = 0
                self.stdout = os.fdopen(read_fd, 'rb', closefd=False)
                self.stderr = None

            def poll(self):
                return self.returncode

            def kill(self):
                self.returncode = -9

            def wait(self, timeout=None):
                return self.returncode

            def communicate(self, timeout=None):
                self.communicated += 1
                if self.communicated == 1:
                    raise real_subprocess.TimeoutExpired(cmd='t', timeout=timeout,
                                                         output=b'pre\n')
                raise real_subprocess.TimeoutExpired(cmd='t', timeout=timeout)

        undrainable = UndrainableTracer()
        mod.subprocess = types.SimpleNamespace(Popen=lambda argv, **kwargs: undrainable,
                                               TimeoutExpired=real_subprocess.TimeoutExpired,
                                               DEVNULL=real_subprocess.DEVNULL,
                                               PIPE=real_subprocess.PIPE)
        undrain_dir = fresh_dir('undrain')
        receipt = mod._trace_old(runner, undrain_dir, 'undrain', 'init', '/x/db', '/x',
                                 kill=False)
        note('drain_failure_incomplete',
             receipt.get('timed_out') is True and receipt.get('exit_code') == -9 and
             receipt.get('cleanup_complete') is False and
             receipt.get('stream_complete') is False,
             {key: receipt.get(key) for key in ('timed_out', 'exit_code', 'cleanup_complete',
                                                'stream_complete')})
        os.close(write_fd)
        os.close(read_fd)
        os.close(late_write_fd)
        os.close(late_read_fd)
        # kill=True still refuses before ANY effect; row_c4 stays structured NOT_RUN.
        calls = []

        def recording_popen(argv, **kwargs):
            calls.append(list(argv))
            raise AssertionError('Popen must not be called in fenced kill mode')

        mod.subprocess = types.SimpleNamespace(Popen=recording_popen,
                                               TimeoutExpired=real_subprocess.TimeoutExpired,
                                               DEVNULL=real_subprocess.DEVNULL,
                                               PIPE=real_subprocess.PIPE)
        fence_dir = fresh_dir('fence')
        fenced = None
        try:
            mod._trace_old(runner, fence_dir, 'fence', 'init', '/x/db', '/x', kill=True)
        except ValueError as failure:
            fenced = failure
        note('kill_mode_refused_pre_effect',
             fenced is not None and not calls and os.listdir(fence_dir) == [],
             {'calls': calls, 'dir': os.listdir(fence_dir)})
        c4_record = mod.row_c4(object(), fence_dir)
        note('row_c4_structured_not_run',
             c4_record.get('status') == 'NOT_RUN' and
             c4_record.get('tracee_identity') == 'UNKNOWN' and
             c4_record.get('product_crash_position_and_exit') == 'NOT_ESTABLISHED' and
             c4_record.get('product_reap') == 'NOT_ESTABLISHED' and
             os.listdir(fence_dir) == [],
             c4_record)
        # C4receipt evaluation: equal classifications never outrank missing eligibility.
        evaluation = getattr(mod, '_c4receipt_evaluation', None)
        note('evaluation_helper_present', callable(evaluation), {})
        if callable(evaluation):
            ok_window = {'verdict': 'OK'}
            degraded = {'verdict': 'DEGRADED'}
            comparable_raw = {'comparable': True}
            comparable_log = {'comparable': True, 'logical_equal': True}
            complete_step = {'exit_code': 2, 'timed_out': False, 'cleanup_complete': True,
                             'stream_complete': True}
            timeout_step = {'exit_code': -9, 'timed_out': True, 'cleanup_complete': True,
                            'stream_complete': True}

            def side(step, before, after, raw, logical):
                return {'step': step, 'before': before, 'after': after, 'raw': raw,
                        'logical': logical,
                        'classification': {'exit_code': step['exit_code'], 'error': 'unsupported',
                                           'added': [], 'removed': [], 'changed': [],
                                           'main_equal': True, 'business_equal': True}}

            both_timeout = {'traced': side(timeout_step, ok_window, ok_window, comparable_raw,
                                           comparable_log),
                            'untraced': side(timeout_step, ok_window, ok_window, comparable_raw,
                                             comparable_log)}
            result = evaluation(both_timeout, True)
            note('both_timeout_not_eligible',
                 result['mechanism']['traced_untraced_equivalent'] is True and
                 result['pass_eligible'] is False and
                 'traced_timeout' in result['not_pass_reasons'] and
                 'untraced_timeout' in result['not_pass_reasons'] and
                 result['status'] != 'PASS', result)
            dirty_cleanup = {'traced': side(
                {'exit_code': 2, 'timed_out': False, 'cleanup_complete': False,
                 'stream_complete': True}, ok_window, ok_window, comparable_raw, comparable_log),
                'untraced': side(complete_step, ok_window, ok_window, comparable_raw,
                                 comparable_log)}
            result = evaluation(dirty_cleanup, True)
            note('cleanup_incomplete_reason',
                 result['pass_eligible'] is False and
                 'traced_cleanup_incomplete' in result['not_pass_reasons'], result)
            degraded_window = {'traced': side(complete_step, degraded, ok_window, comparable_raw,
                                              comparable_log),
                               'untraced': side(complete_step, ok_window, ok_window,
                                                comparable_raw, comparable_log)}
            result = evaluation(degraded_window, True)
            note('window_incomplete_reason',
                 result['pass_eligible'] is False and
                 'traced_window_incomplete' in result['not_pass_reasons'], result)
            valid = {'traced': side(complete_step, ok_window, ok_window, comparable_raw,
                                    comparable_log),
                     'untraced': side(complete_step, ok_window, ok_window, comparable_raw,
                                      comparable_log)}
            result = evaluation(valid, True)
            note('valid_case_eligible',
                 result['pass_eligible'] is True and not result['not_pass_reasons'] and
                 result['status'] == 'PASS', result)
    finally:
        mod.ROW_TIMEOUT_S = original_timeout
        mod.subprocess = real_subprocess
    ok = all(item['ok'] for item in checks)
    return case.finish('PASS' if ok else 'FAIL',
                       'timeout truth, cleanup completeness and the C4 fence all hold'
                       if ok else 'a timeout/cleanup/fence boundary misbehaved',
                       checks=checks)


def case_fo3_libsqlite_dedup(lab, case):
    """R4: identical libsqlite map paths hash once; distinct paths and order survive."""
    mod = _lifecycle_module()
    path_a = '/synthetic/lib/libsqlite3.so.0'
    path_b = '/synthetic/other/libsqlite3.so.0'
    synthetic_maps = '\n'.join([
        f'7f0000000000-7f0000010000 r--p 00000000 fd:01 11 {path_a}',
        f'7f0000010000-7f0000020000 r-xp 00001000 fd:01 11 {path_a}',
        f'7f0000020000-7f0000030000 r--p 00002000 fd:01 11 {path_a}',
        f'7f1000000000-7f1000010000 r--p 00000000 fd:01 12 {path_b}',
        '7f2000000000-7f2000010000 r--p 00000000 fd:01 13 /synthetic/lib/libother.so.9',
    ]) + '\n'
    real_open = open
    real_hash = mod.file_sha256
    counts = {}

    def spy_hash(path):
        counts[str(path)] = counts.get(str(path), 0) + 1
        return '0' * 64

    def fake_open(file, *args, **kwargs):
        if isinstance(file, str) and file == '/proc/self/maps':
            return io.StringIO(synthetic_maps)
        return real_open(file, *args, **kwargs)

    identity = mod.Identity.__new__(mod.Identity)
    identity.started_utc = 'synthetic-utc'
    identity.evidence = types.SimpleNamespace(root=lab.root, tmp=lab.root)
    identity.args = types.SimpleNamespace(synthetic='selfcheck')
    had_open = 'open' in vars(mod)
    mod.open = fake_open
    mod.file_sha256 = spy_hash
    try:
        environment = identity._environment()
    finally:
        mod.file_sha256 = real_hash
        if had_open:
            mod.open = real_open
        else:
            del mod.__dict__['open']
    recorded = [entry.get('path') for entry in environment.get('libsqlite3_paths', [])]
    ok = (recorded == [path_a, path_b] and
          counts.get(path_a) == 1 and counts.get(path_b) == 1 and
          '/synthetic/lib/libother.so.9' not in counts)
    return case.finish('PASS' if ok else 'FAIL',
                       'each unique libsqlite path hashed exactly once in first-seen order'
                       if ok else 'libsqlite path collection still mis-deduplicates',
                       recorded_paths=recorded, hash_counts={key: counts[key] for key in
                                                             sorted(counts)
                                                             if 'libsqlite3' in key})


CASES = [
    case_noop_stability,
    case_clean_copy,
    case_committed_wal_row,
    case_copy_same_length,
    case_copy_truncation,
    case_copy_missing_member,
    case_copy_wrong_mapping,
    case_wal_same_length_change,
    case_shm_only_change,
    case_main_byte_change,
    case_inode_replacement,
    case_sidecar_added_removed,
    case_permission_change,
    case_time_changes,
    case_same_mode_fchmod,
    case_refused_members,
    case_identity_chain,
    case_wal_truncated_mid_commit,
    case_wal_frame_byte_flip,
    case_wal_header_flip,
    case_close_failure_recorded,
    case_atime_control,
    case_f04_missing_evidence_gates,
    case_f04_row_gate_propagation,
    case_f04_dump_validity_matrix,
    case_f04_copy_and_stability_gates,
    case_f04_shared_row_gates,
    case_f05_stopped_child_stub_write_failure,
    case_f05_exception_matrix,
    case_f05_holder_integrations,
    case_f06_trace_timeout_receipt,
    case_f06_boundary_and_c4_fence,
    case_fo3_libsqlite_dedup,
]

MECHANISM_CASES = {
    case_f04_missing_evidence_gates,
    case_f04_row_gate_propagation,
    case_f04_dump_validity_matrix,
    case_f04_copy_and_stability_gates,
    case_f04_shared_row_gates,
    case_f05_stopped_child_stub_write_failure,
    case_f05_exception_matrix,
    case_f05_holder_integrations,
    case_f06_trace_timeout_receipt,
    case_f06_boundary_and_c4_fence,
    case_fo3_libsqlite_dedup,
}


def main(argv):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True,
                        help='allowed root for throwaway fixtures (data disk, 0700)')
    parser.add_argument('--out', required=True, help='evidence JSON output path')
    parser.add_argument('--only', default=None,
                        help='comma list of case names to run (baseline red-group '
                             'selection); the default runs the complete entry')
    arguments = parser.parse_args(argv)
    if os.geteuid() == 0:
        raise SystemExit('refusing to run the selfcheck as root')
    previous_umask = os.umask(0o077)
    builders = list(CASES)
    selected = None
    if arguments.only:
        selected = [item.strip() for item in arguments.only.split(',') if item.strip()]
        known = {builder.__name__ for builder in CASES}
        unknown = [item for item in selected if item not in known]
        if unknown:
            raise SystemExit(f'unknown case name(s) in --only: {unknown}')
        builders = [builder for builder in CASES if builder.__name__ in set(selected)]
    fixtures_root = os.path.join(arguments.root, 'selfcheck-fixtures')
    os.makedirs(fixtures_root, mode=0o700, exist_ok=True)
    lab = Lab(fixtures_root)
    started = utc()
    records = []
    for builder in builders:
        case = Case(builder.__name__,
                    'conditional' if builder in (
                        case_same_mode_fchmod, case_atime_control) else
                    'mechanism' if builder in MECHANISM_CASES else
                    'positive' if builder.__name__.startswith('case_noop') or
                    builder in (case_clean_copy, case_committed_wal_row) else 'negative')
        started_case = time.monotonic()
        try:
            records.append(builder(lab, case))
        except Exception as failure:  # keep going; record the honest failure
            records.append(case.finish('FAIL', f'exception: [{type(failure).__name__}] {failure}'))
        records[-1]['elapsed_s'] = round(time.monotonic() - started_case, 6)
    observer_path = os.path.abspath(obs.__file__)
    summary = {
        'tool': 'store_compatibility_observer_selfcheck',
        'started_at': started, 'ended_at': utc(),
        'observer_file': observer_path,
        'observer_sha256': hashlib.sha256(open(observer_path, 'rb').read()).hexdigest(),
        'python': sys.version, 'sqlite_runtime': sqlite3.sqlite_version,
        'mount_regime': obs.mount_records(arguments.root)['options'],
        'umask': '0077',
        'selected_only': selected,
        'case_count': len(records),
        'passed': sum(1 for item in records if item['status'] == 'PASS'),
        'failed': sum(1 for item in records if item['status'] == 'FAIL'),
        'not_established': sum(1 for item in records if item['status'] == 'NOT_ESTABLISHED'),
        'observer_methods_exercised': [
            'snapshot', 'stability_check', 'compare_snapshots', 'frozen_copy',
            'logical_dump', 'read_member', 'stat_member', '_assert_same_identity',
            '_copy_stream', 'encode_value', 'committed_fact_visible',
            'snapshot_completeness', 'logical_dump_completeness', 'diff_logical'],
    }
    if 'module' in _LIFECYCLE_CACHE:
        summary['lifecycle_file'] = _LIFECYCLE_CACHE['path']
        summary['lifecycle_sha256'] = _LIFECYCLE_CACHE['sha256']
    summary['status'] = ('SELFCHECK_PASS' if summary['failed'] == 0 else 'SELFCHECK_FAIL')
    obs.write_json(arguments.out, {'summary': summary, 'cases': records})
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if summary['failed'] == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
