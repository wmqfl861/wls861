#!/usr/bin/env python3
"""Store refusal and WAL compatibility through the actual CLI.

Usage: store_open_compatibility_tests.py WLS_BINARY SOURCE_ROOT [unittest args]

The future user_version is a disposable compatibility canary, not a migration.
Strict checks include every database-prefix file, its identity, mode and bytes;
no sidecar is removed or normalized to obtain equality. The active-WAL test
separately verifies that a main-file header cannot authorize an open while a
newer committed version is still in WAL. It does not claim quiescent raw-file
equality for a live SQLite connection's shared-memory coordination bytes.
"""
from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import sqlite3
import stat
import subprocess
import sys
import tempfile
import unittest

from immediate_unittest import ImmediateTextRunner


if len(sys.argv) < 3:
    raise SystemExit(__doc__)
BINARY = Path(sys.argv[1]).resolve(strict=True)
SOURCE = Path(sys.argv[2]).resolve(strict=True)
sys.argv = [sys.argv[0], *sys.argv[3:]]


def file_identity(info):
    return (info.st_dev, info.st_ino, info.st_uid, info.st_gid, info.st_mode,
            info.st_nlink, info.st_size)


def read_identity(info):
    return file_identity(info), info.st_mtime_ns, info.st_ctime_ns


def prefix_snapshot(database):
    """Read test-owned regular files without opening a SQLite connection."""
    directory = database.parent.lstat()
    names = sorted(path.name for path in database.parent.iterdir()
                   if path.name.startswith(database.name))
    files = {}
    for name in names:
        path = database.parent / name
        before = path.lstat()
        if not stat.S_ISREG(before.st_mode) or before.st_nlink != 1:
            raise AssertionError('unexpected database-prefix file: ' + name)
        descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
        try:
            opened = os.fstat(descriptor)
            if read_identity(before) != read_identity(opened):
                raise AssertionError('file changed during open: ' + name)
            total = 0
            digest = hashlib.sha256()
            while block := os.read(descriptor, 1024 * 1024):
                total += len(block)
                if total > 16 * 1024 * 1024:
                    raise AssertionError('test fixture exceeds its read bound')
                digest.update(block)
            if (total != before.st_size or read_identity(before) != read_identity(os.fstat(descriptor))
                    or read_identity(before) != read_identity(path.lstat())):
                raise AssertionError('file changed during read: ' + name)
            files[name] = {'identity': file_identity(before), 'sha256': digest.hexdigest()}
        finally:
            os.close(descriptor)
    after_names = sorted(path.name for path in database.parent.iterdir()
                         if path.name.startswith(database.name))
    if names != after_names or file_identity(directory) != file_identity(database.parent.lstat()):
        raise AssertionError('fixture directory changed during observation')
    return {'directory': file_identity(directory), 'files': files}


class StoreOpenCompatibility(unittest.TestCase):
    def setUp(self):
        self.assertNotEqual(os.geteuid(), 0, 'ordinary-user Store integration is required')
        previous_umask = os.umask(0o077)
        self.addCleanup(os.umask, previous_umask)
        temporary = tempfile.TemporaryDirectory(prefix='wls-store-open-compat-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.database = self.directory / 'fixture.sqlite'

    def invoke(self, command, *, error=None, database=None):
        result = subprocess.run([str(BINARY), command, '--db', str(database or self.database),
                                 '--source-root', str(SOURCE)], cwd=self.directory,
                                stdin=subprocess.DEVNULL, capture_output=True, text=True,
                                encoding='utf-8', timeout=20, check=False)
        if error is None:
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stderr, '')
            return json.loads(result.stdout)
        self.assertEqual(result.returncode, 2, result.stdout + result.stderr)
        self.assertEqual(result.stdout, '')
        self.assertEqual(len(result.stderr.strip().splitlines()), 1, result.stderr)
        failure = json.loads(result.stderr)
        self.assertEqual(failure['error'], error, failure)
        return failure

    def future_fixture(self):
        connection = sqlite3.connect(self.database)
        try:
            self.assertEqual(connection.execute('PRAGMA journal_mode=WAL').fetchone()[0], 'wal')
            connection.execute('CREATE TABLE canary(value TEXT)')
            connection.execute("INSERT INTO canary VALUES ('preserve exact bytes')")
            connection.execute('PRAGMA user_version=999')
            connection.commit()
        finally:
            connection.close()
        # Existing writer-lock identity belongs to the fixture. The refused
        # open must not replace it or create/remove any SQLite sidecar.
        self.database.with_name(self.database.name + '.writer-lock').touch(mode=0o600)
        self.assertEqual(int.from_bytes(self.database.read_bytes()[60:64], 'big'), 999)
        self.assertFalse(Path(str(self.database) + '-wal').exists())
        self.assertFalse(Path(str(self.database) + '-shm').exists())

    def assert_refusal_preserves_prefix(self, command):
        before = prefix_snapshot(self.database)
        self.invoke(command, error='unsupported')
        after = prefix_snapshot(self.database)
        self.assertEqual(before, after, 'refusal changed raw database bytes, identity, mode or sidecar inventory')

    def test_future_wal_readonly_refusal_keeps_exact_inventory(self):
        self.future_fixture()
        self.assert_refusal_preserves_prefix('pending')

    def test_future_wal_takeover_refusal_keeps_exact_inventory(self):
        self.future_fixture()
        self.assert_refusal_preserves_prefix('init')

    def test_future_refusal_keeps_preexisting_empty_wal_and_shm(self):
        self.future_fixture()
        connection = sqlite3.connect(self.database.as_uri() + '?mode=ro', uri=True)
        try:
            self.assertEqual(connection.execute('PRAGMA user_version').fetchone()[0], 999)
        finally:
            connection.close()
        self.assertEqual(Path(str(self.database) + '-wal').stat().st_size, 0)
        self.assertGreater(Path(str(self.database) + '-shm').stat().st_size, 0)
        self.assert_refusal_preserves_prefix('init')

    def test_future_refusal_does_not_chmod_unrelated_database(self):
        self.future_fixture()
        self.database.chmod(0o640)
        self.assert_refusal_preserves_prefix('init')

    def test_main_header_never_authorizes_newer_committed_wal(self):
        self.invoke('init')
        main_bytes = self.database.read_bytes()
        self.assertEqual(int.from_bytes(main_bytes[60:64], 'big'), 411)
        connection = sqlite3.connect(self.database)
        try:
            connection.execute('PRAGMA wal_autocheckpoint=0')
            connection.execute('PRAGMA user_version=999')
            connection.commit()
            self.assertEqual(int.from_bytes(self.database.read_bytes()[60:64], 'big'), 411)
            wal = Path(str(self.database) + '-wal')
            wal_bytes = wal.read_bytes()
            self.assertGreater(len(wal_bytes), 32)
            self.invoke('pending', error='unsupported')
            self.assertEqual(self.database.read_bytes(), main_bytes)
            self.assertEqual(wal.read_bytes(), wal_bytes)
            self.assertEqual(connection.execute('PRAGMA user_version').fetchone()[0], 999)
        finally:
            connection.close()

    def test_supported_411_readonly_behavior_is_unchanged(self):
        self.invoke('init')
        original = self.database.read_bytes()
        self.assertEqual(int.from_bytes(original[60:64], 'big'), 411)
        self.invoke('pending')
        self.assertEqual(self.database.read_bytes(), original)

    def test_symlink_cannot_supply_a_future_database_header(self):
        self.future_fixture()
        alias = self.directory / 'alias.sqlite'
        alias.symlink_to(self.database)
        before = prefix_snapshot(self.database)
        self.invoke('pending', database=alias, error='storage')
        self.assertEqual(prefix_snapshot(self.database), before)
        self.assertTrue(alias.is_symlink())

    def test_invalid_magic_is_not_accepted_as_a_future_sqlite_header(self):
        header = bytearray(100)
        header[60:64] = (999).to_bytes(4, 'big')
        self.database.write_bytes(header)
        before = prefix_snapshot(self.database)
        self.invoke('pending', error='corrupt')
        self.assertEqual(prefix_snapshot(self.database), before)

    def test_incomplete_header_does_not_replace_sqlite_corruption_guard(self):
        header = bytearray(64)
        header[:16] = b'SQLite format 3\x00'
        header[60:64] = (999).to_bytes(4, 'big')
        self.database.write_bytes(header)
        before = prefix_snapshot(self.database)
        self.invoke('pending', error='corrupt')
        self.assertEqual(prefix_snapshot(self.database), before)


if __name__ == '__main__':
    unittest.main(verbosity=2, testRunner=ImmediateTextRunner)
