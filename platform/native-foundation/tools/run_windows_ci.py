#!/usr/bin/env python3
"""Run the pinned, offline native build on an ephemeral Windows Actions runner.

This checks W-BUILD slice mechanics only. It does not accept Windows 11,
ordinary-user ACLs, Workspace, Store, process supervision or sandbox security.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--work-root', type=Path, required=True)
    args = parser.parse_args()
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('this runner script requires an ephemeral native Windows Actions job')
    source, work = args.source_root.resolve(strict=True), args.work_root.absolute()
    work.mkdir(parents=True, exist_ok=False)
    evidence = work / 'evidence'
    evidence.mkdir()
    temporary = work / 'tmp'
    temporary.mkdir()
    environment = os.environ.copy()
    environment.update(TEMP=str(temporary), TMP=str(temporary), PYTHONUTF8='1', PYTHONDONTWRITEBYTECODE='1')

    def record(name, value):
        (evidence / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

    def run(name, command, timeout=120):
        command = [os.fspath(value) for value in command]
        started = time.time()
        try:
            result = subprocess.run(command, cwd=work, env=environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except subprocess.TimeoutExpired as error:
            (evidence / (name + '.stdout')).write_bytes(error.stdout or b'')
            (evidence / (name + '.stderr')).write_bytes(error.stderr or b'')
            record(name + '.command.json', dict(argv=command, cwd=str(work), started_unix=started,
                   ended_unix=time.time(), exit_code=None, status='TIMEOUT'))
            raise
        (evidence / (name + '.stdout')).write_bytes(result.stdout)
        (evidence / (name + '.stderr')).write_bytes(result.stderr)
        record(name + '.command.json', dict(argv=command, cwd=str(work), started_unix=started,
               ended_unix=time.time(), exit_code=result.returncode))
        sys.stdout.buffer.write(result.stdout)
        sys.stderr.buffer.write(result.stderr)
        if result.returncode:
            raise RuntimeError(f'{name} failed with exit {result.returncode}')
        return result.stdout

    def source_snapshot():
        listing = subprocess.check_output(['git', '-C', str(source), 'ls-files', '-z'])
        result = {}
        for item in listing.split(b'\0'):
            if not item:
                continue
            relative = item.decode('utf-8')
            data = (source / relative).read_bytes()
            result[relative] = dict(sha256=hashlib.sha256(data).hexdigest(), size_bytes=len(data))
        return result

    record('environment.json', dict(platform=platform.platform(), windows_version=list(sys.getwindowsversion()),
        machine=platform.machine(), python=sys.version, python_executable=sys.executable,
        image_os=os.environ.get('ImageOS'), image_version=os.environ.get('ImageVersion'),
        github_sha=os.environ.get('GITHUB_SHA'), toolset='14.44.35207', sdk='10.0.26100.0',
        windows_11_machine_acceptance='NOT_RUN', ordinary_user_security_acceptance='NOT_RUN',
        full_native_runtime='NOT_IMPLEMENTED'))
    try:
        vswhere = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
        installations = json.loads(run('vswhere', [vswhere, '-products', '*', '-version', '[17.14,17.15)',
            '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-format', 'json', '-utf8']))
        if len(installations) != 1:
            raise RuntimeError('exactly one preinstalled VS 2022 17.14 installation is required')
        installation = Path(installations[0]['installationPath'])
        compiler = installation / 'VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'
        sdk = Path(os.environ['ProgramFiles(x86)']) / 'Windows Kits/10/Include/10.0.26100.0/um/Windows.h'
        if not compiler.is_file() or not sdk.is_file():
            raise RuntimeError('pinned preinstalled MSVC/SDK is unavailable; no substitute/download is permitted')
        record('pinned-tool-inputs.json', {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
                                         for path in (compiler, sdk)})
        run('git-head', ['git', '-C', source, 'rev-parse', 'HEAD'])
        status = run('git-status', ['git', '-C', source, 'status', '--porcelain'])
        if status.strip():
            raise RuntimeError('checkout is not clean')
        before = source_snapshot()
        record('source-inputs-before.json', before)
        run('cmake-version', ['cmake', '--version'])
        build = work / 'build'
        try:
            run('configure', ['cmake', '-S', source / 'platform/native-foundation', '-B', build,
                '-G', 'Visual Studio 17 2022', '-A', 'x64,version=10.0.26100.0',
                '-T', 'v143,host=x64,version=14.44.35207',
                '-DCMAKE_GENERATOR_INSTANCE=' + str(installation),
                '-DCMAKE_SYSTEM_VERSION=10.0.26100.0', '-DPython3_EXECUTABLE=' + sys.executable], timeout=180)
        finally:
            for name in ('CMakeCache.txt', 'native-toolchain.txt'):
                if (build / name).is_file():
                    shutil.copyfile(build / name, evidence / name)
        compiler_configs = sorted((build / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'))
        if len(compiler_configs) != 1:
            raise RuntimeError('expected exactly one generated compiler identity')
        shutil.copyfile(compiler_configs[0], evidence / 'CMakeCXXCompiler.cmake')
        run('build', ['cmake', '--build', build, '--config', 'Release', '--parallel', '2'], timeout=600)
        run('ctest', ['ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure',
            '--output-junit', evidence / 'ctest.xml'], timeout=300)
        binary_inputs = {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in sorted((build / 'Release').glob('*.exe'))}
        record('binary-inputs.json', binary_inputs)
        installed = work / 'installed'
        run('install', ['cmake', '--install', build, '--config', 'Release', '--prefix', installed])
        relocated = work / 'relocated \u65e0\u91cf\u5c71 \U0001f3d4'
        installed.rename(relocated)
        binary = relocated / 'bin/wls-native-foundation.exe'
        run('host-capabilities', [binary, 'capabilities'])
        shutil.copyfile(source / 'platform/native-foundation/tools/verify_detached.py', work / 'verify_detached.py')
        moved = source.with_name(source.name + '-offline-for-native-proof')
        if moved.exists():
            raise RuntimeError('detached source destination already exists')
        source.rename(moved)
        try:
            run('detached', [sys.executable, work / 'verify_detached.py', '--executable', binary,
                '--original-source-root', source])
        finally:
            moved.rename(source)
        after = source_snapshot()
        record('source-inputs-after.json', after)
        if before != after:
            raise RuntimeError('source input bytes changed during verification')
        record('result.json', dict(status='PASSED', scope='native build/format slice on this hosted runner',
            source_inputs_unchanged=True, original_source_path_absence_verified=True,
            windows_11_machine_acceptance='NOT_RUN', full_native_runtime='NOT_IMPLEMENTED',
            ordinary_user_security_acceptance='NOT_RUN', remote_ci_acceptance='requires actual workflow conclusion'))
    except BaseException as error:
        record('failure.json', dict(status='FAILED', type=type(error).__name__, reason=str(error)[:2048]))
        raise


if __name__ == '__main__':
    main()
