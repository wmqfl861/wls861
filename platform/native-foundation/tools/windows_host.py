"""Read-only native host observations for the Windows 11 component entry.

These checks select an existing test machine. They change no token, account,
ACL, volume, execution policy or production capability.
"""
import ctypes
from ctypes import wintypes
import datetime
import os
import sys


def observe_host(source, work):
    if os.name != 'nt':
        raise RuntimeError('native Windows host observation is required')
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    security = ctypes.WinDLL('advapi32', use_last_error=True)
    kernel.GetCurrentProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    kernel.IsWow64Process2.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.USHORT),
                                      ctypes.POINTER(wintypes.USHORT)]
    kernel.IsWow64Process2.restype = wintypes.BOOL
    kernel.GetVolumePathNameW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD]
    kernel.GetVolumePathNameW.restype = wintypes.BOOL
    kernel.GetDriveTypeW.argtypes = [wintypes.LPCWSTR]
    kernel.GetDriveTypeW.restype = wintypes.UINT
    kernel.GetVolumeInformationW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
        wintypes.LPWSTR, wintypes.DWORD]
    kernel.GetVolumeInformationW.restype = wintypes.BOOL
    security.OpenProcessToken.argtypes = [wintypes.HANDLE, wintypes.DWORD, ctypes.POINTER(wintypes.HANDLE)]
    security.OpenProcessToken.restype = wintypes.BOOL
    security.GetTokenInformation.argtypes = [wintypes.HANDLE, ctypes.c_int, wintypes.LPVOID,
                                           wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)]
    security.GetTokenInformation.restype = wintypes.BOOL
    security.CreateWellKnownSid.argtypes = [ctypes.c_int, wintypes.LPVOID, wintypes.LPVOID,
                                          ctypes.POINTER(wintypes.DWORD)]
    security.CreateWellKnownSid.restype = wintypes.BOOL
    security.CheckTokenMembership.argtypes = [wintypes.HANDLE, wintypes.LPVOID, ctypes.POINTER(wintypes.BOOL)]
    security.CheckTokenMembership.restype = wintypes.BOOL

    def checked(value):
        if not value:
            raise ctypes.WinError(ctypes.get_last_error())

    process_machine, native_machine = wintypes.USHORT(), wintypes.USHORT()
    checked(kernel.IsWow64Process2(kernel.GetCurrentProcess(), ctypes.byref(process_machine),
                                  ctypes.byref(native_machine)))
    token = wintypes.HANDLE()
    checked(security.OpenProcessToken(kernel.GetCurrentProcess(), 0x0008, ctypes.byref(token)))  # TOKEN_QUERY
    try:
        def token_dword(kind):
            value, length = wintypes.DWORD(), wintypes.DWORD()
            checked(security.GetTokenInformation(token, kind, ctypes.byref(value), ctypes.sizeof(value),
                                                 ctypes.byref(length)))
            if length.value != ctypes.sizeof(value):
                raise RuntimeError('unexpected token observation size')
            return value.value
        elevated = token_dword(20)  # TokenElevation
        elevation_type = token_dword(18)  # TokenElevationType; 1=default, 2=full, 3=limited
        admin_sid, length = ctypes.create_string_buffer(68), wintypes.DWORD(68)
        checked(security.CreateWellKnownSid(26, None, admin_sid, ctypes.byref(length)))  # Administrators
        member = wintypes.BOOL()
        # NULL observes the current effective token, including thread impersonation.
        checked(security.CheckTokenMembership(None, admin_sid, ctypes.byref(member)))
    finally:
        checked(kernel.CloseHandle(token))

    volumes = {}
    for label, path in (('source', source), ('work', work)):
        volume, filesystem = ctypes.create_unicode_buffer(32768), ctypes.create_unicode_buffer(256)
        checked(kernel.GetVolumePathNameW(str(path), volume, len(volume)))
        checked(kernel.GetVolumeInformationW(volume.value, None, 0, None, None, None,
                                             filesystem, len(filesystem)))
        volumes[label] = dict(filesystem=filesystem.value, drive_type=kernel.GetDriveTypeW(volume.value))
    version = sys.getwindowsversion()
    return dict(schema='wls-native-windows11-host-v1',
        observed_at_utc=datetime.datetime.now(datetime.timezone.utc).isoformat(),
        source='GetTokenInformation/CheckTokenMembership/IsWow64Process2/GetVolumeInformationW/sys.getwindowsversion',
        os_major=version.major, os_minor=version.minor, os_build=version.build,
        os_product_type=version.product_type, native_machine=native_machine.value,
        process_machine=process_machine.value, python_pointer_bytes=ctypes.sizeof(ctypes.c_void_p),
        token_elevated=elevated, token_elevation_type=elevation_type,
        effective_administrators_member=bool(member.value), volumes=volumes)


def require_windows11(facts):
    if (facts.get('os_major') != 10 or type(facts.get('os_build')) is not int or
            facts['os_build'] < 22000 or facts.get('os_product_type') != 1):
        raise RuntimeError('Windows 11 workstation build 22000+ is required; Server does not qualify')
    if (facts.get('native_machine') != 0x8664 or facts.get('process_machine') != 0 or
            facts.get('python_pointer_bytes') != 8):
        raise RuntimeError('native x64 Windows and x64 Python are required')
    if (facts.get('token_elevated') != 0 or facts.get('token_elevation_type') != 1 or
            facts.get('effective_administrators_member') is not False):
        raise RuntimeError('an ordinary non-elevated non-administrator token is required')
    volumes = facts.get('volumes', {})
    if set(volumes) != {'source', 'work'} or any(
            value.get('filesystem') != 'NTFS' or value.get('drive_type') != 3 for value in volumes.values()):
        raise RuntimeError('source and work must be on fixed local NTFS volumes')
