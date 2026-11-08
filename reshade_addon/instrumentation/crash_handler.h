#pragma once
// ===========================================================================
//  crash_handler.h -- OURS. Fault reporting for a port that runs someone else's engine.
//
//  When the port faults, the game dies with a Windows error box and the addon log ends mid-line -- which
//  tells us WHEN and nothing about WHERE.
//
//  This writes the call stack (module + offset per frame) to its OWN file the moment a fault happens.
// ===========================================================================

namespace CrashHandler {

void InitReporting(); // always safe; sets up the report file + module range
// Installs the handler. Safe to call once at addon init.
void Install();
void Uninstall();

void SetContext(const wchar_t* what);
void ClearContext();
const wchar_t* Context();
void FormatCallers(char* buf, size_t cap, unsigned skip);

struct ScopedContext {
    explicit ScopedContext(const wchar_t* what) { SetContext(what); }
    ~ScopedContext() { ClearContext(); }
    ScopedContext(const ScopedContext&) = delete;
    ScopedContext& operator=(const ScopedContext&) = delete;
};

int Filter(void* exception_pointers, const char* where);

// MSVC refuses __try in a function that needs C++ unwinding (C2712), and our callbacks are full of
// std::string and lock_guard. So the guard is a thin outer function holding no C++ objects, calling the
// real body. Use as:
//
//     static bool on_draw_indexed_body(...)  { ...the existing code... }
//     static bool on_draw_indexed(...)
//     { WWMI_GUARD_BEGIN("on_draw_indexed") return on_draw_indexed_body(...); WWMI_GUARD_END(false) }
//
#define WWMI_GUARD_BEGIN(where_) __try {
#define WWMI_GUARD_END(fallback_) \
    } \
    __except (::CrashHandler::Filter(GetExceptionInformation(), __FUNCTION__)) \
    { \
        return fallback_; \
    }
#define WWMI_GUARD_END_VOID() \
    } \
    __except (::CrashHandler::Filter(GetExceptionInformation(), __FUNCTION__)) \
    { \
        return; \
    }

} // namespace CrashHandler
