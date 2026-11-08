#pragma once
// The chain's own units and their couplings: the per-instance store, the dispatch-scope queue, the identity
// searches, and the resolver and the replay that use them.
#include "shapekey_chain.h"
#include <wrl.h>
#include <mutex>
#include <atomic>
#include <memory>
#include <vector>
#include <set>
#include <map>
#include <deque>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <cstdio>
#include <cassert>

#include "../core/log.h"
#include "../textures/mod_texture_store.h"
#include "../core/fnv.h"
#include "../addon/addon.h"
#include "../mods/buffer_relocation.h" // g_buffer_reloc -- the chain binds relocated buffers
#include "../hooks/cmd_bind_hooks.h" // ResolveBound -- the chain asks what is bound at a register
#include "../hooks/resource_tracker.h" // g_resources -- liveness before any dereference
#include "../mods/character_recognition.h"
#include "../mods/port_section.h" // LogBudget -- the chain's per-namespace log allowance
#include "../instrumentation/diag_gate.h"
#include "../instrumentation/profiler.h"
#include "../mods/ib_hash.h"
#include "../migoto/frame_analysis_driver.h"
#include "DrawCallInfo.h"
#include "../migoto/port_api.h"
#include "../migoto/dx12_pipeline.h" // PipeState
#include "../migoto/dx12_skcap.h" // SkCapSetCaller
#include "../migoto/dx12_bridge.h" // GetDx12Bridge
#include "../instrumentation/morph_dump.h"
#include "../migoto/dx12_wrap.h" // AsShaderCode / ShaderCode
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "sk_toggles.h"

using namespace reshade::api;

// ---- the per-instance store (sk_store.cpp) ----------------------------------------------------------

struct SkNsState {
    SkBufferBinding reg[kRegisterFileTotal];

    std::unordered_map<unsigned, std::unordered_map<unsigned long long, std::unordered_map<unsigned, SkBufferBinding>>>
        snap;

    bool valid = false;
};

/* The name the replay path reads: "the whole compute state" and "this owner's bindings" are one thing. */
using SkComputeState = SkNsState;

inline const SkBufferBinding& SkReg(const SkNsState& s, TargetClass c, unsigned slot)
{
    const unsigned i = RegisterIndex(c, slot);
    assert(i != kNoRegister && "SkReg called with a slot outside the class's register file");
    return s.reg[i];
}

inline SkBufferBinding& SkRegMut(SkNsState& s, TargetClass c, unsigned slot)
{
    const unsigned i = RegisterIndex(c, slot);
    assert(i != kNoRegister && "SkRegMut called with a slot outside the class's register file");
    return s.reg[i];
}

// What a shapekey dispatch publishes for its instance's draw: the register file, the cb0 snapshots per queuing
// dispatch, and the zero sources the clears copy from.
class ShapekeyStore {
public:
    bool GetBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg, SkBufferBinding* out);
    uint8_t ZeroBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg,
        reshade::api::command_list* cmd_list, const char* why);
    void ZeroStats(TargetClass kind, unsigned reg, unsigned long long* cleared, unsigned long long* not_ours,
        unsigned long long* not_ready);
    bool SetSnap(const std::wstring& ns, ID3D12Resource* inst, unsigned reg, unsigned long long cs, unsigned tgy,
        ID3D12Resource* res, uint64_t va, unsigned bytes);
    bool GetSnap(const std::wstring& ns, ID3D12Resource* inst, unsigned reg, unsigned long long cs, unsigned tgy,
        SkBufferBinding* out);
    // Copy the dispatch's cb0 on the game's own command list, keyed by the queuing dispatch.
    void SnapshotCb0(reshade::api::command_list* cmd_list, const std::wstring& ns, ID3D12Resource* inst,
        ID3D12Resource* src, uint64_t va, unsigned size, unsigned long long cs_hash, unsigned tgy);
    // Publish a whole-state snapshot for the owner's instance. The register file only; its cb0 copies stay.
    void SetState(const std::wstring& ns, ID3D12Resource* inst, const SkComputeState& st);
    // The instance's last published register file; false when it has none.
    bool GetState(const std::wstring& ns, ID3D12Resource* inst, SkComputeState* out);
    void Forget(ID3D12Resource* dead);

private:
    using Key = std::pair<std::wstring, ID3D12Resource*>;
    struct ZeroSource {
        ID3D12Resource* buf = nullptr; // >= the largest buffer zeroed through this register so far
        unsigned bytes = 0;
        unsigned long long cleared = 0;
        unsigned long long not_ours = 0;
        unsigned long long not_ready = 0;
    };
    static std::wstring NsKey(const std::wstring& ns);
    SkNsState* FindLocked(const std::wstring& ns, ID3D12Resource* inst);
    SkNsState* EmplaceLocked(const std::wstring& ns, ID3D12Resource* inst);
    void RetireLocked(ID3D12Resource* r);
    void SweepGraveLocked();

    std::mutex mutex_; // store_, zero_ and grave_
    std::map<Key, SkNsState> store_;
    std::unordered_map<unsigned, ZeroSource> zero_; // flat register index -> source
    std::vector<std::pair<ID3D12Resource*, uint64_t>> grave_; // retired buffers, GpuStamp() at retirement
};
extern ShapekeyStore g_sk_store;

// ---- the dispatch-scope queue (sk_queue.cpp) --------------------------------------------------------

struct SkPendingList {
    std::wstring ns;
    const void* list;
    unsigned long long cs;
    unsigned u0hash; // desc hash of the u0 resolved AT THE DISPATCH that queued this list
    unsigned tgy; // the game's thread_group_count_y there -- the batch selector
    uint64_t owner_ns_hash;
    ID3D12Resource* inst; // SkU0Ident::inst of that dispatch; nullptr = it published nothing
};

// The lists a game dispatch's `run =` queued, tagged with that dispatch, and the dispatch list of each frame.
class ShapekeyQueue {
public:
    void NoteDispatchList(const void* native_cl, uint32_t frame);
    // The dispatch list recorded FOR THAT EXACT FRAME, or nullptr when none was seen.
    const void* DispListForFrame(uint32_t f) const;
    void BeginDispatchScope(uint64_t cs_hash, uint32_t group_count_y, const SkU0Ident& ck);
    void EndDispatchScope(bool shapekey); // only clears the observer Begin installed
    unsigned TakeOutcome(uint8_t* reason);
    // This mod instance's queued lists, taken out of the queue (the rest stay for their owners).
    std::vector<SkPendingList> TakeOwnLists(const std::wstring& ns, int mod_id, ID3D12Resource* inst,
        MorphDump::DrainRecord& dr, size_t& left_for_others, size_t& dropped_unowned, size_t& other_inst_lists,
        ID3D12Resource*& other_inst);
    // Drop the queued lists of an instance whose buffer the game destroyed.
    void ForgetFor(ID3D12Resource* dead);

private:
    struct DispAnchor {
        std::atomic<const void*> cl{ nullptr };
        std::atomic<uint32_t> frame{ 0 };
    };
    std::mutex pending_mutex_;
    std::vector<SkPendingList> pending_;
    DispAnchor disp_[2];
};
extern ShapekeyQueue g_sk_queue;

// ---- the identity searches (sk_identity.cpp) --------------------------------------------------------

// Naming a buffer without a descriptor: each mod's declared shapekey hashes, the game's own clear and group
// counts, the draws that bound the instance, and the u0s the chain wrote.
class ShapekeyIdentity {
public:
    bool ModDeclaresCb(int i, unsigned cb) const;
    unsigned ModScaleFor(int i, unsigned cb) const; // the ShapeKeyScale paired with the callback -- cs-u1's identity
    unsigned ModCbFor(int i, unsigned u0hash) const; // mod i's callback hash that u0hash names; its only one; else 0
    void NoteClear(const void* cl, uint32_t gx); // the per-dword clear recorded on this thread
    // The byte range u0 must have for THIS dispatch, from its own geometry; 0/0 when nothing is known.
    void GeomExpect(const void* cl, uint64_t cs_hash, uint32_t gy, unsigned* lo, unsigned* hi) const;
    void NoteDrawVbs(int mod, ID3D12Resource* const* res, unsigned n);
    void NoteChainWroteU0(ID3D12Resource* res, uint32_t frame);
    bool ChainWroteU0Recently(ID3D12Resource* res, uint32_t frame);
    // Every LIVE buffer whose size fits and whose desc hash a shapekey callback declares; the count, the single
    // fit in out when there is one.
    int FindU0ByGeometry(
        reshade::api::command_list* cmd_list, unsigned lo, unsigned hi, ID3D12Resource** out_res, unsigned* out_bytes);
    // Live buffers whose identity size hashes to the owner's declared scale value, minus our relocations.
    int FindU1ByIdentity(reshade::api::command_list* cmd_list, unsigned want_scale, ID3D12Resource* u0,
        ID3D12Resource** out_res, unsigned* out_bytes, char* why, size_t why_len);

private:
    struct PerSet;
    struct DrawVbs {
        ID3D12Resource* res[8] = {};
        uint32_t frame = 0;
    };
    struct U0Written {
        ID3D12Resource* res;
        uint32_t frame;
    };
    using Fits = std::vector<std::pair<ID3D12Resource*, unsigned>>;
    const std::vector<PortApi::ShapeKeySet>* ModSets(int i) const;
    static const PortApi::ShapeKeySet* SetOf(const std::vector<PortApi::ShapeKeySet>* sets, unsigned cb);
    bool ModDrewResource(int mod, ID3D12Resource* res);
    static void KeepBarrieredHere(reshade::api::command_list* cmd_list, Fits& fits);
    static void KeepInUav(Fits& fits);

    std::mutex drawvb_mutex_;
    std::mutex u0written_mutex_;
    std::vector<U0Written> u0written_; // aged by frame, never erased
};
extern ShapekeyIdentity g_sk_identity;

// ---- the resolver (sk_resolve.cpp) and the replay (sk_replay.cpp) ----------------------------------------

class ShapekeyResolver {
public:
    ShapekeyResolver(ShapekeyStore& store, ShapekeyIdentity& identity)
        : store_(store),
          identity_(identity)
    {}
    bool U0ResourceNow(reshade::api::command_list* cmd_list, ID3D12Resource** res, unsigned* size);
    void ResolveU0(reshade::api::command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_x,
        uint32_t group_count_y, uint32_t group_count_z, SkU0Ident& ck);

private:
    bool U0Record(reshade::api::command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_x,
        uint32_t group_count_y, uint32_t group_count_z, SkU0Ident& ck, ResInfo& ri, uint64_t& clay,
        ID3D12Resource*& ck_res_for_ring);
    void U0Claim(SkU0Ident& ck, unsigned cand_hash, unsigned cand_bytes);
    void ResolveU1(reshade::api::command_list* cmd_list, SkU0Ident& ck, ID3D12Resource* cand, unsigned cand_hash,
        std::wstring& owner_ns, unsigned& want_scale, ID3D12Resource*& u1_res, unsigned& u1_bytes);
    void ResolveCb0(reshade::api::command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_y,
        const std::wstring& owner_ns, ID3D12Resource* inst, ID3D12Resource*& cb0_res, uint64_t& cb0_va,
        unsigned& cb0_size);
    void SnapshotState(reshade::api::command_list* cmd_list, uint64_t clay, ID3D12Resource* cand, unsigned cand_bytes,
        ID3D12Resource* u1_res, unsigned u1_bytes, const std::wstring& owner_ns, ID3D12Resource* inst);
    void Publish(reshade::api::command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_y, SkU0Ident& ck,
        ID3D12Resource* cand, unsigned cand_bytes, unsigned cand_hash, uint64_t clay);

    ShapekeyStore& store_;
    ShapekeyIdentity& identity_;
};
extern ShapekeyResolver g_sk_resolver;

class ShapekeyReplay {
public:
    ShapekeyReplay(ShapekeyStore& store, ShapekeyQueue& queue, ShapekeyIdentity& identity)
        : store_(store),
          queue_(queue),
          identity_(identity)
    {}
    void RunPasses(const std::wstring& ns, int mod_id, ID3D12Resource* inst, reshade::api::command_list* cmd_list);

private:
    struct OrdState {
        uint32_t frame;
        uint32_t draws;
        uint32_t nlists;
        const void* lists[8];
        const void* pass_cl;
    };
    bool FirstDrawThisFrame(int mod_id, ID3D12Resource* inst, const void* cl, MorphDump::DrainRecord& dr);
    void OwnBufFallback(
        const std::wstring& ns, int mod_id, unsigned u0hash, ID3D12Resource*& u0_res, unsigned& u0_bytes);
    void RepublishBindings(
        const std::wstring& ns, ID3D12Resource* inst, int mod_id, ID3D12Resource* u0_res, unsigned u0_bytes);
    void ReplayComputeState(const std::wstring& ns, ID3D12Resource* inst, int mod_id);
    void RunLists(const std::wstring& ns, ID3D12Resource* inst, int mod_id, reshade::api::command_list* cmd_list,
        const std::vector<SkPendingList>& lists, MorphDump::DrainRecord& dr, ID3D12Resource* u0_res, unsigned u0_bytes);
    void ReportDrain(const std::wstring& ns, int mod_id, const std::vector<SkPendingList>& lists,
        MorphDump::DrainRecord& dr, size_t left_for_others, size_t dropped_unowned, ID3D12Resource* u0_res,
        unsigned u0_bytes);

    std::mutex ord_mutex_;
    std::map<std::pair<int, ID3D12Resource*>, OrdState> ord_st_;
    ShapekeyStore& store_;
    ShapekeyQueue& queue_;
    ShapekeyIdentity& identity_;
};
extern ShapekeyReplay g_sk_replay;

// ---- submission order (sk_submit_order.cpp) --------------------------------------------------------

// The index at which `list` was submitted in `frame`; -1 when it was not (or the frame has aged out).
int SubmitIndexOf(uint32_t frame, const void* list);
// Lists seen by on_execute_command_list, for the SHAPEKEY-ORDER line. Counted only under g_debug.
extern std::atomic<unsigned long long> g_sub_seen;
