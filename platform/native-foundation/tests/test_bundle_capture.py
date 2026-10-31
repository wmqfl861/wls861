#!/usr/bin/env python3
"""Check native build resource failures before any capture destination effects."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest


PARSER = argparse.ArgumentParser()
PARSER.add_argument('--source-root', required=True, type=Path)
ARGS, UNITTEST_ARGS = PARSER.parse_known_args()
SOURCE = ARGS.source_root.resolve()
PACKAGER = SOURCE / 'platform/native-foundation/tools/prepare_bundle.py'
EXPECTED = (SOURCE / 'platform/native-foundation/manifest.sha256').read_text().strip()
MANIFEST = json.loads((SOURCE / 'resources/runtime-manifest.json').read_bytes())


class BundleCapture(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='wls-native-packager-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.source = self.root / 'source'
        for relative in ['resources/runtime-manifest.json', 'src/runtime_resource_catalog.inc',
                         *(item['path'] for item in MANIFEST['assets'])]:
            target = self.source / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(SOURCE / relative, target)
        self.destination = self.root / 'destination'

    def capture(self, success):
        result = subprocess.run([sys.executable, str(PACKAGER), '--source-root', str(self.source),
                                 '--destination', str(self.destination),
                                 '--expected-manifest-sha256', EXPECTED], capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0 if success else 2, (result.stdout, result.stderr))
        if not success:
            self.assertFalse(self.destination.exists())
        return result

    def test_original_bytes_and_full_catalog(self):
        result = json.loads(self.capture(True).stdout)
        self.assertEqual((result['asset_count'], result['asset_bytes']), (35, 160549))
        for item in MANIFEST['assets']:
            captured = (self.destination / item['path']).read_bytes()
            self.assertEqual(len(captured), item['size_bytes'])
            self.assertEqual(hashlib.sha256(captured).hexdigest(), item['sha256'])

    def test_missing_resource_fails_before_destination(self):
        (self.source / 'prompts/worker.md').unlink()
        self.capture(False)

    def test_same_size_tampered_resource_fails_before_destination(self):
        path = self.source / 'prompts/worker.md'
        original = path.read_bytes()
        path.write_bytes(bytes([original[0] ^ 1]) + original[1:])
        self.capture(False)

    def test_changed_manifest_fails_before_destination(self):
        path = self.source / 'resources/runtime-manifest.json'
        path.write_bytes(path.read_bytes() + b' ')
        self.capture(False)

    def test_changed_catalog_fails_before_destination(self):
        path = self.source / 'src/runtime_resource_catalog.inc'
        original = path.read_bytes()
        path.write_bytes(original.replace(b'5284U', b'5285U', 1))
        self.assertNotEqual(path.read_bytes(), original)
        self.capture(False)

    def test_wrong_source_object_fails_before_destination(self):
        path = self.source / 'prompts/worker.md'
        path.unlink()
        path.mkdir()
        self.capture(False)


if __name__ == '__main__':
    unittest.main(argv=[__file__, *UNITTEST_ARGS], verbosity=2)
