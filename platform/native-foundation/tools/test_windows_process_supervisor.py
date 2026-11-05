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
import hashlib
import json
import os
from pathlib import Path
import signal
import struct
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
        if not root.is_absolute():
            # Anchor the evidence root once to the current cwd so every marker
            # and outcome path derived from it survives child-side cwd changes.
            # No resolve/realpath and no normalization: the anchored path keeps
            # its own separators and any '..' components as given.
            root = Path.cwd() / root
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


# --------------------------------------------------------------------------------------
# Synthetic ABI regressions: production declarations and constructor bindings
# --------------------------------------------------------------------------------------
#
# The cases above drive the supervisor state machine through FakeWin32API and
# never reach the real Win32 declarations or the real ``Win32ProcessAPI``
# constructor; the platform-refusal case stops at the constructor's ``os.name``
# guard. They therefore cannot detect declaration/binding defects such as a
# missing ``ctypes.c_ulongptr`` carrier. The three cases below keep those
# fakes untouched and check the production ABI itself against an independent
# SDK byte oracle. They are in-memory only: no process is created, terminated
# or queried for real, and no Win32 function is executed.


class _NtPlatformShim:
    """Delegates to the real ``os`` module; only ``name`` reports Windows."""

    def __init__(self, real):
        object.__setattr__(self, '_real', real)

    def __getattr__(self, item):
        return getattr(object.__getattribute__(self, '_real'), item)

    @property
    def name(self):
        return 'nt'


class _ProbeDllFunction:
    """Accepts only argtypes/restype assignment; any call fails the case."""

    def __init__(self, name, attempted_calls):
        self.name = name
        self.argtypes = None
        self.restype = None
        self._attempted_calls = attempted_calls

    def __call__(self, *args, **kwargs):
        self._attempted_calls.append(self.name)
        raise CaseFailure(f'construction must not call the Win32 {self.name} function')


class _ProbeWinDLL:
    """Kernel32 loader replacement: hands out bindable names, executes none."""

    def __init__(self, attempted_calls):
        self.__dict__['attempted_calls'] = attempted_calls
        self.__dict__['functions'] = {}

    def __getattr__(self, name):
        functions = self.__dict__['functions']
        if name not in functions:
            functions[name] = _ProbeDllFunction(name, self.__dict__['attempted_calls'])
        return functions[name]


class _CtypesLoaderShim:
    """Delegates to the real ctypes module except the WinDLL loader and the
    Windows-only last-error source: Linux ctypes provides no
    ``get_last_error``, so the probe supplies an inert zero source that the
    constructor only stores (it is read solely through recorded failures)."""

    def __init__(self, real, loader, last_error):
        object.__setattr__(self, '_real', real)
        object.__setattr__(self, '_loader', loader)
        object.__setattr__(self, '_last_error', last_error)

    def __getattr__(self, item):
        if item == 'WinDLL':
            return object.__getattribute__(self, '_loader')
        if item == 'get_last_error':
            return object.__getattribute__(self, '_last_error')
        return getattr(object.__getattribute__(self, '_real'), item)


class patched_win32_constructor:
    """Patch ONLY the platform identification, the kernel32 DLL loader and the
    last-error source for the real ``Win32ProcessAPI.__init__``; everything
    restores on exit. The production constructor, declarations and bindings
    run unmodified; ``ctypes`` itself is never monkey patched and FakeWin32API
    is never substituted for the constructor under test."""

    def __enter__(self):
        self._saved_os = windows_process.os
        self._saved_ctypes = windows_process.ctypes
        self.attempted_calls = []
        self.loader_state = {'names': [], 'use_last_error': []}
        self.dlls = []
        self.last_error_source_reads = 0

        def last_error():
            self.last_error_source_reads += 1
            return 0

        def loader(name, use_last_error=False):
            self.loader_state['names'].append(name)
            self.loader_state['use_last_error'].append(bool(use_last_error))
            dll = _ProbeWinDLL(self.attempted_calls)
            self.dlls.append(dll)
            return dll

        windows_process.os = _NtPlatformShim(self._saved_os)
        windows_process.ctypes = _CtypesLoaderShim(self._saved_ctypes, loader, last_error)
        return self

    def __exit__(self, *args):
        windows_process.os = self._saved_os
        windows_process.ctypes = self._saved_ctypes
        return False


def synthetic_abi_declaration_sdk_layout(case):
    # The expected bytes come from an independent struct.pack oracle in the
    # official SDK order, never from the production _fields_.
    structures = windows_process._declare_structures()
    pointer_width = ctypes.sizeof(ctypes.c_void_p)
    for alias in ('ULONG_PTR', 'DWORD_PTR'):
        carrier = getattr(windows_process, alias, None)
        case.check(carrier is not None, f'{alias} must be declared as a real carrier')
        case.check(ctypes.sizeof(carrier) == pointer_width,
                   f'{alias} must be pointer-width')
        high_value = (1 << (pointer_width * 8 - 1)) | 0x3210
        case.check(carrier(high_value).value == high_value,
                   f'{alias} must be unsigned and keep high-bit values')
    limit_fields = dict(structures['JOBOBJECT_BASIC_LIMIT_INFORMATION']._fields_)
    case.check(ctypes.sizeof(limit_fields['Affinity']) == pointer_width,
               'Affinity must use the pointer-width ULONG_PTR carrier')
    extended = structures['JOBOBJECT_EXTENDED_LIMIT_INFORMATION']()
    high_affinity = (1 << (pointer_width * 8 - 1)) | 0xFEDC
    extended.BasicLimitInformation.Affinity = high_affinity
    case.check(int(extended.BasicLimitInformation.Affinity) == high_affinity,
               'a high-bit Affinity must round-trip without truncation')
    accounting = structures['JOBOBJECT_BASIC_ACCOUNTING_INFORMATION']
    # Literal official SDK order (TotalUserTime, TotalKernelTime, then the two
    # ThisPeriod values, then the four DWORD counters), written here from the
    # published declaration — never reconstructed from the production _fields_.
    official_accounting_fields = [
        'TotalUserTime', 'TotalKernelTime',
        'ThisPeriodTotalUserTime', 'ThisPeriodTotalKernelTime',
        'TotalPageFaultCount', 'TotalProcesses', 'ActiveProcesses',
        'TotalTerminatedProcesses']
    declared_accounting_fields = [name for name, _ in accounting._fields_]
    case.check(declared_accounting_fields == official_accounting_fields,
               'accounting must declare exactly the official SDK fields in order; '
               f'expected {official_accounting_fields}, '
               f'actual {declared_accounting_fields}')
    payload = struct.pack('<qqqqIIII', 111, 222, 333, 444, 777, 9, 2, 7)
    case.check(len(payload) == 48, 'the SDK accounting oracle must be exactly 48 bytes')
    decoded = accounting.from_buffer_copy(payload)
    case.check(int(decoded.TotalUserTime) == 111, 'SDK oracle total user time')
    case.check(int(decoded.TotalKernelTime) == 222, 'SDK oracle total kernel time')
    case.check(int(decoded.ThisPeriodTotalUserTime) == 333,
               'SDK oracle this-period user time')
    case.check(int(decoded.ThisPeriodTotalKernelTime) == 444,
               'SDK oracle this-period kernel time')
    case.check(int(decoded.TotalPageFaultCount) == 777, 'SDK oracle page faults')
    case.check(int(decoded.TotalProcesses) == 9, 'SDK oracle total processes')
    case.check(int(decoded.ActiveProcesses) == 2, 'SDK oracle active processes')
    case.check(int(decoded.TotalTerminatedProcesses) == 7, 'SDK oracle terminated processes')
    pid_list = structures['JOBOBJECT_BASIC_PROCESS_IDLIST']
    case.check([name for name, _ in pid_list._fields_] == [
        'NumberOfAssignedProcesses', 'NumberOfProcessIdsInList', 'ProcessIdList'],
        'the PID list must declare the official two-DWORD header')
    if pointer_width == 8:
        case.check(ctypes.sizeof(accounting) == 48,
                   f'x64 accounting size {ctypes.sizeof(accounting)} != 48')
        for field, expected_offset in (('TotalUserTime', 0), ('TotalKernelTime', 8),
                                       ('ThisPeriodTotalUserTime', 16),
                                       ('ThisPeriodTotalKernelTime', 24),
                                       ('TotalPageFaultCount', 32), ('TotalProcesses', 36),
                                       ('ActiveProcesses', 40), ('TotalTerminatedProcesses', 44)):
            actual_offset = getattr(accounting, field).offset
            case.check(actual_offset == expected_offset,
                       f'{field} offset {actual_offset} != {expected_offset}')
        case.check(getattr(pid_list, 'NumberOfAssignedProcesses').offset == 0,
                   'assigned count must live at offset 0')
        case.check(getattr(pid_list, 'NumberOfProcessIdsInList').offset == 4,
                   'returned count must live at offset 4')
        case.check(getattr(pid_list, 'ProcessIdList').offset == 8,
                   'the PID array must start at offset 8')
        for name, expected_size in windows_process.X64_STRUCTURE_SIZES.items():
            actual_size = ctypes.sizeof(structures[name])
            case.check(actual_size == expected_size,
                       f'{name} size {actual_size} != {expected_size}')
    case.record(pointer_width=pointer_width,
                accounting_size=ctypes.sizeof(accounting),
                pid_list_size=ctypes.sizeof(pid_list))


def synthetic_abi_constructor_binding_smoke(case):
    # Only the platform identification and the kernel32 loader are replaced;
    # the real Win32ProcessAPI.__init__ runs unmodified and any Win32 function
    # call during construction fails the case.
    with patched_win32_constructor() as probe:
        api = Win32ProcessAPI()
    case.check(probe.attempted_calls == [],
               f'construction must not call Win32 functions, got {probe.attempted_calls}')
    case.check(probe.loader_state['names'] == ['kernel32'],
               f"only the kernel32 loader may be requested, got {probe.loader_state['names']}")
    case.check(probe.loader_state['use_last_error'] == [True],
               'the kernel32 loader must request use_last_error')
    case.check(len(probe.dlls) == 1 and len(probe.dlls[0].functions) >= 19,
               'the constructor must bind the full declared function set')
    pointer_width = ctypes.sizeof(ctypes.c_void_p)
    for name in ('CreateJobObjectW', 'QueryInformationJobObject',
                 'UpdateProcThreadAttribute', 'TerminateJobObject'):
        function = getattr(api, name)
        case.check(function.restype is not None, f'{name} must declare a restype')
        case.check(function.argtypes, f'{name} must declare argtypes')
    # UpdateProcThreadAttribute(lpAttributeList, dwFlags, Attribute, lpValue,
    # cbSize, lpPreviousValue, lpReturnSize): Attribute is DWORD_PTR and
    # cbSize is SIZE_T — both pointer-width unsigned with high bits kept.
    attribute_type = api.UpdateProcThreadAttribute.argtypes[2]
    size_type = api.UpdateProcThreadAttribute.argtypes[4]
    for label, carrier in (('Attribute(DWORD_PTR)', attribute_type),
                           ('cbSize(SIZE_T)', size_type)):
        case.check(ctypes.sizeof(carrier) == pointer_width,
                   f'UpdateProcThreadAttribute {label} must be pointer-width')
        high_value = (1 << (pointer_width * 8 - 1)) | 0x5678
        case.check(carrier(high_value).value == high_value,
                   f'UpdateProcThreadAttribute {label} must keep high-bit values')
    case.check(ctypes.sizeof(api.CreateJobObjectW.restype) == pointer_width,
               'HANDLE returns must stay pointer-width')
    case.check(ctypes.sizeof(api.TerminateJobObject.argtypes[1]) == 4,
               'DWORD parameters must stay 4 bytes')
    case.record(bound_functions=sorted(probe.dlls[0].functions),
                attempted_win32_calls=probe.attempted_calls,
                last_error_source_reads=probe.last_error_source_reads)


def synthetic_abi_pidlist_returned_count(case):
    # The test QueryInformationJobObject writes ONLY the specified bytes into
    # the production buffer; the real job_process_ids must report the copied
    # count (NumberOfProcessIdsInList), never the assigned count, and never
    # padding or unwritten array slots. No real PID is created or terminated.
    pointer_width = ctypes.sizeof(ctypes.c_void_p)
    entry_format = 'Q' if pointer_width == 8 else 'I'
    if pointer_width == 8:
        pid_first, pid_second = 0x0000ABCD12345678, 0x0F0E0D0C0B0A0908
    else:
        pid_first, pid_second = 0x12345678, 0x0B0A0908
    copied_payload = struct.pack('<II', 5, 2) + struct.pack('<2' + entry_format,
                                                            pid_first, pid_second)
    with patched_win32_constructor() as probe:
        api = Win32ProcessAPI()
    case.check(probe.attempted_calls == [], 'no Win32 call may happen in construction')

    queries = []

    def install_query(payload, result):
        def query(job, information_class, buffer, size, return_length):
            queries.append({'information_class': int(information_class),
                            'size': int(size)})
            if result == 0:
                return 0
            ctypes.memmove(buffer, payload, len(payload))
            return 1
        api.QueryInformationJobObject = query

    expected_buffer_size = 8 + 64 * pointer_width
    install_query(copied_payload, 1)
    listed = api.job_process_ids(object())
    case.check(listed == [pid_first, pid_second],
               f'only the two copied IDs may be returned, got {listed!r}')
    case.check(len(queries) == 1 and
               queries[0]['information_class'] == windows_process.JobObjectBasicProcessIdList,
               'the PID list query class must be JobObjectBasicProcessIdList')
    case.check(queries[0]['size'] == expected_buffer_size,
               f"the fixed 64-entry buffer must be {expected_buffer_size} bytes, "
               f"got {queries[0]['size']}")
    # A copied count beyond the fixed buffer must fail explicitly, never
    # truncate into a complete-looking list.
    install_query(struct.pack('<II', 70, 70), 1)
    try:
        api.job_process_ids(object())
        raise CaseFailure('a copied count over the fixed buffer must be rejected')
    except WindowsProcessError as error:
        over_limit_reason = str(error)[:512]
    # The original query-failure behaviour is preserved.
    install_query(b'', 0)
    try:
        api.job_process_ids(object())
        raise CaseFailure('a failed PID list query must still fail')
    except WindowsProcessError as error:
        query_failure_type = type(error).__name__
    case.check(query_failure_type == 'Win32CallFailure',
               f'a failed query must raise Win32CallFailure, got {query_failure_type}')
    case.record(copied_ids=[hex(pid_first), hex(pid_second)], assigned_in_oracle=5,
                copied_in_oracle=2, over_limit_reason=over_limit_reason,
                query_failure_type=query_failure_type, queries=len(queries))


# --------------------------------------------------------------------------------------
# Synthetic ABI regression: explicit inheritable-handle list at the real creation boundary
# --------------------------------------------------------------------------------------
#
# The case below runs the REAL production Win32ProcessAPI.create_process_suspended
# method against a fixture that replaces, on its own API instance only, the low-level
# entries that method itself calls (Initialize/Update/Delete attribute list,
# CreateProcessW, GetProcessTimes) plus a programmable last-error source. It is a
# local contract fixture at the real method boundary: no Windows kernel call is
# made, no process is created and nothing here reproduces native Win32 error
# behaviour. The key oracle is the fixed SDK declaration, never the production
# constant under test.

# Pinned SDK WinBase.h: ProcThreadAttributeValue(ProcThreadAttributeHandleList = 2,
# FALSE, TRUE, FALSE) — the PROC_THREAD_ATTRIBUTE_INPUT bit OR the HandleList
# attribute number. The bare INPUT bit 0x00020000 is the ParentProcess key.
SDK_PROC_THREAD_ATTRIBUTE_INPUT = 0x00020000
SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST_NUMBER = 2
SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST = (
    SDK_PROC_THREAD_ATTRIBUTE_INPUT | SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST_NUMBER)
WIN32_ERROR_BAD_LENGTH = 24
CREATE_BREAKAWAY_FROM_JOB = 0x01000000


class _StageLastError:
    """Fixture-local GetLastError source for the handle-list contract case.

    Every synthetic stage programs the value the production failure path must
    observe next: 122 after the Initialize NULL capacity probe, 24 for the
    explicit FAULT_INJECTION Update failure, 0 for the other synthetic stages.
    It is installed on this case's own API instance only; the constructor
    smoke's constant-zero source and any native last-error stay untouched."""

    def __init__(self):
        self.value = 0
        self.reads = 0

    def __call__(self):
        self.reads += 1
        return self.value


class _HandleListContractFixture:
    """Programmable low-level boundary for the real create_process_suspended.

    The three stdio values are synthetic non-zero handle integers; they are
    never used as real process or system handle identities. Every replaced
    entry validates the contract at the exact point the production method
    exercises it and raises CaseFailure with expected/actual values on any
    mismatch, so neither defect can hide behind the other."""

    REPLACED = ('InitializeProcThreadAttributeList', 'UpdateProcThreadAttribute',
                'DeleteProcThreadAttributeList', 'CreateProcessW', 'GetProcessTimes',
                '_last_error')

    def __init__(self, api, case, stdin_value, stdout_value, stderr_value):
        self.api = api
        self.case = case
        self.stdin_value = stdin_value
        self.stdout_value = stdout_value
        self.stderr_value = stderr_value
        self.last_error = _StageLastError()
        self.calls = []
        self.probe_capacity = None
        self.initialized_address = None
        self.update = None
        self.inject_update_failure = False
        self.delete_count = 0
        self.created = None
        self._saved = None

    def capture(self):
        self._saved = {name: getattr(self.api, name) for name in self.REPLACED}
        return self._saved

    def restore(self):
        for name, value in (self._saved or {}).items():
            setattr(self.api, name, value)

    def install(self):
        api, case = self.api, self.case
        pointer_width = ctypes.sizeof(ctypes.c_void_p)

        def initialize(attribute_list, count, flags, size):
            if attribute_list is None:
                self.calls.append('Initialize(NULL-probe)')
                self.last_error.value = 122  # ERROR_INSUFFICIENT_BUFFER contract
                case.check(int(count) == 1 and int(flags) == 0,
                           f'Initialize NULL probe must use count=1 flags=0, '
                           f'got count={count} flags={flags}')
                capacity = ctypes.cast(size, ctypes.POINTER(ctypes.c_size_t))
                capacity.contents.value = 4 * pointer_width
                self.probe_capacity = capacity.contents.value
                return 0
            self.calls.append('Initialize')
            self.last_error.value = 0
            case.check(int(count) == 1 and int(flags) == 0,
                       f'Initialize must use count=1 flags=0, '
                       f'got count={count} flags={flags}')
            address = ctypes.addressof(attribute_list)
            case.check(self.initialized_address is None,
                       'only one real list initialization is expected')
            case.check(self.probe_capacity is not None and
                       self.probe_capacity >= pointer_width,
                       'the real initialization must follow the NULL capacity probe')
            self.initialized_address = address
            return 1

        def update(attribute_list, flags, attribute, value, size_bytes, previous,
                   return_size):
            self.calls.append('Update')
            address = ctypes.addressof(attribute_list)
            case.check(address == self.initialized_address,
                       f'Update must target the initialized list '
                       f'{self.initialized_address:#x}, got {address:#x}')
            record = {'attribute_key': int(attribute), 'flags': int(flags),
                      'handles': [int(item) if item is not None else None
                                  for item in value],
                      'cb_bytes': int(size_bytes),
                      'previous_value': previous, 'return_size': return_size}
            self.update = record
            if self.inject_update_failure:
                self.last_error.value = WIN32_ERROR_BAD_LENGTH  # FAULT_INJECTION only
                return 0
            self.last_error.value = 0
            case.check(record['attribute_key'] == SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                       f'UpdateProcThreadAttribute attribute key must be '
                       f'{SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST:#x} (SDK '
                       f'PROC_THREAD_ATTRIBUTE_INPUT | HandleList number), got '
                       f'{record["attribute_key"]:#x}')
            case.check(record['flags'] == 0, 'UpdateProcThreadAttribute flags must be 0')
            case.check(record['handles'] == [self.stdin_value, self.stdout_value,
                                             self.stderr_value],
                       'the inherited list must be exactly the original '
                       f'stdin/stdout/stderr handles in order, got {record["handles"]!r}')
            case.check(record['cb_bytes'] == 3 * pointer_width,
                       f'the handle-list payload must be 3 pointer-width entries '
                       f'({3 * pointer_width} bytes), got {record["cb_bytes"]}')
            case.check(record['previous_value'] is None and
                       record['return_size'] is None,
                       'lpPreviousValue and lpReturnSize must both stay NULL')
            return 1

        def create_process(application, command_line, process_attributes,
                           thread_attributes, inherit_handles, creation_flags,
                           environment, current_directory, startup_info,
                           process_information):
            self.calls.append('CreateProcessW')
            self.last_error.value = 0
            case.check('Delete' not in self.calls,
                       'DeleteProcThreadAttributeList must not precede CreateProcessW')
            extended = ctypes.cast(startup_info,
                                   ctypes.POINTER(api.STARTUPINFOEXW)).contents
            startup = extended.StartupInfo
            case.check(ctypes.addressof(extended) == ctypes.addressof(startup),
                       'CreateProcessW must receive the STARTUPINFOEX with its '
                       'embedded StartupInfo as one object')
            submitted = extended.lpAttributeList
            actual = int(submitted) if submitted is not None else None
            expected = self.initialized_address
            case.check(actual is not None and actual == expected,
                       'CreateProcessW STARTUPINFOEX.lpAttributeList must submit the '
                       f'initialized/updated attribute list {expected:#x}, got '
                       f'{"NULL" if actual is None else hex(actual)}')
            case.check(int(startup.cb) == ctypes.sizeof(api.STARTUPINFOEXW),
                       f'cb must be sizeof(STARTUPINFOEXW)='
                       f'{ctypes.sizeof(api.STARTUPINFOEXW)}, got {int(startup.cb)}')
            case.check(int(startup.dwFlags) & windows_process.STARTF_USESTDHANDLES,
                       'STARTF_USESTDHANDLES must stay set')
            stdio = (int(startup.hStdInput), int(startup.hStdOutput),
                     int(startup.hStdError))
            case.check(stdio == (self.stdin_value, self.stdout_value, self.stderr_value),
                       f'the three stdio members must stay the original handles, '
                       f'got {stdio!r}')
            case.check(bool(inherit_handles),
                       'bInheritHandles must stay TRUE for the explicit whitelist')
            flags_value = int(creation_flags)
            for required, label in ((windows_process.CREATE_SUSPENDED, 'CREATE_SUSPENDED'),
                                    (windows_process.CREATE_UNICODE_ENVIRONMENT,
                                     'CREATE_UNICODE_ENVIRONMENT'),
                                    (windows_process.EXTENDED_STARTUPINFO_PRESENT,
                                     'EXTENDED_STARTUPINFO_PRESENT')):
                case.check(flags_value & required, f'{label} must stay set')
            case.check(not flags_value & CREATE_BREAKAWAY_FROM_JOB,
                       'no breakaway flag may be added')
            information = ctypes.cast(process_information,
                                      ctypes.POINTER(api.PROCESS_INFORMATION)).contents
            information.hProcess = 0x0AAA0001
            information.hThread = 0x0AAA0002
            information.dwProcessId = 4242
            information.dwThreadId = 4243
            self.created = {'process': information.hProcess, 'thread': information.hThread,
                            'pid': information.dwProcessId, 'creation_flags': flags_value,
                            'application': str(application),
                            'attribute_list_address': actual}
            return 1

        def delete(attribute_list):
            self.calls.append('Delete')
            address = ctypes.addressof(attribute_list)
            case.check(address == self.initialized_address,
                       f'Delete must target the initialized list '
                       f'{self.initialized_address:#x}, got {address:#x}')
            self.delete_count += 1
            return None

        def get_process_times(process, creation, exit_time, kernel, user):
            self.calls.append('GetProcessTimes')
            self.last_error.value = 0
            created = ctypes.cast(creation, ctypes.POINTER(api.FILETIME)).contents
            created.dwLowDateTime = 0x55667788
            created.dwHighDateTime = 0x11223344
            return 1

        api.InitializeProcThreadAttributeList = initialize
        api.UpdateProcThreadAttribute = update
        api.DeleteProcThreadAttributeList = delete
        api.CreateProcessW = create_process
        api.GetProcessTimes = get_process_times
        api._last_error = self.last_error


def synthetic_abi_create_process_handle_list_contract(case):
    # The real production method must submit an explicit HANDLE_LIST attribute
    # through a correctly keyed Update AND pass the same initialized list inside
    # the STARTUPINFOEX to CreateProcessW. Neither defect may mask the other.
    case.check(SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST == 0x20002,
               'the SDK oracle must be the fixed HandleList key 0x20002')
    pointer_width = ctypes.sizeof(ctypes.c_void_p)
    stdin_value, stdout_value, stderr_value = 0x0AAA11, 0x0AAA22, 0x0AAA33
    case.check(len({stdin_value, stdout_value, stderr_value}) == 3 and
               all(value != 0 and value != windows_process.INVALID_HANDLE_VALUE
                   for value in (stdin_value, stdout_value, stderr_value)),
               'the three synthetic stdio values must be distinct, non-zero and '
               'non-pseudo')
    with patched_win32_constructor() as probe:
        api = Win32ProcessAPI()
    case.check(probe.attempted_calls == [],
               f'construction must not call Win32 functions, got {probe.attempted_calls}')
    fixture = _HandleListContractFixture(api, case, stdin_value, stdout_value,
                                         stderr_value)
    fixture.capture()
    expected_filetime = (0x11223344 << 32) | 0x55667788
    try:
        fixture.install()
        process, thread, pid, creation_filetime = api.create_process_suspended(
            application='C:\\synthetic-contract\\tool.exe',
            command_line='"C:\\synthetic-contract\\tool.exe" --handle-list-contract',
            current_directory='C:\\synthetic-contract',
            environment_block=b'\x00\x00',
            stdin_handle=stdin_value, stdout_handle=stdout_value,
            stderr_handle=stderr_value)
        case.check((process, thread, pid) == (0x0AAA0001, 0x0AAA0002, 4242),
                   f'the returned identities must be the controlled low-level '
                   f'writes, got {(process, thread, pid)!r}')
        case.check(creation_filetime == expected_filetime,
                   f'the creation FILETIME must round-trip the controlled write, '
                   f'got {creation_filetime!r}')
        case.check(fixture.calls == ['Initialize(NULL-probe)', 'Initialize', 'Update',
                                     'CreateProcessW', 'Delete', 'GetProcessTimes'],
                   f'the real method boundary call order mismatched: {fixture.calls!r}')
        case.check(fixture.delete_count == 1,
                   'the initialized list must be deleted exactly once after creation')
        case.record(
            production_handle_list_constant=windows_process.PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            sdk_oracle_key=hex(SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST),
            update_contract=fixture.update,
            initialized_list_address=hex(fixture.initialized_address),
            submitted_list_address=hex(fixture.created['attribute_list_address']),
            call_order=fixture.calls, pointer_width=pointer_width)
        # FAULT_INJECTION sub-branch: an explicitly injected Update failure with
        # Win32 error 24 must propagate as the original exception with no
        # CreateProcessW call and exactly one list deletion. The injection is a
        # synthetic stage behaviour, NOT a real Windows error reproduction.
        fault = _HandleListContractFixture(api, case, stdin_value, stdout_value,
                                           stderr_value)
        fault.inject_update_failure = True
        fault.install()
        try:
            api.create_process_suspended(
                application='C:\\synthetic-contract\\tool.exe',
                command_line='"C:\\synthetic-contract\\tool.exe" --fault-injection',
                current_directory='C:\\synthetic-contract',
                environment_block=b'\x00\x00',
                stdin_handle=stdin_value, stdout_handle=stdout_value,
                stderr_handle=stderr_value)
            raise CaseFailure('the injected Update failure must propagate')
        except Win32CallFailure as failure:
            case.check(failure.function == 'UpdateProcThreadAttribute',
                       f'expected the original UpdateProcThreadAttribute failure, '
                       f'got {failure.function!r}')
            case.check(failure.last_error == WIN32_ERROR_BAD_LENGTH,
                       f'the injected last error must stay '
                       f'{WIN32_ERROR_BAD_LENGTH}, got {failure.last_error!r}')
        case.check(fault.calls == ['Initialize(NULL-probe)', 'Initialize', 'Update',
                                   'Delete'],
                   f'the fault path must stop after Update with one Delete, '
                   f'got {fault.calls!r}')
        case.check(fault.delete_count == 1 and 'CreateProcessW' not in fault.calls,
                   'no CreateProcess, resume or fallback may run after the Update '
                   'failure')
        case.record(fault_injection={'marker': 'FAULT_INJECTION',
                                     'injected_win32_error': WIN32_ERROR_BAD_LENGTH,
                                     'call_order': fault.calls,
                                     'note': 'synthetic stage programming only; not a '
                                             'real Windows error reproduction'})
    finally:
        fixture.restore()


# --------------------------------------------------------------------------------------
# Fixture-contract regressions: CLI dispatch, cross-cwd evidence roots, N6
# outcome preservation and N9 exception receipts (harness-only oracles)
# --------------------------------------------------------------------------------------
#
# The three cases below keep the harness itself under test. They run on any
# platform, use only finite self-created fixtures, bound every subprocess and
# restore each patched boundary in a finally block. They are engineering tool
# cases: they are not native product suites and prove no native Windows
# behaviour.


def _bounded_cli_run(argv, cwd, timeout=60):
    # Bounded subprocess probe reserved for finite, side-effect-free CLI forms.
    started = time.monotonic()
    completed = subprocess.run(argv, cwd=str(cwd), capture_output=True, timeout=timeout)
    return {'exit_code': completed.returncode,
            'stdout': completed.stdout.decode('utf-8', 'replace'),
            'stderr': completed.stderr.decode('utf-8', 'replace'),
            'elapsed_seconds': round(time.monotonic() - started, 3)}


def fixture_cli_dispatch_contract(case):
    # Two independent sub-checks, ENTRY and PATH. Each captures its own
    # failures into facts and both always run; the verdict comes at the end so
    # an argparse failure can never mask the cross-cwd path failure.
    workspace = case._recorder.root / 'cli-dispatch-probe'
    workspace.mkdir(parents=True, exist_ok=True)
    entry = {}

    def parse_exit(argv):
        try:
            parse_args(argv)
            return None
        except SystemExit as error:
            return error.code

    # ENTRY: a real finite leaf (hold=0, no Win32 dependency) must dispatch
    # through the real script argv without the ordinary selfcheck flags.
    probe_cwd = workspace / 'entry-cwd'
    probe_cwd.mkdir()
    entry['leaf_probe'] = _bounded_cli_run(
        _fixture_argv('leaf', hold=0, index='cli-entry-probe'), probe_cwd)
    entry['leaf_exit_zero'] = entry['leaf_probe']['exit_code'] == 0
    entry['leaf_marker_passed_through'] = 'child-marker cli-entry-probe' in \
        entry['leaf_probe']['stdout']
    entry['leaf_required_args_stderr'] = 'the following arguments are required' in \
        entry['leaf_probe']['stderr']

    # Every existing fixture name must parse for dispatch without the
    # ordinary mode/evidence-dir flags.
    entry['fixture_names_accepted'] = {}
    for name in sorted(FIXTURES):
        try:
            options, _values = parse_args(['--fixture', name])
            entry['fixture_names_accepted'][name] = bool(
                options.fixture == name and options.mode is None)
        except SystemExit as error:
            entry['fixture_names_accepted'][name] = f'SystemExit:{error.code}'
    entry['all_fixture_names_accepted'] = all(
        value is True for value in entry['fixture_names_accepted'].values())

    # The ordinary entry keeps both flags required, explicit values stay
    # validated, unknown/empty fixture names are argument errors and the
    # residual key/value parsing stays lenient.
    entry['ordinary_no_args_exit'] = parse_exit([])
    entry['ordinary_mode_only_exit'] = parse_exit(['--mode', 'synthetic'])
    entry['ordinary_evidence_only_exit'] = parse_exit(['--evidence-dir', 'x'])
    entry['unknown_fixture_exit'] = parse_exit(['--fixture', 'no-such-fixture'])
    entry['empty_fixture_exit'] = parse_exit(['--fixture', ''])
    entry['fixture_invalid_mode_exit'] = parse_exit(['--fixture', 'leaf', '--mode', 'bogus'])
    try:
        _options, residual = parse_args(['--fixture', 'leaf', '--hold', '0',
                                         '--custom-token', 'v'])
        entry['residual_options'] = residual
        entry['residual_lenient'] = (residual.get('hold') == '0' and
                                     residual.get('custom_token') == 'v')
    except SystemExit as error:
        entry['residual_options'] = None
        entry['residual_lenient'] = f'SystemExit:{error.code}'

    # An unknown fixture name must be rejected before any side effect,
    # observable through a real CLI process run in a clean directory.
    unknown_cwd = workspace / 'unknown-fixture-cwd'
    unknown_cwd.mkdir()
    entry['unknown_fixture_cli'] = _bounded_cli_run(
        [sys.executable, '-B', str(Path(__file__).resolve()), '--fixture',
         'no-such-fixture'], unknown_cwd)
    entry['unknown_fixture_rejected_before_effects'] = (
        entry['unknown_fixture_cli']['exit_code'] == 2 and
        not any(unknown_cwd.iterdir()))

    # A valid fixture plus valid ordinary flags stays fixture-only: the fixture
    # runs and the ordinary evidence directory is never created.
    ordinary_dir = workspace / 'ordinary-evidence-must-not-exist'
    entry['priority_probe'] = _bounded_cli_run(
        _fixture_argv('leaf', hold=0, index='cli-priority-probe', mode='synthetic',
                      evidence_dir=ordinary_dir), probe_cwd)
    entry['fixture_priority_kept'] = (entry['priority_probe']['exit_code'] == 0 and
                                      'child-marker cli-priority-probe' in
                                      entry['priority_probe']['stdout'])
    entry['ordinary_evidence_dir_not_created'] = not ordinary_dir.exists()

    # --help keeps its argparse exit-0 behaviour without fixture effects.
    help_cwd = workspace / 'help-cwd'
    help_cwd.mkdir()
    entry['help_probe'] = _bounded_cli_run(
        [sys.executable, '-B', str(Path(__file__).resolve()), '--help'], help_cwd)
    entry['help_exit_zero_without_effects'] = (entry['help_probe']['exit_code'] == 0 and
                                               not any(help_cwd.iterdir()))
    case.record(**entry)

    # PATH: bypassing the CLI gate, a relative evidence root with a space is
    # established through the real Recorder from one cwd, the sentinel argv is
    # produced through the real _fixture_argv, and the real finite sentinel
    # handler then runs from a different cwd with parse-shaped options whose
    # values come from the actual argv tokens. An absolute root keeps its
    # original positive behaviour.
    original_cwd = Path.cwd()
    relative_root_name = 'evidence root with space'

    def sentinel_probe(parent_dir, child_dir, root_value):
        os.chdir(parent_dir)
        try:
            recorder = Recorder(root_value)
            recorder.begin(root_value)
            argv = _fixture_argv('sentinel', seconds=0,
                                 start_marker=recorder.root / 'sentinel-started.txt',
                                 end_marker=recorder.root / 'sentinel-finished.txt')
            tokens = {}
            for index, token in enumerate(argv[:-1]):
                if token in ('--start-marker', '--end-marker', '--seconds'):
                    tokens[token] = argv[index + 1]
            os.chdir(child_dir)
            options = {'start_marker': tokens['--start-marker'],
                       'end_marker': tokens['--end-marker'],
                       'seconds': tokens['--seconds']}
            try:
                _fixture_sentinel(options)
            except Exception as error:
                # Keep the actual child-cwd write failure as a recorded fact so
                # the sub-check stays non-masking and bounded; KeyboardInterrupt
                # and SystemExit still propagate.
                probe_error = f'{type(error).__name__}: {error}'
            else:
                probe_error = None
        finally:
            os.chdir(original_cwd)
        return recorder, options, probe_error

    parent_a = workspace / 'path-parent-a'
    parent_b = workspace / 'path-parent-b'
    parent_a.mkdir()
    parent_b.mkdir()
    recorder, relative_options, relative_error = sentinel_probe(
        parent_a, parent_b, relative_root_name)
    relative_probe = {
        'recorder_root': str(recorder.root),
        'root_is_absolute': recorder.root.is_absolute(),
        'started_marker_at_original_root':
            (recorder.root / 'sentinel-started.txt').is_file(),
        'finished_marker_at_original_root':
            (recorder.root / 'sentinel-finished.txt').is_file(),
        'misplaced_directory_in_child_cwd': (parent_b / relative_root_name).exists(),
        'sentinel_error': relative_error,
        'sentinel_options': relative_options}
    absolute_root = workspace / 'absolute-evidence-root'
    recorder_absolute, absolute_options, absolute_error = sentinel_probe(
        parent_a, parent_b, absolute_root)
    absolute_probe = {
        'recorder_root': str(recorder_absolute.root),
        'root_is_absolute': recorder_absolute.root.is_absolute(),
        'started_marker_at_original_root':
            (recorder_absolute.root / 'sentinel-started.txt').is_file(),
        'finished_marker_at_original_root':
            (recorder_absolute.root / 'sentinel-finished.txt').is_file(),
        'misplaced_directory_in_child_cwd': (parent_b / absolute_root.name).exists(),
        'sentinel_error': absolute_error,
        'sentinel_options': absolute_options}
    case.record(relative_probe=relative_probe, absolute_probe=absolute_probe)

    failures = []
    if not entry['leaf_exit_zero'] or not entry['leaf_marker_passed_through']:
        failures.append(f"ENTRY: a valid finite fixture must dispatch without ordinary "
                        f"flags (exit {entry['leaf_probe']['exit_code']})")
    if not entry['all_fixture_names_accepted']:
        failures.append('ENTRY: every existing fixture name must parse for dispatch '
                        'without --mode/--evidence-dir')
    if entry['ordinary_no_args_exit'] != 2 or entry['ordinary_mode_only_exit'] != 2 or \
            entry['ordinary_evidence_only_exit'] != 2:
        failures.append('ENTRY: the ordinary entry must keep --mode/--evidence-dir '
                        'required')
    if entry['unknown_fixture_exit'] != 2 or entry['empty_fixture_exit'] != 2 or \
            not entry['unknown_fixture_rejected_before_effects']:
        failures.append('ENTRY: unknown/empty fixture names must be rejected before '
                        'any side effect')
    if entry['fixture_invalid_mode_exit'] != 2:
        failures.append('ENTRY: an explicit invalid --mode must stay rejected')
    if not entry['fixture_priority_kept'] or not entry['ordinary_evidence_dir_not_created']:
        failures.append('ENTRY: a valid fixture plus valid ordinary flags must stay '
                        'fixture-only')
    if not entry['help_exit_zero_without_effects']:
        failures.append('ENTRY: --help must exit 0 without fixture effects')
    if entry['residual_lenient'] is not True:
        failures.append('ENTRY: parse_known_args residual key/value handling must stay')
    if relative_probe['root_is_absolute'] is not True:
        failures.append('PATH: a relative evidence root must be anchored to the cwd '
                        'where the Recorder established it')
    if not relative_probe['started_marker_at_original_root'] or \
            not relative_probe['finished_marker_at_original_root']:
        failures.append('PATH: sentinel markers must land under the original evidence '
                        'root across the child cwd change')
    if relative_probe['misplaced_directory_in_child_cwd']:
        failures.append('PATH: no misplaced evidence directory may appear under the '
                        'child cwd')
    if relative_probe['sentinel_error'] is not None:
        failures.append('PATH: the finite sentinel handler must complete against the '
                        'evidence root from the child cwd')
    if not absolute_probe['root_is_absolute'] or \
            not absolute_probe['started_marker_at_original_root'] or \
            not absolute_probe['finished_marker_at_original_root'] or \
            absolute_probe['misplaced_directory_in_child_cwd'] or \
            absolute_probe['sentinel_error'] is not None:
        failures.append('PATH: an absolute evidence root must keep its positive '
                        'behaviour')
    case.check(not failures, '; '.join(failures) or 'fixture CLI and path contract holds')


def fixture_ctrlc_outcome_preservation(case):
    # Calls the REAL _fixture_ctrlc_controller with only its external boundaries
    # (the low-level run_owned launch and the console-ctrl event generator)
    # replaced by controlled stand-ins; the controller body itself is never
    # replaced. The runner stand-in takes the outcome path from the actually
    # received runner argv and exclusively writes fixed inner interrupt bytes,
    # so a controller that forwards its own outcome path destroys those bytes
    # at the same location. This is a local data-flow counterexample only, it
    # is never a real Ctrl-C reproduction.
    workspace = case._recorder.root / 'ctrlc-outcome-probe'
    workspace.mkdir(parents=True, exist_ok=True)
    root = workspace / 'controller-root'
    root.mkdir()
    ready = root / 'ctest-ready'
    controller_outcome = root / 'controller-outcome.json'
    inner_fields = {'cleanup_status': 'PROVEN_ZERO',
                    'keyboard_interrupt_observed': True,
                    'termination_reason': 'INTERRUPTED',
                    'tree_exit_proven': True}
    inner_bytes = (json.dumps(inner_fields, indent=2) + '\n').encode('utf-8')
    inner_sha = hashlib.sha256(inner_bytes).hexdigest()
    observed = {}

    class _RunnerResult:
        exit_code = 0
        tree_exit_proven = True

    def controlled_run_owned(command, **kwargs):
        argv = [str(item) for item in command]
        observed['runner_argv'] = argv
        outcome_index = argv.index('--outcome') + 1
        observed['runner_outcome_path'] = argv[outcome_index]
        ready.write_text('ready', encoding='utf-8')
        Path(observed['runner_outcome_path']).write_bytes(inner_bytes)
        return _RunnerResult()

    def controlled_console_event():
        observed['console_event_calls'] = observed.get('console_event_calls', 0) + 1
        return True

    original_run_owned = run_owned
    original_event = _generate_console_ctrl_event
    original_handler = signal.getsignal(signal.SIGINT)
    options = {'root': str(root), 'ready': str(ready),
               'outcome': str(controller_outcome), 'budget': '18'}
    try:
        globals()['run_owned'] = controlled_run_owned
        globals()['_generate_console_ctrl_event'] = controlled_console_event
        observed['controller_exit_code'] = _fixture_ctrlc_controller(options)
    finally:
        globals()['run_owned'] = original_run_owned
        globals()['_generate_console_ctrl_event'] = original_event
        signal.signal(signal.SIGINT, original_handler)
    facts = {'controller_exit_code': observed.get('controller_exit_code'),
             'runner_outcome_path': observed.get('runner_outcome_path'),
             'controller_outcome_path': str(controller_outcome),
             'console_event_calls': observed.get('console_event_calls')}
    runner_path = Path(observed['runner_outcome_path'])
    facts['paths_differ'] = runner_path != controller_outcome
    facts['runner_file_bytes_sha256'] = hashlib.sha256(
        runner_path.read_bytes()).hexdigest() if runner_path.is_file() else None
    facts['inner_bytes_preserved'] = facts['runner_file_bytes_sha256'] == inner_sha
    facts['controller_record'] = None
    if controller_outcome.is_file():
        facts['controller_record'] = json.loads(controller_outcome.read_text())
    record = facts['controller_record'] or {}
    facts['controller_error'] = record.get('controller_error')
    facts['console_event_sent'] = (record.get('console_event') or {}).get('sent')
    facts['controller_embeds_runner_outcome'] = record.get('runner_outcome') == inner_fields
    facts['controller_embeds_runner_sha256'] = record.get('runner_outcome_sha256') == \
        inner_sha
    consumer = json.loads(runner_path.read_bytes()) if runner_path.is_file() else {}
    facts['consumer_keyboard_interrupt_observed'] = consumer.get('keyboard_interrupt_observed')
    facts['consumer_tree_exit_proven'] = consumer.get('tree_exit_proven')
    facts['consumer_termination_reason'] = consumer.get('termination_reason')
    case.record(**facts)
    case.check(facts['paths_differ'],
               'the controller record and the runner outcome must use different paths')
    case.check(observed.get('controller_exit_code') == 0,
               'the controller must still report success for the simulated run')
    case.check(facts['controller_error'] is None and facts['console_event_sent'] is True,
               'the controlled boundaries must satisfy the controller observation')
    case.check(facts['inner_bytes_preserved'],
               'the inner runner outcome bytes must survive the controller unchanged')
    case.check(facts['controller_embeds_runner_outcome'] and
               facts['controller_embeds_runner_sha256'],
               'the controller record must embed the runner outcome content and identity')
    case.check(facts['consumer_keyboard_interrupt_observed'] is True and
               facts['consumer_tree_exit_proven'] is True and
               facts['consumer_termination_reason'] == 'INTERRUPTED',
               'the consumer must still verify the original interrupt and tree-proof '
               'fields on the runner record')


def fixture_integration_error_receipt(case):
    # Drives the real _integration_router closure with a controlled lower-level
    # run_owned entry that raises an actual TimeoutExpired carrying an original
    # dict process receipt. The same exception object must be re-raised, the
    # same receipt must gain exactly the expected substitution note and every
    # original field must survive. Success results and exceptions without a
    # dict receipt keep their original semantics and no receipt is fabricated.
    # The receipt attribute is injected by this probe; this is not a real
    # Windows timeout reproduction.
    workspace = case._recorder.root / 'integration-error-probe'
    workspace.mkdir(parents=True, exist_ok=True)
    ctest_argv = [sys.executable, '-B', '-c', 'print("never executed")']
    fixture = {'ctest_command': ctest_argv, 'interruptible': False}
    note = INTEGRATION_TIMEOUT_OVERRIDE_NOTE
    facts = {}
    raised = {}

    class _SuccessResult:
        def __init__(self):
            self.receipt = {'outcome': 'COMPLETED', 'tree_exit_proven': True,
                            'exit_code': 0}

    def timeout_run_owned(command, **kwargs):
        facts['routed_argv'] = [str(item) for item in command]
        facts['routed_timeout'] = kwargs.get('timeout')
        error = subprocess.TimeoutExpired(ctest_argv, 6.0)
        error.process_receipt = {'outcome': 'EXECUTION_DEADLINE',
                                 'tree_exit_proven': True,
                                 'termination': {'reason': 'EXECUTION_DEADLINE'},
                                 'output': {'complete': False}}
        raised['timeout'] = error
        raise error

    def interrupt_without_receipt(command, **kwargs):
        error = KeyboardInterrupt('synthetic interrupt without a receipt')
        raised['interrupt'] = error
        raise error

    def success_run_owned(command, **kwargs):
        return _SuccessResult()

    original_run_owned = windows_process.run_owned

    def call_router(entry):
        windows_process.run_owned = entry
        try:
            router = _integration_router(fixture, [])
            return {'returned': router(ctest_argv, cwd=str(workspace), env={},
                                       timeout=6.0, spool_root=workspace / 'spool',
                                       command_id='ctest')}
        except BaseException as error:
            return {'raised': error}
        finally:
            windows_process.run_owned = original_run_owned

    outcome_timeout = call_router(timeout_run_owned)
    outcome_interrupt = call_router(interrupt_without_receipt)
    outcome_success = call_router(success_run_owned)

    caught_timeout = outcome_timeout.get('raised')
    facts['timeout_exception_type'] = type(caught_timeout).__name__ \
        if caught_timeout else None
    facts['timeout_exception_same_object'] = caught_timeout is raised.get('timeout')
    receipt = getattr(caught_timeout, 'process_receipt', None) if caught_timeout else None
    facts['timeout_receipt_same_object'] = receipt is \
        getattr(raised.get('timeout'), 'process_receipt', None)
    facts['timeout_receipt_note'] = receipt.get('integration_substitution') \
        if isinstance(receipt, dict) else None
    facts['timeout_fields_preserved'] = isinstance(receipt, dict) and all([
        receipt.get('outcome') == 'EXECUTION_DEADLINE',
        receipt.get('tree_exit_proven') is True,
        (receipt.get('termination') or {}).get('reason') == 'EXECUTION_DEADLINE',
        (receipt.get('output') or {}).get('complete') is False])

    caught_interrupt = outcome_interrupt.get('raised')
    facts['interrupt_exception_same_object'] = caught_interrupt is \
        raised.get('interrupt')
    facts['interrupt_receipt_still_absent'] = getattr(
        caught_interrupt, 'process_receipt', None) is None if caught_interrupt else None

    returned = outcome_success.get('returned')
    facts['success_returned'] = returned is not None
    facts['success_receipt_note'] = returned.receipt.get('integration_substitution') \
        if returned is not None else None
    facts['success_fields_preserved'] = returned is not None and \
        returned.receipt.get('outcome') == 'COMPLETED' and \
        returned.receipt.get('tree_exit_proven') is True
    case.record(**facts)

    case.check(facts['timeout_receipt_note'] == note,
               'a substituted failing command must carry the integration-substitution '
               'note on its original exception receipt')
    case.check(facts['timeout_exception_same_object'] and
               facts['timeout_receipt_same_object'],
               'the original TimeoutExpired object and its receipt must be re-raised '
               'unchanged')
    case.check(facts['timeout_fields_preserved'],
               'the original failure fields must survive the note attachment')
    case.check(facts['routed_argv'] == [str(item) for item in ctest_argv] and
               facts['routed_timeout'] == 6.0,
               'the router must keep the original ctest argv and short-timeout '
               'parameters')
    case.check(facts['interrupt_exception_same_object'] and
               facts['interrupt_receipt_still_absent'] is True,
               'an exception without a dict receipt must be re-raised unchanged with '
               'no fabricated receipt')
    case.check(facts['success_returned'] and facts['success_receipt_note'] == note and
               facts['success_fields_preserved'],
               'the success path must keep its existing note and fields')


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
    ('abi-declaration-sdk-layout', synthetic_abi_declaration_sdk_layout),
    ('abi-constructor-binding-smoke', synthetic_abi_constructor_binding_smoke),
    ('abi-pidlist-returned-count', synthetic_abi_pidlist_returned_count),
    ('abi-create-process-handle-list-contract',
     synthetic_abi_create_process_handle_list_contract),
    ('fixture-cli-dispatch-contract', fixture_cli_dispatch_contract),
    ('fixture-ctrlc-outcome-preservation', fixture_ctrlc_outcome_preservation),
    ('fixture-integration-error-receipt', fixture_integration_error_receipt),
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


def _native_handle_list_key_contrast(case, api):
    """Same-condition real-API contrast on this exact Win32ProcessAPI binding.

    Two freshly initialized real attribute lists, the same three fixture-owned
    inheritable handles (one NUL input handle and two private spool file
    handles), the same 3-entry pointer-width payload, flags 0, both reserved
    parameters NULL and count 1: only the attribute key differs. The old key
    0x00020000 (the SDK ParentProcess key value) is expected to make the real
    UpdateProcThreadAttribute fail with Win32 error 24; the SDK handle-list key
    must succeed. The contrast never calls CreateProcessW and grants no
    parent-process or kill authority; every list and handle it creates is
    deleted/closed in its finally block. Unexpected observations fail the case
    as-is; expectations are never rewritten to match a different result."""
    contrast_root = case._recorder.root / 'native-spool' / 'attribute-contrast'
    contrast_root.mkdir(parents=True, exist_ok=True)
    pointer_width = ctypes.sizeof(ctypes.c_void_p)
    payload_bytes = 3 * pointer_width
    case.check(windows_process.PROC_THREAD_ATTRIBUTE_HANDLE_LIST ==
               SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
               'the corrected production constant must equal the SDK handle-list key')
    contrast = {'pointer_width': pointer_width, 'handle_entries': 3,
                'payload_bytes': payload_bytes, 'initialize_count': 1,
                'initialize_flags': 0, 'reserved_parameters': 'both NULL',
                'create_process_called': False, 'keys': {}}
    stdin_handle = None
    output_handles = []
    closes = []
    try:
        stdin_handle = api.open_input_null()
        for name in ('contrast-first.spool', 'contrast-second.spool'):
            output_handles.append(api.open_output_spool(contrast_root / name))
        inherited = (ctypes.c_void_p * 3)(stdin_handle, output_handles[0],
                                          output_handles[1])
        case.check(ctypes.sizeof(inherited) == payload_bytes,
                   'the contrast payload must be three pointer-width entries')
        for label, key in (
                ('old-key-0x00020000-parent-process-value', 0x00020000),
                ('sdk-key-0x00020002-handle-list',
                 SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST)):
            attribute_size = ctypes.c_size_t()
            if api.InitializeProcThreadAttributeList(None, 1, 0,
                                                     ctypes.byref(attribute_size)):
                raise CaseFailure(f'{label}: the NULL capacity probe unexpectedly '
                                  'succeeded')
            probe_error = api._last_error()
            case.check(probe_error == windows_process.ERROR_INSUFFICIENT_BUFFER,
                       f'{label}: the NULL capacity probe must report error 122, '
                       f'got {probe_error}')
            buffer = ctypes.create_string_buffer(attribute_size.value)
            if not api.InitializeProcThreadAttributeList(buffer, 1, 0,
                                                         ctypes.byref(attribute_size)):
                raise CaseFailure(f'{label}: real list initialization failed with '
                                  f'Win32 error {api._last_error()}')
            try:
                updated = bool(api.UpdateProcThreadAttribute(
                    buffer, 0, key, inherited, payload_bytes, None, None))
                if key == 0x00020000:
                    error_code = api._last_error()
                    case.check(updated is False,
                               f'{label}: Update must fail, got BOOL={updated}')
                    case.check(error_code == WIN32_ERROR_BAD_LENGTH,
                               f'{label}: the immediate error must be 24 '
                               f'(ERROR_BAD_LENGTH), got {error_code}; the '
                               'expectation is not rewritten to match the result')
                    contrast['keys'][label] = {'bool': False,
                                               'immediate_last_error': error_code}
                else:
                    case.check(updated is True,
                               f'{label}: Update must succeed, got BOOL={updated}, '
                               f'error={api._last_error()}')
                    contrast['keys'][label] = {
                        'bool': True,
                        'immediate_last_error': 'NOT_APPLICABLE_SUCCESS'}
            finally:
                api.DeleteProcThreadAttributeList(buffer)
    finally:
        for handle in [stdin_handle, *output_handles]:
            if handle is not None:
                try:
                    api.close_handle(handle)
                    closes.append('CLOSED')
                except WindowsProcessError as error:
                    closes.append(f'CLOSE_FAILED:{error}')
        contrast['handle_closes'] = closes
        contrast['handles_source'] = ('fixture-owned only: one NUL input handle and '
                                      'two private spool file handles, created and '
                                      'closed by this contrast')
    case.check(len(closes) == 3 and all(item == 'CLOSED' for item in closes),
               f'all three contrast fixture handles must close, got {closes!r}')
    return contrast


class _AttributeCallRecorder:
    """Narrow per-instance observation of the real attribute-list boundary.

    The wrappers record the actual arguments and order of the real
    Initialize/Update/Delete attribute-list calls and CreateProcessW, forward
    every argument unchanged to the real Win32 function and return its result
    untouched: no result substitution, no last-error modification, no
    permission or inheritance change. Before the real CreateProcessW runs, the
    submitted STARTUPINFOEX attribute list is verified to be the same list the
    successful Update targeted. The instance functions are restored on exit."""

    def __init__(self, case, api):
        self.case = case
        self.api = api
        self.calls = []
        self.updated_list_address = None
        self.last_update = None
        self._saved = None

    def __enter__(self):
        api = self.api
        names = ('InitializeProcThreadAttributeList', 'UpdateProcThreadAttribute',
                 'DeleteProcThreadAttributeList', 'CreateProcessW')
        self._saved = {name: getattr(api, name) for name in names}
        real_initialize = self._saved['InitializeProcThreadAttributeList']
        real_update = self._saved['UpdateProcThreadAttribute']
        real_delete = self._saved['DeleteProcThreadAttributeList']
        real_create = self._saved['CreateProcessW']
        recorder = self

        def initialize(attribute_list, count, flags, size):
            stage = 'Initialize(NULL-probe)' if attribute_list is None else 'Initialize'
            recorder.calls.append({'function': stage, 'count': int(count),
                                   'flags': int(flags)})
            return real_initialize(attribute_list, count, flags, size)

        def update(attribute_list, flags, attribute, value, size_bytes, previous,
                   return_size):
            record = {'function': 'UpdateProcThreadAttribute',
                      'list_address': ctypes.addressof(attribute_list),
                      'flags': int(flags), 'attribute_key': int(attribute),
                      'handle_entries': [int(item) if item is not None else None
                                         for item in value],
                      'cb_bytes': int(size_bytes),
                      'previous_value_none': previous is None,
                      'return_size_none': return_size is None}
            recorder.calls.append(record)
            result = real_update(attribute_list, flags, attribute, value, size_bytes,
                                 previous, return_size)
            if result:
                recorder.updated_list_address = record['list_address']
                recorder.last_update = record
            return result

        def create_process(application, command_line, process_attributes,
                           thread_attributes, inherit_handles, creation_flags,
                           environment, current_directory, startup_info,
                           process_information):
            extended = ctypes.cast(startup_info,
                                   ctypes.POINTER(api.STARTUPINFOEXW)).contents
            startup = extended.StartupInfo
            submitted = extended.lpAttributeList
            submitted_address = int(submitted) if submitted is not None else None
            case = recorder.case
            case.check(submitted_address is not None,
                       'the production CreateProcessW must submit a non-NULL '
                       'attribute list')
            case.check(recorder.updated_list_address is not None and
                       submitted_address == recorder.updated_list_address,
                       f'the CreateProcessW attribute list {submitted_address!r} must '
                       f'be the same list the successful Update targeted '
                       f'{recorder.updated_list_address!r}')
            update = recorder.last_update
            case.check(update is not None and
                       update['attribute_key'] == SDK_PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                       'the submitted list must be the one updated with the SDK '
                       'handle-list key')
            case.check(update['handle_entries'] == [
                int(startup.hStdInput), int(startup.hStdOutput),
                int(startup.hStdError)],
                'the three stdio members must be the updated handle entries')
            case.check(update['cb_bytes'] == 3 * ctypes.sizeof(ctypes.c_void_p),
                       'the updated handle-list payload must be three pointer-width '
                       'entries')
            case.check(bool(inherit_handles), 'bInheritHandles must stay TRUE')
            flags_value = int(creation_flags)
            for required, label in (
                    (windows_process.CREATE_SUSPENDED, 'CREATE_SUSPENDED'),
                    (windows_process.CREATE_UNICODE_ENVIRONMENT,
                     'CREATE_UNICODE_ENVIRONMENT'),
                    (windows_process.EXTENDED_STARTUPINFO_PRESENT,
                     'EXTENDED_STARTUPINFO_PRESENT')):
                case.check(flags_value & required, f'{label} must stay set')
            case.check(not flags_value & CREATE_BREAKAWAY_FROM_JOB,
                       'no breakaway flag may appear')
            case.check(int(startup.cb) == ctypes.sizeof(api.STARTUPINFOEXW),
                       'cb must stay sizeof(STARTUPINFOEXW)')
            case.check(int(startup.dwFlags) & windows_process.STARTF_USESTDHANDLES,
                       'STARTF_USESTDHANDLES must stay set')
            recorder.calls.append({'function': 'CreateProcessW',
                                   'startupinfoex_address': ctypes.addressof(extended),
                                   'cb': int(startup.cb),
                                   'attribute_list_address': submitted_address,
                                   'inherit_handles': bool(inherit_handles),
                                   'creation_flags': flags_value,
                                   'stdio_handles': [int(startup.hStdInput),
                                                     int(startup.hStdOutput),
                                                     int(startup.hStdError)]})
            return real_create(application, command_line, process_attributes,
                               thread_attributes, inherit_handles, creation_flags,
                               environment, current_directory, startup_info,
                               process_information)

        def delete(attribute_list):
            recorder.calls.append({'function': 'DeleteProcThreadAttributeList',
                                   'list_address': ctypes.addressof(attribute_list)})
            return real_delete(attribute_list)

        api.InitializeProcThreadAttributeList = initialize
        api.UpdateProcThreadAttribute = update
        api.DeleteProcThreadAttributeList = delete
        api.CreateProcessW = create_process
        return self

    def __exit__(self, *args):
        for name, function in self._saved.items():
            setattr(self.api, name, function)
        return False


def native_startup_checks(case):
    api = Win32ProcessAPI()
    sizes = {name: ctypes.sizeof(getattr(api, name))
             for name in windows_process.X64_STRUCTURE_SIZES}
    for name, expected in windows_process.X64_STRUCTURE_SIZES.items():
        case.check(sizes[name] == expected, f'{name} size {sizes[name]} != {expected}')
    case.check(ctypes.sizeof(ctypes.c_void_p) == 8, 'x64 pointer width required')
    # Official accounting/PID-list layouts beyond the size table: all eight
    # accounting fields (four LARGE_INTEGER times then the four DWORD counters)
    # and the PID-list header/array sit at their SDK offsets, and the module
    # DWORD_PTR alias keeps pointer width.
    accounting_class = api.JOBOBJECT_BASIC_ACCOUNTING_INFORMATION
    accounting_offsets = {name: getattr(accounting_class, name).offset
                          for name, _ in accounting_class._fields_}
    for field, expected_offset in (('TotalUserTime', 0), ('TotalKernelTime', 8),
                                   ('ThisPeriodTotalUserTime', 16),
                                   ('ThisPeriodTotalKernelTime', 24),
                                   ('TotalPageFaultCount', 32), ('TotalProcesses', 36),
                                   ('ActiveProcesses', 40), ('TotalTerminatedProcesses', 44)):
        case.check(accounting_offsets[field] == expected_offset,
                   f'{field} offset {accounting_offsets[field]} != {expected_offset}')
    pid_list_class = api.JOBOBJECT_BASIC_PROCESS_IDLIST
    pid_list_offsets = {name: getattr(pid_list_class, name).offset
                        for name, _ in pid_list_class._fields_}
    for field, expected_offset in (('NumberOfAssignedProcesses', 0),
                                   ('NumberOfProcessIdsInList', 4), ('ProcessIdList', 8)):
        case.check(pid_list_offsets[field] == expected_offset,
                   f'{field} offset {pid_list_offsets[field]} != {expected_offset}')
    case.check(ctypes.sizeof(windows_process.DWORD_PTR) == ctypes.sizeof(ctypes.c_void_p),
               'DWORD_PTR must stay pointer-width')
    # NULL and INVALID_HANDLE_VALUE stay distinct failure identities.
    try:
        api.open_output_spool(case._recorder.root / 'native-cwd' / 'missing-dir' / 'x')
        raise CaseFailure('CreateFileW on an impossible path must fail')
    except Win32CallFailure:
        pass
    # Same-condition real-API contrast on this exact binding: identical three
    # fixture-owned inheritable handles, identical 3-entry payload, flags 0,
    # both reserved parameters NULL and count 1; only the attribute key differs.
    # The old key 0x00020000 (SDK ParentProcess value) is expected to fail with
    # Win32 error 24 and the SDK handle-list key must succeed. This contrast
    # never calls CreateProcessW and grants no parent-process or kill authority.
    contrast_started = time.monotonic()
    contrast = _native_handle_list_key_contrast(case, api)
    contrast['elapsed_seconds'] = round(time.monotonic() - contrast_started, 3)
    # Nested Job allowance under the Actions outer job is verified by running
    # one real supervised command end to end, now with narrow per-instance
    # observation of the attribute-list boundary forwarded unchanged to Win32.
    observing = ObservingAPI(api)
    with _AttributeCallRecorder(case, api) as attribute_calls:
        result, error, _ = _native_budgeted_launch(
            case, observing, [sys.executable, '-c', 'print("ok")'],
            command_id='startup-probe', timeout=10)
    case.check(error is None and result.stdout.strip() == b'ok', 'startup probe failed')
    case.check(result.tree_exit_proven, 'startup probe must prove its tree exit')
    attribute_order = [call['function'] for call in attribute_calls.calls]
    case.check(attribute_order == ['Initialize(NULL-probe)', 'Initialize',
                                   'UpdateProcThreadAttribute', 'CreateProcessW',
                                   'DeleteProcThreadAttributeList'],
               f'the supervised probe must exercise the attribute boundary in '
               f'order, got {attribute_order!r}')
    case.record(structure_sizes=sizes, accounting_offsets=accounting_offsets,
                pid_list_offsets=pid_list_offsets,
                pid_list_size=ctypes.sizeof(pid_list_class),
                dword_ptr_width=ctypes.sizeof(windows_process.DWORD_PTR),
                nested_job_assignment='ALLOWED_AND_PROVEN',
                handle_list_key_contrast=contrast,
                production_attribute_calls=attribute_calls.calls,
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
    runner_outcome = root / 'supervised-runner-outcome.json'
    controller_outcome = root / 'controller-outcome.json'
    controller = _fixture_argv('ctrlc-controller', ready=root / 'ctest-ready',
                               outcome=controller_outcome, budget=18)
    result, error, elapsed = _native_budgeted_launch(
        case, api, controller, command_id='n6-ctrlc-controller', timeout=22,
        extra_creation_flags=windows_process.CREATE_NEW_CONSOLE)
    case.check(error is None, f'controller failed: {error!r}')
    case.check(result.exit_code == 0, f'controller exit {result.exit_code}: {result.stderr!r}')
    case.check(result.tree_exit_proven, 'controller tree must be proven')
    observation = json.loads(controller_outcome.read_text())
    case.check(runner_outcome.is_file(), 'the runner outcome file must survive the controller')
    outcome = json.loads(runner_outcome.read_text())
    case.check(outcome.get('keyboard_interrupt_observed') is True, 'real Ctrl-C observed')
    case.check(outcome.get('tree_exit_proven') is True, 'interrupted tree proven')
    case.check(outcome.get('termination_reason') == 'INTERRUPTED', 'interrupt recorded')
    case.record(controller_receipt=_summary(result.receipt),
                controller_observation=observation, runner_outcome=outcome,
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
        try:
            result = real_run_owned(argv, cwd=cwd, env=env, timeout=timeout,
                                    spool_root=spool_root, command_id=command_id,
                                    extra_env=extra_env, **kwargs)
        except BaseException as error:
            # A substituted command that fails keeps its original exception
            # identity and is re-raised unchanged; when that exception already
            # carries a dict process receipt, the same substitution note is
            # attached to it. No receipt is ever fabricated.
            receipt = getattr(error, 'process_receipt', None)
            if note is not None and isinstance(receipt, dict):
                receipt['integration_substitution'] = note
            raise
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
    runner_outcome_path = root / 'supervised-runner-outcome.json'
    ready = Path(options['ready'])
    runner_argv = [sys.executable, '-B', str(Path(__file__).resolve()), '--fixture',
                   'supervised-runner', '--ready', str(ready),
                   '--outcome', str(runner_outcome_path)]
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
    # The inner runner keeps its own separate outcome file byte-for-byte; the
    # controller only reads it and embeds its content and identity in its own
    # record. The two files are never merged or overwritten.
    if runner_outcome_path.is_file():
        runner_bytes = runner_outcome_path.read_bytes()
        observation['runner_outcome_sha256'] = hashlib.sha256(runner_bytes).hexdigest()
        try:
            observation['runner_outcome'] = json.loads(runner_bytes)
        except ValueError as error:
            observation['controller_error'] = observation.get('controller_error') or \
                f'runner outcome unreadable: {error}'
    else:
        observation['controller_error'] = observation.get('controller_error') or \
            'runner outcome missing'
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
    parser.add_argument('--mode', choices=('synthetic', 'native'))
    parser.add_argument('--evidence-dir', type=Path)
    parser.add_argument('--fixture')
    options, rest = parser.parse_known_args(argv)
    if options.fixture is not None:
        # A valid existing fixture dispatches without the ordinary selfcheck
        # flags; an unknown or empty fixture name is an argument error here,
        # before any fixture side effect.
        if options.fixture not in FIXTURES:
            parser.error(f'unknown --fixture {options.fixture!r}; expected one of '
                         f'{sorted(FIXTURES)}')
    else:
        # The ordinary synthetic/native entry keeps both flags required.
        missing = [flag for flag, value in (('--mode', options.mode),
                                            ('--evidence-dir', options.evidence_dir))
                   if value is None]
        if missing:
            parser.error(f'the following arguments are required: {", ".join(missing)}')
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
