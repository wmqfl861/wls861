#!/usr/bin/env python3
"""Ordinary-user, real CMake prefix/DESTDIR runtime bundle integration.

With a third argument this installs the current project's real CMake build.
The Make gate supplies two arguments and configures an installation-only CMake
fixture around the same built executable and original installation module.
All prefixes, copies, faults, configurations and databases are disposable.
"""
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest


if len(sys.argv) not in (3, 4):
    raise SystemExit('usage: runtime_resources_cli_tests.py WLS_BINARY SOURCE_ROOT [CMAKE_BUILD]')
BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
CMAKE_BUILD = Path(sys.argv[3]).resolve() if len(sys.argv) == 4 else None
sys.argv = [sys.argv[0]]


class RuntimeResourceInstallation(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if os.geteuid() == 0:
            raise AssertionError('runtime resource integration must run as an ordinary user')
        cls.shared_temporary = tempfile.TemporaryDirectory(prefix='wls-resource-install-')
        cls.addClassCleanup(cls.shared_temporary.cleanup)
        cls.shared = Path(cls.shared_temporary.name)
        cls.manifest = json.loads((SOURCE / 'resources/runtime-manifest.json').read_text())
        build = CMAKE_BUILD
        if build is None:
            fixture = cls.shared / 'cmake-fixture'
            fixture.mkdir(mode=0o700)
            (fixture / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(wls_resource_install_fixture NONE)\n'
                f'set(Python3_EXECUTABLE [==[{sys.executable}]==])\n'
                f'include([==[{SOURCE / "cmake/runtime_resources.cmake"}]==])\n'
                f'install(PROGRAMS [==[{BINARY}]==] DESTINATION bin RENAME wls COMPONENT wls-runtime)\n'
                'wls_install_runtime_resources()\n')
            build = cls.shared / 'cmake-build'
            result = subprocess.run(['cmake', '-S', str(fixture), '-B', str(build)],
                                    capture_output=True, text=True, timeout=30)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        stage = cls.shared / 'destdir'
        environment = os.environ.copy()
        environment['DESTDIR'] = str(stage)
        result = subprocess.run(['cmake', '--install', str(build), '--component', 'wls-runtime',
                                 '--prefix', '/opt/wls-runtime-test'], env=environment,
                                capture_output=True, text=True, timeout=30)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)
        cls.template = stage / 'opt/wls-runtime-test'
        if not (cls.template / 'bin/wls').is_file():
            raise AssertionError('CMake did not install the real runtime executable under DESTDIR/prefix')

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='wls-resource-cli-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.prefix = self.directory / 'installed'
        self.prefix.mkdir(mode=0o700)
        (self.prefix / 'bin').mkdir(mode=0o755)
        # Only the never-modified executable is hard-linked. Resource fault
        # fixtures always have independent byte copies and file identities.
        os.link(self.template / 'bin/wls', self.prefix / 'bin/wls')
        shutil.copytree(self.template / 'share', self.prefix / 'share')
        self.binary = self.prefix / 'bin/wls'
        self.bundle = self.prefix / 'share/wls861'
        self.cwd = self.directory / 'unrelated-cwd'
        self.cwd.mkdir(mode=0o700)
        self.config = self.directory / 'external-config'
        shutil.copytree(SOURCE / 'config/examples', self.config)

    def invoke(self, *arguments, error=None, timeout=20, environment=None):
        completed = subprocess.run([str(self.binary), *(str(value) for value in arguments)],
            cwd=self.cwd, env=environment, capture_output=True, text=True, encoding='utf-8', timeout=timeout)
        if error is not None:
            self.assertNotEqual(completed.returncode, 0, completed.stdout)
            self.assertEqual(completed.stdout, '')
            self.assertEqual(len(completed.stderr.strip().splitlines()), 1, completed.stderr)
            result = json.loads(completed.stderr)
            self.assertIn(result['error'], error if isinstance(error, tuple) else (error,), result)
            return result
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stderr, '')
        return json.loads(completed.stdout)

    def config_arguments(self):
        return ['--config', self.config / 'system.yaml', '--functions-dir', self.config / 'functions']

    def change_file(self, path, content):
        path.chmod(0o600)
        path.write_bytes(content)

    def test_real_prefix_destdir_install_has_only_readonly_program_resources(self):
        observed = self.invoke('resources')
        self.assertEqual(observed['resources']['layout'], 'installed-bundle')
        self.assertEqual(observed['resources']['root'], str(self.bundle))
        self.assertEqual(observed['resources']['asset_count'], len(self.manifest['assets']))
        self.assertEqual(observed['migrations']['target_version'], 410)
        self.assertEqual([x['to_version'] for x in observed['migrations']['migrations']], list(range(402, 411)))
        self.assertFalse((self.bundle / 'config').exists())
        self.assertFalse((self.bundle / 'tests').exists())
        for entry in self.manifest['assets']:
            path = self.bundle / entry['path']
            self.assertEqual(hashlib.sha256(path.read_bytes()).hexdigest(), entry['sha256'])
            self.assertEqual(path.stat().st_size, entry['size_bytes'])
            self.assertEqual(stat.S_IMODE(path.stat().st_mode) & 0o222, 0)
        database = self.directory / 'state.sqlite'
        self.assertEqual(self.invoke('init', '--db', database)['user_version'], 410)

    def test_relocated_executable_ignores_cwd_and_environment_resource_poison(self):
        (self.cwd / 'resources').mkdir()
        (self.cwd / 'resources/runtime-manifest.json').write_text('untrusted cwd manifest')
        (self.cwd / 'db').mkdir()
        (self.cwd / 'db/schema.sql').write_text('untrusted cwd SQL')
        environment = os.environ.copy()
        environment['WLS_RESOURCE_ROOT'] = str(self.cwd)
        environment['WLS_SOURCE_ROOT'] = str(self.cwd)
        relocated = self.directory / 'relocated prefix'
        self.prefix.rename(relocated)
        self.prefix = relocated
        self.binary = relocated / 'bin/wls'
        self.bundle = relocated / 'share/wls861'
        observed = self.invoke('resources', environment=environment)
        self.assertEqual(observed['resources']['root'], str(self.bundle))
        self.assertEqual(self.invoke('init', '--db', self.directory / 'relocated.sqlite',
                                     environment=environment)['user_version'], 410)

    def test_explicit_roots_and_schema_inputs_keep_legacy_compatibility(self):
        installed = self.invoke('resources')
        source = self.invoke('resources', '--source-root', SOURCE)
        explicit = self.invoke('resources', '--resource-root', self.bundle)
        self.assertEqual(source['resources']['layout'], 'source-tree')
        self.assertEqual(source['resources']['bundle_sha256'], installed['resources']['bundle_sha256'])
        self.assertEqual(explicit, installed)
        self.invoke('resources', '--source-root', SOURCE, '--resource-root', self.bundle, error='invalid_input')
        result = self.invoke('schema', 'check', '--schema', 'plan', '--input', SOURCE / 'schemas/examples/plan.json')
        self.assertTrue(result['valid'])
        legacy = self.invoke('config', 'check', '--source-root', SOURCE)
        external = self.invoke('config', 'check', *self.config_arguments())
        self.assertEqual(legacy['snapshot_sha256'], external['snapshot_sha256'])

    def test_installed_configuration_is_explicit_and_not_bundled(self):
        self.invoke('config', 'check', error='invalid_input')
        self.invoke('config', 'check', '--config', self.config / 'system.yaml', error='invalid_input')
        self.invoke('config', 'check', '--functions-dir', self.config / 'functions', error='invalid_input')
        self.assertTrue(self.invoke('config', 'check', *self.config_arguments())['valid'])

    def test_external_prompts_and_frozen_requests_have_separate_identity(self):
        program_before = self.invoke('resources')['resources']['bundle_sha256']
        original = self.invoke('config', 'check', *self.config_arguments())
        prompts = self.directory / 'external-prompts'
        shutil.copytree(SOURCE / 'prompts', prompts)
        with (prompts / 'common.md').open('a') as stream:
            stream.write('\nExplicit caller-owned synthetic prompt variation.\n')
        changed = self.invoke('config', 'check', *self.config_arguments(), '--prompts-dir', prompts)
        self.assertNotEqual(original['snapshot_sha256'], changed['snapshot_sha256'])
        self.assertEqual(self.invoke('resources')['resources']['bundle_sha256'], program_before)
        material = self.directory / 'request.txt'
        material.write_text('Preserve this original request and its frozen configuration.\n')
        database = self.directory / 'request.sqlite'
        admitted = self.invoke('submit', '--db', database, '--key', 'installed-request', '--text-file', material,
                               *self.config_arguments())
        request = admitted['request']['request_id']
        before = self.invoke('config-snapshot', '--db', database, '--request', request)
        (self.config / 'system.yaml').write_text('invalid: [changed current external configuration\n')
        self.assertEqual(self.invoke('config-snapshot', '--db', database, '--request', request), before)

    def test_bundle_failures_reject_before_database_or_writer_effects(self):
        original_manifest = (self.bundle / 'runtime-manifest.json').read_bytes()
        original_sql = (self.bundle / 'db/schema.sql').read_bytes()
        cases = ('sql', 'self-reported-hash', 'missing', 'extra-field', 'omitted-asset', 'duplicate-key', 'unknown-version', 'symlink')
        for index, case in enumerate(cases):
            with self.subTest(case=case):
                sql = self.bundle / 'db/schema.sql'
                if sql.is_symlink():
                    sql.unlink()
                if not sql.exists():
                    sql.write_bytes(original_sql)
                self.change_file(sql, original_sql)
                self.change_file(self.bundle / 'runtime-manifest.json', original_manifest)
                manifest = json.loads(original_manifest)
                expected = 'corrupt'
                if case in ('sql', 'self-reported-hash'):
                    altered = original_sql + b'\n-- unrecognized SQL\n'
                    self.change_file(sql, altered)
                    if case == 'self-reported-hash':
                        entry = next(item for item in manifest['assets'] if item['path'] == 'db/schema.sql')
                        entry.update(sha256=hashlib.sha256(altered).hexdigest(), size_bytes=len(altered))
                elif case == 'missing':
                    sql.unlink()
                elif case == 'extra-field':
                    manifest['unknown_field'] = True
                elif case == 'omitted-asset':
                    manifest['assets'].pop()
                elif case == 'duplicate-key':
                    self.change_file(self.bundle / 'runtime-manifest.json', b'{"format":"x","format":"y"}')
                    expected = 'invalid_input'
                elif case == 'unknown-version':
                    manifest['format'] = 'wls-runtime-resources-v999'
                    expected = 'unsupported'
                elif case == 'symlink':
                    sql.unlink()
                    sql.symlink_to(SOURCE / 'db/schema.sql')
                    expected = 'unauthorized'
                if case != 'duplicate-key':
                    self.change_file(self.bundle / 'runtime-manifest.json', json.dumps(manifest).encode())
                database = self.directory / f'rejected-{index}.sqlite'
                self.invoke('init', '--db', database, error=expected)
                self.assertFalse(database.exists())
                self.assertFalse(Path(str(database) + '.writer-lock').exists())

    def test_fifo_resource_is_refused_without_waiting_for_a_writer(self):
        sql = self.bundle / 'db/schema.sql'
        sql.unlink()
        os.mkfifo(sql, 0o600)
        database = self.directory / 'fifo.sqlite'
        self.invoke('init', '--db', database, timeout=3, error='unauthorized')
        self.assertFalse(database.exists())

    def source_fixture(self):
        root = self.directory / 'install-source'
        for entry in self.manifest['assets']:
            destination = root / entry['path']
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / entry['path'], destination)
        for relative in ('resources/runtime-manifest.json', 'src/runtime_resource_catalog.inc'):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / relative, destination)
        return root

    def run_installer(self, source, destination):
        return subprocess.run([sys.executable, str(SOURCE / 'scripts/install_runtime_resources.py'),
            '--source-root', str(source), '--manifest', str(source / 'resources/runtime-manifest.json'),
            '--destination', str(destination)], capture_output=True, text=True, timeout=10)

    def test_installed_runtime_survives_removal_of_its_installation_source(self):
        source = self.source_fixture()
        prefix = self.directory / 'independent-installation'
        result = self.run_installer(source, prefix / 'share/wls861')
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        (prefix / 'bin').mkdir()
        os.link(self.template / 'bin/wls', prefix / 'bin/wls')
        shutil.rmtree(source)
        self.assertFalse(source.exists())
        self.binary = prefix / 'bin/wls'
        self.bundle = prefix / 'share/wls861'
        observed = self.invoke('resources')
        self.assertEqual(observed['resources']['root'], str(self.bundle))
        self.assertEqual(observed['resources']['layout'], 'installed-bundle')
        self.assertEqual(self.invoke('init', '--db', self.directory / 'without-source.sqlite')['user_version'], 410)
        self.assertTrue(self.invoke('config', 'check', *self.config_arguments())['valid'])

    def test_installer_validates_all_source_bytes_before_destination_effects(self):
        source = self.source_fixture()
        # Damage a late asset to prove early assets were captured, not installed.
        (source / self.manifest['assets'][-1]['path']).write_text('altered late resource')
        destination = self.directory / 'must-remain-absent'
        failure = self.run_installer(source, destination)
        self.assertEqual(failure.returncode, 2, failure.stdout + failure.stderr)
        self.assertFalse(destination.exists())
        (source / self.manifest['assets'][-1]['path']).unlink()
        os.mkfifo(source / self.manifest['assets'][-1]['path'], 0o600)
        failure = self.run_installer(source, destination)
        self.assertEqual(failure.returncode, 2, failure.stdout + failure.stderr)
        self.assertFalse(destination.exists())

    def test_installer_refuses_destination_symlink_without_writing_target(self):
        source = self.source_fixture()
        outside = self.directory / 'outside'
        outside.mkdir()
        destination = self.directory / 'linked-destination'
        destination.symlink_to(outside, target_is_directory=True)
        failure = self.run_installer(source, destination)
        self.assertEqual(failure.returncode, 2, failure.stdout + failure.stderr)
        self.assertEqual(list(outside.iterdir()), [])


if __name__ == '__main__':
    unittest.main(verbosity=2)
