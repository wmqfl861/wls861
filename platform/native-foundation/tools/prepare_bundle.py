#!/usr/bin/env python3
"""Capture trusted build-source public resources for the isolated native slice.

No dependencies are downloaded. The existing runtime catalog, exact manifest
lock and every registered asset must agree before creating any destination.
This build tool is not a Windows Workspace or secure production installer.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import sys
import tempfile


def require(condition, message):
    if not condition:
        raise ValueError(message)


def ordinary(path, directory):
    status = path.lstat()
    require(not stat.S_ISLNK(status.st_mode) and not (
        getattr(status, 'st_file_attributes', 0) & 0x400),
        'build source/destination link or reparse point refused')
    require(stat.S_ISDIR(status.st_mode) if directory else stat.S_ISREG(status.st_mode),
            'build source/destination object type refused')
    return status


def read_regular(path, maximum):
    path = Path(os.path.abspath(path))
    for parent in reversed(path.parents):
        ordinary(parent, True)
    before = ordinary(path, False)
    require(0 <= before.st_size <= maximum, 'build resource exceeds byte bound')
    with path.open('rb') as stream:
        content = stream.read(maximum + 1)
    after = ordinary(path, False)
    identity = lambda value: (value.st_dev, value.st_ino, value.st_size,
                              value.st_mtime_ns, value.st_ctime_ns)
    require(len(content) <= maximum and len(content) == before.st_size and
            identity(before) == identity(after), 'build resource changed during capture')
    return content


def capture(source, expected):
    require(re.fullmatch('[0-9a-f]{64}', expected), 'invalid compiled manifest digest')
    raw = read_regular(source / 'resources/runtime-manifest.json', 65536)
    require(hashlib.sha256(raw).hexdigest() == expected, 'exact public manifest identity mismatch')
    manifest = json.loads(raw)
    require(set(manifest) == {'format', 'store_version', 'assets'} and
            manifest['format'] == 'wls-runtime-resources-v1' and
            type(manifest['store_version']) is int and manifest['store_version'] == 410,
            'unsupported public manifest')
    catalog = read_regular(source / 'src/runtime_resource_catalog.inc', 65536).decode('utf-8')
    registered = []
    declared = None
    for line in catalog.splitlines():
        line = line.strip()
        if not line or line.startswith('//') or line == '}};':
            continue
        declaration = re.fullmatch(r'constexpr std::array<ResourceAsset, ([0-9]+)> resource_assets\{\{', line)
        if declaration:
            require(declared is None, 'duplicate public catalog declaration')
            declared = int(declaration[1])
            continue
        entry = re.fullmatch(r'\{"([^"\\]+)", "(sql|schema|prompt)", "([0-9a-f]{64})", ([0-9]+)U\},', line)
        require(entry is not None, 'invalid public resource catalog')
        registered.append(dict(path=entry[1], kind=entry[2], sha256=entry[3], size_bytes=int(entry[4])))
    require(declared == len(registered) == 35 and manifest['assets'] == registered,
            'manifest and original compiled catalog differ')
    captured = []
    total = 0
    previous = ''
    for asset in registered:
        relative = asset['path']
        require(re.fullmatch(r'(db/(schema\.sql|migrations/[0-9]{3}_[a-z0-9_]+\.sql)|'
                             r'schemas/[a-z0-9_-]+\.schema\.json|'
                             r'prompts/(common|dispatcher|team_lead|worker)\.md)', relative) and
                previous < relative and len(relative) <= 512, 'invalid or unordered public resource path')
        previous = relative
        content = read_regular(source / relative, 1024 * 1024)
        require(len(content) == asset['size_bytes'] and
                hashlib.sha256(content).hexdigest() == asset['sha256'],
                'public resource differs from original catalog: ' + relative)
        total += len(content)
        require(total <= 8 * 1024 * 1024, 'public resource total exceeds bound')
        captured.append((relative, content))
    captured.append(('runtime-manifest.json', raw))
    return captured, total


def write_capture(destination, captured):
    destination.mkdir(parents=True, exist_ok=True)
    for parent in reversed((destination / 'placeholder').parents):
        ordinary(parent, True)
    for relative, content in captured:
        target = destination / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        for parent in reversed(target.parents):
            ordinary(parent, True)
        if target.exists() or target.is_symlink():
            ordinary(target, False)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(dir=target.parent, prefix='.native-capture-', delete=False) as stream:
                temporary = Path(stream.name)
                stream.write(content)
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, target)
            temporary = None
        finally:
            if temporary is not None:
                temporary.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--destination', type=Path, required=True)
    parser.add_argument('--expected-manifest-sha256', required=True)
    args = parser.parse_args()
    captured, total = capture(args.source_root, args.expected_manifest_sha256)
    write_capture(Path(os.path.abspath(args.destination)), captured)
    print(json.dumps({'format': 'wls-native-build-resource-capture-v1',
                      'asset_count': len(captured) - 1, 'asset_bytes': total,
                      'manifest_sha256': args.expected_manifest_sha256}))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(json.dumps({'error': 'native_build_resource_capture', 'message': str(error)[:1024]}), file=sys.stderr)
        raise SystemExit(2)
