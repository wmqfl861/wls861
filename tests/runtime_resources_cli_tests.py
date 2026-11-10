#!/usr/bin/env python3
"""Ordinary-user, real CMake prefix/DESTDIR runtime bundle integration.

With a third argument this installs the current project's real CMake build.
The Make gate supplies two arguments and configures an installation-only CMake
fixture around the same built executable and original installation module.
The fourth argument is the no-seam production resolver probe; staged copies of
that probe run next to the helper bytes under test — under a staging root
anchored in the verified source tree, never below a permissive-umask build
directory — so install and relocation assertions exercise the shipped
parent-side lifecycle. All prefixes, copies, faults, configurations and
databases are disposable.
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


if len(sys.argv) not in (3, 4, 5):
    raise SystemExit('usage: runtime_resources_cli_tests.py WLS_BINARY SOURCE_ROOT [CMAKE_BUILD] '
                     '[RESOLVER_PROBE_BINARY]')
BINARY = Path(sys.argv[1]).resolve()
SOURCE = Path(sys.argv[2]).resolve()
CMAKE_BUILD = Path(sys.argv[3]).resolve() if len(sys.argv) >= 4 else None
RESOLVER_PROBE = Path(sys.argv[4]).resolve() if len(sys.argv) == 5 else None
sys.argv = [sys.argv[0]]


def verified_source_root(source: Path) -> Path:
    """Full-ancestor preflight for the resolver staging anchor.

    Every component of the source root must be a real owner-pinned directory
    (root or the effective user) without symlink components or group/world
    writes; a permissive-umask build tree is exactly the shared-write ancestry
    the production pin refuses, so positive cases never stage under it. An
    unsafe root is an honest fixture preflight failure — never a chmod, skip
    or relaxed production guard.
    """
    root = source.resolve()
    current = Path(root.anchor)
    for part in root.parts[1:]:
        current = current / part
        info = current.lstat()
        if not stat.S_ISDIR(info.st_mode) or stat.S_ISLNK(info.st_mode):
            raise AssertionError(f'unsafe staging ancestor (not a pinned directory): {current}')
        if info.st_uid not in (0, os.geteuid()):
            raise AssertionError(f'staging ancestor not owner-pinned: {current}')
        if info.st_mode & 0o022:
            raise AssertionError(f'staging ancestor is shared-write: {current}')
    return root


SOURCE_STAGING_ROOT = verified_source_root(SOURCE)


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
        # The pinned resolver helper is installed in both gates: the real
        # CMake build installs it as a program target, and the plain-Make gate
        # supplies it next to the built wls binary; no case is skipped.
        helper = BINARY.parent / 'wls861-resolve-ipv4'
        if CMAKE_BUILD is None and not helper.is_file():
            raise AssertionError('the plain-Make gate must build wls861-resolve-ipv4 next to wls')
        if build is None:
            fixture = cls.shared / 'cmake-fixture'
            fixture.mkdir(mode=0o700)
            (fixture / 'CMakeLists.txt').write_text(
                'cmake_minimum_required(VERSION 3.20)\n'
                'project(wls_resource_install_fixture NONE)\n'
                f'set(Python3_EXECUTABLE [==[{sys.executable}]==])\n'
                f'include([==[{SOURCE / "cmake/runtime_resources.cmake"}]==])\n'
                f'install(PROGRAMS [==[{BINARY}]==] DESTINATION bin RENAME wls COMPONENT wls-runtime)\n'
                f'install(PROGRAMS [==[{helper}]==] DESTINATION bin RENAME wls861-resolve-ipv4 '
                'COMPONENT wls-runtime)\n'
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

    def resolver_probe(self):
        # The relocation driver is the no-seam production probe. Its staged
        # copy runs next to the helper bytes it pins, under the verified
        # source-anchored staging root — never below a permissive-umask build
        # tree or a shared other-writable temporary directory.
        probe = RESOLVER_PROBE if RESOLVER_PROBE is not None else \
            BINARY.parent / 'https_resolver_probe'
        self.assertTrue(probe.is_file(), probe)
        return probe

    def resolver_stage(self, name):
        # Exclusively created (mkdtemp, 0700) directory below the verified
        # source-anchored root; never below the build tree, and no existing
        # path is ever reused, appended to or re-permissioned.
        root = Path(tempfile.mkdtemp(prefix=f'resolver-stage-{name}-',
                                     dir=str(SOURCE_STAGING_ROOT)))
        self.addCleanup(shutil.rmtree, root, ignore_errors=True)
        return root

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

    def test_resolver_helper_installs_and_pins_next_to_the_program(self):
        template_helper = self.template / 'bin/wls861-resolve-ipv4'
        self.assertTrue(template_helper.is_file(), template_helper)
        mode = stat.S_IMODE(template_helper.stat().st_mode)
        self.assertEqual(mode & 0o111, 0o111)
        self.assertEqual(mode & 0o022, 0)
        with template_helper.open('rb') as stream:
            self.assertEqual(stream.read(4), b'\x7fELF')
        # The helper is a program asset, never a manifest resource: the
        # resource bundle layout and asset count stay original.
        for entry in self.manifest['assets']:
            self.assertNotIn('wls861-resolve-ipv4', entry['path'])
        self.assertEqual(self.invoke('resources')['resources']['asset_count'],
                         len(self.manifest['assets']))
        # Same bin directory as the installed wls: execute the helper there
        # from an unrelated cwd with the real system resolver on loopback.
        os.link(template_helper, self.prefix / 'bin/wls861-resolve-ipv4')
        helper = self.prefix / 'bin/wls861-resolve-ipv4'
        completed = subprocess.run([str(helper), 'localhost', '443'], cwd=self.cwd,
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(completed.stdout.startswith('WLS-RESOLVE-1\nok\n'), completed.stdout)
        self.assertIn('127.0.0.1', completed.stdout)
        # The no-seam production probe verifies executable-relative
        # pin/resolve/reap against these installed helper bytes from an
        # unrelated cwd after the installation prefix moved away; the staged
        # tree keeps only its own two copies and no installation source.
        probe = self.resolver_probe()
        stage = self.resolver_stage('cmake')
        shutil.copyfile(probe, stage / 'driver')
        os.chmod(stage / 'driver', 0o755)
        shutil.copyfile(helper, stage / 'wls861-resolve-ipv4')
        os.chmod(stage / 'wls861-resolve-ipv4', 0o755)
        # Staged bytes are the real bytes: pin/resolve/reap exercise the
        # actual installed helper and probe, not a substituted copy.
        self.assertEqual((stage / 'driver').read_bytes(), probe.read_bytes())
        self.assertEqual((stage / 'wls861-resolve-ipv4').read_bytes(), helper.read_bytes())
        self.prefix.rename(self.directory / 'installed-moved-away')
        relocated = stage.with_name(stage.name + '-moved')
        stage.rename(relocated)
        self.addCleanup(shutil.rmtree, relocated, ignore_errors=True)
        resolved = subprocess.run([str(relocated / 'driver'), '--resolve-once', 'localhost', '443'],
                                  cwd=self.cwd, capture_output=True, text=True, timeout=10)
        self.assertEqual(resolved.returncode, 0, resolved.stdout + resolved.stderr)
        self.assertIn('ok=1', resolved.stdout)
        self.assertIn('127.0.0.1', resolved.stdout)
        # A tampered staged helper is still refused after relocation: the
        # identity checks do not depend on the installation source.
        os.chmod(relocated / 'wls861-resolve-ipv4', 0o664)
        refused = subprocess.run([str(relocated / 'driver'), '--resolve-once', 'localhost', '443'],
                                 cwd=self.cwd, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(refused.returncode, 0, refused.stdout)
        self.assertIn('threw=1', refused.stdout)

    def test_make_gate_runs_real_make_install_and_relocated_production_helper(self):
        # O1: the plain-Make gate executes the real `make install` entry — the
        # installation-only CMake fixture above does not count for this — and
        # then exercises the installed production helper bytes after the whole
        # installation tree and the source tree are moved away.
        if CMAKE_BUILD is not None:
            raise unittest.SkipTest(
                'the real make install entry is executed by the plain-Make gate')
        destdir = self.directory / 'make-install-root'
        prefix = '/opt/wls-make-install'
        result = subprocess.run(['make', '-C', str(SOURCE), 'install',
                                 f'BUILD_DIR={BINARY.parent}', f'DESTDIR={destdir}',
                                 f'PREFIX={prefix}'],
                                capture_output=True, text=True, timeout=120)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        installed = destdir / prefix.lstrip('/')
        bin_dir = installed / 'bin'
        for name in ('wls', 'wls861', 'wls861-resolve-ipv4'):
            program = bin_dir / name
            self.assertTrue(program.is_file(), program)
            mode = stat.S_IMODE(program.stat().st_mode)
            self.assertEqual(mode & 0o111, 0o111)
            self.assertEqual(mode & 0o022, 0)
        self.assertTrue((installed / 'share/wls861/runtime-manifest.json').is_file())
        # Same bin directory as the installed tools: execute the helper there
        # from an unrelated cwd with the real system resolver on loopback.
        helper = bin_dir / 'wls861-resolve-ipv4'
        completed = subprocess.run([str(helper), 'localhost', '443'], cwd=self.cwd,
                                   capture_output=True, text=True, timeout=10)
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertTrue(completed.stdout.startswith('WLS-RESOLVE-1\nok\n'), completed.stdout)
        self.assertIn('127.0.0.1', completed.stdout)
        # Relocate away from the installation source, then pin/resolve/reap
        # through the no-seam production probe from an external cwd.
        probe = self.resolver_probe()
        stage = self.resolver_stage('make')
        shutil.copyfile(probe, stage / 'driver')
        os.chmod(stage / 'driver', 0o755)
        shutil.copyfile(helper, stage / 'wls861-resolve-ipv4')
        os.chmod(stage / 'wls861-resolve-ipv4', 0o755)
        # Staged bytes are the real bytes: pin/resolve/reap exercise the
        # actual installed helper and probe, not a substituted copy.
        self.assertEqual((stage / 'driver').read_bytes(), probe.read_bytes())
        self.assertEqual((stage / 'wls861-resolve-ipv4').read_bytes(), helper.read_bytes())
        destdir.rename(self.directory / 'make-install-root-moved-away')
        relocated = stage.with_name(stage.name + '-moved')
        stage.rename(relocated)
        self.addCleanup(shutil.rmtree, relocated, ignore_errors=True)
        resolved = subprocess.run([str(relocated / 'driver'), '--resolve-once', 'localhost', '443'],
                                  cwd=self.cwd, capture_output=True, text=True, timeout=10)
        self.assertEqual(resolved.returncode, 0, resolved.stdout + resolved.stderr)
        self.assertIn('ok=1', resolved.stdout)
        self.assertIn('127.0.0.1', resolved.stdout)
        # A tampered staged helper is refused after relocation.
        os.chmod(relocated / 'wls861-resolve-ipv4', 0o664)
        refused = subprocess.run([str(relocated / 'driver'), '--resolve-once', 'localhost', '443'],
                                 cwd=self.cwd, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(refused.returncode, 0, refused.stdout)
        self.assertIn('threw=1', refused.stdout)
        # A missing staged helper is refused the same way.
        os.remove(relocated / 'wls861-resolve-ipv4')
        missing = subprocess.run([str(relocated / 'driver'), '--resolve-once', 'localhost', '443'],
                                 cwd=self.cwd, capture_output=True, text=True, timeout=10)
        self.assertNotEqual(missing.returncode, 0, missing.stdout)
        self.assertIn('threw=1', missing.stdout)

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
