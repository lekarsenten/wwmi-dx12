#include "../addon_common.h"
#include "hot_path.h"
#include <TraceLoggingProvider.h>
#include <memory>
#include <mutex>
#include <unordered_map>

TRACELOGGING_DEFINE_PROVIDER(
    g_hp_etw, "WWMI-DX12-HotPath", (0x3ab0fca7, 0xf037, 0x4eee, 0xb9, 0x1d, 0x56, 0x81, 0xd8, 0xb5, 0x10, 0xe1));

namespace HotPath {

struct ListRec {
    uint64_t ours_ns = 0;
    uint32_t claimed = 0;
    uint32_t hook_n[HK_COUNT] = {};
};

struct Rec {
    ListRec cur;
    GpuRec* gpu = nullptr;
};

GpuRec*& GpuOf(Rec* r)
{
    return r->gpu;
}

namespace {

std::mutex g_mtx;
std::unordered_map<uint64_t, std::unique_ptr<Rec>> g_lists;
uint32_t g_frame_no = 0;

thread_local uint64_t t_last_list = 0;
thread_local Rec* t_last_rec = nullptr;

} // namespace

Rec* RecFor(reshade::api::command_list* cl)
{
    const uint64_t key = reinterpret_cast<uint64_t>(cl);
    if (key == t_last_list) return t_last_rec;
    Rec* r;
    {
        std::lock_guard l(g_mtx);
        std::unique_ptr<Rec>& p = g_lists[key];
        if (!p) p = std::make_unique<Rec>();
        r = p.get();
    }
    t_last_list = key;
    t_last_rec = r;
    return r;
}

void HookDone(int hook, uint64_t self_ns, Rec* rec)
{
    if (!rec) return;
    ++rec->cur.hook_n[hook];
    rec->cur.ours_ns += self_ns;
}

void Init()
{
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    TraceLoggingRegister(g_hp_etw);
}

void OnReset(reshade::api::command_list* cl)
{
    if (!g_on.load(std::memory_order_relaxed) || !cl) return;
    Rec* r = RecFor(cl);
    r->cur = ListRec{};
    GpuListReset(r);
    TraceLoggingWrite(g_hp_etw, "ListReset", TraceLoggingPointer(cl, "list"));
}

void NoteClaimed()
{
    if (t_rec) ++t_rec->cur.claimed;
}

void OnClose(reshade::api::command_list* cl)
{
    if (!g_on.load(std::memory_order_relaxed) || !cl) return;
    Rec* r = RecFor(cl);
    ListRec& c = r->cur;
    TraceLoggingWrite(g_hp_etw, "ListClose", TraceLoggingPointer(cl, "list"),
        TraceLoggingUInt32((uint32_t)(c.ours_ns / 1000), "ours_us"),
        TraceLoggingUInt32(c.hook_n[HK_draw_indexed], "draws"), TraceLoggingUInt32(c.claimed, "claimed"),
        TraceLoggingUInt32(c.hook_n[HK_dispatch], "dispatches"));
    GpuListClose(r, cl, c.claimed);
}

void OnExecute(reshade::api::command_queue* queue, reshade::api::command_list* cl)
{
    if (!g_on.load(std::memory_order_relaxed) || !cl) return;
    TraceLoggingWrite(g_hp_etw, "Execute", TraceLoggingPointer(cl, "list"), TraceLoggingPointer(queue, "queue"));
    GpuExecuted(RecFor(cl), queue);
}

void FrameEnd()
{
    const bool was_on = g_on.load(std::memory_order_relaxed);
    const bool etw = TraceLoggingProviderEnabled(g_hp_etw, 0, 0);
    g_on.store(etw, std::memory_order_relaxed);
    g_gpu_on.store(etw, std::memory_order_relaxed);
    g_gpu_detail.store(etw && TraceLoggingProviderEnabled(g_hp_etw, 0, 0x2), std::memory_order_relaxed);
    if (was_on) TraceLoggingWrite(g_hp_etw, "Frame", TraceLoggingUInt32(++g_frame_no, "frame"));
}

} // namespace HotPath
