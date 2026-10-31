#!/usr/bin/env python3
"""Verify an installed binary while its ORIGINAL build source path is absent.

The caller moves only a disposable build-source copy and restores it afterward.
This script never renames the operator's checkout or rebuilds anything.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--executable', required=True, type=Path)
    parser.add_argument('--original-source-root', required=True, type=Path)
    args = parser.parse_args()
    if args.original_source_root.exists() or args.original_source_root.is_symlink():
        raise ValueError('original build source path is still present')
    executable = args.executable.resolve(strict=True)
    environment = os.environ.copy()
    environment.update(WLS_RESOURCE_ROOT=str(args.original_source_root / 'missing'),
                       WLS_SOURCE_ROOT=str(args.original_source_root))
    with tempfile.TemporaryDirectory(prefix='wls-native-detached-') as temporary:
        def invoke(arguments, expected=0):
            result = subprocess.run([str(executable), *arguments], cwd=temporary, env=environment,
                                    capture_output=True, timeout=15)
            if result.returncode != expected:
                raise ValueError(f'detached command failed: {arguments!r}: {result!r}')
            return json.loads(result.stdout if expected == 0 else result.stderr)
        resources = invoke(['resources'])
        if resources['asset_count'] != 35 or resources['asset_bytes'] != 160549:
            raise ValueError('detached public resource set differs')
        config = invoke(['resolve-config', '--root-model', 'detached', '--root-reasoning', 'effort:high',
                         '--capability', 'detached=effort:high'])
        if config['model'] != 'detached' or config['reasoning'] != {'mode': 'explicit', 'effort': 'high'}:
            raise ValueError('detached original config module differs')
        refusal = invoke(['run', '--program', 'never-executed'], expected=2)
        if refusal['error'] != 'unsupported':
            raise ValueError('detached business execution was not refused')
        host = invoke(['capabilities'])
        print(json.dumps({'format': 'wls-native-detached-verification-v1', 'source_path_absent': True,
                          'resources': resources, 'host': host, 'config': config,
                          'business_refusal': refusal}, ensure_ascii=False))


if __name__ == '__main__':
    main()
