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
MACHINE_ENVIRONMENT_KEY = r'SYSTEM\CurrentControlSet\Control\Session Manager\Environment'


def machine_image_environment():
    """Read only two named machine values; never expand or enumerate environment."""
    result = dict(schema='wls-native-machine-image-environment-v1',
        captured_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        source='read-only Windows Registry HKEY_LOCAL_MACHINE',
        key=MACHINE_ENVIRONMENT_KEY, view='64-bit',
        values={key: None for key in IMAGE_KEYS},
        field_status={key: 'NOT_CHECKED' for key in IMAGE_KEYS},
        status='NOT_CHECKED', reason=None)
    if os.name != 'nt':
        result['reason'] = 'NATIVE_WINDOWS_REQUIRED'
        return result
    try:
        import winreg
    except ImportError:
        result['reason'] = 'REGISTRY_API_UNAVAILABLE'
        return result
    try:
        # Query-only, explicit native view. No process/user fallback: the
        # inherited process values are captured separately under their own name.
        with winreg.OpenKey(winreg.HKEY_LOCAL_MACHINE, MACHINE_ENVIRONMENT_KEY, 0,
                            winreg.KEY_QUERY_VALUE | winreg.KEY_WOW64_64KEY) as key:
            for name in IMAGE_KEYS:
                try:
                    value, kind = winreg.QueryValueEx(key, name)
                except FileNotFoundError:
                    result['field_status'][name] = 'MISSING'
                except PermissionError:
                    result['field_status'][name] = 'DENIED'
                except OSError:
                    result['field_status'][name] = 'QUERY_FAILED'
                else:
                    if (kind != winreg.REG_SZ or not isinstance(value, str) or not value or
                            len(value) > 256 or any(ord(item) < 32 or ord(item) == 127 for item in value)):
                        result['field_status'][name] = 'UNSUPPORTED_VALUE'
                    else:
                        result['values'][name] = value
                        result['field_status'][name] = 'OBSERVED'
    except FileNotFoundError:
        result['reason'] = 'MACHINE_ENVIRONMENT_KEY_MISSING'
    except PermissionError:
        result['reason'] = 'MACHINE_ENVIRONMENT_KEY_DENIED'
    except OSError:
        result['reason'] = 'MACHINE_ENVIRONMENT_KEY_UNAVAILABLE'
    observed = sum(value == 'OBSERVED' for value in result['field_status'].values())
    result['status'] = 'OBSERVED' if observed == len(IMAGE_KEYS) else 'PARTIAL' if observed else 'NOT_CHECKED'
    return result


def require_actions():
    if os.name != 'nt' or os.environ.get('GITHUB_ACTIONS') != 'true':
        raise RuntimeError('this runner script requires an ephemeral native Windows Actions job')


def image_environment(checkout_sha):
    """Observe named process variables; these are not the Runner Image log header."""
    return dict(schema='wls-native-image-environment-v1',
        captured_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        source='inherited process environment; no job-header or image-release inference',
        image_environment={key: os.environ.get(key) for key in IMAGE_KEYS},
        machine_image_environment=machine_image_environment(),
        github_context={key: os.environ.get(key) for key in CONTEXT_KEYS},
        github_job_meaning='workflow job key, not the numeric Actions API job ID',
        actual_checkout_sha=checkout_sha)


def image_provenance(earlier, current):
    if earlier.get('schema') != 'wls-native-image-environment-v1':
        raise RuntimeError('unrecognized workflow image environment snapshot')
    changed = [key for key in ('image_environment', 'github_context', 'actual_checkout_sha')
               if earlier.get(key) != current[key]]
    process_changed = bool(changed)
    machine_keys = ('schema', 'source', 'key', 'view', 'values', 'field_status', 'status', 'reason')
    before_machine, after_machine = (value.get('machine_image_environment') for value in (earlier, current))
    machine_consistency = 'NOT_CHECKED'
    if isinstance(before_machine, dict) and isinstance(after_machine, dict):
        machine_changed = any(before_machine.get(key) != after_machine.get(key) for key in machine_keys)
        machine_consistency = 'CHANGED' if machine_changed else 'UNCHANGED'
        if machine_changed:
            changed.append('machine_image_environment')
    values = current['image_environment']
    return dict(schema='wls-native-image-provenance-v1', workflow_capture=earlier,
        runner_entry=current, changed_fields=changed,
        process_environment_consistency='CHANGED' if process_changed else 'UNCHANGED',
        machine_environment_consistency=machine_consistency,
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
    build = work / 'build'
    before = binary_before = None
    build_completed = False
    capture_errors = []

    def remember_capture_error(name, error):
        capture_errors.append((name, error))

    def capture_error_records():
        return [dict(evidence=name, type=type(error).__name__, reason=str(error)[:2048])
                for name, error in capture_errors]

    def record(name, value):
        (evidence / name).write_text(json.dumps(value, ensure_ascii=False, indent=2) + '\n', encoding='utf-8')

    def run(name, command, timeout=120):
        command = [os.fspath(value) for value in command]
        started = time.time()

        def retain_output(stdout, stderr, details, *, emit_output):
            observations = [
                (name + '.stdout', lambda: (evidence / (name + '.stdout')).write_bytes(stdout)),
                (name + '.stderr', lambda: (evidence / (name + '.stderr')).write_bytes(stderr)),
                (name + '.command.json', lambda: record(name + '.command.json', dict(
                    argv=command, cwd=str(work), started_unix=started, ended_unix=time.time(), **details))),
            ]
            if emit_output:
                observations.extend(((name + '.terminal-stdout', lambda: sys.stdout.buffer.write(stdout)),
                                     (name + '.terminal-stderr', lambda: sys.stderr.buffer.write(stderr))))
            first_error = None
            for label, observe in observations:
                try:
                    observe()
                except BaseException as error:
                    remember_capture_error(label, error)
                    if first_error is None:
                        first_error = error
            return first_error

        try:
            result = subprocess.run(command, cwd=work, env=environment,
                                    stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=timeout)
        except BaseException as error:
            status = ('TIMEOUT' if isinstance(error, subprocess.TimeoutExpired) else
                      'INTERRUPTED' if isinstance(error, (KeyboardInterrupt, SystemExit)) else 'LAUNCH_ERROR')
            retain_output(getattr(error, 'stdout', None) or b'', getattr(error, 'stderr', None) or b'',
                dict(exit_code=None, status=status, output_status='PARTIAL_OR_UNAVAILABLE'), emit_output=False)
            raise
        execution_failure = RuntimeError(f'{name} failed with exit {result.returncode}') if result.returncode else None
        capture_failure = retain_output(result.stdout, result.stderr, dict(exit_code=result.returncode), emit_output=True)
        if execution_failure is not None:
            raise execution_failure
        if capture_failure is not None:
            raise capture_failure
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
        if not result:
            raise RuntimeError('source input snapshot is empty')
        return result

    def binary_snapshot():
        result = {str(path.relative_to(build)): hashlib.sha256(path.read_bytes()).hexdigest()
                  for path in sorted((build / 'Release').glob('*.exe'))}
        if not result:
            raise RuntimeError('native binary input snapshot is empty')
        return result

    def snapshot_record(name, snapshot):
        value = snapshot()
        record(name, value)
        return value

    def finalize_identities(original_error):
        # Collect both sides independently; one failed read must not prevent the
        # other available identity from being retained or mask an original error.
        after = binary_after = None
        try:
            after = snapshot_record('source-inputs-after.json', source_snapshot)
        except BaseException as error:
            remember_capture_error('source-inputs-after.json', error)
        if build_completed:
            try:
                binary_after = snapshot_record('binary-inputs-after.json', binary_snapshot)
            except BaseException as error:
                remember_capture_error('binary-inputs-after.json', error)
        source_pair = before is not None and after is not None
        binary_pair = binary_before is not None and binary_after is not None
        source_unchanged = before == after if source_pair else None
        binary_unchanged = binary_before == binary_after if binary_pair else None
        identities_complete = source_pair and binary_pair and not capture_errors
        try:
            record('identity-finalization.json', dict(
                scope='retained source and executable identities; not a product pass result',
                captured_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
                original_execution_error=type(original_error).__name__ if original_error else None,
                build_completed=build_completed, source_pair_complete=source_pair,
                binary_pair_complete=binary_pair, identities_complete=identities_complete,
                source_inputs_unchanged=source_unchanged, binary_inputs_unchanged=binary_unchanged,
                capture_errors=capture_error_records()))
        except BaseException as error:
            remember_capture_error('identity-finalization.json', error)
        if original_error is None:
            if capture_errors:
                raise capture_errors[0][1]
            if not identities_complete:
                raise RuntimeError('complete source and binary input identities are unavailable')
            if not source_unchanged:
                raise RuntimeError('source input bytes changed during verification')
            if not binary_unchanged:
                raise RuntimeError('binary input bytes changed during verification')

    def execute():
        nonlocal before, binary_before, build_completed
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
        before = snapshot_record('source-inputs-before.json', source_snapshot)
        run('cmake-version', ['cmake', '--version'])
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
        build_completed = True
        binary_before = snapshot_record('binary-inputs-before.json', binary_snapshot)
        ctest_error = None
        try:
            run('ctest', ['ctest', '--test-dir', build, '-C', 'Release', '--output-on-failure',
                '--output-junit', evidence / 'ctest.xml'], timeout=300)
        except BaseException as error:
            ctest_error = error
            raise
        finally:
            try:
                retain_logs(build, evidence)
            except BaseException as error:
                remember_capture_error('ctest-log-files.json', error)
                if ctest_error is None:
                    raise
        methods = verify_methods(source, evidence)
        # Preserve the original successful-method-verification compatibility file.
        snapshot_record('binary-inputs.json', binary_snapshot)
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
        return dict(status='PASSED', scope='native build/format slice on Windows 11 ordinary user' if windows11 else 'native build/format slice on this hosted runner',
            source_inputs_unchanged=True, original_source_path_absence_verified=True,
            binary_inputs_unchanged=True, complete_input_identities_verified=True,
            native_suites=methods['suite_count'], native_methods=methods['method_count'],
            complete_original_method_logs_verified=True,
            windows_11_machine_acceptance='NOT_RUN', full_native_runtime='NOT_IMPLEMENTED',
            windows_11_component_validation='PASSED' if windows11 else 'NOT_RUN',
            ordinary_user_security_acceptance='NOT_RUN',
            exact_runner_image_identity='NOT_APPLICABLE_LOCAL_MACHINE' if windows11 else provenance['exact_runner_image_identity'],
            remote_ci_acceptance='NOT_APPLICABLE_LOCAL_MACHINE' if windows11 else 'requires actual workflow conclusion')

    execution_error = None
    try:
        try:
            result = execute()
        except BaseException as error:
            execution_error = error
            raise
        finally:
            finalize_identities(execution_error)
        record('result.json', result)
    except BaseException as error:
        try:
            record('failure.json', dict(status='FAILED', type=type(error).__name__, reason=str(error)[:2048],
                termination='TIMEOUT' if isinstance(error, subprocess.TimeoutExpired) else
                            'INTERRUPTED' if isinstance(error, (KeyboardInterrupt, SystemExit)) else 'FAILED',
                capture_errors=capture_error_records()))
        except BaseException as capture_error:
            # An unwritable evidence destination cannot replace the original
            # command, timeout, interrupt or identity failure.
            try:
                sys.stderr.buffer.write(('failure evidence could not be recorded: ' +
                    type(capture_error).__name__ + ': ' + str(capture_error)[:2048] + '\n').encode('utf-8'))
            except BaseException:
                pass
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
