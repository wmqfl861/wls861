#!/usr/bin/env python3
"""Owned native Windows command lifecycle supervision (E002 fix).

Every supervised command runs inside one unnamed, non-inheritable Job Object
with ``JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE``. The process is created suspended
and assigned to the Job before its primary thread resumes, so no target code
can run before the owned subtree boundary exists. No breakaway or silent
breakaway limit is configured and the supervisor itself is never placed into a
Job it terminates. The Job is a supervision custody design for trusted
tooling, not a security sandbox; it makes no claim about handles held by
external processes.

Runtime completion is proven only by three observed facts on the same
command: the root process HANDLE is signaled, its actual exit code was read,
and a successful ``QueryInformationJobObject`` on the same Job returned
``ActiveProcesses == 0``. ``TerminateJobObject`` success, a close-Job request,
a root-only wait or an absent PID are not exit proof. ``STILL_ACTIVE`` (259)
is never used as a wait result; a process may legitimately exit with 259.
Unproven observations stay ``UNPROVEN``/``NOT_COMPLETE`` with the actual
errors recorded; they are never written as 0, true or PASSED.

This module imports everywhere but instantiates the real Win32 binding only
on native Windows. :func:`run_owned` refuses unsupported platforms instead of
falling back to an unsupervised path; tests may inject a narrow fake API and
clock, and production callers cannot disable the Job boundary or the exit
proof.
"""
import contextlib
import ctypes
import datetime
import errno
import hashlib
import os
from pathlib import Path
import shutil
import signal
import subprocess
import tempfile
import time

RECEIPT_SCHEMA = 'wls-owned-process-receipt-v1'
DEFAULT_CLEANUP_TIMEOUT_SECONDS = 5.0
DEFAULT_SETTLE_SECONDS = 1.0
DEFAULT_POLL_SECONDS = 0.1
TERMINATION_EXIT_CODE = 1
STILL_ACTIVE = 259

WAIT_OBJECT_0 = 0x00000000
WAIT_TIMEOUT = 0x00000102
WAIT_FAILED = 0xFFFFFFFF
INFINITE_MS = 0xFFFFFFFF
INVALID_HANDLE_VALUE = 0xFFFFFFFFFFFFFFFF
ERROR_INSUFFICIENT_BUFFER = 122

JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE = 0x00002000
JobObjectExtendedLimitInformation = 9
JobObjectBasicAccountingInformation = 1
JobObjectBasicProcessIdList = 3
PROCESS_QUERY_LIMITED_INFORMATION = 0x00001000
PROCESS_SYNCHRONIZE = 0x00100000
CREATE_SUSPENDED = 0x00000004
CREATE_UNICODE_ENVIRONMENT = 0x00000400
CREATE_NEW_CONSOLE = 0x00000010
EXTENDED_STARTUPINFO_PRESENT = 0x00080000
STARTF_USESTDHANDLES = 0x00000100
PROC_THREAD_ATTRIBUTE_HANDLE_LIST = 0x00020000
HANDLE_FLAG_INHERIT = 0x00000001
FILE_SHARE_READ = 0x00000001
FILE_SHARE_WRITE = 0x00000002
GENERIC_READ = 0x80000000
GENERIC_WRITE = 0x40000000
OPEN_EXISTING = 3
CREATE_ALWAYS = 2
FILE_ATTRIBUTE_NORMAL = 0x00000080

# x64 ABI expectations for the structures declared below. They are asserted
# by the native selfcheck on real Windows; ctypes itself applies the native
# alignment rules on the host that instantiates the real API. The accounting
# entry follows the official SDK layout: four LARGE_INTEGER values then the
# four DWORD counters (size 48, DWORD offsets 32/36/40/44). The process-id
# list entry covers the two-DWORD header plus the fixed 64-entry diagnostic
# buffer (8 + 64 * 8 = 520).
X64_STRUCTURE_SIZES = {
    'STARTUPINFOW': 104,
    'STARTUPINFOEXW': 112,
    'PROCESS_INFORMATION': 24,
    'JOBOBJECT_BASIC_LIMIT_INFORMATION': 64,
    'IO_COUNTERS': 48,
    'JOBOBJECT_EXTENDED_LIMIT_INFORMATION': 144,
    'JOBOBJECT_BASIC_ACCOUNTING_INFORMATION': 48,
    'JOBOBJECT_BASIC_PROCESS_IDLIST': 520,
}

# Python's ctypes has no ``c_ulongptr``, and on the LLP64 Windows target an
# ``unsigned long`` is 32 bits and cannot carry pointer-width values, so a
# ``c_ulong`` fallback would corrupt them. The pointer-width unsigned carrier
# Python actually provides is ``c_size_t`` (SIZE_T): ULONG_PTR, DWORD_PTR and
# SIZE_T therefore share it on every supported platform. HANDLE stays
# ``c_void_p`` and DWORD stays ``c_uint32``; ctypes is never monkey patched
# and no 32-bit platform support is declared.
ULONG_PTR = ctypes.c_size_t
DWORD_PTR = ctypes.c_size_t


class WindowsProcessError(OSError):
    """Base class for owned-process supervision failures."""


class UnsupportedPlatform(WindowsProcessError):
    """The real supervision API exists only on native Windows."""


class Win32CallFailure(WindowsProcessError):
    """One declared Win32 call failed; the original last error is retained."""

    def __init__(self, function, last_error):
        self.function = function
        self.last_error = last_error
        super().__init__(f'{function} failed with Win32 error {last_error}')


class OwnedCommandLaunchError(WindowsProcessError):
    """The command never reached supervised execution (establishment failed)."""


def _utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


class _RealClock:
    is_injected = False

    @staticmethod
    def monotonic():
        return time.monotonic()

    @staticmethod
    def sleep(seconds):
        time.sleep(max(0.0, seconds))


class OwnedCommandResult:
    """Outcome of one supervised command, including its full process receipt."""

    def __init__(self, command_id, exit_code):
        self.command_id = command_id
        self.exit_code = exit_code
        self.stdout = b''
        self.stderr = b''
        self.receipt = {}

    @property
    def outcome(self):
        return self.receipt.get('outcome', 'UNPROVEN')

    @property
    def tree_exit_proven(self):
        return bool(self.receipt.get('tree_exit_proven'))

    def summarize(self):
        receipt = self.receipt
        return {
            'command_id': self.command_id,
            'outcome': receipt.get('outcome'),
            'exit_code_available': receipt.get('exit_code_available'),
            'exit_code': receipt.get('exit_code'),
            'tree_exit_proven': receipt.get('tree_exit_proven'),
            'termination_reason': (receipt.get('termination') or {}).get('reason'),
            'settle_status': (receipt.get('settle') or {}).get('status'),
        }


# --------------------------------------------------------------------------------------
# Win32 declarations
# --------------------------------------------------------------------------------------

def _declare_structures():
    class JOBOBJECT_BASIC_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ('PerProcessUserTimeLimit', ctypes.c_int64),
            ('PerJobUserTimeLimit', ctypes.c_int64),
            ('LimitFlags', ctypes.c_uint32),
            ('MinimumWorkingSetSize', ctypes.c_size_t),
            ('MaximumWorkingSetSize', ctypes.c_size_t),
            ('ActiveProcessLimit', ctypes.c_uint32),
            ('Affinity', ULONG_PTR),
            ('PriorityClass', ctypes.c_uint32),
            ('SchedulingClass', ctypes.c_uint32),
        ]

    class IO_COUNTERS(ctypes.Structure):
        _fields_ = [(name, ctypes.c_ulonglong) for name in
                    ('ReadOperationCount', 'WriteOperationCount', 'OtherOperationCount',
                     'ReadTransferCount', 'WriteTransferCount', 'OtherTransferCount')]

    class JOBOBJECT_EXTENDED_LIMIT_INFORMATION(ctypes.Structure):
        _fields_ = [
            ('BasicLimitInformation', JOBOBJECT_BASIC_LIMIT_INFORMATION),
            ('IoInfo', IO_COUNTERS),
            ('ProcessMemoryLimit', ctypes.c_size_t),
            ('JobMemoryLimit', ctypes.c_size_t),
            ('PeakProcessMemoryUsed', ctypes.c_size_t),
            ('PeakJobMemoryUsed', ctypes.c_size_t),
        ]

    class JOBOBJECT_BASIC_ACCOUNTING_INFORMATION(ctypes.Structure):
        # Official SDK layout: TotalUserTime, TotalKernelTime,
        # ThisPeriodTotalUserTime, ThisPeriodTotalKernelTime (four LARGE_INTEGER
        # at x64 offsets 0/8/16/24), then the DWORD counters TotalPageFaultCount,
        # TotalProcesses, ActiveProcesses, TotalTerminatedProcesses at x64
        # offsets 32/36/40/44 (size 48). The former trailing
        # ActiveProcessesLimit/Peak* entries belong to other structures and had
        # shifted the real counter offsets.
        _fields_ = [
            ('TotalUserTime', ctypes.c_int64),
            ('TotalKernelTime', ctypes.c_int64),
            ('ThisPeriodTotalUserTime', ctypes.c_int64),
            ('ThisPeriodTotalKernelTime', ctypes.c_int64),
            ('TotalPageFaultCount', ctypes.c_uint32),
            ('TotalProcesses', ctypes.c_uint32),
            ('ActiveProcesses', ctypes.c_uint32),
            ('TotalTerminatedProcesses', ctypes.c_uint32),
        ]

    class JOBOBJECT_BASIC_PROCESS_IDLIST(ctypes.Structure):
        # Official SDK header: DWORD NumberOfAssignedProcesses (offset 0) and
        # DWORD NumberOfProcessIdsInList (offset 4), then the ULONG_PTR
        # entries. Fixed 64-entry read-only diagnostic view; a Job larger
        # than the buffer makes its query fail rather than truncating here.
        _fields_ = [('NumberOfAssignedProcesses', ctypes.c_uint32),
                    ('NumberOfProcessIdsInList', ctypes.c_uint32),
                    ('ProcessIdList', ULONG_PTR * 64)]

    class STARTUPINFOW(ctypes.Structure):
        _fields_ = [
            ('cb', ctypes.c_uint32),
            ('lpReserved', ctypes.c_wchar_p),
            ('lpDesktop', ctypes.c_wchar_p),
            ('lpTitle', ctypes.c_wchar_p),
            ('dwX', ctypes.c_uint32), ('dwY', ctypes.c_uint32),
            ('dwXSize', ctypes.c_uint32), ('dwYSize', ctypes.c_uint32),
            ('dwXCountChars', ctypes.c_uint32), ('dwYCountChars', ctypes.c_uint32),
            ('dwFillAttribute', ctypes.c_uint32),
            ('dwFlags', ctypes.c_uint32),
            ('wShowWindow', ctypes.c_uint16),
            ('cbReserved2', ctypes.c_uint16),
            ('lpReserved2', ctypes.c_void_p),
            ('hStdInput', ctypes.c_void_p),
            ('hStdOutput', ctypes.c_void_p),
            ('hStdError', ctypes.c_void_p),
        ]

    class STARTUPINFOEXW(ctypes.Structure):
        _fields_ = [('StartupInfo', STARTUPINFOW), ('lpAttributeList', ctypes.c_void_p)]

    class PROCESS_INFORMATION(ctypes.Structure):
        _fields_ = [('hProcess', ctypes.c_void_p), ('hThread', ctypes.c_void_p),
                    ('dwProcessId', ctypes.c_uint32), ('dwThreadId', ctypes.c_uint32)]

    class FILETIME(ctypes.Structure):
        _fields_ = [('dwLowDateTime', ctypes.c_uint32), ('dwHighDateTime', ctypes.c_uint32)]

    return locals()


class Win32ProcessAPI:
    """Declared-ABI Win32 bindings; instantiated only on native Windows.

    Every function declares ``argtypes``/``restype``. HANDLE, LPVOID,
    ULONG_PTR and SIZE_T values keep pointer width; DWORD/BOOL and structure
    layouts follow the native ABI. API failures raise immediately with the
    captured ``GetLastError`` value. NULL and INVALID_HANDLE_VALUE are checked
    separately where the Win32 contract distinguishes them.
    """

    is_injected = False

    def __init__(self):
        if os.name != 'nt':
            raise UnsupportedPlatform('the real Win32 supervision API requires native Windows')
        structures = _declare_structures()
        for name, value in structures.items():
            setattr(self, name, value)
        kernel32 = ctypes.WinDLL('kernel32', use_last_error=True)
        self._kernel32 = kernel32
        HANDLE = ctypes.c_void_p
        LPCWSTR = ctypes.c_wchar_p
        DWORD = ctypes.c_uint32
        BOOL = ctypes.c_int
        SIZE_T = ctypes.c_size_t

        def bind(name, argtypes, restype):
            function = getattr(kernel32, name)
            function.argtypes = argtypes
            function.restype = restype
            return function

        self._last_error = ctypes.get_last_error
        self.CreateJobObjectW = bind('CreateJobObjectW', [ctypes.c_void_p, LPCWSTR], HANDLE)
        self.SetInformationJobObject = bind('SetInformationJobObject',
            [HANDLE, ctypes.c_int, ctypes.c_void_p, DWORD], BOOL)
        self.QueryInformationJobObject = bind('QueryInformationJobObject',
            [HANDLE, ctypes.c_int, ctypes.c_void_p, DWORD, ctypes.POINTER(DWORD)], BOOL)
        self.AssignProcessToJobObject = bind('AssignProcessToJobObject', [HANDLE, HANDLE], BOOL)
        self.TerminateJobObject = bind('TerminateJobObject', [HANDLE, ctypes.c_uint32], BOOL)
        self.CreateProcessW = bind('CreateProcessW',
            [LPCWSTR, LPCWSTR, ctypes.c_void_p, ctypes.c_void_p, BOOL, DWORD,
             ctypes.c_void_p, LPCWSTR, ctypes.POINTER(structures['STARTUPINFOW']),
             ctypes.POINTER(structures['PROCESS_INFORMATION'])], BOOL)
        self.ResumeThread = bind('ResumeThread', [HANDLE], DWORD)
        self.WaitForSingleObject = bind('WaitForSingleObject', [HANDLE, DWORD], DWORD)
        self.GetExitCodeProcess = bind('GetExitCodeProcess', [HANDLE, ctypes.POINTER(DWORD)], BOOL)
        self.CloseHandle = bind('CloseHandle', [HANDLE], BOOL)
        self.TerminateProcess = bind('TerminateProcess', [HANDLE, ctypes.c_uint32], BOOL)
        self.OpenProcess = bind('OpenProcess', [DWORD, BOOL, DWORD], HANDLE)
        self.GetProcessId = bind('GetProcessId', [HANDLE], DWORD)
        self.GetProcessTimes = bind('GetProcessTimes',
            [HANDLE, ctypes.POINTER(structures['FILETIME']), ctypes.POINTER(structures['FILETIME']),
             ctypes.POINTER(structures['FILETIME']), ctypes.POINTER(structures['FILETIME'])], BOOL)
        self.SetHandleInformation = bind('SetHandleInformation', [HANDLE, DWORD, DWORD], BOOL)
        self.CreateFileW = bind('CreateFileW',
            [LPCWSTR, DWORD, DWORD, ctypes.c_void_p, DWORD, DWORD, HANDLE], HANDLE)
        self.InitializeProcThreadAttributeList = bind('InitializeProcThreadAttributeList',
            [ctypes.c_void_p, DWORD, DWORD, ctypes.POINTER(SIZE_T)], BOOL)
        self.UpdateProcThreadAttribute = bind('UpdateProcThreadAttribute',
            [ctypes.c_void_p, DWORD, DWORD_PTR, ctypes.c_void_p, SIZE_T,
             ctypes.c_void_p, ctypes.POINTER(SIZE_T)], BOOL)
        self.DeleteProcThreadAttributeList = bind('DeleteProcThreadAttributeList',
            [ctypes.c_void_p], None)

    def _fail(self, function):
        return Win32CallFailure(function, self._last_error())

    # ---- Job lifecycle -----------------------------------------------------------
    def create_job(self):
        job = self.CreateJobObjectW(None, None)
        if job is None or job == 0:
            raise self._fail('CreateJobObjectW')
        return job

    def set_job_kill_on_close(self, job):
        information = self.JOBOBJECT_EXTENDED_LIMIT_INFORMATION()
        information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE
        if not self.SetInformationJobObject(job, JobObjectExtendedLimitInformation,
                                            ctypes.byref(information), ctypes.sizeof(information)):
            raise self._fail('SetInformationJobObject')

    def query_job_active_processes(self, job):
        accounting = self.JOBOBJECT_BASIC_ACCOUNTING_INFORMATION()
        if not self.QueryInformationJobObject(job, JobObjectBasicAccountingInformation,
                                              ctypes.byref(accounting), ctypes.sizeof(accounting), None):
            raise self._fail('QueryInformationJobObject')
        return int(accounting.ActiveProcesses)

    def assign_process_to_job(self, job, process):
        if not self.AssignProcessToJobObject(job, process):
            raise self._fail('AssignProcessToJobObject')

    def terminate_job(self, job, exit_code):
        if not self.TerminateJobObject(job, exit_code):
            raise self._fail('TerminateJobObject')

    def job_process_ids(self, job):
        """Read-only Job membership list for observers (diagnostic, not proof)."""
        listing = self.JOBOBJECT_BASIC_PROCESS_IDLIST()
        if not self.QueryInformationJobObject(job, JobObjectBasicProcessIdList,
                                              ctypes.byref(listing), ctypes.sizeof(listing), None):
            raise self._fail('QueryInformationJobObject(idlist)')
        copied = int(listing.NumberOfProcessIdsInList)
        if copied > len(listing.ProcessIdList):
            # The fixed 64-entry diagnostic buffer cannot hold more copied IDs;
            # the extra entries are never silently truncated into a complete-
            # looking list. NumberOfAssignedProcesses stays diagnostic only.
            raise WindowsProcessError(
                'QueryInformationJobObject(idlist) copied '
                f'{copied} process IDs into a {len(listing.ProcessIdList)}-entry '
                'buffer; refusing to report a truncated membership list')
        return [int(listing.ProcessIdList[index]) for index in range(copied)]

    # ---- Standard handles --------------------------------------------------------
    def open_input_null(self):
        handle = self.CreateFileW('NUL', GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                  None, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, None)
        if handle is None or handle == 0 or handle == INVALID_HANDLE_VALUE:
            raise self._fail('CreateFileW(NUL)')
        self._make_inheritable(handle)
        return handle

    def open_output_spool(self, path):
        handle = self.CreateFileW(str(path), GENERIC_WRITE, FILE_SHARE_READ,
                                  None, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, None)
        if handle is None or handle == 0 or handle == INVALID_HANDLE_VALUE:
            raise self._fail('CreateFileW(spool)')
        self._make_inheritable(handle)
        return handle

    def _make_inheritable(self, handle):
        if not self.SetHandleInformation(handle, HANDLE_FLAG_INHERIT, HANDLE_FLAG_INHERIT):
            raise self._fail('SetHandleInformation')

    # ---- Process creation --------------------------------------------------------
    def create_process_suspended(self, *, application, command_line, current_directory,
                                 environment_block, stdin_handle, stdout_handle,
                                 stderr_handle, extra_creation_flags=0):
        startup = self.STARTUPINFOEXW()
        startup.StartupInfo.cb = ctypes.sizeof(self.STARTUPINFOEXW)
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES
        startup.StartupInfo.hStdInput = stdin_handle
        startup.StartupInfo.hStdOutput = stdout_handle
        startup.StartupInfo.hStdError = stderr_handle

        attribute_size = ctypes.c_size_t()
        if self.InitializeProcThreadAttributeList(None, 1, 0, ctypes.byref(attribute_size)):
            raise Win32CallFailure('InitializeProcThreadAttributeList', 0)
        if self._last_error() != ERROR_INSUFFICIENT_BUFFER:
            raise self._fail('InitializeProcThreadAttributeList')
        attribute_buffer = ctypes.create_string_buffer(attribute_size.value)
        if not self.InitializeProcThreadAttributeList(attribute_buffer, 1, 0,
                                                      ctypes.byref(attribute_size)):
            raise self._fail('InitializeProcThreadAttributeList')
        try:
            inherited = (ctypes.c_void_p * 3)(stdin_handle, stdout_handle, stderr_handle)
            if not self.UpdateProcThreadAttribute(attribute_buffer, 0,
                    PROC_THREAD_ATTRIBUTE_HANDLE_LIST, inherited, ctypes.sizeof(inherited), None, None):
                raise self._fail('UpdateProcThreadAttribute')
            information = self.PROCESS_INFORMATION()
            command = ctypes.create_unicode_buffer(command_line)
            environment = ctypes.cast(ctypes.c_char_p(environment_block), ctypes.c_void_p)
            flags = (CREATE_SUSPENDED | CREATE_UNICODE_ENVIRONMENT |
                     EXTENDED_STARTUPINFO_PRESENT | int(extra_creation_flags))
            if not self.CreateProcessW(str(application), command, None, None, True, flags,
                                       environment, str(current_directory),
                                       ctypes.byref(startup.StartupInfo), ctypes.byref(information)):
                raise self._fail('CreateProcessW')
            if information.hProcess is None or information.hThread is None:
                raise Win32CallFailure('CreateProcessW', self._last_error())
            process, thread = information.hProcess, information.hThread
            pid = int(information.dwProcessId)
        finally:
            self.DeleteProcThreadAttributeList(attribute_buffer)
        return process, thread, pid, self._creation_time(process)

    def _creation_time(self, process):
        created, exited, kernel, user = (self.FILETIME(), self.FILETIME(),
                                         self.FILETIME(), self.FILETIME())
        if not self.GetProcessTimes(process, ctypes.byref(created), ctypes.byref(exited),
                                    ctypes.byref(kernel), ctypes.byref(user)):
            return None
        return (int(created.dwHighDateTime) << 32) | int(created.dwLowDateTime)

    def resume_thread(self, thread):
        previous = self.ResumeThread(thread)
        if previous == 0xFFFFFFFF:
            raise self._fail('ResumeThread')
        return int(previous)

    def wait_process(self, process, milliseconds):
        result = self.WaitForSingleObject(process, int(milliseconds))
        if result == WAIT_FAILED:
            raise self._fail('WaitForSingleObject')
        if result == WAIT_OBJECT_0:
            return 'SIGNALED'
        return 'TIMEOUT'

    def process_exit_code(self, process):
        code = ctypes.c_uint32()
        if not self.GetExitCodeProcess(process, ctypes.byref(code)):
            raise self._fail('GetExitCodeProcess')
        return int(code.value)

    def process_id(self, process):
        pid = self.GetProcessId(process)
        if pid == 0:
            raise self._fail('GetProcessId')
        return int(pid)

    def terminate_process(self, process, exit_code):
        if not self.TerminateProcess(process, exit_code):
            raise self._fail('TerminateProcess')

    def close_handle(self, handle):
        if not self.CloseHandle(handle):
            raise self._fail('CloseHandle')

    def open_process_snapshot(self, pid):
        """Observer handle to an externally created process; diagnostic only."""
        handle = self.OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION | PROCESS_SYNCHRONIZE,
                                  False, int(pid))
        if handle is None or handle == 0:
            raise self._fail('OpenProcess')
        return handle


# --------------------------------------------------------------------------------------
# Input validation
# --------------------------------------------------------------------------------------

def _validate_command(command):
    if not isinstance(command, (list, tuple)) or not command:
        raise ValueError('command must be a non-empty argv list')
    arguments = []
    for item in command:
        if not isinstance(item, str):
            raise ValueError('command arguments must be strings')
        if '\x00' in item:
            raise ValueError('command arguments must not contain NUL')
        arguments.append(item)
    return arguments


def _validate_text(value, label):
    if not isinstance(value, str):
        raise ValueError(f'{label} must be a string')
    if '\x00' in value:
        raise ValueError(f'{label} must not contain NUL')
    return value


def _build_environment_block(env, extra_env):
    merged = dict(env)
    if extra_env:
        merged.update(extra_env)
    for key, value in merged.items():
        _validate_text(key, 'environment key')
        _validate_text(value, 'environment value for ' + key)
        if '=' in key:
            raise ValueError('environment keys must not contain "="')
    # CreateProcessW documents an alphabetically sorted environment block.
    ordered = sorted(merged.items(), key=lambda item: item[0].upper())
    block = ''.join(f'{key}={value}\x00' for key, value in ordered) + '\x00'
    return block.encode('utf-16-le', errors='strict')


# --------------------------------------------------------------------------------------
# Supervisor
# --------------------------------------------------------------------------------------

class _Supervision:
    """Mutable state for one run_owned call; the receipt is its projection."""

    def __init__(self, command, cwd, command_id, timeout, cleanup_timeout,
                 settle_configured, poll_seconds, extra_env, extra_creation_flags,
                 api_injected, clock_injected):
        self.argv = command
        self.cwd = cwd
        self.command_id = command_id
        self.timeout = float(timeout)
        self.cleanup_timeout = float(cleanup_timeout)
        self.settle_configured = float(settle_configured)
        self.poll_seconds = float(poll_seconds)
        self.extra_env = dict(extra_env) if extra_env else {}
        self.extra_creation_flags = int(extra_creation_flags)
        self.api_injected = api_injected
        self.clock_injected = clock_injected

        self.started_utc = _utc_now()
        self.started_monotonic = None
        self.phases = []
        self.handles = []  # (label, handle) closed in reverse order
        self.closed = {}
        self.job = None
        self.process = None
        self.thread = None
        self.pid = None
        self.creation_filetime = None
        self.kill_on_close_configured = False
        self.assigned = False
        self.resumed = False
        self.executed = False

        self.exit_code = None
        self.exit_code_available = False
        self.root_signalled_monotonic = None
        self.root_signalled_utc = None
        self.root_wait_result = None
        self.job_query_ok = False
        self.job_active_last = None
        self.job_zero_observed = False
        self.job_query_failures = []
        self.terminate_reason = None
        self.terminate_requested = False
        self.terminate_api = None
        self.job_terminate_succeeded = None
        self.pre_assign_terminate_succeeded = None
        self.settle_status = 'NOT_ENTERED'
        self.settle_effective = None
        self.settle_elapsed = None
        self.cleanup_status = 'NOT_RUN'
        self.cleanup_started_monotonic = None
        self.cleanup_elapsed = None
        self.cleanup_errors = []
        self.second_interrupt = False
        self.tree_exit_proven = False
        self.outcome = 'UNPROVEN'
        self.failure_reason = None
        self.spool_directory = None
        self.owned_temporary = None
        self.stdout_path = None
        self.stderr_path = None

    def phase(self, name):
        entry = {'phase': name, 'status': 'OK'}
        self.phases.append(entry)

        @contextlib.contextmanager
        def marker():
            try:
                yield entry
            except BaseException:
                entry['status'] = 'FAILED'
                raise
        return marker()

    def record_handle(self, label, handle):
        self.handles.append((label, handle))

    def drop_handle(self, label):
        self.handles = [(name, handle) for name, handle in self.handles if name != label]

    def query_zero(self, api):
        """One ActiveProcesses query; returns True when the Job is observed empty."""
        try:
            active = api.query_job_active_processes(self.job)
        except WindowsProcessError as error:
            self.job_query_failures.append(_error_record(error))
            self.job_query_ok = False
            return False
        self.job_query_ok = True
        self.job_active_last = active
        if active == 0:
            self.job_zero_observed = True
            return True
        return False


def _error_record(error):
    return {'type': type(error).__name__, 'function': getattr(error, 'function', None),
            'win32_error': getattr(error, 'last_error', None), 'reason': str(error)[:512]}


def _deferred_signals(state):
    """Record (defer) a second interrupt during the bounded cleanup window."""
    import sys as _sys
    installed = []
    if not hasattr(signal, 'SIGINT'):
        return installed

    def deferred(signum, frame):
        state.second_interrupt = True

    for name in ('SIGINT', 'SIGBREAK'):
        number = getattr(signal, name, None)
        if number is None:
            continue
        try:
            previous = signal.signal(number, deferred)
        except (ValueError, OSError):
            continue
        installed.append((number, previous))
    return installed


def _restore_signals(installed):
    for number, previous in installed:
        try:
            signal.signal(number, previous)
        except (ValueError, OSError):
            pass


def _closeout(state, api, clock, *, reason):
    """Bounded owned-tree cleanup: terminate if needed, prove zero, close handles.

    Never raises: failures are recorded as parallel facts on the receipt.
    """
    state.cleanup_status = 'IN_PROGRESS'
    state.cleanup_started_monotonic = clock.monotonic()
    if state.process is None:
        # No process was ever created: there is no owned tree to reclaim or
        # prove, only handles to close. Never reported as an unproven tree.
        state.terminate_reason = state.terminate_reason or 'NOTHING_OWNED'
        state.cleanup_status = 'NOTHING_OWNED'
        state.cleanup_elapsed = 0.0
    installed = _deferred_signals(state)
    try:
        deadline = state.cleanup_started_monotonic + state.cleanup_timeout
        if state.process is not None and state.assigned:
            if not state.job_zero_observed:
                state.terminate_reason = state.terminate_reason or reason
                state.terminate_requested = True
                state.terminate_api = 'TerminateJobObject'
                try:
                    api.terminate_job(state.job, TERMINATION_EXIT_CODE)
                    state.job_terminate_succeeded = True
                except WindowsProcessError as error:
                    state.job_terminate_succeeded = False
                    state.cleanup_errors.append(_error_record(error))
            while clock.monotonic() < deadline:
                if state.query_zero(api) and state.exit_code_available and \
                        state.root_signalled_monotonic is not None:
                    break
                if not state.job_zero_observed:
                    clock.sleep(min(state.poll_seconds,
                                    max(0.0, deadline - clock.monotonic())))
                if state.root_signalled_monotonic is None and state.process is not None:
                    try:
                        if api.wait_process(state.process, 50) == 'SIGNALED':
                            state.root_signalled_monotonic = clock.monotonic()
                            state.root_signalled_utc = _utc_now()
                            if not state.exit_code_available:
                                try:
                                    state.exit_code = api.process_exit_code(state.process)
                                    state.exit_code_available = True
                                except WindowsProcessError as error:
                                    state.cleanup_errors.append(_error_record(error))
                    except WindowsProcessError as error:
                        state.root_wait_result = 'FAILED'
                        state.cleanup_errors.append(_error_record(error))
        elif state.process is not None:
            # Pre-assignment failure: the suspended process never ran target
            # code; reclaim it directly through the owned process handle.
            state.terminate_reason = state.terminate_reason or reason
            state.terminate_requested = True
            state.terminate_api = 'TerminateProcess(suspended-owned)'
            try:
                api.terminate_process(state.process, TERMINATION_EXIT_CODE)
                state.pre_assign_terminate_succeeded = True
            except WindowsProcessError as error:
                state.pre_assign_terminate_succeeded = False
                state.cleanup_errors.append(_error_record(error))
            try:
                api.wait_process(state.process, int(state.cleanup_timeout * 1000))
                state.root_signalled_monotonic = clock.monotonic()
                state.root_signalled_utc = _utc_now()
            except WindowsProcessError as error:
                state.cleanup_errors.append(_error_record(error))
        state.tree_exit_proven = bool(
            state.exit_code_available and state.root_signalled_monotonic is not None
            and state.job is not None and state.job_zero_observed and state.job_query_ok)
        if state.job is not None and not state.tree_exit_proven and state.assigned:
            state.cleanup_errors.append({
                'type': 'UnprovenTreeExit',
                'reason': 'Job zero not observed within the cleanup budget; the '
                          'final CloseHandle is only a KILL_ON_JOB_CLOSE request'})
    finally:
        # Every handle owned by this call is closed exactly once, in reverse
        # creation order. Closing the Job handle requests KILL_ON_JOB_CLOSE as
        # a last resort; it is never recorded as observed exit proof.
        for label, handle in reversed(state.handles):
            try:
                api.close_handle(handle)
                state.closed[label] = 'CLOSED'
            except WindowsProcessError as error:
                state.closed[label] = 'CLOSE_FAILED'
                state.cleanup_errors.append(_error_record(error))
        state.handles = []
        _restore_signals(installed)
        state.cleanup_elapsed = clock.monotonic() - state.cleanup_started_monotonic
        if state.cleanup_status == 'NOTHING_OWNED':
            pass
        elif state.tree_exit_proven:
            state.cleanup_status = 'PROVEN_ZERO'
        elif state.cleanup_errors:
            state.cleanup_status = 'UNPROVEN_WITH_ERRORS'
        else:
            state.cleanup_status = 'UNPROVEN'


def _read_spool(state):
    stdout = stderr = b''
    output = {'stdout_bytes': None, 'stdout_sha256': None, 'stderr_bytes': None,
              'stderr_sha256': None, 'complete': False, 'capture_errors': [],
              'spool_directory': str(state.spool_directory) if state.spool_directory else None,
              'spool_retained': True}
    if state.spool_directory is None:
        output['capture_errors'].append({'type': 'SpoolUnavailable',
                                         'reason': 'no spool files were created'})
        return stdout, stderr, output
    for label, attribute in (('stdout', 'stdout'), ('stderr', 'stderr')):
        path = getattr(state, label + '_path')
        try:
            data = path.read_bytes()
        except OSError as error:
            output['capture_errors'].append({'type': type(error).__name__,
                                             'reason': str(error)[:512]})
            continue
        if attribute == 'stdout':
            stdout = data
        else:
            stderr = data
        output[attribute + '_bytes'] = len(data)
        output[attribute + '_sha256'] = hashlib.sha256(data).hexdigest()
    output['complete'] = not output['capture_errors']
    return stdout, stderr, output


def _build_receipt(state, clock, output, execution_elapsed):
    receipt = {
        'schema': RECEIPT_SCHEMA,
        'command_id': state.command_id,
        'argv': state.argv,
        'cwd': state.cwd,
        'supervision_design': 'unnamed non-inheritable Job with KILL_ON_JOB_CLOSE; '
                              'created suspended, assigned before resume; no breakaway; '
                              'custody design, not a sandbox',
        'execution_timeout_seconds': state.timeout,
        'cleanup_timeout_seconds': state.cleanup_timeout,
        'started_at_utc': state.started_utc,
        'ended_at_utc': _utc_now(),
        'execution_elapsed_seconds': execution_elapsed,
        'task_level_env_overrides': state.extra_env,
        'extra_creation_flags': state.extra_creation_flags or None,
        'api_source': 'injected' if state.api_injected else 'native-win32',
        'clock_source': 'injected' if state.clock_injected else 'monotonic',
        'phases': state.phases,
        'establishment': {
            'job_created': state.job is not None,
            'kill_on_close_configured': state.kill_on_close_configured,
            'process_created_suspended': state.process is not None,
            'assigned_to_job': state.assigned,
            'primary_thread_resumed': state.resumed,
            'primary_thread_resume_precedes_any_target_code': True,
        },
        'root_identity': {
            'pid': state.pid,
            'pid_and_creation_time_are_diagnostic_only': True,
            'creation_filetime': state.creation_filetime,
            'handle_is_operational_identity': True,
        },
        'exit_code_available': state.exit_code_available,
        'exit_code': state.exit_code if state.exit_code_available else None,
        'still_active_value_never_used_as_wait_state': True,
        'termination': {
            'reason': state.terminate_reason,
            'requested': state.terminate_requested,
            'api': state.terminate_api,
            'requested_exit_code': TERMINATION_EXIT_CODE if state.terminate_requested else None,
            'job_terminate_succeeded': state.job_terminate_succeeded,
            'pre_assign_process_terminate_succeeded': state.pre_assign_terminate_succeeded,
        },
        'job_zero_observation': {
            'query_ok': state.job_query_ok,
            'last_active_processes': state.job_active_last,
            'zero_observed': state.job_zero_observed,
            'query_failures': state.job_query_failures,
        },
        'root_wait': {
            'result': state.root_wait_result,
            'signaled': state.root_signalled_monotonic is not None,
            'signaled_at_utc': state.root_signalled_utc,
        },
        'settle': {
            'configured_seconds': state.settle_configured,
            'effective_seconds': state.settle_effective,
            'elapsed_seconds': state.settle_elapsed,
            'status': state.settle_status,
            'root_exit_observed_at_utc': state.root_signalled_utc if state.settle_status != 'NOT_ENTERED' else None,
        },
        'cleanup': {
            'timeout_seconds': state.cleanup_timeout,
            'elapsed_seconds': state.cleanup_elapsed,
            'status': state.cleanup_status,
            'errors': state.cleanup_errors,
            'handles_closed': state.closed,
            'second_interrupt_recorded_and_deferred': state.second_interrupt,
        },
        'output': output,
        'tree_exit_proven': state.tree_exit_proven,
        'proof_rule': 'root handle signaled AND actual exit code read AND successful '
                      'same-Job ActiveProcesses==0 query',
        'outcome': state.outcome,
        'failure_reason': state.failure_reason,
    }
    return receipt


def _finalize(state, api, clock, *, closeout_reason, result, error):
    """Unconditional bounded cleanup, then receipt attachment. Never raises."""
    stdout = stderr = b''
    try:
        execution_elapsed = None
        if state.started_monotonic is not None:
            execution_elapsed = clock.monotonic() - state.started_monotonic
        if closeout_reason is not None or result is not None or error is not None:
            _closeout(state, api, clock, reason=closeout_reason or 'COMPLETION')
        else:
            state.cleanup_status = 'NOT_RUN'
        stdout, stderr, output = _read_spool(state)
        if state.owned_temporary is not None and state.tree_exit_proven and output['complete']:
            # Capture-only convenience spool: remove only after exit proof and
            # complete capture; failures retain the originals.
            shutil.rmtree(state.owned_temporary, ignore_errors=True)
            output['spool_retained'] = False
        receipt = _build_receipt(state, clock, output, execution_elapsed)
    except BaseException as build_error:  # a receipt must never mask the original outcome
        receipt = {'schema': RECEIPT_SCHEMA, 'command_id': state.command_id,
                   'receipt_build_error': _error_record(build_error),
                   'tree_exit_proven': False, 'outcome': 'UNPROVEN'}
        result = None
        error = error or build_error
    if error is not None:
        try:
            error.process_receipt = receipt
        except BaseException:
            pass
        if isinstance(error, _DeadlineExceededMarker):
            timeout_error = subprocess.TimeoutExpired(
                subprocess.list2cmdline(state.argv), state.timeout, output=stdout, stderr=stderr)
            timeout_error.process_receipt = receipt
            return timeout_error, None
        return error, None
    if result is not None:
        result.stdout = stdout
        result.stderr = stderr
        result.receipt = receipt
    return None, result


class _DeadlineExceededMarker(Exception):
    """Internal marker: the execution deadline expired before root exit."""


def run_owned(command, *, cwd, env, timeout, spool_root=None, command_id=None,
              cleanup_timeout_seconds=DEFAULT_CLEANUP_TIMEOUT_SECONDS,
              settle_configured_seconds=DEFAULT_SETTLE_SECONDS,
              poll_seconds=DEFAULT_POLL_SECONDS, extra_env=None, api=None,
              clock=None, extra_creation_flags=0):
    """Run one command inside an owned Job and prove its whole tree exited.

    Returns an :class:`OwnedCommandResult` for any reached root exit (including
    non-zero codes and owned-tree failures). Raises the original
    ``subprocess.TimeoutExpired``/``KeyboardInterrupt``/``SystemExit``/launch
    error after bounded owned cleanup; every outcome carries a full process
    receipt on ``process_receipt``. There is no unsupervised fallback: without
    an injected test API this requires native Windows.
    """
    argv = _validate_command(command)
    cwd = _validate_text(os.fspath(cwd), 'cwd')
    timeout = float(timeout)
    if timeout <= 0:
        raise ValueError('timeout must be positive')
    api = api if api is not None else Win32ProcessAPI()
    clock = clock if clock is not None else _RealClock()
    command_id = command_id or subprocess.list2cmdline(argv)

    if spool_root is None:
        owned = Path(tempfile.mkdtemp(prefix='wls-owned-spool-'))
        spool = owned
    else:
        spool = Path(spool_root)
        spool.mkdir(parents=True, exist_ok=True)
        owned = None
    state = _Supervision(argv, cwd, command_id, timeout, cleanup_timeout_seconds,
                         settle_configured_seconds, poll_seconds, extra_env,
                         extra_creation_flags, getattr(api, 'is_injected', False),
                         getattr(clock, 'is_injected', False))
    state.spool_directory = spool
    state.owned_temporary = owned
    state.stdout_path = spool / f'{command_id}.stdout.spool'
    state.stderr_path = spool / f'{command_id}.stderr.spool'

    result = None
    error = None
    closeout_reason = None
    settle_elapsed = None
    try:
        # ---- establishment: job -> limit -> handles -> suspended process -> assign -> resume
        with state.phase('create-job'):
            state.job = api.create_job()
            state.record_handle('job', state.job)
        with state.phase('configure-job'):
            api.set_job_kill_on_close(state.job)
            state.kill_on_close_configured = True
        with state.phase('open-stdio'):
            stdin = api.open_input_null()
            state.record_handle('stdin-nul', stdin)
            stdout_handle = api.open_output_spool(state.stdout_path)
            state.record_handle('stdout-spool', stdout_handle)
            stderr_handle = api.open_output_spool(state.stderr_path)
            state.record_handle('stderr-spool', stderr_handle)
        application = shutil.which(argv[0], path=env.get('PATH'))
        if application is None:
            raise FileNotFoundError(errno.ENOENT, os.strerror(errno.ENOENT), argv[0])
        application = os.path.abspath(application)
        environment_block = _build_environment_block(env, state.extra_env)
        with state.phase('create-process-suspended'):
            state.process, state.thread, state.pid, state.creation_filetime = \
                api.create_process_suspended(
                    application=application,
                    command_line=subprocess.list2cmdline(argv),
                    current_directory=os.path.abspath(cwd),
                    environment_block=environment_block,
                    stdin_handle=stdin, stdout_handle=stdout_handle,
                    stderr_handle=stderr_handle,
                    extra_creation_flags=state.extra_creation_flags)
            state.record_handle('process', state.process)
            state.record_handle('thread', state.thread)
        with state.phase('assign-job'):
            try:
                api.assign_process_to_job(state.job, state.process)
            except WindowsProcessError as stage_error:
                # The suspended process never ran target code. Reject the
                # command and reclaim it through the owned process handle; no
                # breakaway is attempted.
                state.failure_reason = 'AssignProcessToJobObject failed; command rejected'
                raise OwnedCommandLaunchError(state.failure_reason) from stage_error
            state.assigned = True
        with state.phase('resume-thread'):
            try:
                api.resume_thread(state.thread)
            except WindowsProcessError as stage_error:
                state.failure_reason = 'ResumeThread failed; owned Job reclaimed'
                raise OwnedCommandLaunchError(state.failure_reason) from stage_error
            state.resumed = True
            state.executed = True
            state.drop_handle('thread')
            try:
                api.close_handle(state.thread)
                state.closed['thread'] = 'CLOSED_AFTER_RESUME'
            except WindowsProcessError:
                # Recorded as still-open so the finally close attempts it again.
                state.record_handle('thread', state.thread)
                state.closed['thread'] = 'PENDING_FINAL_CLOSE'
        state.started_monotonic = clock.monotonic()
        deadline = state.started_monotonic + timeout

        # ---- supervised execution
        while True:
            remaining = deadline - clock.monotonic()
            if remaining <= 0:
                closeout_reason = 'TIMEOUT'
                state.terminate_reason = 'EXECUTION_DEADLINE'
                error = _DeadlineExceededMarker()
                break
            wait_ms = max(1, int(min(state.poll_seconds, remaining) * 1000))
            with state.phase('wait-root'):
                try:
                    waited = api.wait_process(state.process, wait_ms)
                except WindowsProcessError as wait_error:
                    state.root_wait_result = 'FAILED'
                    state.failure_reason = 'WaitForSingleObject failed'
                    raise WindowsProcessError(
                        f'owned root wait failed: {wait_error}') from wait_error
            if waited == 'TIMEOUT':
                continue
            state.root_wait_result = 'SIGNALED'
            state.root_signalled_monotonic = clock.monotonic()
            state.root_signalled_utc = _utc_now()
            with state.phase('read-exit-code'):
                state.exit_code = api.process_exit_code(state.process)
                state.exit_code_available = True
            if state.exit_code != 0:
                state.settle_status = 'NOT_ENTERED'
                state.outcome = 'FAILED_NONZERO_EXIT'
                state.terminate_reason = state.terminate_reason or 'ROOT_NONZERO_EXIT'
                closeout_reason = 'ROOT_NONZERO_EXIT'
                result = OwnedCommandResult(state.command_id, state.exit_code)
                break
            # Root exited 0: bounded natural settle for descendants, never
            # exceeding the original execution deadline.
            settle_start = clock.monotonic()
            state.settle_effective = min(state.settle_configured,
                                         max(0.0, deadline - settle_start))
            if state.settle_effective <= 0:
                state.settle_status = 'EXECUTION_DEADLINE'
            settle_deadline = settle_start + state.settle_effective
            while True:
                if state.query_zero(api):
                    if state.settle_status != 'EXECUTION_DEADLINE':
                        state.settle_status = 'NATURAL_ZERO'
                    settle_elapsed = clock.monotonic() - settle_start
                    state.settle_elapsed = settle_elapsed
                    state.outcome = 'COMPLETED'
                    closeout_reason = 'COMPLETION'
                    result = OwnedCommandResult(state.command_id, 0)
                    break
                if state.job_query_failures and not state.job_query_ok:
                    state.settle_status = 'QUERY_ERROR'
                    settle_elapsed = clock.monotonic() - settle_start
                    state.settle_elapsed = settle_elapsed
                    break
                if clock.monotonic() >= settle_deadline:
                    state.settle_status = 'EXPIRED' if state.settle_status not in (
                        'EXECUTION_DEADLINE', 'QUERY_ERROR') else state.settle_status
                    settle_elapsed = clock.monotonic() - settle_start
                    state.settle_elapsed = settle_elapsed
                    break
                clock.sleep(min(state.poll_seconds,
                                max(0.0, settle_deadline - clock.monotonic())))
            if result is not None:
                break
            if state.settle_status == 'QUERY_ERROR':
                state.outcome = 'UNPROVEN'
                state.failure_reason = ('Job ActiveProcesses query failed during the '
                                        'natural settle window')
                state.terminate_reason = 'SETTLE_QUERY_ERROR'
                closeout_reason = 'SETTLE_QUERY_ERROR'
                result = OwnedCommandResult(state.command_id, 0)
                break
            # Settle window expired or execution deadline blocked settling with
            # descendants still alive: the command must not pass on root exit 0.
            state.outcome = 'FAILED_DESCENDANTS_AFTER_ROOT_EXIT'
            state.failure_reason = ('root exited 0 but owned descendants remained '
                                    f'(settle status {state.settle_status})')
            state.terminate_reason = 'DESCENDANTS_AFTER_ROOT_EXIT'
            closeout_reason = 'DESCENDANTS_AFTER_ROOT_EXIT'
            result = OwnedCommandResult(state.command_id, 0)
            break
    except BaseException as execution_error:
        if error is None:
            error = execution_error
            if closeout_reason is None:
                closeout_reason = ('INTERRUPTED' if isinstance(execution_error,
                                   (KeyboardInterrupt, SystemExit)) else 'LAUNCH_ERROR')
            if isinstance(execution_error, (OwnedCommandLaunchError, FileNotFoundError,
                                            ValueError)):
                state.outcome = 'LAUNCH_ERROR'

    if closeout_reason is None and error is not None:
        closeout_reason = 'LAUNCH_ERROR'
    final_error, final_result = _finalize(state, api, clock,
                                          closeout_reason=closeout_reason,
                                          result=result, error=error)
    if final_error is not None:
        raise final_error
    if state.owned_temporary is not None and final_result.receipt.get(
            'output', {}).get('spool_retained'):
        final_result.receipt['output']['spool_directory_note'] = (
            'private temporary spool retained after unproven cleanup or incomplete capture')
    return final_result
