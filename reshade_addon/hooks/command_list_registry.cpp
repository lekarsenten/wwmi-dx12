// The command-list registry and the hooks that follow a list's life: reset, close, execute.
#include "../addon_common.h"
#include "command_list_registry.h"
#include "hooks_toggles.h"
#include "../addon/addon.h"
#include "../core/gpu_progress.h"
#include "../mods/mod_set.h"
#include "../instrumentation/diag_gate.h"
#include "../instrumentation/hot_path.h"
#include "../instrumentation/morph_dump.h"
#include "../instrumentation/removal_pin.h"
#include "../migoto/dx12_pipeline.h"
#include "../present/frame_sync.h"
#include "../present/present_log.h"
#include "../present/present_hook.h"
#include "../shapekeys/shapekey_chain.h"
#include "../textures/tex_readback.h"

CommandListRegistry g_command_lists;

uint32_t CommandListRegistry::BlockMap::hash(uintptr_t k, uint32_t cap)
{
    uint64_t x = (uint64_t)k * 0x9E3779B97F4A7C15ull;
    return (uint32_t)(x >> 32) & (cap - 1);
}

CmdBlock* CommandListRegistry::BlockMap::find_in(const Tab* t, uintptr_t key)
{
    for (uint32_t i = hash(key, t->cap);; i = (i + 1) & (t->cap - 1)) {
        const uintptr_t k = t->e[i].key.load(std::memory_order_acquire);
        if (k == key) return t->e[i].val.load(std::memory_order_acquire);
        if (k == 0) return nullptr;
    }
}

void CommandListRegistry::BlockMap::put_in(Tab* t, uintptr_t key, CmdBlock* b)
{ // under add; val before key, so a found key has its block
    uint32_t i = hash(key, t->cap);
    while (t->e[i].key.load(std::memory_order_relaxed)) i = (i + 1) & (t->cap - 1);
    t->e[i].val.store(b, std::memory_order_relaxed);
    t->e[i].key.store(key, std::memory_order_release);
}

CmdBlock* CommandListRegistry::BlockMap::get(uintptr_t key)
{
    if (!key) return nullptr;
    thread_local uintptr_t t_key = 0;
    thread_local CmdBlock* t_blk = nullptr;
    if (key == t_key) return t_blk;
    if (CmdBlock* b = find_in(tab.load(std::memory_order_acquire), key)) {
        t_key = key;
        t_blk = b;
        return b;
    }
    std::lock_guard l(add);
    Tab* t = tab.load(std::memory_order_relaxed);
    if (CmdBlock* b = find_in(t, key)) return b;
    if ((used + 1) * 2 > t->cap) {
        Tab* g = new Tab{ t->cap * 2, new Entry[t->cap * 2]() };
        for (uint32_t i = 0; i < t->cap; ++i)
            if (const uintptr_t k = t->e[i].key.load(std::memory_order_relaxed))
                put_in(g, k, t->e[i].val.load(std::memory_order_relaxed));
        tab.store(g, std::memory_order_release);
        t = g;
    }
    CmdBlock* b = new CmdBlock(); // once per command list, never freed
    put_in(t, key, b);
    ++used;
    return b;
}

std::unique_lock<std::mutex> CommandListRegistry::NullLock()
{
    ProfScope s(PROF_CmdLock);
    return std::unique_lock<std::mutex>(null_mutex_);
}

CmdBlock* CommandListRegistry::Block(const void* cmd_list)
{
    return blocks_.get(reinterpret_cast<uintptr_t>(cmd_list));
}

CmdState& CommandListRegistry::Gfx(const void* cmd_list)
{
    CmdBlock* b = Block(cmd_list);
    if (b) return b->gfx;
    std::lock_guard l(null_mutex_);
    return null_gfx_[reinterpret_cast<uintptr_t>(cmd_list)];
}

CmdCbvBinds& CommandListRegistry::Cbv(const void* cmd_list)
{
    CmdBlock* b = Block(cmd_list);
    if (b) return b->cbv;
    std::lock_guard l(null_mutex_);
    return null_cbv_[reinterpret_cast<uintptr_t>(cmd_list)];
}

CmdRT& CommandListRegistry::Rt(const void* cmd_list)
{
    CmdBlock* b = Block(cmd_list);
    if (b) return b->rt;
    static CmdRT overflow_rt;
    overflow_rt = CmdRT{};
    return overflow_rt;
}

CmdHeaps& CommandListRegistry::Heaps(const void* cmd_list)
{
    CmdBlock* b = Block(cmd_list);
    if (b) return b->heaps;
    static CmdHeaps overflow_heaps;
    overflow_heaps = CmdHeaps{};
    return overflow_heaps;
}

uint64_t& CommandListRegistry::GfxLayout(const void* cmd_list)
{
    CmdBlock* b = Block(cmd_list);
    if (b) return b->gfx_layout;
    static uint64_t overflow_gfx;
    overflow_gfx = 0;
    return overflow_gfx;
}

uint64_t& CommandListRegistry::CmpLayout(const void* cmd_list)
{
    CmdBlock* b = Block(cmd_list);
    if (b) return b->cmp_layout;
    static uint64_t overflow_cmp;
    overflow_cmp = 0;
    return overflow_cmp;
}

CmdGfxRoot& CommandListRegistry::GfxRoot(const void* cmd_list)
{
    CmdBlock* b = Block(cmd_list);
    if (b) return b->gfx_root;
    static CmdGfxRoot overflow_root;
    overflow_root = CmdGfxRoot{};
    return overflow_root;
}

uint64_t CommandListRegistry::Gen(const void* cmd_list)
{
    CmdBlock* b = Block(cmd_list);
    return b ? b->gen : 0;
}

void CommandListRegistry::NoteDraw(const void* cmd_list)
{
    if (CmdBlock* b = Block(cmd_list)) b->drew_gen = b->gen;
}

bool CommandListRegistry::DrewThisRecording(const void* cmd_list)
{
    const CmdBlock* b = Block(cmd_list);
    return b && b->drew_gen == b->gen;
}

bool CommandListRegistry::EndsGameFrame(const void* cmd_list)
{
    const CmdBlock* b = Block(cmd_list);
    return b && b->frame_end_gen == b->gen;
}

uint32_t CommandListRegistry::NextRtPass()
{
    return rt_pass_gen_.fetch_add(1, std::memory_order_relaxed) + 1;
}

void CommandListRegistry::ForgetPipeline(uint64_t pso)
{
    ForEach([&](CmdBlock& b) {
        if (b.gfx.gfx_pso == pso) {
            b.gfx.gfx_pso = 0;
            b.gfx.gfx_set = 0;
        }
        if (b.gfx.cmp_pso == pso) b.gfx.cmp_pso = 0;
    });
    std::unique_lock l = NullLock(); // a null list's state, for completeness
    for (auto& kv : null_gfx_) {
        if (kv.second.gfx_pso == pso) {
            kv.second.gfx_pso = 0;
            kv.second.gfx_set = 0;
        }
        if (kv.second.cmp_pso == pso) kv.second.cmp_pso = 0;
    }
}

void on_close_command_list(command_list* cmd_list)
{
    PROF(PROF_CmdClose);
    HotPath::OnClose(cmd_list);
    if (!cmd_list) return;
    auto* nl = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    MigotoPort::ListBuffersToCommon(nl);
    RemovalPin::OnClose(nl);
}

void on_reset_command_list(command_list* cmd_list)
{
    PROF(PROF_CmdReset);
    if (!cmd_list) return;
    ++t_list_resets;
    HotPath::OnReset(cmd_list);
    RemovalPin::OnReset(reinterpret_cast<const void*>(cmd_list->get_native())); // the tally is per recording
    MigotoPort::ListBuffersForget(reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()));
    CmdBlock* b = g_command_lists.Block(cmd_list);
    if (b) {
        ++b->gen; // a new recording (CommandListRegistry::Gen): independent of the clear A/B below
        for (HeapBind& hb : b->heaps) hb.fresh = false;
        b->gfx_root = CmdGfxRoot{};
        b->mod_pins.Reset(g_mod_sets);
    }
    if (!g_hooks_toggles.cmd_reset_clears) return;
    if (b) {
        b->gfx = CmdState{};
        b->cbv = CmdCbvBinds{};
        b->rt = CmdRT{};
        b->gfx_layout = 0;
        b->cmp_layout = 0;
    }
    g_hooks_stats.cmd_resets.fetch_add(1, std::memory_order_relaxed);
}

void on_destroy_command_list(command_list* cmd_list)
{
    if (CmdBlock* const b = g_command_lists.Block(cmd_list)) b->mod_pins.Reset(g_mod_sets);
}

void on_execute_command_list(command_queue* queue, command_list* cl)
{
    PROF(PROF_CmdExecute);
    if (!cl) return;
    if (CmdBlock* const pins = g_command_lists.Block(cl)) pins->mod_pins.Executed(g_mod_sets, GpuStamp());
    const void* native = (const void*)cl->get_native();
    if (g_command_lists.DrewThisRecording(cl)) {
        g_frames.game_frame_submitted.store(true, std::memory_order_relaxed);
        g_frames.game_submit_tid.store(GetCurrentThreadId(), std::memory_order_relaxed);
        if (queue) g_frames.game_submit_queue.store(queue->get_native(), std::memory_order_relaxed);
    }
#ifdef WWMI_DEBUG
    if (g_present_log.left.load(std::memory_order_relaxed) > 0) {
        g_present_log.execs.fetch_add(1, std::memory_order_relaxed);
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        const uint32_t tid = GetCurrentThreadId();
        const uint64_t q = queue ? (uint64_t)queue->get_native() : 0;
        std::lock_guard l(g_present_log.mtx);
        PresentLogSubmitter* s = nullptr;
        for (PresentLogSubmitter& e : g_present_log.subs)
            if (e.tid == tid && e.queue == q) s = &e;
        if (!s) {
            g_present_log.subs.push_back({});
            s = &g_present_log.subs.back();
            s->tid = tid;
            s->queue = q;
        }
        ++s->lists;
        if (native == g_frames.claim_last_list.load(std::memory_order_relaxed)) ++s->claimed;
        s->last_qpc = now.QuadPart;
        const CmdBlock* fe = g_command_lists.Block(cl);
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        const int64_t last = g_present_log.qpc.load(std::memory_order_relaxed);
        const double since = last ? (double)(now.QuadPart - last) * 1000.0 / (double)freq.QuadPart : -1.0;
        if (fe && fe->bb_notes_gen == fe->gen && !fe->bb_notes.empty()) {
            std::string notes;
            for (const BbNote& n : fe->bb_notes) {
                char e[96];
                snprintf(e, sizeof(e), " %llx%s:%x>%x", (unsigned long long)n.res,
                    std::find(g_present_log.bbs.begin(), g_present_log.bbs.end(), n.res) != g_present_log.bbs.end()
                        ? "(sc)"
                        : "",
                    n.old_s, n.new_s);
                notes += e;
            }
            logger::report("BBCOMMON tid=%u queue=%llx list=%p since_present=%.2f ms:%s", tid, (unsigned long long)q,
                native, since, notes.c_str());
        }
    }
#endif
    HotPath::OnExecute(queue, cl);
    g_tex_readback.OnExecute(queue, cl);
    if (queue && native == g_frames.claim_last_list.load(std::memory_order_relaxed)) {
        // FUNCTIONAL: [Present] reads this under g_present_toggles.on_game_queue. Always recorded.
        g_frames.claim_exec_queue.store((const void*)queue->get_native(), std::memory_order_relaxed);
        if (g_debug) g_frames.claim_exec_seen.fetch_add(1, std::memory_order_relaxed); // heartbeat only
    }
    if (queue && g_frames.frame_end_armed.load(std::memory_order_relaxed) && g_command_lists.EndsGameFrame(cl) &&
        GetCurrentThreadId() == g_frames.game_submit_tid.load(std::memory_order_relaxed) &&
        queue->get_native() == g_frames.game_submit_queue.load(std::memory_order_relaxed) &&
        GetCurrentThreadId() != g_frames.presenter_tid.load(std::memory_order_relaxed))
        FrameEndStepGuarded(queue);
    if (!g_debug) return;
    if (queue) {
        auto* nl = reinterpret_cast<ID3D12CommandList*>(cl->get_native());
        auto* nq = reinterpret_cast<ID3D12CommandQueue*>(queue->get_native());
        RemovalPin::OnExecute(g_addon.d3d12_device, native, nl ? (int)nl->GetType() : -1,
            nq ? (int)nq->GetDesc().Type : -1, "ExecuteCommandLists");
    }
    if (queue && MorphDump::Armed()) {
        static std::atomic<unsigned> s_seq{ 0 };
        logger::write("X26 EXEC frame=%u seq=%u queue=%p type=%d list=%p", g_frame_id,
            s_seq.fetch_add(1, std::memory_order_relaxed), (const void*)queue->get_native(), (int)queue->get_type(),
            native);
    }
    SkNoteSubmitted(native);
}
