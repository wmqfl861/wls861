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
"""
from __future__ import annotations

import hashlib
import json
import os
import shutil
import sqlite3
import stat
import sys
import time

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
]


def main(argv):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', required=True,
                        help='allowed root for throwaway fixtures (data disk, 0700)')
    parser.add_argument('--out', required=True, help='evidence JSON output path')
    arguments = parser.parse_args(argv)
    if os.geteuid() == 0:
        raise SystemExit('refusing to run the selfcheck as root')
    previous_umask = os.umask(0o077)
    fixtures_root = os.path.join(arguments.root, 'selfcheck-fixtures')
    os.makedirs(fixtures_root, mode=0o700, exist_ok=True)
    lab = Lab(fixtures_root)
    started = utc()
    records = []
    for builder in CASES:
        case = Case(builder.__name__, 'conditional' if builder in (
            case_same_mode_fchmod, case_atime_control) else
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
        'case_count': len(records),
        'passed': sum(1 for item in records if item['status'] == 'PASS'),
        'failed': sum(1 for item in records if item['status'] == 'FAIL'),
        'not_established': sum(1 for item in records if item['status'] == 'NOT_ESTABLISHED'),
        'observer_methods_exercised': [
            'snapshot', 'stability_check', 'compare_snapshots', 'frozen_copy',
            'logical_dump', 'read_member', 'stat_member', '_assert_same_identity',
            '_copy_stream', 'encode_value', 'committed_fact_visible'],
    }
    summary['status'] = ('SELFCHECK_PASS' if summary['failed'] == 0 else 'SELFCHECK_FAIL')
    obs.write_json(arguments.out, {'summary': summary, 'cases': records})
    print(json.dumps(summary, ensure_ascii=False, indent=2))
    return 0 if summary['failed'] == 0 else 1


if __name__ == '__main__':
    raise SystemExit(main(sys.argv[1:]))
