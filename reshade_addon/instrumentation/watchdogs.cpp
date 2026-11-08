// watchdogs.cpp -- see watchdogs.h.
#include "../addon_common.h"
#include "watchdogs.h"
#include "removal_pin.h" // DumpOnStall -- the close / execute rings (a failed Close asserts the engine)
#include "diag_arms.h"
#include "../addon/debug_layer.h"
#include "../core/frame_clock.h"
#include "../core/log.h"
#include "../mods/ib_hash.h"
#include "../migoto/dump_watchdog.h" // LogThreadStack -- the suspend-and-unwind stack of a thread
#include <chrono>

using namespace reshade::api;

Watchdogs g_watchdogs;

void Watchdogs::StallTick(bool focused)
{
    present_tid_.store(GetCurrentThreadId(), std::memory_order_relaxed);
    focused_.store(focused, std::memory_order_relaxed);
    ticks_.fetch_add(1, std::memory_order_release);
    bool expected = false;
    if (!stall_started_.compare_exchange_strong(expected, true)) return;
    std::lock_guard<std::mutex> lk(stall_thread_mutex_);
    if (stall_stop_.load(std::memory_order_acquire)) return; // stopped before it could start (teardown)
    stall_thread_.t = std::thread([this] { StallRun(); });
    logger::report("STALL watchdog started (threshold %u ms, 100 ms poll)", g_diag_arms.stall_ms);
}

void Watchdogs::StallRun()
{
    using clock = std::chrono::steady_clock;
    uint32_t last = ticks_.load(std::memory_order_acquire);
    clock::time_point since = clock::now();
    bool stalled = false;
    unsigned stalls = 0;
    uint32_t stall_frame = 0;
    while (!stall_stop_.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (stall_stop_.load(std::memory_order_acquire)) break;
        const uint32_t now = ticks_.load(std::memory_order_acquire);
        if (now != last) {
            if (stalled) {
                const unsigned ms =
                    (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - since).count();
                logger::report("STALL #%u ended: presents resumed after %u ms (frame %u -> %u)", stalls, ms,
                    stall_frame, g_frame_id);
                stalled = false;
            }
            last = now;
            since = clock::now();
            continue;
        }
        if (!g_diag_arms.stall_watchdog || stalled) continue;
        // Not while the game sits in the background: it presents nothing there, and that is not a stall.
        if (!focused_.load(std::memory_order_relaxed)) {
            since = clock::now();
            continue;
        }
        const unsigned ms =
            (unsigned)std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - since).count();
        if (ms < g_diag_arms.stall_ms) continue;
        stalled = true;
        ++stalls;
        stall_frame = g_frame_id;
        g_diag_arms.stall_count.fetch_add(1, std::memory_order_relaxed);
        if (stalls > 32 && (stalls % 64) != 0) continue;
        const unsigned long tid = present_tid_.load(std::memory_order_relaxed);
        logger::report("STALL #%u: no present for %u ms (frame %u, threshold %u ms) -- the present thread %lu:", stalls,
            ms, stall_frame, g_diag_arms.stall_ms, tid);
        MigotoPort::LogThreadStack(tid, "STALL");
        IbHash::LogInflight("stall");
        RemovalPin::DumpOnStall(tid);
    }
}

void Watchdogs::DumpDeviceRemoval(ID3D12Device* d3d, HRESULT rr)
{
    const char* what = (rr == DXGI_ERROR_DEVICE_HUNG) ? "DEVICE_HUNG (GPU stopped responding -- TDR)"
        : (rr == DXGI_ERROR_DEVICE_REMOVED)           ? "DEVICE_REMOVED"
        : (rr == DXGI_ERROR_DEVICE_RESET)             ? "DEVICE_RESET"
        : (rr == DXGI_ERROR_DRIVER_INTERNAL_ERROR)    ? "DRIVER_INTERNAL_ERROR"
        : (rr == DXGI_ERROR_INVALID_CALL)             ? "INVALID_CALL (our command stream)"
                                                      : "unknown";
    logger::report("*** DEVICE REMOVED: hr=%08x %s  frame=%llu", (unsigned)rr, what, (unsigned long long)g_frame_id);
    IbHash::LogInflight("device removed");
    g_debug_layer.Drain("at removal");
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (SUCCEEDED(d3d->QueryInterface(IID_PPV_ARGS(&dred))) && dred) {
        ID3D12DeviceRemovedExtendedData1* dred1 = nullptr;
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT1 bc1{};
        const bool have1 = SUCCEEDED(dred->QueryInterface(IID_PPV_ARGS(&dred1))) && dred1 &&
            SUCCEEDED(dred1->GetAutoBreadcrumbsOutput1(&bc1));
        if (have1) {
            int n = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE1* node = bc1.pHeadAutoBreadcrumbNode; node && n < 256;
                node = node->pNext, ++n) {
                const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                char ours[240];
                RemovalPin::Describe(node->pCommandList, ours, sizeof ours);
                logger::report("  DRED node %d: cmdlist='%S' %p queue='%S' %p executed %u of %u ops, %u marker(s) | %s",
                    n, node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"(unnamed)",
                    (void*)node->pCommandList,
                    node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"(unnamed)",
                    (void*)node->pCommandQueue, done, node->BreadcrumbCount, node->BreadcrumbContextsCount, ours);
                if (node->pCommandHistory && done < node->BreadcrumbCount) {
                    logger::report("      faulting op index %u = D3D12_AUTO_BREADCRUMB_OP %d", done,
                        (int)node->pCommandHistory[done]);
                    const D3D12_DRED_BREADCRUMB_CONTEXT* c = node->pBreadcrumbContexts;
                    const UINT cn = c ? node->BreadcrumbContextsCount : 0;
                    UINT first_after = cn;
                    for (UINT k = 0; k < cn; ++k)
                        if (c[k].BreadcrumbIndex > done) {
                            first_after = k;
                            break;
                        }
                    const UINT from = first_after >= 4 ? first_after - 4 : 0;
                    for (UINT k = from; k < cn && k <= first_after; ++k)
                        logger::report("      marker op %u%s: '%S'", c[k].BreadcrumbIndex,
                            c[k].BreadcrumbIndex > done ? " (after the fault)" : "",
                            c[k].pContextString ? c[k].pContextString : L"");
                    if (!cn) logger::report("      no markers on this list (nothing of ours, no claimed draw)");
                }
            }
            if (!n) logger::report("  DRED: no breadcrumb nodes");
        }
        if (dred1) dred1->Release();
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc{};
        if (!have1 && SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc))) {
            int n = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE* node = bc.pHeadAutoBreadcrumbNode; node && n < 256;
                node = node->pNext, ++n) {
                const UINT done = node->pLastBreadcrumbValue ? *node->pLastBreadcrumbValue : 0;
                // The list pointer is the pin's key: an unnamed game list still says what WE recorded on it.
                char ours[240];
                RemovalPin::Describe(node->pCommandList, ours, sizeof ours);
                logger::report("  DRED node %d: cmdlist='%S' %p queue='%S' %p executed %u of %u ops | %s", n,
                    node->pCommandListDebugNameW ? node->pCommandListDebugNameW : L"(unnamed)",
                    (void*)node->pCommandList,
                    node->pCommandQueueDebugNameW ? node->pCommandQueueDebugNameW : L"(unnamed)",
                    (void*)node->pCommandQueue, done, node->BreadcrumbCount, ours);
                // The op still running when the GPU died is the one after the last completed.
                if (node->pCommandHistory && done < node->BreadcrumbCount)
                    logger::report("      faulting op index %u = D3D12_AUTO_BREADCRUMB_OP %d", done,
                        (int)node->pCommandHistory[done]);
            }
            if (!n) logger::report("  DRED: no breadcrumb nodes");
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT pf{};
        const HRESULT pfhr = dred->GetPageFaultAllocationOutput(&pf);
        if (FAILED(pfhr) || !pf.PageFaultVA)
            logger::report(
                "  DRED page fault: none (hr=%08x VA=0) -- a HUNG without a fault is a wait that never completes "
                "or a shader that never ends",
                (unsigned)pfhr);
        if (SUCCEEDED(pfhr) && pf.PageFaultVA) {
            logger::report("  DRED page fault VA=%llx", (unsigned long long)pf.PageFaultVA);
            auto walk = [](const char* label, const D3D12_DRED_ALLOCATION_NODE* head) {
                int i = 0;
                for (const D3D12_DRED_ALLOCATION_NODE* a = head; a && i < 32; a = a->pNext, ++i) {
                    char name[256] = "(unnamed)";
                    if (a->ObjectNameW)
                        WideCharToMultiByte(CP_UTF8, 0, a->ObjectNameW, -1, name, sizeof(name), nullptr, nullptr);
                    else if (a->ObjectNameA)
                        strncpy_s(name, a->ObjectNameA, _TRUNCATE);
                    logger::report("    %s[%d] type=%d '%s'", label, i, (int)a->AllocationType, name);
                }
                if (!i) logger::report("    %s: none", label);
            };
            walk("freed  ", pf.pHeadRecentFreedAllocationNode);
            walk("existing", pf.pHeadExistingAllocationNode);
        }
        dred->Release();
    } else {
        logger::report("  DRED: interface unavailable -- reason code only");
    }
}

void Watchdogs::StartRemoval(swapchain* sc)
{
    bool expected = false;
    if (sc && sc->get_device() && removal_started_.compare_exchange_strong(expected, true)) {
        ID3D12Device* dev_for_watch = reinterpret_cast<ID3D12Device*>(sc->get_device()->get_native());
        if (dev_for_watch) {
            dev_for_watch->AddRef(); // outlives the render loop; released when the poll ends
            ID3D12Fence* fence = nullptr;
            HANDLE ev = nullptr;
            if (SUCCEEDED(dev_for_watch->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
                ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (!ev || FAILED(fence->SetEventOnCompletion(UINT64_MAX, ev))) {
                    if (ev) CloseHandle(ev);
                    ev = nullptr;
                    fence->Release();
                    fence = nullptr;
                }
            }
            std::thread([this, dev_for_watch, fence, ev] {
                while (!removal_stop_.load(std::memory_order_acquire)) {
                    if (ev)
                        WaitForSingleObject(ev, 200);
                    else
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                    HRESULT rr = dev_for_watch->GetDeviceRemovedReason();
                    if (FAILED(rr)) {
                        RemovalPin::g_device_removed.store(true, std::memory_order_release);
                        DumpDeviceRemoval(dev_for_watch, rr);
                        return;
                    }
                }
                if (fence) fence->Release();
                if (ev) CloseHandle(ev);
                dev_for_watch->Release();
            }).detach();
            logger::report("DRED: removal watchdog started (%s)", ev ? "fence event + 200ms poll" : "200ms poll");
        }
    }
}

void Watchdogs::Stop()
{
    stall_stop_.store(true, std::memory_order_release);
    {
        std::lock_guard<std::mutex> lk(stall_thread_mutex_);
        if (stall_thread_.t.joinable()) {
            stall_thread_.t.join();
            logger::report("STALL watchdog stopped (device destroyed / addon unloading)");
        }
    }
    removal_stop_.store(true, std::memory_order_release);
}
