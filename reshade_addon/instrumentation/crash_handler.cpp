// ===========================================================================
//  crash_handler.cpp -- OURS. Fault reports for the port, module + offset per frame.
//
//  WHY ITS OWN FILE, not the addon log: src/log.h keeps level 0 (the default) silent. A crash report is
//  exactly the thing you cannot ask the user to have enabled in advance, so it goes to wwmi_dx12_crash.log
//  -- always written, never gated. The normal log keeps its contract.
//
//  LAST-CHANCE, deliberately. A first-chance vectored handler sees every exception the engine throws and
//  handles routinely, and symbol loading inside the dispatch takes the loader lock while the anti-cheat is
//  in the same chain.
//
//  So: SetUnhandledExceptionFilter, which fires only when nothing else handled the fault and the process is
//  dying anyway; no dbghelp (module + offset, resolved offline against wwmi_dx12.map); and the previous
//  filter is always chained, so the game's own crash reporting still runs. We observe, we never swallow.
//
//  There is no "is it ours" filter: a test for any frame inside our module is ALWAYS true, because this
//  handler is in our module and on every stack it captures. The '*' markers say which frames are ours.
// ===========================================================================
#include "crash_handler.h"
#include "diag_gate.h"
#include "../addon/addon.h"
#include "../build_stamp.h" // WWMI_VERSION / WWMI_BUILD_SHA -- which build's map the offsets need
#include "../core/log.h" // logger::utf8_locale -- the context is a wide section name

#include <windows.h>
#include <psapi.h>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <cstdarg>

namespace CrashHandler {

namespace {

LPTOP_LEVEL_EXCEPTION_FILTER g_prev_filter = nullptr;
bool g_installed = false;
HMODULE g_self = nullptr;
uintptr_t g_self_base = 0;
size_t g_self_size = 0;
std::atomic<int> g_reports{ 0 };
CRITICAL_SECTION g_cs;
bool g_cs_ready = false;

// Thread-local so concurrent draw-recording threads do not overwrite each other's breadcrumb.
thread_local const wchar_t* t_context = nullptr;

// Where the report goes. Next to the addon log, which is next to the executable.
wchar_t g_path[MAX_PATH] = L"wwmi_dx12_crash.log";

void ResolveSelf()
{
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            (LPCSTR)&ResolveSelf, &g_self) ||
        !g_self)
        return;
    MODULEINFO mi = {};
    // GetModuleInformation lives in psapi; K32 forwarder avoids the extra import library.
    if (GetModuleInformation(GetCurrentProcess(), g_self, &mi, sizeof(mi))) {
        g_self_base = (uintptr_t)mi.lpBaseOfDll;
        g_self_size = mi.SizeOfImage;
    }
}

bool AddressIsOurs(uintptr_t a)
{
    return g_self_size && a >= g_self_base && a < g_self_base + g_self_size;
}

const char* CodeName(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "ACCESS_VIOLATION";
    case EXCEPTION_STACK_OVERFLOW: return "STACK_OVERFLOW";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "ILLEGAL_INSTRUCTION";
    case EXCEPTION_PRIV_INSTRUCTION: return "PRIV_INSTRUCTION";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "INT_DIVIDE_BY_ZERO";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "ARRAY_BOUNDS_EXCEEDED";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "DATATYPE_MISALIGNMENT";
    case EXCEPTION_IN_PAGE_ERROR: return "IN_PAGE_ERROR";
    default: return nullptr; // not fatal / not ours to report
    }
}

void Write(FILE* f, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    _vfprintf_l(f, fmt, logger::utf8_locale(), ap); // the context line is a wide section name (%ls)
    va_end(ap);
}

LONG CALLBACK Handler(EXCEPTION_POINTERS* ep)
{
    if (!ep) return EXCEPTION_CONTINUE_SEARCH;
    const EXCEPTION_RECORD* rec = ep->ExceptionRecord;
    const DWORD code = rec ? rec->ExceptionCode : 0;
    const char* name = CodeName(code);
    if (!rec || !name) return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;

    if (g_reports.load() >= 8) return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;

    void* frames[62] = {};
    const USHORT n = RtlCaptureStackBackTrace(0, 62, frames, nullptr);

    if (g_reports.fetch_add(1) >= 8) return EXCEPTION_EXECUTE_HANDLER;
    if (g_cs_ready) EnterCriticalSection(&g_cs);

    FILE* f = nullptr;
    if (_wfopen_s(&f, g_path, L"a") || !f) {
        if (g_cs_ready) LeaveCriticalSection(&g_cs);
        return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
    }

    SYSTEMTIME st;
    GetLocalTime(&st);
    Write(f, "\n=============================================================================\n");
    Write(f, "[%02u:%02u:%02u.%03u] %s (0x%08X) at %p  thread %lu\n", st.wHour, st.wMinute, st.wSecond,
        st.wMilliseconds, name, code, rec->ExceptionAddress, GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        const ULONG_PTR op = rec->ExceptionInformation[0];
        Write(f, "  %s address %p\n",
            op == 0       ? "reading"
                : op == 1 ? "writing"
                          : "executing",
            (void*)rec->ExceptionInformation[1]);
    }
    if (t_context) Write(f, "  context: %ls\n", t_context);
    Write(f, "  module base %p size 0x%zx, build %s %s  (subtract the base for that build's map-file offsets)\n",
        (void*)g_self_base, g_self_size, WWMI_VERSION, WWMI_BUILD_SHA);

    Write(f, "  stack (%u frames, '*' = ours):\n", (unsigned)n);
    for (USHORT i = 0; i < n; ++i) {
        const uintptr_t a = (uintptr_t)frames[i];
        const bool mine = AddressIsOurs(a);
        char modname[MAX_PATH] = "?";
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)a, &hm) &&
            hm) {
            char full[MAX_PATH];
            if (GetModuleFileNameA(hm, full, MAX_PATH)) {
                const char* b = strrchr(full, '\\');
                strncpy_s(modname, b ? b + 1 : full, _TRUNCATE);
            }
        }
        Write(f, "   %c [%02u] %p  %-28s", mine ? '*' : ' ', (unsigned)i, (void*)a, modname);
        if (mine) Write(f, "  +0x%zx  <- look up in wwmi_dx12.map", (size_t)(a - g_self_base));
        Write(f, "\n");
    }
    fflush(f);
    fclose(f);
    if (g_cs_ready) LeaveCriticalSection(&g_cs);
    return g_prev_filter ? g_prev_filter(ep) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

int Filter(void* exception_pointers, const char* where)
{
    EXCEPTION_POINTERS* ep = (EXCEPTION_POINTERS*)exception_pointers;
    const DWORD code = ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0;
    static std::atomic<int> n{ 0 };
    if (n.fetch_add(1) < 16) {
        const char* nm = CodeName(code);
        if (g_cs_ready) EnterCriticalSection(&g_cs);
        FILE* f = nullptr;
        if (!_wfopen_s(&f, g_path, L"a") && f) {
            SYSTEMTIME st;
            GetLocalTime(&st);
            Write(f, "\n[%02u:%02u:%02u.%03u] CAUGHT in %s: %s (0x%08X) at %p  thread %lu\n", st.wHour, st.wMinute,
                st.wSecond, st.wMilliseconds, where ? where : "?", nm ? nm : "exception", code,
                ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr, GetCurrentThreadId());
            if (t_context) Write(f, "  context: %ls\n", t_context);
            Write(f, "  module base %p, build %s %s  (offsets resolve in that build's wwmi_dx12.map)\n",
                (void*)g_self_base, WWMI_VERSION, WWMI_BUILD_SHA);
            void* frames[32] = {};
            const USHORT fn = RtlCaptureStackBackTrace(0, 32, frames, nullptr);
            for (USHORT i = 0; i < fn; ++i) {
                const uintptr_t a = (uintptr_t)frames[i];
                if (AddressIsOurs(a))
                    Write(f, "   * [%02u] %p  +0x%zx\n", (unsigned)i, (void*)a, (size_t)(a - g_self_base));
                else
                    Write(f, "     [%02u] %p\n", (unsigned)i, (void*)a);
            }
            Write(f, "  -> skipped this call; the add-on is off until the game restarts\n");
            fflush(f);
            fclose(f);
        }
        if (g_cs_ready) LeaveCriticalSection(&g_cs);
    }
    g_addon.faulted.store(true);
    return EXCEPTION_EXECUTE_HANDLER; // swallow: skip this callback, keep the frame alive
}

void SetContext(const wchar_t* what)
{
    t_context = what;
}
void ClearContext()
{
    t_context = nullptr;
}
const wchar_t* Context()
{
    return t_context;
}

void FormatCallers(char* buf, size_t cap, unsigned skip)
{
    if (!buf || !cap) return;
    buf[0] = 0;
    void* frames[40] = {};
    const USHORT n = RtlCaptureStackBackTrace(skip + 1, 40, frames, nullptr);
    size_t at = 0;
    for (USHORT i = 0; i < n && at + 1 < cap; ++i) {
        const uintptr_t a = (uintptr_t)frames[i];
        char modname[MAX_PATH] = "?";
        uintptr_t base = 0;
        HMODULE hm = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)a, &hm) &&
            hm) {
            base = (uintptr_t)hm;
            char full[MAX_PATH];
            if (GetModuleFileNameA(hm, full, MAX_PATH)) {
                const char* b = strrchr(full, '\\');
                strncpy_s(modname, b ? b + 1 : full, _TRUNCATE);
            }
        }
        const int w = _snprintf_s(buf + at, cap - at, _TRUNCATE, "%s%s%s+0x%zx", i ? " < " : "",
            AddressIsOurs(a) ? "*" : "", modname, (size_t)(a - base));
        if (w < 0) break;
        at += (size_t)w;
    }
}

void InitReporting()
{
    static bool once = false;
    if (once) return;
    once = true;
    InitializeCriticalSection(&g_cs);
    g_cs_ready = true;
    ResolveSelf();
    wchar_t exe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) {
            *slash = 0;
            _snwprintf_s(g_path, MAX_PATH, _TRUNCATE, L"%s\\wwmi_dx12_crash.log", exe);
        }
    }
}

void Install()
{
    if (g_installed) return;
    g_installed = true;
    InitReporting();
    (void)logger::utf8_locale(); // created now, so the report never allocates one while the process dies
    g_prev_filter = SetUnhandledExceptionFilter(Handler);
    if (g_prev_filter == Handler) g_prev_filter = nullptr;
}

void Uninstall()
{
    if (g_installed) {
        SetUnhandledExceptionFilter(g_prev_filter);
        g_installed = false;
    }
    if (g_cs_ready) {
        DeleteCriticalSection(&g_cs);
        g_cs_ready = false;
    }
}

} // namespace CrashHandler
