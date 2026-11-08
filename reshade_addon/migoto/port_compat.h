#pragma once

#include <windows.h>

// Their global (port_globals.cpp), assigned in AddonInit.
extern HINSTANCE migoto_handle;

namespace MigotoPort {
void SetIniRoot(const wchar_t* dir);
const wchar_t* IniRoot(); // nullptr when unset
void MigotoDir(wchar_t* out, unsigned long count);
}

inline DWORD PortRealGetModuleFileNameW(HMODULE h, wchar_t* buf, DWORD n)
{
    return ::GetModuleFileNameW(h, buf, n);
}

inline DWORD PortGetModuleFileNameW(HMODULE h, wchar_t* buf, DWORD n)
{
    // Not their DLL (NULL = the exe, or any other module): answer truthfully.
    if (!h || h != migoto_handle || !buf || n == 0) return PortRealGetModuleFileNameW(h, buf, n);

    MigotoPort::MigotoDir(buf, n);
    if (!buf[0]) return 0;

    const size_t len = wcslen(buf);
    if (len + 10 < (size_t)n) wcscpy_s(buf + len, (size_t)n - len, L"d3d11.dll");
    return (DWORD)wcslen(buf);
}

#undef GetModuleFileName
#undef GetModuleFileNameW
#define GetModuleFileNameW PortGetModuleFileNameW
#define GetModuleFileName PortGetModuleFileNameW
