// dump_watchdog.cpp -- OURS. PORT-LEAF. See dump_watchdog.h.
#include "dump_watchdog.h"
#include "../core/log.h"
#include <windows.h>
#include <cstring>
#include <atomic>
#include <chrono>
#include <thread>

namespace MigotoPort {

namespace {
std::atomic<const char*> g_stage{ "idle" };
std::atomic<unsigned long long> g_detail{ 0 };
std::atomic<unsigned long> g_tid{ 0 };
std::atomic<unsigned> g_gen{ 0 };
std::atomic<bool> g_run{ false };
struct WatchdogThread {
    std::thread t;
    ~WatchdogThread()
    {
        if (t.joinable()) t.detach();
    }
} g_thread;
}

namespace {
unsigned CaptureThreadStack(DWORD tid, void** frames, unsigned cap)
{
    HANDLE h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) {
        logger::write("DUMP WATCHDOG: OpenThread(%lu) failed (%lu)", tid, GetLastError());
        return 0;
    }
    unsigned n = 0;
    DWORD ctx_err = 0; // logged only after the thread runs again: it may hold the log's lock
    bool walk_faulted = false;
    if (SuspendThread(h) != (DWORD)-1) {
        CONTEXT ctx;
        memset(&ctx, 0, sizeof(ctx));
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(h, &ctx)) {
            __try {
                for (; n < cap && ctx.Rip; ++n) {
                    frames[n] = (void*)ctx.Rip;
                    DWORD64 base = 0;
                    RUNTIME_FUNCTION* fe = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                    if (!fe) {
                        ctx.Rip = *(DWORD64*)ctx.Rsp;
                        ctx.Rsp += 8;
                    } else {
                        void* hd = nullptr;
                        DWORD64 est = 0;
                        RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, fe, &ctx, &hd, &est, nullptr);
                    }
                }
            } __except (EXCEPTION_EXECUTE_HANDLER) {
                walk_faulted = true;
            }
        } else
            ctx_err = GetLastError() ? GetLastError() : 1;
        ResumeThread(h);
        if (ctx_err) logger::write("DUMP WATCHDOG: GetThreadContext(%lu) failed (%lu)", tid, ctx_err);
        if (walk_faulted)
            logger::write("DUMP WATCHDOG: stack walk of thread %lu stopped by a fault after %u frames", tid, n);
    } else
        logger::write("DUMP WATCHDOG: SuspendThread(%lu) failed (%lu)", tid, GetLastError());
    CloseHandle(h);
    return n;
}

}

void LogThreadStack(unsigned long tid, const char* tag)
{
    void* frames[48] = {};
    const unsigned n = CaptureThreadStack(tid, frames, 48);
    logger::write("%s: stack of thread %lu (%u frames, innermost first)", tag, tid, n);
    DumpLogFrames(tag, frames, n);
}

void DumpLogFrames(const char* tag, void* const* frames, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) {
        HMODULE hm = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)frames[i], &hm) &&
            hm) {
            char full[MAX_PATH];
            if (GetModuleFileNameA(hm, full, MAX_PATH)) {
                const char* b = strrchr(full, '\\');
                strncpy_s(name, b ? b + 1 : full, _TRUNCATE);
            }
            logger::write("%s:   [%02u] %p  %s+0x%llx", tag, i, frames[i], name,
                (unsigned long long)((uintptr_t)frames[i] - (uintptr_t)hm));
        } else
            logger::write("%s:   [%02u] %p  (no module)", tag, i, frames[i]);
    }
}

void DumpStage(const char* stage, unsigned long long detail)
{
    g_stage.store(stage, std::memory_order_relaxed);
    g_detail.store(detail, std::memory_order_relaxed);
    g_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    g_gen.fetch_add(1, std::memory_order_release);
}

const char* DumpStageName()
{
    return g_stage.load(std::memory_order_relaxed);
}
unsigned long long DumpStageDetail()
{
    return g_detail.load(std::memory_order_relaxed);
}

void DumpWatchdogStart()
{
    if (g_run.exchange(true)) return;
    DumpStage("armed");
    g_thread.t = std::thread([] {
        unsigned last_gen = g_gen.load(std::memory_order_acquire);
        unsigned quiet = 0;
        while (g_run.load(std::memory_order_relaxed)) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            const unsigned gen = g_gen.load(std::memory_order_acquire);
            if (gen != last_gen) {
                last_gen = gen;
                quiet = 0;
                continue;
            }
            ++quiet;
            if (quiet % 5 == 0)
                logger::write("DUMP WATCHDOG: no progress for %u s -- stage '%s' detail=%llx thread=%lu", quiet,
                    g_stage.load(std::memory_order_relaxed), g_detail.load(std::memory_order_relaxed),
                    g_tid.load(std::memory_order_relaxed));
            if (quiet == 10 || quiet % 60 == 0) LogThreadStack(g_tid.load(std::memory_order_relaxed), "DUMP WATCHDOG");
        }
    });
}

void DumpWatchdogStop()
{
    if (!g_run.exchange(false)) return;
    if (g_thread.t.joinable()) g_thread.t.join();
    DumpStage("idle");
}

}
