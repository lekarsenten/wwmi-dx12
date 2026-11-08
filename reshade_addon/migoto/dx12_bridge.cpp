// See dx12_bridge.h.
#include "dx12_bridge.h"
#include "port_toggles.h"
#include "../core/log.h"
#include <mutex>
#include <vector>
#include <atomic>

namespace MigotoPort {

static Dx12Bridge s_bridge;
static std::vector<unsigned long> s_owner_stack; // X30: which thread's scope held the global before
static std::mutex s_bridge_mtx; // guards s_bridge and s_owner_stack
static std::atomic<unsigned long> s_owner_tid{
    0
}; // X30: the thread whose scope set the global bridge last (0 = none open)
static std::atomic<unsigned> s_depth{ 0 };
static std::atomic<unsigned long long> s_displaced{ 0 };
static std::atomic<unsigned long long> s_wrong_draw{ 0 };
static std::atomic<unsigned long long> s_wrong_disp{ 0 };
static thread_local Dx12Bridge t_bridge; // X30: this thread's innermost open scope
static thread_local std::vector<Dx12Bridge> t_stack; // X30: what this thread's scopes were built from
static thread_local unsigned t_depth = 0;
static thread_local bool t_game_state_owed = false;

static std::atomic<unsigned long long> s_foreign_cl{ 0 };

void SetDx12Bridge(const Dx12Bridge& b)
{
    std::lock_guard<std::mutex> l(s_bridge_mtx);
    s_bridge = b;
    s_bridge.owner_tid = GetCurrentThreadId();
}

const Dx12Bridge& GetDx12Bridge()
{
    if (t_depth) return t_bridge;
    static thread_local Dx12Bridge view;
    {
        std::lock_guard<std::mutex> l(s_bridge_mtx);
        view = s_bridge;
    }
    const unsigned long tid = GetCurrentThreadId();
    if (view.cl && view.owner_tid && view.owner_tid != tid) {
        const unsigned long long n = s_foreign_cl.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12 || (n % 5000) == 0)
            logger::write("X33 FOREIGN-LIST #%llu: thread %lu has no scope open and the global bridge names cl=%p, "
                          "published by thread %lu -- handing it no list instead",
                n, tid, (void*)view.cl, view.owner_tid);
        view.cl = nullptr;
    }
    return view;
}

unsigned long long BridgeForeignListRefusals()
{
    return s_foreign_cl.load(std::memory_order_relaxed);
}

void OweGameState(const Dx12Bridge& b)
{
    if (!b.restore_game_state) return;
    if (t_depth && g_lazy_restore) {
        t_game_state_owed = true;
        return;
    }
    b.restore_game_state(b.restore_user);
}

void PayGameState()
{
    if (!t_game_state_owed) return;
    t_game_state_owed = false;
    if (t_depth && t_bridge.restore_game_state) t_bridge.restore_game_state(t_bridge.restore_user);
}

void BridgePush(const Dx12Bridge& scoped)
{
    PayGameState();
    const unsigned long tid = GetCurrentThreadId();
    // X30: another thread's scope currently owns the global bridge.
    const unsigned long owner = s_owner_tid.load(std::memory_order_relaxed);
    if (owner && owner != tid) {
        const unsigned long long n = s_displaced.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 12 || (n % 5000) == 0)
            logger::write("X30 BRIDGE-DISPLACED #%llu: thread %lu opens a draw scope (cl=%p) while thread %lu's scope "
                          "holds the global bridge (cl=%p)",
                n, tid, (void*)scoped.cl, owner, (void*)s_bridge.cl);
    }
    t_stack.push_back(t_bridge); // restored by BridgePop; meaningless once t_depth is back to 0
    t_bridge = scoped;
    t_bridge.owner_tid = tid;
    ++t_depth;
    s_depth.fetch_add(1, std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> l(s_bridge_mtx);
        s_owner_stack.push_back(owner);
        s_owner_tid.store(tid, std::memory_order_relaxed);
    }
}

void BridgePop()
{
    PayGameState();
    if (t_depth) {
        --t_depth;
        if (!t_stack.empty()) {
            t_bridge = t_stack.back();
            t_stack.pop_back();
        }
    }
    if (s_depth.load(std::memory_order_relaxed)) s_depth.fetch_sub(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> l(s_bridge_mtx);
    if (!s_owner_stack.empty()) {
        s_owner_tid.store(s_owner_stack.back(), std::memory_order_relaxed);
        s_owner_stack.pop_back();
    } else
        s_owner_tid.store(0, std::memory_order_relaxed);
}

void BridgeSetTargets(DXGI_FORMAT rt_format, DXGI_FORMAT ds_format)
{
    if (!t_depth) return;
    t_bridge.rt_format = rt_format;
    t_bridge.ds_format = ds_format;
}

void BridgeSetBackBuffer(ID3D12Resource* bb)
{
    if (!t_depth) return;
    t_bridge.back_buffer = bb;
    t_bridge.back_buffer_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
}

void BridgeSetGameBindings(const GameBindings& game)
{
    if (t_depth) t_bridge.game = game;
}

const Dx12Bridge* BridgeScope()
{
    return t_depth ? &t_bridge : nullptr;
}

unsigned BridgeDepth()
{
    return s_depth.load(std::memory_order_relaxed);
}

bool BridgeNoteIssue(ID3D12GraphicsCommandList* cl, bool dispatch, unsigned a, unsigned b, unsigned c)
{
    if (!t_depth || !t_bridge.cl || cl == t_bridge.cl) return false;
    std::atomic<unsigned long long>& ctr = dispatch ? s_wrong_disp : s_wrong_draw;
    const unsigned long long n = ctr.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 12 || (n % 5000) == 0)
        logger::write("X30 WRONG-LIST %s #%llu: thread %lu's scope named cl=%p but the engine records onto cl=%p "
                      "(%s %u,%u,%u)",
            dispatch ? "DISPATCH" : "DRAW", n, GetCurrentThreadId(), (void*)t_bridge.cl, (void*)cl,
            dispatch ? "groups" : "count,start,instances", a, b, c);
    return true;
}

unsigned long long BridgeDisplacedCount()
{
    return s_displaced.load(std::memory_order_relaxed);
}
unsigned long long BridgeWrongListDraws()
{
    return s_wrong_draw.load(std::memory_order_relaxed);
}
unsigned long long BridgeWrongListDispatches()
{
    return s_wrong_disp.load(std::memory_order_relaxed);
}

// Plain function pointer, no lock: installed once at startup, read on the render threads.
static uint32_t (*s_res_state_query)(void*) = nullptr;
void SetResourceStateQuery(uint32_t (*fn)(void*))
{
    s_res_state_query = fn;
}
uint32_t ObservedResourceState(void* res)
{
    return (s_res_state_query && res) ? s_res_state_query(res) : kResStateUnknown;
}
unsigned QueryResourceState(void* res)
{
    const uint32_t s = ObservedResourceState(res);
    return s == kResStateUnknown ? 0u : s;
}

static std::wstring s_dump_dir; // written once at config load, before any capture can run
void SetDumpDir(const wchar_t* dir)
{
    s_dump_dir = dir ? dir : L"";
}
const std::wstring& DumpDir()
{
    return s_dump_dir;
}

static std::atomic<unsigned long long> s_vb_null_unbinds{ 0 };
unsigned long long VbNullUnbinds()
{
    return s_vb_null_unbinds.load(std::memory_order_relaxed);
}
void NoteVbNullUnbind()
{
    s_vb_null_unbinds.fetch_add(1, std::memory_order_relaxed);
}

static thread_local unsigned t_emit_n = 0;
static thread_local uint32_t t_emit_x = 0;
static thread_local uint32_t t_emit_y = 0;
static thread_local uint32_t t_emit_z = 0;
void NoteEmittedDispatch(uint32_t x, uint32_t y, uint32_t z)
{
    ++t_emit_n;
    t_emit_x = x;
    t_emit_y = y;
    t_emit_z = z;
}
void ResetEmittedDispatch()
{
    t_emit_n = 0;
    t_emit_x = t_emit_y = t_emit_z = 0;
}
unsigned TakeEmittedDispatch(uint32_t* x, uint32_t* y, uint32_t* z)
{
    if (x) *x = t_emit_x;
    if (y) *y = t_emit_y;
    if (z) *z = t_emit_z;
    const unsigned n = t_emit_n;
    ResetEmittedDispatch();
    return n;
}

static std::atomic<unsigned> s_vb_calls{ 0 };
static std::atomic<unsigned> s_vb_nonnull{ 0 };
static std::atomic<uint64_t> s_vb_last_ps{ 0 };
void NoteVbBind(const void* state, int nonnull)
{
    s_vb_calls.fetch_add(1, std::memory_order_relaxed);
    if (nonnull) {
        s_vb_nonnull.fetch_add(1, std::memory_order_relaxed);
        s_vb_last_ps.store(reinterpret_cast<uint64_t>(state), std::memory_order_relaxed);
    }
}
void VbBindStats(unsigned* calls, unsigned* nonnull, uint64_t* last_ps)
{
    if (calls) *calls = s_vb_calls.load(std::memory_order_relaxed);
    if (nonnull) *nonnull = s_vb_nonnull.load(std::memory_order_relaxed);
    if (last_ps) *last_ps = s_vb_last_ps.load(std::memory_order_relaxed);
}

} // namespace MigotoPort
