"""One native synthetic observation; no file-reader guard is changed or inferred."""
import ctypes
from ctypes import wintypes
import hashlib
import json
import os
from pathlib import Path
import sys
import tempfile


def observe_windows_stat_fields():
    if os.name != 'nt':
        raise RuntimeError('native Windows observation required')
    import msvcrt

    class BasicInfo(ctypes.Structure):
        _fields_ = [('CreationTime', ctypes.c_int64), ('LastAccessTime', ctypes.c_int64),
                    ('LastWriteTime', ctypes.c_int64), ('ChangeTime', ctypes.c_int64),
                    ('FileAttributes', wintypes.DWORD)]

    class IdentityInfo(ctypes.Structure):
        _fields_ = [('VolumeSerialNumber', ctypes.c_uint64), ('FileId', ctypes.c_ubyte * 16)]

    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.GetFileInformationByHandleEx.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID, wintypes.DWORD]
    kernel.GetFileInformationByHandleEx.restype = wintypes.BOOL

    def native(fd):
        handle = wintypes.HANDLE(msvcrt.get_osfhandle(fd))
        basic, identity = BasicInfo(), IdentityInfo()
        if not kernel.GetFileInformationByHandleEx(handle, 0, ctypes.byref(basic), ctypes.sizeof(basic)):
            raise ctypes.WinError(ctypes.get_last_error())
        if not kernel.GetFileInformationByHandleEx(handle, 18, ctypes.byref(identity), ctypes.sizeof(identity)):
            raise ctypes.WinError(ctypes.get_last_error())
        return dict(basic={name: getattr(basic, name) for name, _ in BasicInfo._fields_},
                    volume_serial=identity.VolumeSerialNumber, file_id_128=bytes(identity.FileId).hex(),
                    last_write_unix_ns=(basic.LastWriteTime - 116444736000000000) * 100,
                    creation_unix_ns=(basic.CreationTime - 116444736000000000) * 100,
                    change_unix_ns=(basic.ChangeTime - 116444736000000000) * 100)

    fields = ('st_dev', 'st_ino', 'st_mode', 'st_nlink', 'st_size', 'st_mtime_ns',
              'st_ctime_ns', 'st_birthtime_ns')
    current_guard_fields = ('st_dev', 'st_ino', 'st_size', 'st_mtime_ns', 'st_ctime_ns')

    def values(info):
        return {name: getattr(info, name, None) for name in fields}

    rows = []
    with tempfile.TemporaryDirectory(prefix='wls-synthetic-stat-probe-') as temporary:
        for sample in ('new_file', 'same_size_overwrite', 'explicit_ns_timestamp'):
            path = Path(temporary) / (sample + '.txt')
            path.write_bytes(b'synthetic-file-stat-observation-0001\n')
            if sample == 'same_size_overwrite':
                path.write_bytes(b'synthetic-file-stat-observation-0002\n')
            if sample == 'explicit_ns_timestamp':
                os.utime(path, ns=(1700000000123456700, 1700000000123456700))
            before = values(path.lstat())
            flags = os.O_RDONLY | getattr(os, 'O_BINARY', 0) | getattr(os, 'O_NOFOLLOW', 0) | getattr(os, 'O_NONBLOCK', 0)
            with os.fdopen(os.open(path, flags), 'rb') as stream:
                opened = values(os.fstat(stream.fileno()))
                native_before = native(stream.fileno())
                raw = stream.read(4097)
                after = values(os.fstat(stream.fileno()))
                native_after = native(stream.fileno())
            final = values(path.lstat())
            stages = dict(path_before=before, descriptor_before=opened, descriptor_after=after, path_after=final)
            rows.append(dict(sample=sample, python_observations=stages,
                differing_guard_fields=[name for name in current_guard_fields if len({entry[name] for entry in stages.values()}) != 1],
                native_before=native_before, native_after=native_after,
                captured_bytes=len(raw), synthetic_sha256=hashlib.sha256(raw).hexdigest()))
    version = sys.getwindowsversion()
    return dict(schema='wls-windows-synthetic-stat-observation-v1', python_version=list(sys.version_info[:3]),
        os_version=dict(major=version.major, minor=version.minor, build=version.build, product_type=version.product_type),
        pointer_bytes=ctypes.sizeof(ctypes.c_void_p), native_basic_struct_bytes=ctypes.sizeof(BasicInfo),
        native_id_struct_bytes=ctypes.sizeof(IdentityInfo), sample_count=len(rows), samples=rows,
        product_test=False, guard_changed=False, cause_inferred=False, real_paths_or_contents_emitted=False)


def emit_windows_stat_probe():
    try:
        result = observe_windows_stat_fields()
    except Exception as error:
        result = dict(schema='wls-windows-synthetic-stat-observation-v1', observation='UNAVAILABLE',
            error_type=type(error).__name__, winerror=getattr(error, 'winerror', None),
            product_test=False, guard_changed=False, cause_inferred=False,
            real_paths_or_contents_emitted=False)
    print(json.dumps(result, sort_keys=True), flush=True)
