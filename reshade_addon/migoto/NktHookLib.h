#pragma once
// ===========================================================================
//  NktHookLib.h -- MINIMAL STUB. PORT-LEAF.
//
//  Nektra's Deviare hooking library. Third-party (a prebuilt library under the reference tree's Nektra\),
//  and 3dmigoto uses it chiefly to get its DLL in front of the game's d3d11.dll and hook device/context
//  creation.
//  ReShade already loaded us into the process and hands us the device, so there is nothing to hook.
//
//  Only the declarations their headers need are provided, so their sources compile unmodified.
//  DLLMainHook.h declares `extern CNktHookLib cHookMgr;` and globals.h consumes the EnableHooks enum
//  beside it -- the enum is theirs and is used when parsing `[System] hook =`, which we accept and
//  ignore (nothing is hooked, so no combination of values changes behaviour).
//
//  A call to Hook means their code reached a hooking path (cursor.cpp's mouse hooks, lock.cpp's lock
//  debugging), which cannot work here: every method returns a failure code rather than appearing to succeed.
// ===========================================================================

#include <windows.h>

// Their cursor.cpp uses these two helpers to find GetCursorInfo/ScreenToClient in user32 for the
// cursor-upscaling bypass. Plain Win32 equivalents: no hooking involved, so these are real, not inert.
namespace NktHookLibHelpers {
inline HINSTANCE GetModuleBaseAddress(LPCWSTR szModuleName)
{
    return szModuleName ? ::GetModuleHandleW(szModuleName) : nullptr;
}
inline LPVOID GetProcedureAddress(HINSTANCE hModule, LPCSTR szProcName)
{
    return (hModule && szProcName) ? (LPVOID)::GetProcAddress((HMODULE)hModule, szProcName) : nullptr;
}
}

class CNktHookLib {
public:
    DWORD Hook(SIZE_T* /*nHookId*/, LPVOID* /*lpCallOriginal*/, LPVOID /*lpProcToHook*/, LPVOID /*lpNewProcAddr*/,
        DWORD /*dwFlags*/ = 0)
    {
        return ERROR_NOT_SUPPORTED;
    }
    DWORD UnhookAll() { return ERROR_NOT_SUPPORTED; }
    void SetEnableDebugOutput(BOOL /*bEnable*/) {}
};
