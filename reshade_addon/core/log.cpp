#include "log.h"
#include <string>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <share.h>
#include <locale.h> // _create_locale / _locale_t for the per-call UTF-8 formatting locale

namespace logger {
// Non-inline storage to ensure a single instance across the DLL
FILE* g_log_file = nullptr;
CRITICAL_SECTION g_log_cs;
bool g_log_inited = false;
std::wstring g_log_path;

DWORD copy_to(const wchar_t* dst)
{
    if (!g_log_inited) return ERROR_FILE_NOT_FOUND;
    HANDLE in = CreateFileW(g_log_path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (in == INVALID_HANDLE_VALUE) return GetLastError();
    HANDLE out = CreateFileW(dst, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        CloseHandle(in);
        return err;
    }
    DWORD err = ERROR_SUCCESS;
    std::vector<char> buf(1 << 20);
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(in, buf.data(), (DWORD)buf.size(), &got, nullptr)) {
            err = GetLastError();
            break;
        }
        if (!got) break;
        DWORD put = 0;
        if (!WriteFile(out, buf.data(), got, &put, nullptr) || put != got) {
            err = GetLastError();
            break;
        }
    }
    CloseHandle(out);
    CloseHandle(in);
    return err;
}

void init(const wchar_t* log_path)
{
    if (g_log_inited) return;
    InitializeCriticalSection(&g_log_cs);
    g_log_path = log_path;
    {
        std::wstring prev(log_path);
        size_t dot = prev.find_last_of(L'.');
        prev =
            (dot == std::wstring::npos) ? (prev + L".prev.log") : (prev.substr(0, dot) + L".prev" + prev.substr(dot));
        _wremove(prev.c_str());
        (void)_wrename(log_path, prev.c_str());
    }
    g_log_file = _wfsopen(log_path, L"w", _SH_DENYNO);
    g_log_inited = true;
}

int g_level = 0; // 0 = OFF: nothing is written at all until the debug toggle raises it
const char* g_build_id = "unknown";

_locale_t utf8_locale()
{
    static _locale_t loc = _create_locale(LC_CTYPE, ".UTF-8");
    return loc; // null if unavailable; the _l calls below fall back to the C locale, ASCII-only
}

namespace {
struct Site {
    double tokens;
    ULONGLONG last;
    unsigned dropped;
    bool said;
};
std::unordered_map<const char*, Site> g_sites; // by format string; under g_log_cs
std::atomic<bool> g_unlimited{ false };
constexpr double kBurst = 100.0;
constexpr double kRefillMs = 10000.0;
}

void set_unlimited(bool on)
{
#ifdef WWMI_DEBUG
    g_unlimited.store(on, std::memory_order_relaxed);
#else
    (void)on;
#endif
}
bool unlimited()
{
    return g_unlimited.load(std::memory_order_relaxed);
}

static void vwrite(const char* fmt, va_list args, bool limited = true)
{
    if (!g_log_file) return;
    EnterCriticalSection(&g_log_cs);

    unsigned dropped = 0;
    bool notice = false;
    if (limited && !unlimited()) {
        const ULONGLONG now = GetTickCount64();
        Site& s = g_sites.try_emplace(fmt, Site{ kBurst, now, 0, false }).first->second;
        s.tokens += (double)(now - s.last) / kRefillMs;
        if (s.tokens > kBurst) s.tokens = kBurst;
        s.last = now;
        if (s.tokens < 1.0) {
            ++s.dropped;
            LeaveCriticalSection(&g_log_cs);
            return;
        }
        s.tokens -= 1.0;
        dropped = s.dropped;
        s.dropped = 0;
        if (s.tokens < 1.0 && !s.said) {
            s.said = true;
            notice = true;
        }
    }

    SYSTEMTIME t;
    GetLocalTime(&t);
    fprintf(g_log_file, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    char buf[4096];
    int n = _vsnprintf_s_l(buf, sizeof(buf), _TRUNCATE, fmt, utf8_locale(), args);
    if (n < 0) n = (int)strnlen(buf, sizeof(buf) - 1); // truncated: emit what fitted
    fwrite(buf, 1, (size_t)n, g_log_file);
    if (dropped) fprintf(g_log_file, " [+%u not shown]", dropped);
    fputc('\n', g_log_file);
    if (notice)
        fprintf(g_log_file,
            "[%02d:%02d:%02d.%03d] LOG-LIMIT: the line above keeps repeating -- from here it is written "
            "at most once every 10 s\n",
            t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    fflush(g_log_file);

    LeaveCriticalSection(&g_log_cs);
}

void set_level(int lvl)
{
#ifndef WWMI_DEBUG
    if (lvl > 2) lvl = 2;
#endif
    const int was = g_level;
    g_level = lvl;
    if (lvl >= 1 && was < 1) write("WWMI-DX12 build %s (log level %d)", g_build_id, lvl);
}

void write(_Printf_format_string_ const char* fmt, ...)
{
    if (g_level < 1) return;
    va_list args;
    va_start(args, fmt);
    vwrite(fmt, args);
    va_end(args);
}
// Level 2: one-off events (what loaded, what was recognised).
void info(_Printf_format_string_ const char* fmt, ...)
{
    if (g_level < 2) return;
    va_list args;
    va_start(args, fmt);
    vwrite(fmt, args);
    va_end(args);
}
// Level 3: per-frame / per-draw telemetry.
void trace(_Printf_format_string_ const char* fmt, ...)
{
    if (g_level < 3) return;
    va_list args;
    va_start(args, fmt);
    vwrite(fmt, args);
    va_end(args);
}
void report(_Printf_format_string_ const char* fmt, ...)
{
    if (g_level < 1) return;
    va_list args;
    va_start(args, fmt);
    vwrite(fmt, args, false);
    va_end(args);
}
}
