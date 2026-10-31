#!/usr/bin/env python3
"""Real process/module tests of the isolated offline native executable."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


PARSER = argparse.ArgumentParser()
PARSER.add_argument('--executable', required=True, type=Path)
PARSER.add_argument('--bundle', required=True, type=Path)
ARGS, UNITTEST_ARGS = PARSER.parse_known_args()
EXE = ARGS.executable.resolve()
BUNDLE = ARGS.bundle.resolve()
MANIFEST_SHA256 = hashlib.sha256((BUNDLE / 'runtime-manifest.json').read_bytes()).hexdigest()


class NativeCLI(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='wls-native-cli-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.install = self.root / 'installed'
        self.binary = self.install / 'bin' / EXE.name
        self.binary.parent.mkdir(parents=True)
        shutil.copy2(EXE, self.binary)
        self.resources = self.install / 'share/wls861'
        shutil.copytree(BUNDLE, self.resources)
        self.cwd = self.root / 'unrelated-cwd'
        self.cwd.mkdir()

    def invoke(self, *arguments, success=True, code=None, binary=None, cwd=None, env=None):
        result = subprocess.run([str(binary or self.binary), *arguments], cwd=cwd or self.cwd,
                                env=env, capture_output=True, timeout=15)
        self.assertEqual(result.returncode, 0 if success else 2,
                         (result.stdout, result.stderr))
        self.assertEqual(result.stderr if success else result.stdout, b'')
        document = json.loads((result.stdout if success else result.stderr).decode('utf-8'))
        if code is not None:
            self.assertEqual(document['error'], code)
        return document

    def resolve(self, *extra, success=True, code=None, root='root'):
        return self.invoke('resolve-config', '--root-model', root, '--root-reasoning', 'effort:high',
                           '--capability', root + '=off,provider_default,effort:high', *extra,
                           success=success, code=code)

    def test_controlled_config_input_and_direct_root_inheritance(self):
        resolved = self.resolve('--model', 'alternate', '--capability', 'alternate=off,effort:high')
        self.assertEqual(resolved['model'], 'alternate')
        self.assertEqual(resolved['reasoning'], {'mode': 'explicit', 'effort': 'high'})
        self.assertEqual(self.resolve('--reasoning', 'off')['reasoning']['mode'], 'off')
        self.assertEqual(self.resolve('--reasoning', 'provider_default')['reasoning']['mode'], 'provider_default')
        self.resolve('--reasoning', 'effort:low', success=False, code='configuration_rejected')
        self.resolve('--model', 'absent', success=False, code='configuration_rejected')
        self.resolve('--root-model', 'duplicate', success=False, code='invalid_input')
        self.invoke('resolve-config', '--root-model', 'root', '--root-reasoning', 'inherit',
                    '--capability', 'root=off', success=False, code='configuration_rejected')

    def test_unicode_model_input_roundtrip(self):
        model = '\u79bb\u7ebf\u6a21\u578b\U0001f3d4'
        self.assertEqual(self.resolve(root=model)['model'], model)

    def test_real_tool_grant_calculation_does_not_grant_execution(self):
        options = ['--role-tools', 'read,write', '--system-tools', 'read,write', '--request-tools', 'read']
        worker = self.invoke('tool-grants', '--role', 'worker', *options)
        self.assertEqual(worker['tools'], ['read'])
        self.assertTrue(worker['calculation_only'])
        for manager in ('root', 'dispatcher', 'teamlead'):
            self.assertEqual(self.invoke('tool-grants', '--role', manager, *options)['tools'], [])
        self.invoke('fs-read', '--path', str(self.root / 'new-file'), success=False, code='unsupported')
        self.assertFalse((self.root / 'new-file').exists())

    def test_capabilities_have_separate_status_and_real_host_facts(self):
        result = self.invoke('capabilities')
        self.assertEqual(result['platform'], 'windows-native' if os.name == 'nt' else 'linux-verification-only')
        self.assertIn(result['token_elevation'], ('elevated', 'not_elevated', 'not_checked', 'ordinary_uid', 'root'))
        states = {item['name']: item['status'] for item in result['capabilities']}
        for name in ('store', 'workspace', 'process', 'sandbox', 'cleanup'):
            self.assertEqual(states[name], 'unsupported')
        self.assertEqual(states['config_resolution'], 'available')
        self.assertEqual(states['model_calls'], 'denied')
        self.assertEqual(states['network_isolation'], 'not_checked')
        for command in ('store', 'fs-read', 'fs-write', 'run', 'cleanup', 'call-model'):
            self.invoke(command, '--target', str(self.root / 'side-effect'), success=False,
                        code='denied' if command == 'call-model' else 'unsupported')
            self.assertFalse((self.root / 'side-effect').exists())

    def test_all_original_resources_are_captured_from_executable_location(self):
        result = self.invoke('resources')
        self.assertEqual((result['asset_count'], result['asset_bytes']), (35, 160549))
        self.assertEqual(result['manifest_sha256'], MANIFEST_SHA256)
        self.assertEqual(self.invoke('resources', '--resource-root', str(self.resources)), result)

    def test_relocated_unicode_executable_and_resources(self):
        relocated = self.root / '\u65e0\u91cf\u5c71 \U0001f3d4 # relocated'
        self.install.rename(relocated)
        result = self.invoke('resources', binary=relocated / 'bin' / EXE.name)
        self.assertEqual(result['manifest_sha256'], MANIFEST_SHA256)
        self.assertEqual(result['asset_count'], 35)

    def test_no_cwd_or_environment_fallback(self):
        decoy = self.cwd / 'share/wls861'
        shutil.copytree(BUNDLE, decoy)
        shutil.rmtree(self.resources)
        environment = os.environ.copy()
        environment.update(WLS_RESOURCE_ROOT=str(decoy), WLS_SOURCE_ROOT=str(self.cwd))
        self.invoke('resources', env=environment, success=False, code='resource_missing')

    def test_missing_asset_does_not_fall_through_to_another_valid_bundle(self):
        shutil.copytree(BUNDLE, self.binary.parent / 'share/wls861')
        (self.resources / 'prompts/worker.md').unlink()
        self.invoke('resources', success=False, code='resource_missing')

    def test_tampered_asset_of_same_size_is_rejected(self):
        asset = self.resources / 'prompts/worker.md'
        original = asset.read_bytes()
        asset.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        self.assertEqual(asset.stat().st_size, len(original))
        self.invoke('resources', success=False, code='resource_mismatch')

    def test_self_reported_new_hash_and_manifest_bytes_are_rejected(self):
        manifest_path = self.resources / 'runtime-manifest.json'
        manifest = json.loads(manifest_path.read_bytes())
        asset = self.resources / 'prompts/worker.md'
        content = asset.read_bytes() + b'changed\n'
        asset.write_bytes(content)
        for item in manifest['assets']:
            if item['path'] == 'prompts/worker.md':
                item.update(sha256=hashlib.sha256(content).hexdigest(), size_bytes=len(content))
        manifest_path.write_text(json.dumps(manifest), encoding='utf-8')
        self.invoke('resources', success=False, code='resource_mismatch')

    def test_oversized_asset_is_rejected(self):
        asset = self.resources / 'prompts/worker.md'
        with asset.open('ab') as stream:
            stream.write(b'x')
        self.invoke('resources', success=False, code='resource_mismatch')

    def test_hard_link_and_directory_instead_of_asset_are_rejected(self):
        asset = self.resources / 'prompts/worker.md'
        other = self.root / 'other-worker.md'
        asset.rename(other)
        os.link(other, asset)
        self.assertGreaterEqual(asset.stat().st_nlink, 2)
        self.invoke('resources', success=False, code='denied')
        asset.unlink()
        asset.mkdir()
        self.invoke('resources', success=False, code='denied')

    def test_directory_alias_and_special_objects_are_rejected(self):
        prompts = self.resources / 'prompts'
        outside = self.root / 'public-prompts'
        prompts.rename(outside)
        if os.name == 'nt':
            result = subprocess.run(['cmd.exe', '/d', '/c', 'mklink', '/J', str(prompts), str(outside)],
                                    capture_output=True, timeout=10)
            self.assertEqual(result.returncode, 0, (result.stdout, result.stderr))
        else:
            prompts.symlink_to(outside, target_is_directory=True)
        try:
            self.invoke('resources', success=False, code='denied')
        finally:
            if os.name == 'nt':
                os.rmdir(prompts)
            else:
                prompts.unlink()
        outside.rename(prompts)
        if os.name != 'nt':
            asset = prompts / 'worker.md'
            asset.unlink()
            os.mkfifo(asset)
            self.invoke('resources', success=False, code='denied')

    def test_invalid_paths_and_encoding_are_rejected(self):
        self.invoke('resources', '--resource-root', 'relative', success=False, code='invalid_input')
        if os.name == 'nt':
            self.invoke('resources', '--resource-root', r'\\server\untrusted\bundle',
                        success=False, code='denied')
            self.invoke('resolve-config', '\ud800', success=False, code='invalid_input')
        else:
            result = subprocess.run([os.fsencode(self.binary), b'resolve-config', b'\xff'],
                                    capture_output=True, timeout=15)
            self.assertEqual(result.returncode, 2)
            self.assertEqual(json.loads(result.stderr)['error'], 'invalid_input')


if __name__ == '__main__':
    unittest.main(argv=[__file__, *UNITTEST_ARGS], verbosity=2)
