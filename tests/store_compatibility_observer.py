#!/usr/bin/env python3
"""Strict raw and logical observer for Store database-prefix files (N5 evidence tool).

Manually invoked evidence tool, deliberately NOT registered as a CTest suite.
It observes only fixture files owned by the current ordinary user below an
explicitly allowed root on an ext4 mount, opens them read-only with
O_NOATIME|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK descriptors and never writes into a
fixture directory.  Snapshots capture complete stat/statx metadata, xattrs and
full content hashes with an lstat -> fd -> EOF -> fd/path identity recheck.
Frozen copies are verified by an independent readback before any SQLite open;
logical dumps then cover every real table with explicit value encodings.  The
observer never opens an original fixture with SQLite, never checkpoints and
never uses immutable=1.
"""
from __future__ import annotations

import base64
import ctypes
import datetime
import errno
import hashlib
import json
import os
import sqlite3
import stat
import time

UNKNOWN = 'UNKNOWN'
READ_LIMIT_BYTES = 16 * 1024 * 1024
OPEN_FLAGS_NAME = ['O_RDONLY', 'O_NOATIME', 'O_NOFOLLOW', 'O_CLOEXEC', 'O_NONBLOCK']
OPEN_FLAGS = (os.O_RDONLY | os.O_NOATIME | os.O_NOFOLLOW | os.O_CLOEXEC | os.O_NONBLOCK)
DIR_OPEN_FLAGS = os.O_RDONLY | os.O_DIRECTORY | os.O_NOATIME | os.O_NOFOLLOW | os.O_CLOEXEC
IDENTITY_FIELDS = ('dev', 'ino', 'type', 'mode', 'uid', 'gid', 'nlink', 'size',
                   'blocks', 'atime_ns', 'mtime_ns', 'ctime_ns')
SIDE_ROLES = {'-wal': 'wal', '-shm': 'shm', '-journal': 'journal',
              '.writer-lock': 'writer_lock'}

_LIBC = ctypes.CDLL(None, use_errno=True)
_TYPE_NAMES = {0: 'unknown', 1: 'fifo', 2: 'character', 3: 'socket', 4: 'directory',
               5: 'symlink', 6: 'block', 8: 'regular', 9: 'symlink', 10: 'symlink',
               12: 'socket'}


class ObservationError(Exception):
    """A strict observation failed; .reason and .detail keep the exact cause."""

    def __init__(self, reason: str, detail=None):
        super().__init__(reason if detail is None else f'{reason}: {detail}')
        self.reason = reason
        self.detail = detail

    def as_dict(self):
        return {'reason': self.reason, 'detail': _jsonable(self.detail)}


def _jsonable(value):
    if isinstance(value, (str, int, float, bool)) or value is None:
        return value
    if isinstance(value, bytes):
        return value.hex()
    if isinstance(value, (list, tuple)):
        return [_jsonable(item) for item in value]
    if isinstance(value, dict):
        return {str(key): _jsonable(item) for key, item in value.items()}
    return repr(value)


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def canonical_json(payload) -> str:
    return json.dumps(payload, ensure_ascii=False, sort_keys=True,
                      separators=(',', ':'))


def sha256_json(payload) -> str:
    return hashlib.sha256(canonical_json(payload).encode('utf-8')).hexdigest()


def write_json(path, payload, mode=0o600):
    """Create a new evidence file; never overwrite an existing target."""
    data = json.dumps(payload, ensure_ascii=False, indent=2) + '\n'
    fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_CLOEXEC, mode)
    try:
        os.write(fd, data.encode('utf-8'))
    finally:
        os.close(fd)


def _under(path, root):
    # Lexical containment only: realpath would resolve symlink members and
    # defeat the O_NOFOLLOW final-component guard. Intermediate components are
    # private evidence-root directories owned by the current user.
    real = os.path.abspath(path)
    root_real = os.path.abspath(root)
    return os.path.commonpath([real, root_real]) == root_real, real


def require_under(path, root, what):
    ok, real = _under(path, root)
    if not ok:
        raise ObservationError('path_outside_allowed_root',
                               {'what': what, 'path': str(path), 'root': str(root)})
    return real


# ---------------------------------------------------------------- statx/xattr
_STATX_BTIME = 0x00000800
_STATX_MNT_ID = 0x00001000


def statx_extra(path):
    """Birth time and mount id through the statx syscall when supported."""
    try:
        libc = ctypes.CDLL(None, use_errno=True)
        statx = libc.statx
    except AttributeError:
        return {'available': False, 'btime_ns': UNKNOWN, 'mnt_id': UNKNOWN,
                'note': 'statx symbol unavailable'}
    buf = ctypes.create_string_buffer(256)
    at_fdcwd = -100
    at_symlink_nofollow = 0x100
    result = statx(ctypes.c_int(at_fdcwd), os.fsencode(str(path)),
                   ctypes.c_uint(at_symlink_nofollow),
                   ctypes.c_uint(_STATX_BTIME | _STATX_MNT_ID), buf)
    if result != 0:
        return {'available': False, 'errno': ctypes.get_errno(),
                'btime_ns': UNKNOWN, 'mnt_id': UNKNOWN}
    mask = int.from_bytes(buf[0:4], 'little')

    def ts(offset):
        seconds = int.from_bytes(buf[offset:offset + 8], 'little', signed=True)
        nanos = int.from_bytes(buf[offset + 8:offset + 12], 'little')
        return seconds * 10 ** 9 + nanos

    return {'available': True, 'mask': mask,
            'btime_ns': ts(80) if mask & _STATX_BTIME else UNKNOWN,
            'mnt_id': int.from_bytes(buf[144:152], 'little') if mask & _STATX_MNT_ID else UNKNOWN}


def xattr_extra(path, limit=16):
    """Explicit xattr inventory; values above 4096 bytes keep only their length."""
    try:
        names = os.listxattr(path, follow_symlinks=False)
    except OSError as failure:
        return {'available': False, 'errno': failure.errno, 'entries': UNKNOWN}
    entries = {}
    for name in sorted(names)[:limit]:
        try:
            value = os.getxattr(path, name, follow_symlinks=False)
        except OSError as failure:
            entries[name] = {'error': failure.errno}
            continue
        entries[name] = (value.hex() if len(value) <= 4096
                         else {'length': len(value), 'truncated': True})
    if len(names) > limit:
        entries['__truncated__'] = {'listed': len(names), 'kept': limit}
    return {'available': True, 'count': len(names), 'entries': entries}


def mount_records(path):
    """Full mountinfo/mounts entries for the deepest mount covering path."""
    real = os.path.realpath(path)
    with open('/proc/self/mountinfo', encoding='utf-8') as handle:
        mountinfo = handle.read().splitlines()
    with open('/proc/self/mounts', encoding='utf-8') as handle:
        mounts = handle.read().splitlines()
    best = None
    for line in mountinfo:
        fields = line.split()
        if len(fields) < 7 or '-' not in fields:
            continue
        separator = fields.index('-')
        if separator + 2 >= len(fields):
            continue
        mount_point, fstype = fields[4], fields[separator + 1]
        try:
            if os.path.commonpath([real, mount_point]) == mount_point:
                if best is None or len(mount_point) > len(best[0]):
                    best = (mount_point, line, fstype, fields[5],
                            fields[separator + 2] if len(fields) > separator + 2 else UNKNOWN,
                            fields[separator + 3] if len(fields) > separator + 3 else UNKNOWN)
        except ValueError:
            continue
    if best is None:
        return {'mount_point': UNKNOWN, 'mountinfo_entry': UNKNOWN,
                'mounts_entries': [], 'filesystem': UNKNOWN, 'options': UNKNOWN,
                'source': UNKNOWN, 'super_options': UNKNOWN}
    mount_point, line, fstype, options, source, super_options = best
    return {'mount_point': mount_point, 'mountinfo_entry': line,
            'mounts_entries': [entry for entry in mounts
                               if entry.split()[1:2] == [mount_point]],
            'filesystem': fstype, 'options': options, 'source': source,
            'super_options': super_options}


# ---------------------------------------------------------------- raw members
def stat_member(path):
    """Metadata without reading file content: lstat plus statx/xattr extras."""
    info = os.lstat(path)
    kind = stat.S_IFMT(info.st_mode)
    payload = {
        'dev': info.st_dev, 'ino': info.st_ino, 'type': _TYPE_NAMES.get(
            ((kind >> 12) & 0xF) or 0, oct(kind)), 'mode': f'{stat.S_IMODE(info.st_mode):04o}',
        'uid': info.st_uid, 'gid': info.st_gid, 'nlink': info.st_nlink,
        'size': info.st_size, 'blocks': info.st_blocks,
        'atime_ns': info.st_atime_ns, 'mtime_ns': info.st_mtime_ns,
        'ctime_ns': info.st_ctime_ns,
    }
    payload.update(statx_extra(path))
    payload['xattrs'] = xattr_extra(path)
    return payload


def identity_of(info):
    return {field: info[field] for field in IDENTITY_FIELDS}


def _assert_same_identity(before, after, stage):
    left, right = identity_of(before), identity_of(after)
    changed = {field: [left[field], right[field]] for field in IDENTITY_FIELDS
               if left[field] != right[field]}
    if changed:
        raise ObservationError('identity_changed_' + stage, changed)


def _stat_identity(info):
    """Identity fields from a raw os.stat_result."""
    kind = (stat.S_IFMT(info.st_mode) >> 12) & 0xF
    return {'dev': info.st_dev, 'ino': info.st_ino,
            'type': _TYPE_NAMES.get(kind, 'other'),
            'mode': f'{stat.S_IMODE(info.st_mode):04o}', 'uid': info.st_uid,
            'gid': info.st_gid, 'nlink': info.st_nlink, 'size': info.st_size,
            'blocks': info.st_blocks, 'atime_ns': info.st_atime_ns,
            'mtime_ns': info.st_mtime_ns, 'ctime_ns': info.st_ctime_ns}


def read_member(path, allowed_root):
    """Read one member to EOF with O_NOATIME flags and a full identity chain."""
    real = require_under(path, allowed_root, 'member')
    before = stat_member(real)
    if stat.S_ISLNK(os.lstat(real).st_mode):
        raise ObservationError('symlink_member', real)
    if not stat.S_ISREG(os.lstat(real).st_mode):
        raise ObservationError('non_regular_member',
                               {'path': real, 'type': before['type']})
    try:
        descriptor = os.open(real, OPEN_FLAGS)
    except OSError as failure:
        if failure.errno == errno.ELOOP:
            raise ObservationError('symlink_member', real) from failure
        if failure.errno in (errno.EPERM, errno.EINVAL):
            raise ObservationError('noatime_open_refused',
                                   {'path': real, 'errno': failure.errno}) from failure
        raise ObservationError('open_failed', {'path': real, 'errno': failure.errno}) from failure
    try:
        opened = os.fstat(descriptor)
        if opened.st_nlink != 1:
            raise ObservationError('aliased_member',
                                   {'path': real, 'nlink': opened.st_nlink})
        _assert_same_identity(_stat_identity(opened), before, 'during_open')
        digest = hashlib.sha256()
        total = 0
        while True:
            block = os.read(descriptor, 1024 * 1024)
            if not block:
                break
            total += len(block)
            if total > READ_LIMIT_BYTES:
                raise ObservationError('read_bound_exceeded',
                                       {'path': real, 'limit': READ_LIMIT_BYTES})
            digest.update(block)
        final = os.fstat(descriptor)
        _assert_same_identity(_stat_identity(final), before, 'during_read')
        after = stat_member(real)
        _assert_same_identity(before, after, 'after_read')
        return {'meta': before, 'sha256': digest.hexdigest(), 'bytes': total,
                'open_flags': OPEN_FLAGS_NAME}
    finally:
        os.close(descriptor)


def classify_member(database_name, member_name):
    if member_name == database_name:
        return 'main'
    for suffix, role in SIDE_ROLES.items():
        if member_name == database_name + suffix:
            return role
    if member_name.startswith(database_name):
        return 'unknown_prefix_member'
    return 'unrelated'


def _getdents64(descriptor):
    """Directory enumeration through the dir fd without consuming other fds."""
    buffer = ctypes.create_string_buffer(64 * 1024)
    entries = []
    try:
        getdents64 = _LIBC.getdents64
        getdents64.restype = ctypes.c_long
    except AttributeError:
        def getdents64(fd, buf, size, _syscall=_LIBC.syscall):
            _syscall.restype = ctypes.c_long
            return _syscall(ctypes.c_long(217), ctypes.c_int(fd), buf, ctypes.c_uint(size))
    while True:
        count = getdents64(ctypes.c_int(descriptor), buffer, ctypes.c_uint(len(buffer)))
        if count == 0:
            break
        if count < 0:
            raise ObservationError('getdents64_failed', ctypes.get_errno())
        raw, offset = buffer.raw[:count], 0
        while offset < count:
            record_length = int.from_bytes(raw[offset + 16:offset + 18], 'little')
            dtype = raw[offset + 18]
            name = raw[offset + 19:offset + record_length].split(b'\0')[0]
            entries.append((name.decode('utf-8', 'surrogateescape'), int(dtype)))
            offset += record_length
    return entries


def snapshot(database, allowed_root):
    """Complete raw observation of the database directory prefix family."""
    real = require_under(database, allowed_root, 'database')
    directory = os.path.dirname(real)
    require_under(directory, allowed_root, 'fixture directory')
    mounts = mount_records(real)
    if mounts.get('filesystem') != 'ext4':
        raise ObservationError('not_ext4_fixture', mounts.get('filesystem'))
    directory_info = stat_member(directory)
    try:
        dir_fd = os.open(directory, DIR_OPEN_FLAGS)
    except OSError as failure:
        raise ObservationError('directory_open_flags_refused',
                               {'errno': failure.errno, 'flags': 'O_RDONLY|O_DIRECTORY|O_NOATIME|O_NOFOLLOW|O_CLOEXEC'}) from failure
    try:
        entries = sorted(_getdents64(dir_fd))
    finally:
        os.close(dir_fd)
    members = {}
    restricted = []
    for name, dtype in entries:
        role = classify_member(os.path.basename(real), name)
        if role == 'unrelated':
            continue
        try:
            member = read_member(os.path.join(directory, name), allowed_root)
        except ObservationError as failure:
            restricted.append(failure.as_dict())
            members[name] = {'role': role, 'status': 'refused', 'error': failure.as_dict()}
            continue
        members[name] = {'role': role, 'status': 'present', **member}
    return {
        'database': os.path.basename(real), 'directory': directory,
        'directory_identity': directory_info,
        'entries': [{'name': name, 'd_type': _TYPE_NAMES.get(dtype, str(dtype))}
                    for name, dtype in entries],
        'enumeration': {'flags': 'O_RDONLY|O_DIRECTORY|O_NOATIME|O_NOFOLLOW|O_CLOEXEC',
                        'noatime_ok': True},
        'members': members,
        'mount_records': mounts,
        'restricted': restricted,
        'observer': {'pid': os.getpid(), 'uid': os.geteuid(), 'umask_note': 'recorded by caller',
                     'monotonic_ns': time.monotonic_ns(), 'utc': utc_now()},
    }


_VOLATILE_OBSERVER_KEYS = ('monotonic_ns', 'utc')


def _strip_volatile(node):
    if isinstance(node, dict):
        return {key: (_strip_volatile(value) if key not in _VOLATILE_OBSERVER_KEYS else '<volatile>')
                for key, value in node.items()}
    if isinstance(node, list):
        return [_strip_volatile(item) for item in node]
    return node


def _recursive_diff(before, after, path=''):
    differences = []
    if isinstance(before, dict) and isinstance(after, dict):
        for key in sorted(set(before) | set(after)):
            if key not in before:
                differences.append({'field': f'{path}.{key}', 'change': ['<absent>', after[key]]})
            elif key not in after:
                differences.append({'field': f'{path}.{key}', 'change': [before[key], '<absent>']})
            else:
                differences.extend(_recursive_diff(before[key], after[key], f'{path}.{key}'))
    elif before != after:
        differences.append({'field': path, 'change': [before, after]})
    return differences


def compare_snapshots(before, after):
    """Field-level diff; new/missing/unknown members are classified explicitly."""
    stripped_before = _strip_volatile(before)
    stripped_after = _strip_volatile(after)
    differences = _recursive_diff(stripped_before, stripped_after)
    members_before = set(before['members'])
    members_after = set(after['members'])
    added = sorted(members_after - members_before)
    removed = sorted(members_before - members_after)
    per_file = {}
    for name in sorted(members_before & members_after):
        entry_before, entry_after = before['members'][name], after['members'][name]
        content_equal = (entry_before.get('sha256') == entry_after.get('sha256') and
                         entry_before.get('bytes') == entry_after.get('bytes'))
        meta_before = {field: entry_before.get(field, entry_before.get('meta', {}).get(field))
                       for field in IDENTITY_FIELDS}
        meta_after = {field: entry_after.get(field, entry_after.get('meta', {}).get(field))
                      for field in IDENTITY_FIELDS}
        meta_equal = meta_before == meta_after
        per_file[name] = {
            'role': entry_after.get('role'),
            'content_equal': content_equal,
            'metadata_equal': meta_equal,
            'metadata_changed_fields': {field: [meta_before[field], meta_after[field]]
                                        for field in IDENTITY_FIELDS if meta_before[field] != meta_after[field]},
            'raw_equal': entry_before == entry_after,
        }
    for name in added:
        per_file[name] = {'role': after['members'][name].get('role'), 'status': 'added',
                          'content_equal': False, 'metadata_equal': False, 'raw_equal': False}
    for name in removed:
        per_file[name] = {'role': before['members'][name].get('role'), 'status': 'removed',
                          'content_equal': False, 'metadata_equal': False, 'raw_equal': False}
    unknown = sorted(name for name in after['members']
                     if after['members'][name].get('role') == 'unknown_prefix_member')
    return {'strict_equal': not differences, 'diff_count': len(differences),
            'differences': differences[:200], 'added': added, 'removed': removed,
            'per_file': per_file, 'unknown_members': unknown}


def stability_check(database, allowed_root, rounds=2):
    """Observer-only consecutive observations must match, including all times."""
    observations = [snapshot(database, allowed_root) for _ in range(rounds)]
    comparisons = [compare_snapshots(observations[index], observations[index + 1])
                   for index in range(rounds - 1)]
    return {'stable': all(item['strict_equal'] for item in comparisons),
            'rounds': rounds, 'comparisons': comparisons, 'observations': observations}


# ---------------------------------------------------------------- frozen copy
def _copy_stream(source_real, destination_path):
    descriptor = os.open(source_real, OPEN_FLAGS)
    try:
        digest = hashlib.sha256()
        destination = os.open(destination_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL |
                              os.O_CLOEXEC | os.O_NOFOLLOW, 0o600)
        try:
            total = 0
            while True:
                block = os.read(descriptor, 1024 * 1024)
                if not block:
                    break
                os.write(destination, block)
                digest.update(block)
                total += len(block)
            return {'sha256': digest.hexdigest(), 'bytes': total}
        finally:
            os.close(destination)
    finally:
        os.close(descriptor)


def frozen_copy(database, destination_dir, allowed_root):
    """Copy the complete prefix family inside a frozen window and verify readback.

    Originals must be byte- and metadata-identical across the copy; the copy is
    proven by an independent readback against the original before and after the
    copy window before any SQLite connection may touch it.
    """
    real = require_under(database, allowed_root, 'database')
    require_under(destination_dir, allowed_root, 'copy destination')
    os.makedirs(destination_dir, mode=0o700, exist_ok=False)
    before = snapshot(real, allowed_root)
    present = {name: entry for name, entry in before['members'].items()
               if entry.get('status') == 'present'}
    stream = {}
    for name in sorted(present):
        stream[name] = _copy_stream(os.path.join(os.path.dirname(real), name),
                                    os.path.join(destination_dir, name))
    after = snapshot(real, allowed_root)
    if _strip_volatile(before) != _strip_volatile(after):
        raise ObservationError('original_changed_during_copy',
                                compare_snapshots(before, after)['differences'][:20])
    copy_meta = {}
    readback = {}
    for name in sorted(present):
        copy_path = os.path.join(destination_dir, name)
        if os.path.lexists(copy_path) and not (
                os.path.exists(copy_path) and not os.path.islink(copy_path)):
            raise ObservationError('copy_readback_mismatch',
                                   {'name': name, 'copy_kind': 'not_a_plain_regular_file'})
        if not os.path.exists(copy_path):
            raise ObservationError('copy_readback_mismatch', {'name': name, 'missing': True})
        copy_info = stat_member(copy_path)
        if copy_info['type'] != 'regular' or copy_info['nlink'] != 1 or copy_info['uid'] != os.geteuid():
            raise ObservationError('copy_not_private_regular', {'name': name, 'meta': copy_info})
        entry = read_member(copy_path, allowed_root)
        original = present[name]
        mismatch = {}
        if entry['bytes'] != original['bytes']:
            mismatch['size'] = [original['bytes'], entry['bytes']]
        if entry['sha256'] != original['sha256']:
            mismatch['sha256'] = [original['sha256'], entry['sha256']]
        if stream[name]['sha256'] != original['sha256']:
            mismatch['stream_sha'] = [original['sha256'], stream[name]['sha256']]
        readback[name] = {'size': entry['bytes'], 'sha256': entry['sha256'],
                          'stream_sha256': stream[name]['sha256'],
                          'matches_original': not mismatch}
        if mismatch:
            raise ObservationError('copy_readback_mismatch', {'name': name, **mismatch})
        copy_meta[name] = copy_info
    copied = sorted(os.listdir(destination_dir))
    if copied != sorted(present):
        raise ObservationError('copy_mapping_mismatch',
                               {'directory': copied, 'originals': sorted(present)})
    return {'copy_dir': destination_dir, 'original_before': before, 'original_after': after,
            'readback': readback, 'copy_metadata': copy_meta,
            'member_mapping': {name: name for name in sorted(present)}}


# ---------------------------------------------------------------- logical dump
def quote_identifier(name):
    return '"' + name.replace('"', '""') + '"'


def encode_value(value):
    if value is None:
        return {'k': 'null'}
    if isinstance(value, bool):
        return {'k': 'int', 'v': int(value)}
    if isinstance(value, int):
        return {'k': 'int', 'v': value}
    if isinstance(value, float):
        return {'k': 'real', 'bits_hex': value.hex()}
    if isinstance(value, str):
        return {'k': 'text', 'v': value}
    if isinstance(value, (bytes, bytearray, memoryview)):
        return {'k': 'blob', 'b64': base64.b64encode(bytes(value)).decode('ascii')}
    raise ObservationError('unexpected_sqlite_value_type', repr(type(value)))


def logical_dump(database, allowed_root, connect=None):
    """Normal (non-immutable) SQLite read of a verified copy; explicit close."""
    real = require_under(database, allowed_root, 'copy database')
    directory = os.path.dirname(real)
    pre_members = {name: {'size': entry['bytes'], 'sha256': entry['sha256']}
                   for name, entry in snapshot(real, allowed_root)['members'].items()
                   if entry.get('status') == 'present'}
    record = {'database': real, 'sqlite_runtime': sqlite3.sqlite_version,
              'pre_open_members': pre_members, 'open_mode': 'normal_readwrite_not_immutable'}
    connection = None
    try:
        opener = connect or sqlite3.connect
        connection = opener(real)
        pragmas = {}
        for pragma in ('user_version', 'journal_mode', 'page_size', 'page_count',
                       'freelist_count', 'encoding', 'auto_vacuum'):
            pragmas[pragma] = [encode_value(row[0]) for row in connection.execute(f'PRAGMA {pragma}')]
        record['pragmas'] = pragmas
        record['quick_check'] = [list(row) for row in connection.execute('PRAGMA quick_check')]
        schema = []
        for row in connection.execute(
                'SELECT type, name, tbl_name, rootpage, sql FROM sqlite_schema'):
            schema.append([row[0], row[1], row[2],
                           encode_value(row[3]).get('v', row[3]), row[4]])
        record['sqlite_schema'] = schema
        tables = {}
        for kind, name, _, _, _ in schema:
            if kind != 'table':
                continue
            rows = []
            for row in connection.execute(f'SELECT * FROM {quote_identifier(name)}'):
                rows.append([encode_value(cell) for cell in row])
            tables[name] = {'row_count': len(rows), 'rows': rows}
        record['tables'] = tables
        record['logical_sha256'] = sha256_json({'pragmas': pragmas, 'tables': tables,
                                                'sqlite_schema': schema})
        record['dump_utc'] = utc_now()
    except sqlite3.Error as failure:
        record['sqlite_error'] = f'[{type(failure).__name__}] {failure}'
    finally:
        if connection is not None:
            try:
                connection.close()
                record['close'] = 'ok'
            except Exception as failure:  # recorded, never swallowed
                record['close'] = f'[{type(failure).__name__}] {failure}'
    post_members = {}
    for name in sorted(os.listdir(directory)):
        if not name.startswith(os.path.basename(real)):
            continue
        entry = read_member(os.path.join(directory, name), allowed_root)
        post_members[name] = {'size': entry['bytes'], 'sha256': entry['sha256']}
    record['post_close_members'] = post_members
    return record


def diff_logical(before, after):
    """Table-level first differences between two logical dumps."""
    differences = []
    if before.get('logical_sha256') == after.get('logical_sha256'):
        return {'logical_equal': True, 'differences': []}
    left, right = before.get('tables', {}), after.get('tables', {})
    for name in sorted(set(left) | set(right)):
        if name not in left:
            differences.append({'table': name, 'change': 'added'})
        elif name not in right:
            differences.append({'table': name, 'change': 'removed'})
        elif left[name]['rows'] != right[name]['rows']:
            for index, (row_a, row_b) in enumerate(zip(left[name]['rows'], right[name]['rows'])):
                if row_a != row_b:
                    differences.append({'table': name, 'row': index,
                                        'before': row_a, 'after': row_b})
                    break
            else:
                differences.append({'table': name, 'row': min(len(left[name]['rows']),
                                                              len(right[name]['rows'])),
                                    'change': 'row_count', 'before': left[name]['row_count'],
                                    'after': right[name]['row_count']})
    for pragma in sorted(set(before.get('pragmas', {})) | set(after.get('pragmas', {}))):
        if before.get('pragmas', {}).get(pragma) != after.get('pragmas', {}).get(pragma):
            differences.append({'pragma': pragma, 'before': before['pragmas'].get(pragma),
                                'after': after['pragmas'].get(pragma)})
    return {'logical_equal': False, 'differences': differences[:50]}


def committed_fact_visible(dump, table, encoded_row):
    """True when a row equal to encoded_row (encoded cells) is visible in the dump."""
    rows = dump.get('tables', {}).get(table, {}).get('rows', [])
    return any(row == encoded_row for row in rows)


def main(argv):
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=['snapshot', 'stability', 'copy', 'dump'])
    parser.add_argument('database')
    parser.add_argument('--allowed-root', required=True)
    parser.add_argument('--out', required=True)
    parser.add_argument('--dest', help='copy destination directory')
    parser.add_argument('--rounds', type=int, default=2)
    arguments = parser.parse_args(argv)
    if os.geteuid() == 0:
        raise SystemExit('refusing to run the strict observer as root')
    if arguments.action == 'snapshot':
        payload = snapshot(arguments.database, arguments.allowed_root)
    elif arguments.action == 'stability':
        payload = stability_check(arguments.database, arguments.allowed_root, arguments.rounds)
    elif arguments.action == 'copy':
        payload = frozen_copy(arguments.database, arguments.dest, arguments.allowed_root)
    else:
        payload = logical_dump(arguments.database, arguments.allowed_root)
    write_json(arguments.out, payload)
    print(canonical_json({'action': arguments.action,
                          'summary_sha256': sha256_json(payload)}))


if __name__ == '__main__':
    import sys
    main(sys.argv[1:])
