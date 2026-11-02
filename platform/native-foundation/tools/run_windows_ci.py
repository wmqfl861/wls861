#!/usr/bin/env python3
"""Run the pinned, offline native build on an ephemeral Windows Actions runner.

This checks W-BUILD slice mechanics only. It does not accept Windows 11,
ordinary-user ACLs, Workspace, Store, process supervision or sandbox security.
"""
import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import time

from ctest_evidence import retain_logs, verify_methods


IMAGE_KEYS = ('ImageOS', 'ImageVersion')
CONTEXT_KEYS = ('GITHUB_RUN_ID', 'GITHUB_RUN_ATTEMPT', 'GITHUB_JOB', 'GITHUB_SHA',
                'GITHUB_EVENT_NAME', 'GITHUB_WORKFLOW_REF', 'GITHUB_WORKFLOW_SHA',
                'RUNNER_OS', 'RUNNER_ARCH', 'RUNNER_NAME')


def require_actions():
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('this runner script requires an ephemeral native Windows Actions job')


def image_environment(checkout_sha):
    """Observe named process variables; these are not the Runner Image log header."""
    return dict(schema='wls-native-image-environment-v1',
        captured_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        source='inherited process environment; no job-header or image-release inference',
        image_environment={key: os.environ.get(key) for key in IMAGE_KEYS},
        github_context={key: os.environ.get(key) for key in CONTEXT_KEYS},
        github_job_meaning='workflow job key, not the numeric Actions API job ID',
        actual_checkout_sha=checkout_sha)


def image_provenance(earlier, current):
    if earlier.get('schema') != 'wls-native-image-environment-v1':
        raise RuntimeError('unrecognized workflow image environment snapshot')
    changed = [key for key in ('image_environment', 'github_context', 'actual_checkout_sha')
               if earlier.get(key) != current[key]]
    values = current['image_environment']
    return dict(schema='wls-native-image-provenance-v1', workflow_capture=earlier,
        runner_entry=current, changed_fields=changed,
        process_environment_consistency='CHANGED' if changed else 'UNCHANGED',
        image_environment_fields_complete=all(isinstance(values[key], str) and values[key]
                                               for key in IMAGE_KEYS),
        job_header_comparison='NOT_COLLECTED; compare the original same-job Runner Image group externally',
        exact_runner_image_identity='UNRESOLVED_REQUIRES_JOB_HEADER_COMPARISON')


def run_native(source_root, work_root, *, image_snapshot=None, windows11=False):
    if windows11:
        if os.name != 'nt':
            raise RuntimeError('Windows 11 component validation requires native Windows')
    else:
        require_actions()
    source, work = source_root.resolve(strict=True), work_root.absolute()
    if source == work or source in work.parents or work in source.parents:
        raise RuntimeError('source and fresh work directories must be separate')
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

    try:
        if windows11:
            from windows_host import observe_host, require_windows11
            host = observe_host(source, work)
            record('windows11-host.json', host)
            require_windows11(host)
        checkout = run('git-head', ['git', '-C', source, 'rev-parse', 'HEAD']).decode('ascii').strip()
        if len(checkout) != 40 or any(value not in '0123456789abcdef' for value in checkout):
            raise RuntimeError('expected one full checkout SHA')
        current = image_environment(checkout)
        record('environment.json', dict(platform=platform.platform(), windows_version=list(sys.getwindowsversion()),
            machine=platform.machine(), python=sys.version, python_executable=sys.executable,
            image_os=current['image_environment']['ImageOS'],
            image_version=current['image_environment']['ImageVersion'],
            image_fields_source=current['source'], captured_at_utc=current['captured_at_utc'],
            github_sha=os.environ.get('GITHUB_SHA'), github_context=current['github_context'],
            actual_checkout_sha=checkout, execution_scope='windows11-ordinary-user-component' if windows11 else 'hosted-actions-component',
            toolset='14.44.35207', sdk='10.0.26100.0', windows_11_machine_acceptance='NOT_RUN',
            ordinary_user_security_acceptance='NOT_RUN', full_native_runtime='NOT_IMPLEMENTED'))
        if not windows11:
            if image_snapshot is None:
                raise RuntimeError('an earlier workflow image environment snapshot is required')
            with image_snapshot.open('rb') as stream:
                raw = stream.read(65537)
            if len(raw) > 65536:
                raise RuntimeError('image environment snapshot exceeds its bound')
            (evidence / 'image-environment-workflow.json').write_bytes(raw)
            provenance = image_provenance(json.loads(raw), current)
            record('image-provenance.json', provenance)
            if provenance['changed_fields']:
                raise RuntimeError('workflow image environment or run/checkout binding changed')
            if checkout != os.environ.get('GITHUB_SHA'):
                raise RuntimeError('actual checkout differs from the Actions SHA')
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
        try:
            run('ctest', ['ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure',
                '--output-junit', evidence / 'ctest.xml'], timeout=300)
        finally:
            retain_logs(build, evidence)
        methods = verify_methods(source, evidence)
        binary_inputs = {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest()
                         for path in sorted((build / 'Release').glob('*.exe'))}
        record('binary-inputs.json', binary_inputs)
        installed = work / 'installed'
        run('install', ['cmake', '--install', build, '--config', 'Release', '--prefix', installed])
        relocated = work / 'relocated \u65e0\u91cf\u5c71 \U0001f3d4'
        installed.rename(relocated)
        binary = relocated / 'bin/wls-native-foundation.exe'
        capabilities = run('host-capabilities', [binary, 'capabilities'])
        if windows11:
            observed = json.loads(capabilities)
            if observed.get('token_elevation') != 'not_elevated' or observed.get('filesystem') != 'NTFS':
                raise RuntimeError('installed component did not confirm a non-elevated NTFS host')
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
        record('result.json', dict(status='PASSED', scope='native build/format slice on Windows 11 ordinary user' if windows11 else 'native build/format slice on this hosted runner',
            source_inputs_unchanged=True, original_source_path_absence_verified=True,
            native_suites=methods['suite_count'], native_methods=methods['method_count'],
            complete_original_method_logs_verified=True,
            windows_11_machine_acceptance='NOT_RUN', full_native_runtime='NOT_IMPLEMENTED',
            windows_11_component_validation='PASSED' if windows11 else 'NOT_RUN',
            ordinary_user_security_acceptance='NOT_RUN',
            exact_runner_image_identity='NOT_APPLICABLE_LOCAL_MACHINE' if windows11 else provenance['exact_runner_image_identity'],
            remote_ci_acceptance='NOT_APPLICABLE_LOCAL_MACHINE' if windows11 else 'requires actual workflow conclusion'))
    except BaseException as error:
        record('failure.json', dict(status='FAILED', type=type(error).__name__, reason=str(error)[:2048]))
        raise


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source-root', type=Path, required=True)
    parser.add_argument('--work-root', type=Path)
    parser.add_argument('--capture-image-environment', type=Path)
    parser.add_argument('--image-environment-snapshot', type=Path)
    args = parser.parse_args()
    require_actions()
    if args.capture_image_environment is not None:
        if args.work_root is not None or args.image_environment_snapshot is not None:
            parser.error('environment capture does not build or accept execution options')
        source = args.source_root.resolve(strict=True)
        target = args.capture_image_environment.absolute()
        if source == target or source in target.parents:
            parser.error('capture outside the clean source checkout')
        checkout = subprocess.check_output(['git', '-C', str(source), 'rev-parse', 'HEAD'], timeout=30).decode('ascii').strip()
        observation = image_environment(checkout)
        with target.open('x', encoding='utf-8') as stream:
            stream.write(json.dumps(observation, indent=2) + '\n')
        print(json.dumps(observation, sort_keys=True))
        return
    if args.work_root is None:
        parser.error('--work-root is required for execution')
    run_native(args.source_root, args.work_root, image_snapshot=args.image_environment_snapshot)


if __name__ == '__main__':
    main()
