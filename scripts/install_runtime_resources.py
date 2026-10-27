#!/usr/bin/env python3
"""Install a bounded, captured runtime bundle without user configuration.

The manifest is the build's frozen distribution manifest. Runtime loading also
checks its complete contents against identities compiled into the executable.
All source bytes are validated before destination effects. File replacement is
atomic per registered file; interrupted installations fail the runtime's whole
bundle validation until installation is completed.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import secrets
import stat
import sys


MAX_FILE = 1024 * 1024
MAX_TOTAL = 8 * 1024 * 1024
MAX_MANIFEST = 64 * 1024


def require(condition, message):
    if not condition:
        raise ValueError(message)


def strict_json(data):
    def pairs(items):
        result = {}
        for key, value in items:
            require(key not in result, 'duplicate manifest key')
            result[key] = value
        return result
    return json.loads(data, object_pairs_hook=pairs,
                      parse_constant=lambda _: (_ for _ in ()).throw(ValueError('nonfinite manifest value')))


def absolute(path):
    value = os.fspath(path)
    require(value and '\0' not in value and len(os.fsencode(value)) <= 4096, 'invalid installation path')
    result = Path(os.path.abspath(value))
    require(len(os.fsencode(result)) <= 4096, 'installation path exceeds bound')
    return result


def open_directory(path, create=False):
    path = absolute(path)
    descriptor = os.open('/', os.O_PATH | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW)
    try:
        for part in path.parts[1:]:
            require(part not in ('', '.', '..'), 'invalid directory component')
            try:
                child = os.open(part, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=descriptor)
            except FileNotFoundError:
                require(create, 'required source directory is missing')
                try:
                    # Keep the caller's umask and never change an existing or
                    # concurrently replaced directory's permissions.
                    os.mkdir(part, 0o755, dir_fd=descriptor)
                except FileExistsError:
                    pass
                child = os.open(part, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=descriptor)
            try:
                require(stat.S_ISDIR(os.fstat(child).st_mode), 'directory symlink or special file refused')
            except Exception:
                os.close(child)
                raise
            os.close(descriptor)
            descriptor = child
        result, descriptor = descriptor, -1
        return result
    finally:
        if descriptor >= 0:
            os.close(descriptor)


def identity(status):
    return (status.st_dev, status.st_ino, status.st_mode, status.st_size,
            status.st_mtime_ns, status.st_ctime_ns)


def read_regular(path, maximum):
    path = absolute(path)
    directory = open_directory(path.parent)
    inspected = opened = -1
    try:
        inspected = os.open(path.name, os.O_PATH | os.O_CLOEXEC | os.O_NOFOLLOW, dir_fd=directory)
        before = os.fstat(inspected)
        require(stat.S_ISREG(before.st_mode), 'source must be a regular file without a symbolic link')
        require(0 <= before.st_size <= maximum, 'source file exceeds byte bound')
        opened = os.open(f'/proc/self/fd/{inspected}', os.O_RDONLY | os.O_CLOEXEC | os.O_NONBLOCK)
        require(identity(before) == identity(os.fstat(opened)), 'source changed before reading')
        output = bytearray()
        eof = False
        while len(output) <= maximum:
            chunk = os.read(opened, min(16384, maximum + 1 - len(output)))
            if not chunk:
                eof = True
                break
            output.extend(chunk)
        require(eof and len(output) <= maximum and len(output) == before.st_size,
                'source EOF exceeds verified byte bound')
        require(identity(before) == identity(os.fstat(opened)), 'source changed during reading')
        return bytes(output)
    finally:
        for descriptor in (opened, inspected, directory):
            if descriptor >= 0:
                os.close(descriptor)


def capture(source, manifest_path):
    raw_manifest = read_regular(manifest_path, MAX_MANIFEST)
    manifest = strict_json(raw_manifest)
    require(isinstance(manifest, dict) and set(manifest) == {'format', 'store_version', 'assets'},
            'unexpected runtime manifest fields')
    require(manifest['format'] == 'wls-runtime-resources-v1' and
            type(manifest['store_version']) is int and manifest['store_version'] == 410,
            'unsupported runtime bundle version')
    require(isinstance(manifest['assets'], list) and 1 <= len(manifest['assets']) <= 128,
            'invalid runtime asset count')
    catalog = read_regular(source / 'src/runtime_resource_catalog.inc', MAX_MANIFEST).decode('utf-8')
    registered = []
    declared_count = None
    for line in catalog.splitlines():
        line = line.strip()
        if not line or line.startswith('//') or line == '}};':
            continue
        declaration = re.fullmatch(r'constexpr std::array<ResourceAsset, ([0-9]+)> resource_assets\{\{', line)
        if declaration:
            require(declared_count is None, 'duplicate compiled resource declaration')
            declared_count = int(declaration[1])
            continue
        entry = re.fullmatch(r'\{"([^"\\]+)", "(sql|schema|prompt)", "([a-f0-9]{64})", ([0-9]+)U\},', line)
        require(entry is not None, 'invalid compiled resource registration')
        registered.append({'path': entry[1], 'kind': entry[2], 'sha256': entry[3], 'size_bytes': int(entry[4])})
    require(declared_count == len(registered) and manifest['assets'] == registered,
            'runtime manifest differs from compiled resource registrations')
    captured, total, previous = [], 0, ''
    for asset in manifest['assets']:
        require(isinstance(asset, dict) and set(asset) == {'path', 'kind', 'sha256', 'size_bytes'},
                'unexpected runtime asset fields')
        path = asset['path']
        require(isinstance(path, str) and path > previous and len(path) <= 512,
                'asset paths must be unique, sorted and bounded')
        previous = path
        require(re.fullmatch(r'(db/(schema\.sql|migrations/[0-9]{3}_[a-z0-9_]+\.sql)|'
                             r'schemas/[a-z0-9_-]+\.schema\.json|'
                             r'prompts/(common|dispatcher|team_lead|worker)\.md)', path),
                'unregistered resource path class')
        kind = 'sql' if path.startswith('db/') else 'schema' if path.startswith('schemas/') else 'prompt'
        require(asset['kind'] == kind and isinstance(asset['sha256'], str) and
                re.fullmatch('[0-9a-f]{64}', asset['sha256']), 'invalid runtime asset identity')
        require(type(asset['size_bytes']) is int and 0 < asset['size_bytes'] <= MAX_FILE,
                'invalid runtime asset byte bound')
        content = read_regular(source / path, MAX_FILE)
        require(len(content) == asset['size_bytes'] and hashlib.sha256(content).hexdigest() == asset['sha256'],
                'runtime asset differs from the frozen build manifest: ' + path)
        total += len(content)
        require(total <= MAX_TOTAL, 'runtime bundle exceeds total byte bound')
        captured.append((path, content))
    # Only captured bytes reach the effect phase. The manifest is published
    # last, but whole-bundle validation remains mandatory after any interruption.
    captured.append(('runtime-manifest.json', raw_manifest))
    return captured, manifest, total


def install_file(destination, relative, content):
    path = destination / relative
    parent = open_directory(path.parent, create=True)
    temporary = '.wls-install-' + secrets.token_hex(12)
    created = False
    descriptor = -1
    try:
        try:
            current = os.stat(path.name, dir_fd=parent, follow_symlinks=False)
        except FileNotFoundError:
            current = None
        require(current is None or stat.S_ISREG(current.st_mode),
                'existing destination is not a regular file')
        descriptor = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                             0o444, dir_fd=parent)
        created = True
        os.fchmod(descriptor, 0o444)
        remaining = memoryview(content)
        while remaining:
            count = os.write(descriptor, remaining)
            require(count > 0, 'runtime bundle write made no progress')
            remaining = remaining[count:]
        os.fsync(descriptor)
        os.close(descriptor)
        descriptor = -1
        os.replace(temporary, path.name, src_dir_fd=parent, dst_dir_fd=parent)
        created = False
        readable = os.open(f'/proc/self/fd/{parent}', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
        try:
            os.fsync(readable)
        finally:
            os.close(readable)
    finally:
        if descriptor >= 0:
            os.close(descriptor)
        if created:
            os.unlink(temporary, dir_fd=parent)
        os.close(parent)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', required=True, type=Path)
    parser.add_argument('--manifest', required=True, type=Path)
    parser.add_argument('--destination', required=True, type=Path)
    args = parser.parse_args()
    source, destination = absolute(args.source_root), absolute(args.destination)
    captured, manifest, total = capture(source, args.manifest)
    for relative, content in captured:
        install_file(destination, relative, content)
    encoded = json.dumps(manifest, ensure_ascii=False, sort_keys=True, separators=(',', ':')).encode()
    print(json.dumps({'asset_count': len(captured) - 1, 'asset_bytes': total,
                      'bundle_sha256': hashlib.sha256(encoded).hexdigest()}))


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RecursionError) as error:
        print(json.dumps({'error': 'runtime_resource_install', 'message': str(error)[:1024]}), file=sys.stderr)
        raise SystemExit(2)
