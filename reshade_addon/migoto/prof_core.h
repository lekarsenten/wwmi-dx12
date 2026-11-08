#pragma once
#include <atomic>
#include <cstdint>
#include <windows.h>

inline constexpr int kProfMaxBuckets = 96; // room for the addon's bucket list (static_assert in profiler.h)

enum ProfGwKind {
    PGW_ShimCopy = 0, // Dx12Context::CopyResource / CopySubresourceRegion, buffer -> buffer (bytes)
    PGW_ShimCopyTex, // the same calls on textures (count only)
    PGW_Upload, // FlushPendingUploads, buffer: every Unmap / UpdateSubresource / initial data (bytes)
    PGW_UploadTex, // FlushPendingUploads, texture (bytes = the staging footprint)
    PGW_SkCopy, // the shapekey chain's own CopyBufferRegion calls (bytes)
    PGW_ViewCreate, // a D3D11 view created by the shim: one staging descriptor each (count only)
    PGW_Barrier, // Transition(): a ResourceBarrier recorded (count only)
    PGW_Promoted, // Transition(): a state change taken by implicit promotion, no barrier (count only)
    PGW_UavBarrier, // a UAV barrier a compute write owed, recorded (count only, per resource)
    PGW_UavCovered, // a UAV barrier a compute write owed, covered by the transition its next access took (count only)
    PGW_ReadKept, // OwnedTransition: a read of a buffer already in a read state that includes it, no barrier (count only)
    PGW_UploadDedup, // QueueTextureUpload: an update written over one still queued for the same texture (count only)
    PGW_COUNT
};

// The lib's timed steps. The addon binds each to its row (profiler.h, ProfBindLibSteps).
enum ProfLibStep {
    PL_UploadQueue = 0, // QueueUpload / QueueTextureUpload: MakeStaging + Map + memcpy
    PL_UploadFlush, // FlushPendingUploads: copies + transitions + staging retire
    PL_IssueDraw, // MigotoPort::IssueDraw
    PL_IssueDispatch, // MigotoPort::IssueDispatch
    PL_SK_Lock, // EngineLock WAIT inside RunResolvedCommandList
    PL_Disp_Lock, // EngineLock WAIT in DispatchShaderOverride
    PL_DI_PsLookup, // RunSectionByName: the fq name -> section lookup
    PL_PS_Trace, // CommandTrampoline: ini_line narrowed per command
    PL_PS_Observe, // TypedTrampoline: registry keys narrowed + the addon's typed hooks
    PL_TexSel_Engine, // SelectTextureOverride for a hash that HAS a section
    PL_TexSel_Lock, // EngineLock WAIT in it
    PL_COUNT
};

inline bool g_prof_enabled = false;

inline constexpr int kProfThreads = 24;
inline std::atomic<uint32_t> g_prof_tid[kProfThreads]; // 0 = unclaimed; last slot = OTHER
inline std::atomic<uint64_t> g_prof_ns_t[kProfThreads][kProfMaxBuckets]; // INCLUSIVE, this frame, per thread
inline std::atomic<uint64_t> g_prof_self_ns_t[kProfThreads][kProfMaxBuckets]; // EXCLUSIVE, same
inline std::atomic<uint32_t> g_prof_calls_t[kProfThreads][kProfMaxBuckets]; // call count, same
inline std::atomic<int> g_prof_thread_n{ 0 }; // slots claimed (drain only these)

inline int ProfThreadSlot()
{
    static thread_local int slot = -1;
    if (slot >= 0) return slot;
    const uint32_t me = GetCurrentThreadId();
    for (int i = 0; i < kProfThreads - 1; ++i) {
        if (g_prof_tid[i].load(std::memory_order_relaxed) == me) {
            slot = i;
            return slot;
        }
        uint32_t expect = 0;
        if (g_prof_tid[i].compare_exchange_strong(expect, me, std::memory_order_relaxed)) {
            g_prof_thread_n.store(i + 1 > g_prof_thread_n.load(std::memory_order_relaxed)
                    ? i + 1
                    : g_prof_thread_n.load(std::memory_order_relaxed),
                std::memory_order_relaxed);
            slot = i;
            return slot;
        }
    }
    slot = kProfThreads - 1; // OTHER
    g_prof_tid[slot].store(0xFFFFFFFFu, std::memory_order_relaxed);
    g_prof_thread_n.store(kProfThreads, std::memory_order_relaxed);
    return slot;
}

// The calibrated cost of one ProfScope (2 QPC reads + the relaxed atomic adds), for the overhead estimate.
inline double g_prof_percall_ns = 0.0; // calibrated once when the profiler is first enabled

inline std::atomic<uint64_t> g_gw_n[PGW_COUNT];
inline std::atomic<uint64_t> g_gw_bytes[PGW_COUNT];
inline void ProfGpuWork(int kind, uint64_t bytes)
{
    if (!g_prof_enabled) return;
    g_gw_n[kind].fetch_add(1, std::memory_order_relaxed);
    g_gw_bytes[kind].fetch_add(bytes, std::memory_order_relaxed);
}

inline int g_prof_lib_bucket[PL_COUNT] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };

inline int64_t prof_qpc_freq()
{
    static int64_t f = [] {
        LARGE_INTEGER li;
        QueryPerformanceFrequency(&li);
        return li.QuadPart;
    }();
    return f;
}
inline int64_t prof_qpc_now()
{
    LARGE_INTEGER li;
    QueryPerformanceCounter(&li);
    return li.QuadPart;
}

inline void ProfCalibrate()
{
    if (g_prof_percall_ns > 0.0) return;
    static std::atomic<uint64_t> sink{ 0 };
    static std::atomic<uint64_t> sink2{ 0 };
    static std::atomic<uint32_t> sink3{ 0 };
    const int N = 50000;
    const int64_t t0 = prof_qpc_now();
    for (int i = 0; i < N; ++i) {
        const int64_t a = prof_qpc_now();
        const int64_t b = prof_qpc_now();
        sink.fetch_add((uint64_t)(b - a), std::memory_order_relaxed);
        sink3.fetch_add(1u, std::memory_order_relaxed);
        sink2.fetch_add((uint64_t)(b - a), std::memory_order_relaxed);
    }
    const int64_t t1 = prof_qpc_now();
    const double per = (double)(t1 - t0) * 1000000000.0 / (double)prof_qpc_freq() / (double)N;
    g_prof_percall_ns = per > 0.0 ? per : 1.0; // never 0 (would hide overhead)
}

struct ProfScope;
inline thread_local ProfScope* t_prof_cur = nullptr;
struct ProfScope {
    int b;
    int64_t t0 = 0;
    uint64_t child = 0; // ns spent in scopes that ran DIRECTLY inside this one
    ProfScope* prev = nullptr; // the scope this one nests in, on this thread
    int slot = 0; // this thread's per-thread row
    explicit ProfScope(int bucket)
        : b(g_prof_enabled ? bucket : -1)
    {
        if (b >= 0) {
            slot = ProfThreadSlot();
            prev = t_prof_cur;
            t_prof_cur = this;
            t0 = prof_qpc_now();
        }
    }
    explicit ProfScope(ProfLibStep step)
        : ProfScope(g_prof_lib_bucket[step])
    {}
    void End()
    {
        if (b < 0) return;
        const uint64_t ns = (uint64_t)((prof_qpc_now() - t0) * 1000000000.0 / (double)prof_qpc_freq());
        const uint64_t self = (ns > child) ? (ns - child) : 0; // clamped: a clock wobble must not underflow
        g_prof_calls_t[slot][b].fetch_add(1u, std::memory_order_relaxed);
        g_prof_ns_t[slot][b].fetch_add(ns, std::memory_order_relaxed);
        g_prof_self_ns_t[slot][b].fetch_add(self, std::memory_order_relaxed);
        if (t_prof_cur == this) {
            t_prof_cur = prev;
            if (prev) prev->child += ns;
        }
        b = -1;
    }
    ~ProfScope() { End(); }
};
#define PROF_CONCAT2(a, b) a##b
#define PROF_CONCAT(a, b) PROF_CONCAT2(a, b)
#define PROF(bucket) ProfScope PROF_CONCAT(_prof_, __LINE__)(bucket)
