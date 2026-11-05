#!/usr/bin/env python3
"""Selfcheck for the owned Windows process supervisor (E002 fix).

``--mode synthetic`` drives :func:`windows_process.run_owned` with an injected
fake API and fake clock: stage failures, root exit with live descendants,
query/wait failures, bounded budgets, second interrupts and capture failures
are all exercised without Win32. It runs on any platform.

``--mode native`` runs the real Win32 supervisor against real owned-process
fixtures on Windows: parent/child chains, real timeouts, inherited output
holders, an isolated-console Ctrl-C, an assign-failure injection, an
independent sentinel and two full ``run_native`` integrations. On non-Windows
it records NOT_RUN and exits 3 — CI must treat that as a failure, never a
skip. Fixtures are offline, self-created processes only; the supervisor Job
is the harness for every dangerous fixture, and no PID/name-based killing or
taskkill is used anywhere.
"""
import argparse
import ctypes
import datetime
import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

sys.path.insert(0, str(Path(__file__).resolve().parent))
import windows_process
from windows_process import (OwnedCommandLaunchError, Win32CallFailure,
                             WindowsProcessError, UnsupportedPlatform,
                             Win32ProcessAPI, run_owned)

SCENARIO_BUDGET_SECONDS = 25.0
NATIVE_INTERNAL_TOTAL_SECONDS = 280.0
STARTUP_BUDGET_SECONDS = 15.0
FINALIZE_BUDGET_SECONDS = 15.0
EVIDENCE_SCHEMA = 'wls-windows-process-supervisor-selfcheck-v1'


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


class CaseFailure(AssertionError):
    pass


class BudgetExceeded(Exception):
    pass


class Recorder:
    def __init__(self, root):
        self.root = Path(root)
        self.cases = []

    def begin(self, root):
        root = Path(root)
        if root.exists():
            raise SystemExit(f'evidence directory already exists: {root}')
        root.mkdir(parents=True)
        self.root = root

    def case(self, name, group):
        return _CaseWriter(self, name, group, time.monotonic())


class _CaseWriter:
    def __init__(self, recorder, name, group, started):
        self._recorder = recorder
        self.name = name
        self.group = group
        self.started = started
        self.facts = {}
        self.status = None
        self.error = None

    def check(self, condition, description):
        if not condition:
            raise CaseFailure(description)
        return True

    def record(self, **facts):
        self.facts.update(facts)

    def finish(self, status, error=None):
        self.status = status
        self.error = error
        entry = {
            'schema': EVIDENCE_SCHEMA,
            'case': self.name,
            'group': self.group,
            'status': status,
            'elapsed_seconds': round(time.monotonic() - self.started, 3),
            'facts': self.facts,
        }
        if error is not None:
            entry['error'] = {'type': type(error).__name__, 'reason': str(error)[:1024]}
        (self._recorder.root / f'{self.group}-{self.name}.json').write_text(
            json.dumps(entry, indent=2, default=str) + '\n', encoding='utf-8')
        self._recorder.cases.append(entry)
        return entry


def run_cases(cases, recorder, group, budget_seconds):
    for name, function in cases:
        writer = recorder.case(name, group)
        try:
            if time.monotonic() - writer.started > budget_seconds:
                raise BudgetExceeded(f'{name} exceeded its {budget_seconds}s budget')
            function(writer)
            writer.finish('PASS')
        except BaseException as error:  # keep original evidence; never mask
            writer.finish('FAIL', error)
            if isinstance(error, (KeyboardInterrupt, SystemExit)):
                raise


# --------------------------------------------------------------------------------------
# Synthetic mode: fake API and fake clock drive the real supervisor state machine
# --------------------------------------------------------------------------------------

class FakeClock:
    is_injected = True

    def __init__(self, start=1000.0):
        self.now = start
        self.sleep_total = 0.0

    def monotonic(self):
        return self.now

    def sleep(self, seconds):
        seconds = max(0.0, seconds)
        self.now += seconds
        self.sleep_total += seconds


class FakeWin32API:
    """Scripted stand-in for Win32ProcessAPI used only by synthetic cases."""

    is_injected = True

    def __init__(self, script=None):
        self.script = dict(script or {})
        self.calls = []
        self.query_count = 0
        self.wait_count = 0
        self.terminated = False
        self.root_started = None
        self.resumed = False
        self.default_active = self.script.get('active')

    def _log(self, name, **details):
        self.calls.append((name, details))

    # ---- lifecycle ---------------------------------------------------------------
    def create_job(self):
        self._log('create_job')
        if self.script.get('fail') == 'create_job':
            raise Win32CallFailure('CreateJobObjectW', 5)
        return 'fake-job-1'

    def set_job_kill_on_close(self, job):
        self._log('set_job_kill_on_close')
        if self.script.get('fail') == 'set_info':
            raise Win32CallFailure('SetInformationJobObject', 5)

    def open_input_null(self):
        self._log('open_input_null')
        if self.script.get('fail') == 'open_null':
            raise Win32CallFailure('CreateFileW(NUL)', 5)
        return 'fake-stdin'

    def open_output_spool(self, path):
        self._log('open_output_spool', path=str(path))
        if self.script.get('fail') == 'open_spool':
            raise Win32CallFailure('CreateFileW(spool)', 5)
        if not self.script.get('no_spool_files'):
            Path(path).write_bytes(self.script.get('spool_bytes', b''))
        return 'fake-spool-' + Path(path).name

    def create_process_suspended(self, **kwargs):
        self._log('create_process_suspended', application=kwargs['application'])
        if self.script.get('fail') == 'create_process':
            raise Win32CallFailure('CreateProcessW', 2)
        self.root_started = self.script.get('clock_ref').now
        return 'fake-process', 'fake-thread', 4242, 133450000000000000

    def assign_process_to_job(self, job, process):
        self._log('assign_process_to_job')
        if self.script.get('fail') == 'assign':
            raise Win32CallFailure('AssignProcessToJobObject', 5)

    def resume_thread(self, thread):
        self._log('resume_thread')
        if self.script.get('fail') == 'resume':
            raise Win32CallFailure('ResumeThread', 5)
        self.resumed = True
        return 1

    def wait_process(self, process, milliseconds):
        self.wait_count += 1
        clock = self.script['clock_ref']
        clock.now += milliseconds / 1000.0
        self._log('wait_process', wait_count=self.wait_count)
        if self.script.get('interrupt_in_wait') == self.wait_count:
            raise KeyboardInterrupt('synthetic first interrupt in wait')
        if self.script.get('wait_error'):
            raise Win32CallFailure('WaitForSingleObject', 1460)
        if self.terminated:
            return 'SIGNALED'
        exits_at = self.script.get('root_exit_after')
        if exits_at is not None and self.root_started is not None:
            if clock.now >= self.root_started + exits_at:
                return 'SIGNALED'
        return 'TIMEOUT'

    def process_exit_code(self, process):
        self._log('process_exit_code')
        return int(self.script.get('root_exit_code', 0))

    def process_id(self, process):
        return 4242

    def query_job_active_processes(self, job):
        self.query_count += 1
        self._log('query_job_active_processes', query_count=self.query_count)
        query_error = self.script.get('query_error')
        if callable(query_error) and query_error(self.query_count):
            raise Win32CallFailure('QueryInformationJobObject', 5)
        if self.terminated and self.script.get('terminate_clears', True):
            return 0
        active = self.default_active
        if callable(active):
            return int(active(self))
        return int(active or 0)

    def terminate_job(self, job, exit_code):
        self._log('terminate_job')
        if self.script.get('terminate_fails'):
            raise Win32CallFailure('TerminateJobObject', 5)
        self.terminated = True

    def terminate_process(self, process, exit_code):
        self._log('terminate_process')
        self.terminated = True

    def close_handle(self, handle):
        self._log('close_handle', handle=str(handle))
        if str(handle) in self.script.get('close_fails', ()):
            raise Win32CallFailure('CloseHandle', 6)

    def job_process_ids(self, job):
        return []

    def open_process_snapshot(self, pid):
        raise UnsupportedPlatform('fake api has no external processes')


def synthetic_run(case, script, **kwargs):
    clock = FakeClock()
    script = dict(script)
    script['clock_ref'] = clock
    api = FakeWin32API(script)
    spool = case._recorder.root / 'synthetic-spool' / case.name
    command = kwargs.pop('command', [sys.executable, '-c', 'pass'])
    try:
        result = run_owned(command, cwd=str(spool.parent), env={'PATH': os.environ.get('PATH', '')},
                           timeout=kwargs.pop('timeout', 30.0), spool_root=spool,
                           command_id='synthetic-' + case.name, api=api, clock=clock,
                           cleanup_timeout_seconds=kwargs.pop('cleanup_timeout_seconds', 5.0),
                           settle_configured_seconds=kwargs.pop('settle_configured_seconds', 1.0),
                           poll_seconds=kwargs.pop('poll_seconds', 0.1), **kwargs)
        return result, None
    except BaseException as error:
        return None, error


def names_of(calls):
    return [name for name, _ in calls]


def synthetic_normal_completion(case):
    result, error = synthetic_run(case, {'root_exit_after': 0.3, 'active': 0})
    case.check(error is None, f'unexpected error {error!r}')
    case.check(result.outcome == 'COMPLETED', 'expected COMPLETED')
    case.check(result.exit_code == 0, 'expected exit 0')
    case.check(result.tree_exit_proven, 'expected proven tree exit')
    receipt = result.receipt
    case.check(receipt['settle']['status'] == 'NATURAL_ZERO', 'expected NATURAL_ZERO settle')
    case.check(receipt['termination']['requested'] is False, 'no termination expected')
    case.check(receipt['job_zero_observation']['zero_observed'] is True, 'job zero observed')
    case.check(receipt['output']['complete'] is True, 'expected complete spool capture')
    case.record(receipt_summary=_summary(receipt))


def _summary(receipt):
    return dict(outcome=receipt.get('outcome'), tree_exit_proven=receipt.get('tree_exit_proven'),
                exit_code=receipt.get('exit_code'),
                settle=receipt.get('settle'), cleanup=receipt.get('cleanup', {}).get('status'),
                termination=receipt.get('termination'))


def synthetic_stage_failures(case):
    expectations = {
        'create_job': dict(cleanup='NOTHING_OWNED', created=False),
        'set_info': dict(cleanup='NOTHING_OWNED', created=False),
        'open_null': dict(cleanup='NOTHING_OWNED', created=False),
        'open_spool': dict(cleanup='NOTHING_OWNED', created=False),
        'create_process': dict(cleanup='NOTHING_OWNED', created=False),
    }
    for stage, expected in expectations.items():
        result, error = synthetic_run(case, {'fail': stage, 'active': 0})
        case.check(result is None and error is not None, f'{stage}: expected an error')
        receipt = getattr(error, 'process_receipt', None)
        case.check(receipt is not None, f'{stage}: receipt missing')
        case.check(receipt['cleanup']['status'] == expected['cleanup'],
                   f'{stage}: cleanup status {receipt["cleanup"]["status"]}')
        case.check(receipt['establishment']['process_created_suspended'] is expected['created'],
                   f'{stage}: created flag mismatch')
        case.check(receipt['tree_exit_proven'] is False, f'{stage}: must stay unproven')
        case.record(**{stage: _summary(receipt)})


def synthetic_assign_failure_rejects_without_side_effects(case):
    api_events = {}
    clock = FakeClock()
    script = {'fail': 'assign', 'active': 0, 'clock_ref': clock}
    api = FakeWin32API(script)
    marker = case._recorder.root / 'synthetic-spool' / case.name / 'side-effect.txt'
    marker.parent.mkdir(parents=True, exist_ok=True)
    command = [sys.executable, '-c', f'open({str(marker)!r}, "w")']
    try:
        run_owned(command, cwd=str(marker.parent), env={'PATH': os.environ.get('PATH', '')},
                  timeout=10, spool_root=marker.parent, command_id='synthetic-assign-failure',
                  api=api, clock=clock)
        raise CaseFailure('assign failure must reject the command')
    except OwnedCommandLaunchError as error:
        receipt = error.process_receipt
    api_events['calls'] = names_of(api.calls)
    case.check(api.resumed is False, 'the primary thread must never resume')
    case.check('terminate_process' in api_events['calls'],
               'the suspended owned process must be reclaimed directly')
    case.check(not marker.exists(), 'no target code may run before assignment succeeds')
    case.check(receipt['establishment']['assigned_to_job'] is False, 'assignment recorded failed')
    case.check(receipt['establishment']['primary_thread_resumed'] is False, 'resume recorded failed')
    case.check(receipt['outcome'] == 'LAUNCH_ERROR', 'expected LAUNCH_ERROR outcome')
    case.record(receipt_summary=_summary(receipt), api_calls=api_events['calls'])


def synthetic_resume_failure(case):
    clock = FakeClock()
    api = FakeWin32API({'fail': 'resume', 'active': 0, 'clock_ref': clock})
    try:
        run_owned([sys.executable, '-c', 'pass'], cwd=str(case._recorder.root),
                  env={'PATH': os.environ.get('PATH', '')}, timeout=10,
                  spool_root=case._recorder.root / 'synthetic-spool' / case.name,
                  command_id='synthetic-resume-failure', api=api, clock=clock)
        raise CaseFailure('resume failure must reject the command')
    except OwnedCommandLaunchError as error:
        receipt = error.process_receipt
    case.check('terminate_job' in names_of(api.calls), 'assigned Job must be reclaimed')
    case.check(receipt['tree_exit_proven'] is True,
               'suspended subtree termination is provable (query zero + signalled + code)')
    case.record(receipt_summary=_summary(receipt))


def synthetic_root_zero_with_descendants(case):
    result, error = synthetic_run(case, {'root_exit_after': 0.2, 'root_exit_code': 0,
                                         'active': 1}, settle_configured_seconds=1.0)
    case.check(error is None, f'unexpected error {error!r}')
    case.check(result.exit_code == 0, 'root exit code must be preserved')
    case.check(result.outcome == 'FAILED_DESCENDANTS_AFTER_ROOT_EXIT',
               'root exit 0 must not pass while descendants live')
    receipt = result.receipt
    case.check(receipt['settle']['status'] == 'EXPIRED', 'settle window must expire')
    case.check(receipt['termination']['reason'] == 'DESCENDANTS_AFTER_ROOT_EXIT',
               'termination reason mismatch')
    case.check(receipt['termination']['requested'] is True, 'termination expected')
    case.check(receipt['tree_exit_proven'] is True, 'post-termination proof expected')
    case.check(receipt['cleanup']['status'] == 'PROVEN_ZERO', 'cleanup proven expected')
    case.record(receipt_summary=_summary(receipt))


def synthetic_nonzero_root(case):
    result, error = synthetic_run(case, {'root_exit_after': 0.2, 'root_exit_code': 7,
                                         'active': 2})
    case.check(error is None, f'unexpected error {error!r}')
    case.check(result.exit_code == 7, 'nonzero exit code preserved')
    case.check(result.outcome == 'FAILED_NONZERO_EXIT', 'expected FAILED_NONZERO_EXIT')
    case.check(result.receipt['settle']['status'] == 'NOT_ENTERED', 'no settle on nonzero exit')
    case.check(result.receipt['termination']['requested'] is True, 'immediate owned closeout expected')
    case.check(result.tree_exit_proven, 'proof still collected after nonzero exit')
    case.record(receipt_summary=_summary(result.receipt))


def synthetic_query_failure_keeps_unproven(case):
    # Settle expires, terminate succeeds, then every cleanup query fails.
    result, error = synthetic_run(case, {'root_exit_after': 0.1, 'active': 1,
                                         'query_error': lambda count: count > 3})
    case.check(error is None, f'unexpected error {error!r}')
    receipt = result.receipt
    case.check(result.tree_exit_proven is False, 'must stay unproven')
    case.check(receipt['job_zero_observation']['query_ok'] is False, 'query failed recorded')
    case.check(len(receipt['job_zero_observation']['query_failures']) > 0, 'failure retained')
    case.check(receipt['cleanup']['status'] == 'UNPROVEN_WITH_ERRORS',
               'unproven-with-errors expected')
    case.check(receipt['outcome'] not in ('COMPLETED',), 'must not claim completion')
    case.record(receipt_summary=_summary(receipt))


def synthetic_query_never_zero_exhausts_budget(case):
    result, error = synthetic_run(case, {'root_exit_after': 0.1, 'active': 3,
                                         'terminate_clears': False},
                                  cleanup_timeout_seconds=2.0)
    case.check(error is None, f'unexpected error {error!r}')
    receipt = result.receipt
    case.check(receipt['tree_exit_proven'] is False, 'no proof without observed zero')
    case.check(receipt['job_zero_observation']['zero_observed'] is False, 'never zero')
    case.check(receipt['cleanup']['elapsed_seconds'] >= 2.0 - 0.05,
               'cleanup budget must actually be consumed')
    case.check(receipt['cleanup']['status'] in ('UNPROVEN', 'UNPROVEN_WITH_ERRORS'),
               'unproven cleanup status')
    case.check(receipt['job_zero_observation']['last_active_processes'] == 3, 'last count kept')
    case.record(receipt_summary=_summary(receipt))


def synthetic_wait_failure(case):
    result, error = synthetic_run(case, {'wait_error': True, 'active': 0})
    case.check(error is not None and isinstance(error, WindowsProcessError),
               'wait failure must surface as a supervision error')
    receipt = getattr(error, 'process_receipt', None)
    case.check(receipt is not None, 'receipt attached')
    case.check(receipt['tree_exit_proven'] is False, 'unproven after wait failure')
    case.check(receipt['root_wait']['result'] in ('FAILED', None), 'root wait recorded failed')
    case.check(any(item.get('function') == 'WaitForSingleObject'
                   for item in receipt['cleanup']['errors']), 'wait failure retained')
    case.record(receipt_summary=_summary(receipt))


def synthetic_second_interrupt_deferred(case):
    clock = FakeClock()
    state = {'second_fired': False}

    def fire_second(query_index):
        # Fires inside the bounded cleanup, where the deferred handler exists.
        if query_index == 3 and not state['second_fired']:
            handler = signal.getsignal(signal.SIGINT)
            if callable(handler) and getattr(handler, '__name__', '') == 'deferred':
                handler(signal.SIGINT, None)
                state['second_fired'] = True
        return False

    api = FakeWin32API({'root_exit_after': None, 'active': 1, 'clock_ref': clock,
                        'interrupt_in_wait': 2, 'terminate_fails': True,
                        'query_error': fire_second})
    try:
        run_owned([sys.executable, '-c', 'pass'], cwd=str(case._recorder.root),
                  env={'PATH': os.environ.get('PATH', '')}, timeout=5.0,
                  spool_root=case._recorder.root / 'synthetic-spool' / case.name,
                  command_id='synthetic-second-interrupt', api=api, clock=clock,
                  cleanup_timeout_seconds=1.0)
        raise CaseFailure('the original KeyboardInterrupt must propagate')
    except KeyboardInterrupt as error:
        receipt = error.process_receipt
    case.check(receipt['cleanup']['second_interrupt_recorded_and_deferred'] is True,
               'second interrupt recorded, cleanup not skipped')
    case.check(receipt['cleanup']['status'] in ('PROVEN_ZERO', 'UNPROVEN', 'UNPROVEN_WITH_ERRORS'),
               'cleanup completed with a recorded status')
    case.check(receipt['termination']['reason'] == 'INTERRUPTED', 'interrupt termination recorded')
    case.record(receipt_summary=_summary(receipt), second_fired=state['second_fired'])


def synthetic_timeout_preserved_with_capture_failure(case):
    result, error = synthetic_run(case, {'root_exit_after': None, 'active': 1,
                                         'no_spool_files': True},
                                  timeout=2.0, cleanup_timeout_seconds=1.0)
    case.check(isinstance(error, subprocess.TimeoutExpired),
               f'original TimeoutExpired must be preserved, got {error!r}')
    receipt = error.process_receipt
    case.check(receipt is not None, 'receipt attached to the timeout')
    case.check(receipt['termination']['reason'] == 'EXECUTION_DEADLINE',
               'deadline termination recorded')
    case.check(receipt['output']['complete'] is False, 'capture failure explicit')
    case.check(len(receipt['output']['capture_errors']) > 0, 'capture errors retained')
    case.check(error.stdout in (None, b''), 'partial bytes stay partial')
    case.record(receipt_summary=_summary(receipt))


def synthetic_platform_refusal_and_no_fallback(case):
    if os.name != 'nt':
        try:
            run_owned([sys.executable, '-c', 'pass'], cwd='/', env={'PATH': '/usr/bin'},
                      timeout=1.0, spool_root=case._recorder.root / 'synthetic-spool' / case.name)
        except UnsupportedPlatform as error:
            case.record(refused=True, reason=str(error))
            return
        raise CaseFailure('run_owned must refuse unsupported platforms, not fall back')
    case.record(refused='SKIPPED_PLATFORM_NATIVE',
                reason='the real API exists on this host; refusal is exercised on other platforms')


def synthetic_ordering_contract(case):
    # Across every synthetic receipt: Job before process creation, resume only
    # after successful assignment, and no proven receipt without all three
    # observed proof facts.
    clock = FakeClock()
    api = FakeWin32API({'root_exit_after': 0.1, 'active': 0, 'clock_ref': clock})
    result = run_owned([sys.executable, '-c', 'pass'], cwd=str(case._recorder.root),
                       env={'PATH': os.environ.get('PATH', '')}, timeout=10,
                       spool_root=case._recorder.root / 'synthetic-spool' / case.name,
                       command_id='synthetic-ordering', api=api, clock=clock)
    calls = names_of(api.calls)
    case.check(calls.index('create_job') < calls.index('create_process_suspended'),
               'Job must exist before process creation')
    case.check(calls.index('assign_process_to_job') < calls.index('resume_thread'),
               'assignment must precede resume')
    receipt = result.receipt
    if receipt['tree_exit_proven']:
        zero = receipt['job_zero_observation']
        case.check(zero['zero_observed'] and zero['query_ok'], 'proof requires observed zero')
        case.check(receipt['exit_code_available'], 'proof requires a read exit code')
        case.check(receipt['root_wait']['signaled'] is True, 'proof requires a signaled root')
    case.record(call_order=calls[:12])


SYNTHETIC_CASES = [
    ('normal-completion', synthetic_normal_completion),
    ('stage-failures', synthetic_stage_failures),
    ('assign-failure-no-side-effects', synthetic_assign_failure_rejects_without_side_effects),
    ('resume-failure', synthetic_resume_failure),
    ('root-zero-descendants', synthetic_root_zero_with_descendants),
    ('nonzero-root', synthetic_nonzero_root),
    ('query-failure-unproven', synthetic_query_failure_keeps_unproven),
    ('query-never-zero-budget', synthetic_query_never_zero_exhausts_budget),
    ('wait-failure', synthetic_wait_failure),
    ('second-interrupt-deferred', synthetic_second_interrupt_deferred),
    ('timeout-with-capture-failure', synthetic_timeout_preserved_with_capture_failure),
    ('platform-refusal', synthetic_platform_refusal_and_no_fallback),
    ('ordering-contract', synthetic_ordering_contract),
]


# --------------------------------------------------------------------------------------
# Native mode: real Win32 supervisor against real owned fixtures (Windows only)
# --------------------------------------------------------------------------------------

class NativeBudget:
    """Per-scenario 25 s monotonic deadline; overrun is a scenario failure."""

    def __init__(self, seconds=SCENARIO_BUDGET_SECONDS):
        self.deadline = time.monotonic() + seconds

    def remaining(self):
        return self.deadline - time.monotonic()

    def check(self, what):
        if self.remaining() <= 0:
            raise BudgetExceeded(f'{what} exceeded the native scenario budget')


def _wait_until(path, timeout):
    path = Path(path)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if path.is_file():
            return True
        time.sleep(0.05)
    return False


def _fixture_argv(kind, **options):
    argv = [sys.executable, '-B', str(Path(__file__).resolve()), '--fixture', kind]
    for key, value in options.items():
        argv += [f'--{key.replace("_", "-")}', str(value)]
    return argv


class ObservingAPI:
    """Delegate to the real API while recording identities for assertions."""

    is_injected = False  # wraps the real native API; production semantics kept

    def __init__(self, real):
        self._real = real
        self.observations = {'jobs': [], 'processes': []}

    def create_job(self):
        job = self._real.create_job()
        self.observations['jobs'].append(job)
        return job

    def set_job_kill_on_close(self, job):
        return self._real.set_job_kill_on_close(job)

    def create_process_suspended(self, **kwargs):
        result = self._real.create_process_suspended(**kwargs)
        process, thread, pid, creation = result
        self.observations['processes'].append(
            {'pid': pid, 'creation_filetime': creation,
             'application': kwargs['application'], 'handle': process})
        return result

    def __getattr__(self, name):
        return getattr(self._real, name)


def _native_budgeted_launch(case, api, argv, *, command_id, timeout,
                            extra_creation_flags=0, cwd=None):
    spool = case._recorder.root / 'native-spool' / command_id
    cwd = cwd or str(case._recorder.root / 'native-cwd' / command_id)
    Path(cwd).mkdir(parents=True, exist_ok=True)
    started = time.monotonic()
    try:
        result = run_owned(argv, cwd=cwd, env=dict(os.environ), timeout=timeout,
                           spool_root=spool, command_id=command_id, api=api,
                           extra_creation_flags=extra_creation_flags)
        return result, None, time.monotonic() - started
    except BaseException as error:
        return None, error, time.monotonic() - started


def native_startup_checks(case):
    api = Win32ProcessAPI()
    sizes = {name: ctypes.sizeof(getattr(api, name))
             for name in windows_process.X64_STRUCTURE_SIZES}
    for name, expected in windows_process.X64_STRUCTURE_SIZES.items():
        case.check(sizes[name] == expected, f'{name} size {sizes[name]} != {expected}')
    case.check(ctypes.sizeof(ctypes.c_void_p) == 8, 'x64 pointer width required')
    # NULL and INVALID_HANDLE_VALUE stay distinct failure identities.
    try:
        api.open_output_spool(case._recorder.root / 'native-cwd' / 'missing-dir' / 'x')
        raise CaseFailure('CreateFileW on an impossible path must fail')
    except Win32CallFailure:
        pass
    # Nested Job allowance under the Actions outer job is verified by running
    # one real supervised command end to end.
    observing = ObservingAPI(api)
    result, error, _ = _native_budgeted_launch(
        case, observing, [sys.executable, '-c', 'print("ok")'],
        command_id='startup-probe', timeout=10)
    case.check(error is None and result.stdout.strip() == b'ok', 'startup probe failed')
    case.check(result.tree_exit_proven, 'startup probe must prove its tree exit')
    case.record(structure_sizes=sizes, nested_job_assignment='ALLOWED_AND_PROVEN',
                startup_probe=_summary(result.receipt))


def native_n1_normal_parent_child(case):
    api = ObservingAPI(Win32ProcessAPI())
    ready = case._recorder.root / 'native-n1-ready'
    argv = _fixture_argv('chain', ready=ready, parent_hold=0.5, child_hold=0.2,
                         child_count=1, exit_code=0)
    result, error, elapsed = _native_budgeted_launch(
        case, api, argv, command_id='n1-parent-child', timeout=15)
    case.check(error is None, f'unexpected error {error!r}')
    case.check(result.tree_exit_proven, 'tree exit must be proven')
    case.check(result.outcome == 'COMPLETED', 'expected completion')
    case.check(result.exit_code == 0, 'expected exit 0')
    receipt = result.receipt
    case.check(receipt['job_zero_observation']['zero_observed'] is True, 'job zero observed')
    case.check(b'child-marker' in result.stdout and b'parent-marker' in result.stdout,
               'both fixture markers must reach the spool')
    case.record(receipt=_summary(receipt), elapsed_seconds=round(elapsed, 3))


def native_n2_root_exit_descendant_alive(case):
    api = ObservingAPI(Win32ProcessAPI())
    survivor_root = case._recorder.root / 'native-n2'
    survivor_root.mkdir(parents=True, exist_ok=True)
    survivor_alive = survivor_root / 'descendant-survived.txt'
    argv = _fixture_argv('chain', ready=survivor_root / 'ready', parent_hold=0.4,
                         child_hold=6.0, child_count=1, exit_code=0,
                         survivor_marker=survivor_alive)
    result, error, elapsed = _native_budgeted_launch(
        case, api, argv, command_id='n2-descendant-alive', timeout=15)
    case.check(error is None, f'unexpected error {error!r}')
    receipt = result.receipt
    case.check(result.outcome == 'FAILED_DESCENDANTS_AFTER_ROOT_EXIT',
               'descendants after root exit must fail the command')
    case.check(result.exit_code == 0, 'root zero exit preserved')
    case.check(receipt['settle']['status'] in ('EXPIRED', 'EXECUTION_DEADLINE', 'QUERY_ERROR'),
               f'unexpected settle status {receipt["settle"]["status"]}')
    case.check(receipt['termination']['requested'] is True, 'termination expected')
    case.check(result.tree_exit_proven, 'post-termination proof expected')
    case.check(not survivor_alive.exists(), 'the descendant must not outlive the owned Job')
    case.record(receipt=_summary(receipt), elapsed_seconds=round(elapsed, 3))


def native_n3_real_timeout(case):
    api = ObservingAPI(Win32ProcessAPI())
    root = case._recorder.root / 'native-n3'
    root.mkdir(parents=True, exist_ok=True)
    argv = _fixture_argv('hang', ready=root / 'ready', seconds=30)
    budget = NativeBudget(20)
    result, error, elapsed = _native_budgeted_launch(
        case, api, argv, command_id='n3-timeout', timeout=3.0)
    budget.check('n3 cleanup')
    case.check(isinstance(error, subprocess.TimeoutExpired), 'TimeoutExpired must be preserved')
    receipt = error.process_receipt
    case.check(receipt['termination']['reason'] == 'EXECUTION_DEADLINE', 'deadline reason')
    case.check(receipt['tree_exit_proven'] is True, 'bounded cleanup must prove zero')
    case.check(elapsed < 20, 'cleanup must be bounded well below the scenario budget')
    case.record(receipt=_summary(receipt), elapsed_seconds=round(elapsed, 3))


def native_n4_descendants_hold_outputs(case):
    api = ObservingAPI(Win32ProcessAPI())
    root = case._recorder.root / 'native-n4'
    root.mkdir(parents=True, exist_ok=True)
    argv = _fixture_argv('chain', ready=root / 'ready', parent_hold=0.4, child_hold=4.0,
                         child_count=2, exit_code=0, child_writes='both')
    result, error, elapsed = _native_budgeted_launch(
        case, api, argv, command_id='n4-output-holders', timeout=15)
    case.check(error is None, f'unexpected error {error!r}')
    receipt = result.receipt
    case.check(receipt['output']['complete'] is True, 'spool bytes must be captured honestly')
    case.check(result.stdout.count(b'child-marker') >= 2,
               'descendant output must reach the shared spool')
    case.check(result.tree_exit_proven, 'proof after terminating output holders')
    case.check(receipt['settle']['status'] in ('EXPIRED', 'NATURAL_ZERO', 'EXECUTION_DEADLINE'),
               'settle recorded')
    case.record(receipt=_summary(receipt), stdout_bytes=len(result.stdout),
                elapsed_seconds=round(elapsed, 3))


def native_n5_nonzero_root_with_descendants(case):
    api = ObservingAPI(Win32ProcessAPI())
    root = case._recorder.root / 'native-n5'
    root.mkdir(parents=True, exist_ok=True)
    argv = _fixture_argv('chain', ready=root / 'ready', parent_hold=0.3, child_hold=5.0,
                         child_count=1, exit_code=9)
    result, error, elapsed = _native_budgeted_launch(
        case, api, argv, command_id='n5-nonzero-root', timeout=15)
    case.check(error is None, f'unexpected error {error!r}')
    case.check(result.exit_code == 9, 'original nonzero exit preserved')
    case.check(result.outcome == 'FAILED_NONZERO_EXIT', 'expected FAILED_NONZERO_EXIT')
    case.check(result.tree_exit_proven, 'failure must not prevent reclamation')
    case.check(result.receipt['termination']['requested'] is True, 'owned closeout expected')
    case.record(receipt=_summary(result.receipt), elapsed_seconds=round(elapsed, 3))


def native_n6_isolated_console_ctrl_c(case):
    api = ObservingAPI(Win32ProcessAPI())
    root = case._recorder.root / 'native-n6'
    root.mkdir(parents=True, exist_ok=True)
    runner_outcome = root / 'runner-outcome.json'
    controller = _fixture_argv('ctrlc-controller', ready=root / 'ctest-ready',
                               outcome=runner_outcome, budget=18)
    result, error, elapsed = _native_budgeted_launch(
        case, api, controller, command_id='n6-ctrlc-controller', timeout=22,
        extra_creation_flags=windows_process.CREATE_NEW_CONSOLE)
    case.check(error is None, f'controller failed: {error!r}')
    case.check(result.exit_code == 0, f'controller exit {result.exit_code}: {result.stderr!r}')
    case.check(result.tree_exit_proven, 'controller tree must be proven')
    outcome = json.loads(runner_outcome.read_text())
    case.check(outcome.get('keyboard_interrupt_observed') is True, 'real Ctrl-C observed')
    case.check(outcome.get('tree_exit_proven') is True, 'interrupted tree proven')
    case.check(outcome.get('termination_reason') == 'INTERRUPTED', 'interrupt recorded')
    case.record(controller_receipt=_summary(result.receipt), runner_outcome=outcome,
                elapsed_seconds=round(elapsed, 3))


class _AssignFailsAPI(ObservingAPI):
    def __init__(self, real):
        super().__init__(real)
        self.assign_failed = False

    def assign_process_to_job(self, job, process):
        self.assign_failed = True
        raise Win32CallFailure('AssignProcessToJobObject', 5)


def native_n7_assign_failure_injection(case):
    api = _AssignFailsAPI(Win32ProcessAPI())
    root = case._recorder.root / 'native-n7'
    root.mkdir(parents=True, exist_ok=True)
    side_effect = root / 'target-side-effect.txt'
    argv = [sys.executable, '-c', f'open({str(side_effect)!r}, "w")']
    try:
        run_owned(argv, cwd=str(root), env=dict(os.environ), timeout=10,
                  spool_root=root / 'spool', command_id='n7-assign-injection', api=api)
        raise CaseFailure('injected assign failure must reject the command')
    except OwnedCommandLaunchError as error:
        receipt = error.process_receipt
    case.check(api.assign_failed, 'injection must hit the assignment step')
    case.check(not side_effect.exists(), 'no target code may run before assignment')
    case.check(receipt['establishment']['primary_thread_resumed'] is False, 'never resumed')
    case.check(receipt['cleanup']['status'] in ('PROVEN_ZERO', 'UNPROVEN', 'UNPROVEN_WITH_ERRORS',
                                                'NOTHING_OWNED'), 'cleanup recorded')
    case.record(receipt=_summary(receipt))


def native_n8_independent_sentinel(case):
    api = Win32ProcessAPI()
    root = case._recorder.root / 'native-n8'
    root.mkdir(parents=True, exist_ok=True)
    started_marker = root / 'sentinel-started.txt'
    finished_marker = root / 'sentinel-finished.txt'
    sentinel_result = {}

    def run_sentinel():
        observing = ObservingAPI(api)
        sentinel_result['value'] = run_owned(
            _fixture_argv('sentinel', start_marker=started_marker, end_marker=finished_marker,
                          seconds=6),
            cwd=str(root), env=dict(os.environ), timeout=20,
            spool_root=root / 'sentinel-spool', command_id='n8-sentinel', api=observing)

    sentinel_thread = threading.Thread(target=run_sentinel, daemon=True)
    sentinel_thread.start()
    case.check(_wait_until(started_marker, 10), 'sentinel must start')
    target_result = {}
    target_error = {}

    def run_target():
        observing = ObservingAPI(api)
        try:
            target_result['value'] = run_owned(
                _fixture_argv('hang', ready=root / 'target-ready', seconds=10),
                cwd=str(root), env=dict(os.environ), timeout=1.5,
                spool_root=root / 'target-spool', command_id='n8-target', api=observing)
        except BaseException as error:  # original exception preserved for the record
            target_error['value'] = error

    target_thread = threading.Thread(target=run_target, daemon=True)
    target_thread.start()
    target_thread.join(15)
    case.check('value' in target_error and
               isinstance(target_error['value'], subprocess.TimeoutExpired),
               'target must time out')
    case.check(target_error['value'].process_receipt['tree_exit_proven'] is True,
               'target cleanup proven')
    sentinel_thread.join(20)
    value = sentinel_result.get('value')
    case.check(value is not None and value.outcome == 'COMPLETED',
               'sentinel in its own Job must survive the target cleanup')
    case.check(value.tree_exit_proven, 'sentinel exit proven')
    case.check(finished_marker.exists(), 'sentinel finished on its own')
    case.record(target=_summary(target_error['value'].process_receipt),
                sentinel=_summary(value.receipt))


INTEGRATION_TIMEOUT_OVERRIDE_NOTE = 'fixture-only short ctest timeout injection'


def _build_integration_fixture(root, *, interruptible):
    """Real mini checkout + substitution map for the run_native integration rows.

    Every substituted entry still runs as a REAL supervised command producing
    canned bytes; only the ctest entry is the real fixture command and is never
    replaced. The short ctest timeout injection is recorded in the receipt.
    """
    import run_windows_ci as runner
    root.mkdir(parents=True, exist_ok=True)
    source = root / 'source'
    tests = source / 'platform/native-foundation/tests'
    tools = source / 'platform/native-foundation/tools'
    (tests / 'readonly_tests.cpp').parent.mkdir(parents=True, exist_ok=True)
    (tests / 'readonly_tests.cpp').write_text('return native_test::run({\n'
        '  {"a", [] {}},\n  });\n', encoding='utf-8')
    tools.mkdir(parents=True, exist_ok=True)
    (tools / 'verify_detached.py').write_text('print("detached")\n', encoding='utf-8')
    subprocess.run(['git', 'init', '-q', str(source)], check=True, timeout=15)
    subprocess.run(['git', '-C', str(source), 'add', '-A'], check=True, timeout=15)
    subprocess.run(['git', '-C', str(source), '-c', 'user.email=selfcheck@invalid',
                    '-c', 'user.name=selfcheck', 'commit', '-qm', 'fixture'], check=True,
                   timeout=15)
    programs = root / 'program-files'
    (programs / 'Windows Kits/10/Include/10.0.26100.0/um').mkdir(parents=True, exist_ok=True)
    (programs / 'Windows Kits/10/Include/10.0.26100.0/um/Windows.h').write_text('sdk\n')
    installation = root / 'visual-studio'
    compiler = installation / 'VC/Tools/MSVC/14.44.35207/bin/Hostx64/x64/cl.exe'
    compiler.parent.mkdir(parents=True, exist_ok=True)
    compiler.write_text('synthetic pinned tool input\n')
    vswhere = root / 'vswhere.exe'
    vswhere.write_text('not executed; substituted with canned json\n')
    environment = dict(os.environ,
                       GITHUB_ACTIONS='true', GITHUB_SHA='a' * 40, GITHUB_RUN_ID='123',
                       GITHUB_RUN_ATTEMPT='1', GITHUB_JOB='native-foundation',
                       ImageOS='win22', ImageVersion='selfcheck-image',
                       TEMP=str(root / 'tmp'), TMP=str(root / 'tmp'))
    (root / 'tmp').mkdir(exist_ok=True)
    environment['ProgramFiles(x86)'] = str(programs)
    snapshot = root / 'image-environment.json'
    with patch_env(environment):
        observation = runner.image_environment('a' * 40)
    snapshot.write_text(json.dumps(observation), encoding='utf-8')
    ctest_ready = root / 'ctest-ready'
    ctest_command = _fixture_argv('hang', ready=ctest_ready, seconds=45)
    return dict(runner=runner, source=source, environment=environment, snapshot=snapshot,
                vswhere=vswhere, installation=installation, ctest_command=ctest_command,
                ctest_ready=ctest_ready, interruptible=interruptible)


class patch_env:
    def __init__(self, environment):
        self.environment = environment
        self._saved = None

    def __enter__(self):
        self._saved = dict(os.environ)
        os.environ.clear()
        os.environ.update(self.environment)

    def __exit__(self, *args):
        os.environ.clear()
        os.environ.update(self._saved)
        return False


def _integration_router(fixture, substitutions_log):
    real_run_owned = windows_process.run_owned

    def router(command, *, cwd, env, timeout, spool_root, command_id, extra_env=None, **kwargs):
        argv = [os.fspath(item) for item in command]
        note = None
        if command_id == 'git-head':
            argv = [sys.executable, '-c', "print('a' * 40)"]
            note = 'discovery substitution'
        elif command_id == 'vswhere':
            argv = [sys.executable, '-c', 'import json; print(json.dumps([{"installationPath": '
                    + repr(str(fixture['installation'])) + '}]))']
            note = 'discovery substitution'
        elif command_id == 'git-status':
            argv = [sys.executable, '-c', 'print()']
            note = 'discovery substitution'
        elif command_id == 'cmake-version':
            argv = [sys.executable, '-c', 'print("cmake version 3.27 (substituted)")']
            note = 'discovery substitution'
        elif command_id == 'configure':
            build = Path(cwd) / 'build'
            (build / 'CMakeFiles/synthetic').mkdir(parents=True, exist_ok=True)
            for name in ('CMakeCache.txt', 'native-toolchain.txt',
                         'CMakeFiles/synthetic/CMakeCXXCompiler.cmake'):
                (build / name).write_text('synthetic metadata\n')
            argv = [sys.executable, '-c', 'print("configure substituted")']
            note = 'build substitution'
        elif command_id == 'build':
            release = Path(cwd) / 'build/Release'
            release.mkdir(parents=True, exist_ok=True)
            (release / 'synthetic.exe').write_bytes(b'not executed\n')
            argv = [sys.executable, '-c', 'print("build substituted")']
            note = 'build substitution'
        elif command_id == 'ctest':
            argv = list(fixture['ctest_command'])
            timeout = 6.0 if not fixture['interruptible'] else 300.0
            note = INTEGRATION_TIMEOUT_OVERRIDE_NOTE if not fixture['interruptible'] else \
                'real fixture command (interrupt integration)'
        elif command_id == 'install':
            installed = Path(cwd) / 'installed/bin'
            installed.mkdir(parents=True, exist_ok=True)
            (installed / 'wls-native-foundation.exe').write_bytes(b'not executed\n')
            argv = [sys.executable, '-c', 'print("install substituted")']
            note = 'build substitution'
        elif command_id == 'host-capabilities':
            argv = [sys.executable, '-c',
                    'print(\'{"token_elevation":"not_elevated","filesystem":"NTFS"}\')']
            note = 'discovery substitution'
        elif command_id == 'detached':
            argv = [sys.executable, '-c', 'print("detached substituted")']
            note = 'build substitution'
        elif command_id.startswith('source-snapshot'):
            pass  # real git in the real fixture repository
        substitutions_log.append({'command_id': command_id, 'note': note,
                                  'timeout': timeout, 'argv': argv})
        result = real_run_owned(argv, cwd=cwd, env=env, timeout=timeout,
                                spool_root=spool_root, command_id=command_id,
                                extra_env=extra_env, **kwargs)
        if note is not None and isinstance(result.receipt, dict):
            result.receipt['integration_substitution'] = note
        return result

    return router


def _run_integration(fixture):
    runner = fixture['runner']
    substitutions = []
    router = _integration_router(fixture, substitutions)
    work = fixture['source'].parent / 'work'
    original = windows_process.run_owned
    windows_process.run_owned = router
    runner.windows_process.run_owned = router
    started = time.monotonic()
    error = None
    try:
        with patch_env(fixture['environment']):
            runner.run_native(fixture['source'], work, image_snapshot=fixture['snapshot'])
    except BaseException as failure:  # the original exception is the evidence
        error = failure
    finally:
        windows_process.run_owned = original
        runner.windows_process.run_owned = original
    return work, error, substitutions, time.monotonic() - started


def native_n9_run_native_timeout_integration(case):
    fixture = _build_integration_fixture(case._recorder.root / 'native-n9',
                                         interruptible=False)
    work, error, substitutions, elapsed = _run_integration(fixture)
    case.check(isinstance(error, subprocess.TimeoutExpired),
               f'run_native must surface the original TimeoutExpired, got {error!r}')
    evidence = work / 'evidence'
    failure = json.loads((evidence / 'failure.json').read_text())
    case.check(failure['termination'] == 'TIMEOUT', 'failure.json TIMEOUT')
    case.check(failure['owned_process_supervision']['all_owned_process_trees_exited'] is True,
               'every started owned tree must be proven')
    ctest_receipt = json.loads((evidence / 'ctest.process.json').read_text())
    case.check(ctest_receipt['tree_exit_proven'] is True, 'ctest tree proven')
    case.check(ctest_receipt.get('integration_substitution') == INTEGRATION_TIMEOUT_OVERRIDE_NOTE,
               'short-timeout injection must be recorded, never a product deadline change')
    after = json.loads((evidence / 'identity-finalization.json').read_text())
    case.check(after['after_snapshots_available'] is True,
               'proven cleanup must still allow after snapshots')
    case.check(after['identities_complete'] is True, 'identity pairs complete')
    # Cleanup proof strictly precedes the stable after snapshot claim.
    case.check(ctest_receipt['ended_at_utc'] <= after['captured_at_utc'],
               'cleanup proof must precede the after snapshot')
    case.check(not (evidence / 'result.json').exists(), 'no PASSED result after a timeout')
    case.record(failure_termination=failure['termination'],
                substitutions=[row for row in substitutions if row['note']],
                elapsed_seconds=round(elapsed, 3))


def native_n10_run_native_interrupt_integration(case):
    fixture_root = case._recorder.root / 'native-n10'
    fixture_root.mkdir(parents=True, exist_ok=True)
    outcome_path = fixture_root / 'controller-outcome.json'
    controller = _fixture_argv('integration-controller', root=fixture_root,
                               outcome=outcome_path)
    api = ObservingAPI(Win32ProcessAPI())
    result, error, elapsed = _native_budgeted_launch(
        case, api, controller, command_id='n10-controller', timeout=23,
        extra_creation_flags=windows_process.CREATE_NEW_CONSOLE, cwd=str(fixture_root))
    case.check(error is None, f'controller failed: {error!r}')
    case.check(result.exit_code == 0, f'controller exit {result.exit_code}: {result.stderr!r}')
    observation = json.loads(outcome_path.read_text())
    outcome = observation.get('runner') or {}
    case.check(observation.get('console_event', {}).get('sent') is True,
               'a real CTRL_C_EVENT must be sent inside the isolated console')
    case.check(outcome.get('interrupted') is True, 'real Ctrl-C must reach run_native')
    case.check(outcome.get('failure_termination') == 'INTERRUPTED', 'failure.json INTERRUPTED')
    case.check(outcome.get('ctest_command_status') == 'INTERRUPTED', 'ctest status recorded')
    case.check(outcome.get('all_owned_process_trees_exited') is True, 'all trees proven')
    case.check(outcome.get('cleanup_before_after_snapshot') is True,
               'cleanup proof must precede the after snapshot')
    case.check(outcome.get('result_json_absent') is True, 'no PASSED result after interrupt')
    case.record(controller_receipt=_summary(result.receipt), observation=observation,
                elapsed_seconds=round(elapsed, 3))


NATIVE_CASES = [
    ('n1-normal-parent-child', native_n1_normal_parent_child),
    ('n2-root-exit-descendant-alive', native_n2_root_exit_descendant_alive),
    ('n3-real-timeout', native_n3_real_timeout),
    ('n4-descendants-hold-outputs', native_n4_descendants_hold_outputs),
    ('n5-nonzero-root-descendants', native_n5_nonzero_root_with_descendants),
    ('n6-isolated-console-ctrl-c', native_n6_isolated_console_ctrl_c),
    ('n7-assign-failure-injection', native_n7_assign_failure_injection),
    ('n8-independent-sentinel', native_n8_independent_sentinel),
    ('n9-run-native-timeout', native_n9_run_native_timeout_integration),
    ('n10-run-native-interrupt', native_n10_run_native_interrupt_integration),
]


# --------------------------------------------------------------------------------------
# Fixture subprocess entry points (offline, self-created processes only)
# --------------------------------------------------------------------------------------

def _fixture_chain(options):
    ready = Path(options['ready'])
    child_count = int(options.get('child_count', 1))
    child_hold = float(options.get('child_hold', 2))
    parent_hold = float(options.get('parent_hold', 1))
    exit_code = int(options.get('exit_code', 0))
    survivor_marker = options.get('survivor_marker')
    child_writes = options.get('child_writes')
    children = []
    for index in range(child_count):
        argv = [sys.executable, '-B', str(Path(__file__).resolve()), '--fixture', 'leaf',
                '--hold', str(child_hold), '--index', str(index),
                '--survivor-marker', str(survivor_marker or ''),
                '--child-writes', str(child_writes or '')]
        children.append(subprocess.Popen(argv))
    ready.write_text('ready', encoding='utf-8')
    print('parent-marker', flush=True)
    time.sleep(parent_hold)
    for child in children:
        child.poll()
    raise SystemExit(exit_code)


def _fixture_leaf(options):
    hold = float(options.get('hold', 1))
    index = options.get('index', '0')
    survivor_marker = options.get('survivor_marker')
    child_writes = options.get('child_writes')
    print(f'child-marker {index}', flush=True)
    if child_writes:
        print(f'child-stderr-marker {index}', file=sys.stderr, flush=True)
        time.sleep(min(hold, 0.8))
        print(f'child-marker {index} still writing', flush=True)
    time.sleep(hold)
    if survivor_marker:
        Path(survivor_marker).write_text('survived', encoding='utf-8')


def _fixture_hang(options):
    Path(options['ready']).write_text('ready', encoding='utf-8')
    time.sleep(float(options.get('seconds', 30)))


def _fixture_sentinel(options):
    Path(options['start_marker']).write_text('started', encoding='utf-8')
    time.sleep(float(options.get('seconds', 6)))
    Path(options['end_marker']).write_text('finished', encoding='utf-8')


def _generate_console_ctrl_event():
    kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel32.GenerateConsoleCtrlEvent.argtypes = [ctypes.c_uint32, ctypes.c_uint32]
    kernel32.GenerateConsoleCtrlEvent.restype = ctypes.c_int
    CTRL_C_EVENT = 0
    return bool(kernel32.GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0))


def _fixture_ctrlc_controller(options):
    # Shares one NEW console with the runner; ignores CTRL_C itself and sends
    # CTRL_C_EVENT only inside that console, after the inner hang fixture
    # reports ready and while the runner is still inside its supervised wait.
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    root = Path(options['root']) if options.get('root') else Path(options['ready']).parent
    outcome_path = Path(options['outcome'])
    ready = Path(options['ready'])
    runner_argv = [sys.executable, '-B', str(Path(__file__).resolve()), '--fixture',
                   'supervised-runner', '--ready', str(ready), '--outcome', str(outcome_path)]
    event = {}

    def fire_when_ready():
        if _wait_until(ready, 14):
            time.sleep(0.4)
            event['sent'] = _generate_console_ctrl_event()
        else:
            event['never_ready'] = True

    sender = threading.Thread(target=fire_when_ready, daemon=True)
    sender.start()
    observation = {}
    try:
        result = run_owned(runner_argv, cwd=str(root), env=dict(os.environ), timeout=18,
                           spool_root=root / 'runner-spool', command_id='ctrlc-runner')
        observation['controller_error'] = None
        observation['runner_exit_code'] = result.exit_code
        observation['runner_tree_exit_proven'] = result.tree_exit_proven
    except BaseException as error:
        observation['controller_error'] = f'{type(error).__name__}: {error}'
    sender.join(5)
    observation['console_event'] = event
    if event.get('never_ready'):
        observation['controller_error'] = observation.get('controller_error') or \
            'inner fixture never became ready'
    elif event.get('sent') is False:
        observation['controller_error'] = observation.get('controller_error') or \
            'GenerateConsoleCtrlEvent failed'
    outcome_path.write_text(json.dumps(observation, indent=2) + '\n', encoding='utf-8')
    return 0 if observation.get('controller_error') is None and \
        observation.get('runner_exit_code') == 0 else 1


def _fixture_supervised_runner(options):
    ready = Path(options['ready'])
    outcome = {'keyboard_interrupt_observed': False}
    try:
        run_owned([sys.executable, '-B', str(Path(__file__).resolve()), '--fixture', 'hang',
                   '--ready', str(ready), '--seconds', '30'],
                  cwd=str(ready.parent), env=dict(os.environ), timeout=25,
                  spool_root=ready.parent / 'target-spool', command_id='ctrlc-target')
        outcome['unexpected_completion'] = True
    except KeyboardInterrupt as error:
        receipt = error.process_receipt
        outcome['keyboard_interrupt_observed'] = True
        outcome['tree_exit_proven'] = receipt.get('tree_exit_proven')
        outcome['termination_reason'] = (receipt.get('termination') or {}).get('reason')
        outcome['cleanup_status'] = (receipt.get('cleanup') or {}).get('status')
        outcome['second_interrupt'] = (receipt.get('cleanup') or {}).get(
            'second_interrupt_recorded_and_deferred')
    Path(options['outcome']).write_text(json.dumps(outcome, indent=2) + '\n', encoding='utf-8')
    return 0 if outcome.get('keyboard_interrupt_observed') else 2


def _fixture_integration_runner(options):
    # Standard SIGINT handling: the real Ctrl-C must arrive as a
    # KeyboardInterrupt inside the supervised wait of the run_native pipeline.
    root = Path(options['root'])
    outcome_path = Path(options['outcome'])
    fixture = _build_integration_fixture(root, interruptible=True)
    work, error, substitutions, elapsed = _run_integration(fixture)
    outcome = {'interrupted': isinstance(error, KeyboardInterrupt),
               'error_type': type(error).__name__ if error else None}
    evidence = work / 'evidence'
    try:
        failure = json.loads((evidence / 'failure.json').read_text())
        outcome['failure_termination'] = failure['termination']
        outcome['all_owned_process_trees_exited'] = \
            failure['owned_process_supervision']['all_owned_process_trees_exited']
        ctest_receipt = json.loads((evidence / 'ctest.process.json').read_text())
        identity = json.loads((evidence / 'identity-finalization.json').read_text())
        outcome['cleanup_before_after_snapshot'] = \
            ctest_receipt['ended_at_utc'] <= identity['captured_at_utc'] and \
            identity['after_snapshots_available'] is True
        outcome['ctest_command_status'] = json.loads(
            (evidence / 'ctest.command.json').read_text()).get('status')
        outcome['result_json_absent'] = not (evidence / 'result.json').exists()
    except (OSError, ValueError, KeyError) as record_error:
        outcome['record_error'] = f'{type(record_error).__name__}: {record_error}'
    outcome_path.write_text(json.dumps(outcome, indent=2) + '\n', encoding='utf-8')
    return 0 if (outcome['interrupted'] and outcome.get('failure_termination') == 'INTERRUPTED'
                 and outcome.get('all_owned_process_trees_exited')
                 and outcome.get('result_json_absent')
                 and outcome.get('cleanup_before_after_snapshot')) else 1


def _fixture_integration_controller(options):
    signal.signal(signal.SIGINT, signal.SIG_IGN)
    root = Path(options['root'])
    outcome_path = Path(options['outcome'])
    runner_outcome = root / 'integration-runner-outcome.json'
    ctest_ready = root / 'integration-fixture' / 'ctest-ready'
    runner_argv = [sys.executable, '-B', str(Path(__file__).resolve()), '--fixture',
                   'integration-runner', '--root', str(root / 'integration-fixture'),
                   '--outcome', str(runner_outcome)]
    event = {}

    def fire_when_ready():
        if _wait_until(ctest_ready, 16):
            time.sleep(0.4)
            event['sent'] = _generate_console_ctrl_event()
        else:
            event['never_ready'] = True

    sender = threading.Thread(target=fire_when_ready, daemon=True)
    sender.start()
    observation = {}
    try:
        result = run_owned(runner_argv, cwd=str(root), env=dict(os.environ), timeout=21,
                           spool_root=root / 'runner-spool', command_id='integration-runner')
        observation['controller_error'] = None
        observation['runner_exit_code'] = result.exit_code
        observation['runner_tree_exit_proven'] = result.tree_exit_proven
    except BaseException as error:
        observation['controller_error'] = f'{type(error).__name__}: {error}'
    sender.join(5)
    observation['console_event'] = event
    if runner_outcome.is_file():
        observation['runner'] = json.loads(runner_outcome.read_text())
    else:
        observation['controller_error'] = observation.get('controller_error') or \
            'runner outcome missing'
    outcome_path.write_text(json.dumps(observation, indent=2) + '\n', encoding='utf-8')
    return 0 if observation.get('controller_error') is None and \
        observation.get('runner_exit_code') == 0 else 1


FIXTURES = {
    'chain': _fixture_chain,
    'leaf': _fixture_leaf,
    'hang': _fixture_hang,
    'sentinel': _fixture_sentinel,
    'ctrlc-controller': _fixture_ctrlc_controller,
    'supervised-runner': _fixture_supervised_runner,
    'integration-runner': _fixture_integration_runner,
    'integration-controller': _fixture_integration_controller,
}


# --------------------------------------------------------------------------------------
# Entry points
# --------------------------------------------------------------------------------------

def parse_args(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--mode', choices=('synthetic', 'native'), required=True)
    parser.add_argument('--evidence-dir', type=Path, required=True)
    parser.add_argument('--fixture')
    options, rest = parser.parse_known_args(argv)
    values = {}
    rest = [item for item in rest if item != '--']
    for index in range(0, len(rest) - 1, 2):
        key = rest[index].lstrip('-').replace('-', '_')
        values[key] = rest[index + 1]
    return options, values


def write_summary(recorder, mode, status, extra=None):
    summary = {'schema': EVIDENCE_SCHEMA, 'mode': mode, 'status': status,
               'captured_at_utc': utc_now(),
               'cases': {entry['case']: entry['status'] for entry in recorder.cases},
               'case_count': len(recorder.cases),
               'failed': [entry['case'] for entry in recorder.cases if entry['status'] != 'PASS']}
    if extra:
        summary.update(extra)
    (recorder.root / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n',
                                                encoding='utf-8')
    print(json.dumps(summary, sort_keys=True))
    return summary


def main():
    options, fixture_options = parse_args(sys.argv[1:])
    if options.fixture:
        raise SystemExit(FIXTURES[options.fixture](fixture_options))
    recorder = Recorder(options.evidence_dir)
    recorder.begin(options.evidence_dir)
    overall_started = time.monotonic()
    if options.mode == 'synthetic':
        run_cases(SYNTHETIC_CASES, recorder, 'synthetic', budget_seconds=55.0)
        ok = all(entry['status'] == 'PASS' for entry in recorder.cases)
        write_summary(recorder, 'synthetic', 'PASS' if ok else 'FAIL')
        return 0 if ok else 1

    if os.name != 'nt':
        write_summary(recorder, 'native', 'NOT_RUN', extra={
            'reason': 'NATIVE_WINDOWS_REQUIRED',
            'exit_code_expected': 3,
            'note': 'CI must treat native selfcheck NOT_RUN as a failure, not a skip'})
        return 3

    startup_started = time.monotonic()
    startup = recorder.case('startup-abi-nesting', 'native')
    try:
        native_startup_checks(startup)
        startup.finish('PASS')
    except BaseException as error:
        startup.finish('FAIL', error)
        write_summary(recorder, 'native', 'FAIL')
        return 1
    startup_elapsed = time.monotonic() - startup_started
    for name, function in NATIVE_CASES:
        if time.monotonic() - overall_started > NATIVE_INTERNAL_TOTAL_SECONDS:
            recorder.case(name, 'native').finish('FAIL', BudgetExceeded(
                'native internal total budget exhausted'))
            continue
        writer = recorder.case(name, 'native')
        scenario_started = time.monotonic()
        try:
            function(writer)
            elapsed = time.monotonic() - scenario_started
            if elapsed > SCENARIO_BUDGET_SECONDS:
                raise BudgetExceeded(f'{name} took {elapsed:.1f}s > {SCENARIO_BUDGET_SECONDS}s')
            writer.finish('PASS')
        except BaseException as error:
            writer.finish('FAIL', error)
    finalize_started = time.monotonic()
    ok = all(entry['status'] == 'PASS' for entry in recorder.cases)
    summary = write_summary(recorder, 'native', 'PASS' if ok else 'FAIL', extra={
        'startup_elapsed_seconds': round(startup_elapsed, 3),
        'finalize_elapsed_seconds': round(time.monotonic() - finalize_started, 3),
        'internal_total_budget_seconds': NATIVE_INTERNAL_TOTAL_SECONDS,
        'scenario_budget_seconds': SCENARIO_BUDGET_SECONDS})
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main())
