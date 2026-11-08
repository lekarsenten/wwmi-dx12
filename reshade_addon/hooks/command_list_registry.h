#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <mutex>
#include <atomic>
#include <unordered_map>
#include <vector>
#include <array>
#include <cstdint>
#include "../core/set_life.h"
#include "../instrumentation/profiler.h"

class ModSet;

struct ComputeBindNote {
    uint64_t layout = 0; // the signature this binding belongs to
    uint32_t frame = 0;
    uint32_t seq = 0;
};
struct RootParam {
    reshade::api::descriptor_table table = {};
    reshade::api::descriptor_table ctable = {};
    ComputeBindNote ctable_note;
    uint64_t va = 0;
    ID3D12Resource* res = nullptr; // owning resource of a root SRV/UAV (morph matcher: offset-proof identity)
    uint8_t desc_type = 0;

    // The table a DISPATCH sees: the compute-bound one, else the shared one.
    reshade::api::descriptor_table compute_table() const { return ctable.handle ? ctable : table; }
};
struct VertexBind {
    uint64_t va = 0;
    uint32_t size = 0;
    uint32_t stride = 0; // for restoring the game's VBVs after our override
    ID3D12Resource* res = nullptr;
    uint64_t res_width = 0; // the resource's own width, captured in on_bind_vertex_buffers
};
struct IndexBind {
    uint64_t va = 0;
    uint32_t size = 0;
    uint32_t index_bytes = 0; // game's IB (to restore)
    ID3D12Resource* res = nullptr; // IB resource (frame-dump capture needs the object, not just the VA)
};

constexpr unsigned kSectionTexRegs = 9; // ps-t0..t8: the game's textures a component section sees

// Per list: where the last character draw that read each of those registers found its texture (draw_sectex.cpp
// CarryPsReads), and the root signature, of which recording, the registers' places were taken from.
struct PsCarry {
    struct Slot {
        reshade::api::descriptor_table table = {};
        uint32_t offset = 0;
        uint64_t gen = 0; // the recording it was noted in (CommandListRegistry::Gen)
    };
    Slot slot[kSectionTexRegs];
    uint64_t layout = 0;
    uint64_t layout_gen = 0;
    uint8_t param[kSectionTexRegs] = {};
    uint32_t offset[kSectionTexRegs] = {};
};

struct CmdState {
    RootParam root[64];
    uint32_t ctable_seq_next = 0;
    VertexBind vb[16];
    IndexBind ib;
    uint64_t gfx_pso = 0; // currently-bound graphics PSO (reshade pipeline handle == ID3D12PipelineState*)
    uint64_t gfx_set = 0; // what is actually set in its place (the UID-cut or decensor variant); 0 = gfx_pso itself
    uint64_t gfx_restore() const { return gfx_set ? gfx_set : gfx_pso; }
    uint64_t cmp_pso = 0;

    D3D12_VIEWPORT vp[16] = {};
    uint32_t vp_count = 0;
    D3D12_RECT scissor[16] = {};
    uint32_t scissor_count = 0;
    uint32_t topology = 0; // 0 = never seen
    float blend_factor[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    bool blend_factor_set = false;
    uint32_t stencil_ref = 0;
    bool stencil_ref_set = false;
    PsCarry ps_carry;
};

struct RootCbvBind {
    ID3D12Resource* res = nullptr;
    uint64_t va = 0;
    uint32_t size = 0;
};
struct CmdCbvBinds {
    RootCbvBind binds[64];
};

// The game's graphics root signature and every root argument it set on it, to put back after a draw of ours bound
// its own signature on the game's list (draw_restore.cpp RestoreGameRoot).
struct CmdGfxRoot {
    enum Kind : uint8_t { kNone, kTable, kCbv, kSrv, kUav };
    struct Constant {
        uint8_t param = 0;
        uint8_t offset = 0;
        uint32_t value = 0;
    };
    uint64_t layout = 0;
    Kind kind[64] = {};
    uint64_t value[64] = {}; // the table's GPU handle, or the buffer's address
    Constant constants[64]; // a root signature holds at most 64 DWORDs
    uint32_t constant_count = 0;
};

struct CmdRT {
    uint32_t rtv_count = 0;
    bool has_dsv = false;
    uint64_t dsv = 0;
    uint32_t pass_gen = 0;
    uint64_t rtv[8] = {}; // D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT
    bool valid = false;
};

// A buffer the game transitioned to UNORDERED_ACCESS on this list, this frame or the previous one.
struct UavNote {
    uint64_t res;
    uint32_t frame;
};
struct HeapBind {
    ID3D12DescriptorHeap* heap = nullptr;
    uint64_t lo = 0;
    uint64_t hi = 0;
    bool fresh = false;
};
using CmdHeaps = std::array<HeapBind, 2>; // by heap type: CBV_SRV_UAV, sampler
#ifdef WWMI_DEBUG
struct BbNote {
    uint64_t res = 0;
    uint32_t old_s = 0;
    uint32_t new_s = 0;
};
#endif
struct CmdBlock {
    CmdState gfx;
    CmdCbvBinds cbv;
    CmdRT rt;
    CmdHeaps heaps;
    uint64_t gfx_layout = 0; // active graphics root signature for this list
    uint64_t cmp_layout = 0; // ... and the compute one
    CmdGfxRoot gfx_root;
    uint64_t gen = 1;
    uint64_t drew_gen = 0; // the recording (gen) a game draw was recorded in
    uint64_t frame_end_gen = 0; // the recording a back-buffer-sized texture went to COMMON in: the game's frame end
    ListPins<ModSet> mod_pins;
    std::vector<UavNote>
        uav_recent; // most recent last; written by on_barrier, read at dispatch, both on the recording thread
#ifdef WWMI_DEBUG
    std::vector<BbNote>
        bb_notes; // B23: back-buffer-sized textures moved to or from COMMON in recording bb_notes_gen (BBCOMMON)
    uint64_t bb_notes_gen = 0;
#endif
};

// What the game has bound on each command list, as the bind hooks wrote it down: one CmdBlock per list, made
// once and never freed, found without a lock. A null list has a state of its own under a mutex.
class CommandListRegistry {
public:
    // Find-or-create. nullptr only for a null list.
    CmdBlock* Block(const void* cmd_list);
    CmdState& Gfx(const void* cmd_list);
    CmdCbvBinds& Cbv(const void* cmd_list);
    CmdRT& Rt(const void* cmd_list);
    CmdHeaps& Heaps(const void* cmd_list);
    uint64_t& GfxLayout(const void* cmd_list);
    uint64_t& CmpLayout(const void* cmd_list);
    CmdGfxRoot& GfxRoot(const void* cmd_list);
    // 0 for a null list: the port then keeps its old carry-over instead of guessing a recording.
    uint64_t Gen(const void* cmd_list);
    // A game draw in the list's current recording: its submission is game work a step can follow (present_hook.cpp).
    void NoteDraw(const void* cmd_list);
    bool DrewThisRecording(const void* cmd_list);
    bool EndsGameFrame(const void* cmd_list);
    // A new render pass (a depth target bound): its generation.
    uint32_t NextRtPass();
    // A destroyed PSO is bound nowhere.
    void ForgetPipeline(uint64_t pso);
    template <class F> void ForEach(F&& fn)
    {
        const BlockMap::Tab* t = blocks_.tab.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < t->cap; ++i) {
            if (t->e[i].key.load(std::memory_order_acquire) == 0) continue;
            if (CmdBlock* b = t->e[i].val.load(std::memory_order_acquire)) fn(*b);
        }
    }

private:
    struct BlockMap {
        struct Entry {
            std::atomic<uintptr_t> key;
            std::atomic<CmdBlock*> val;
        };
        struct Tab {
            uint32_t cap;
            Entry* e;
        };
        std::atomic<Tab*> tab{ new Tab{ 1u << 13, new Entry[1u << 13]() } };
        std::mutex add;
        uint32_t used = 0; // under add
        static uint32_t hash(uintptr_t k, uint32_t cap);
        static CmdBlock* find_in(const Tab* t, uintptr_t key);
        static void put_in(Tab* t, uintptr_t key, CmdBlock* b);
        CmdBlock* get(uintptr_t key);
    };
    std::unique_lock<std::mutex> NullLock();

    BlockMap blocks_;
    std::mutex null_mutex_; // guards null_gfx_ and null_cbv_
    std::unordered_map<uintptr_t, CmdState> null_gfx_;
    std::unordered_map<uintptr_t, CmdCbvBinds> null_cbv_;
    std::atomic<uint32_t> rt_pass_gen_{ 0 };
};
extern CommandListRegistry g_command_lists;

// Lists this thread began recording. A submission this thread made before its last Reset has reached the queue:
// ReShade raises every list's execute event first and submits the batch after them.
inline thread_local uint64_t t_list_resets = 0;

// After ID3D12GraphicsCommandList::Reset: the list carries nothing, and neither does our record of it.
void on_reset_command_list(reshade::api::command_list* cmd_list);
void on_destroy_command_list(reshade::api::command_list* cmd_list);
void on_close_command_list(reshade::api::command_list* cmd_list);
void on_execute_command_list(reshade::api::command_queue* queue, reshade::api::command_list* cmd_list);
