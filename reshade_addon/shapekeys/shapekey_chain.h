// The shapekey chain's seam to the rest of the addon: what the hooks call into, and the counters the
// overlay reads. The chain's own units and their couplings are in sk_internal.h.
#pragma once

#include <reshade.hpp>
#include <d3d12.h>
#include <cstdint>
#include <string>
#include "../mods/target_class.h"

struct SkBinding {
    ID3D12Resource* res = nullptr;
    unsigned bytes = 0;
};
struct SkBufferBinding : SkBinding {
    uint64_t va = 0;
};

// One register of a mod instance's published state; false = nothing live recorded (or reg out of range).
bool SkGetBinding(const std::wstring& ns, ID3D12Resource* inst, TargetClass kind, unsigned reg, SkBufferBinding* out);
/* The three rates for one register, for the heartbeat (UAVCLEAR-RATE). */
void SkZeroStats(TargetClass kind, unsigned reg, unsigned long long* cleared, unsigned long long* not_ours,
    unsigned long long* not_ready);

struct SkU0Ident {
    bool shapekey = false; // this dispatch is one of the two overridden shapekey computes
    uint64_t layout = 0; // the compute root signature in force
    int have = 0; // a u0 table entry was found for that layout
    unsigned param = 255; // its root param ...
    unsigned off = 0; // ... and the descriptor offset within that table
    unsigned size = 0; // the resolved buffer's extent, 0 if it could not be established
    unsigned desc = 0; // crc32c of the reconstructed D3D11_BUFFER_DESC -- the ini's `hash =`
    int declared = 0; // exactly one on-screen mod claimed it (never a tie -- see the gate)
    int owner = -1; // that mod's index, or -1
    int claims = 0; // how many claimed it; >1 is a REFUSAL, not a tie to break
    int live = 0; // the resource is live (ResourceTracker), so safe to dereference
    int push = -1; // PushGameComputeUav result, -1 = never attempted
    int why = 1; // 0 ok, 1 no u0 bound, 2 heap miss, 3 no resource, 4/5 extent failed
    uint64_t heap = 0;
    unsigned slot = 0;
    int ri_age = -1;
    int ri_src = 0;
    int ri_buf = -1; // ResInfo::is_buffer  -- 0 here means cs-u0 resolved to a NON-BUFFER
    int ri_tex = -1; // ResInfo::tex_cand
    int ct_match = -1;
    int ct_age = -1;
    int ct_seqd = -1;
    int by_owner = 0; // u0 came from the owner's declared hash, the slot having had no answer
    double filter = 0.0; // the `cs` operand their engine evaluated for this dispatch
    unsigned geo_lo = 0;
    unsigned geo_hi = 0;
    int geo = 0;
    ID3D12Resource* inst = nullptr; // the game's u0 this dispatch published under, before relocation
};

// Which buffer is this dispatch's u0, and whose character owns it (sk_resolve.cpp).
void SkResolveU0(reshade::api::command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_x, uint32_t group_count_y,
    uint32_t group_count_z, SkU0Ident& ck);
bool SkU0ResourceNow(reshade::api::command_list* cmd_list, ID3D12Resource** res, unsigned* size);
bool SkChainWroteU0Recently(ID3D12Resource* res, uint32_t frame);

// The dispatch scope, called by the generic compute hook around each dispatch (sk_queue.cpp).
void SkNoteDispatchList(const void* native_cl, uint32_t frame);
void SkBeginDispatchScope(uint64_t cs_hash, uint32_t group_count_y, const SkU0Ident& ck);
void SkEndDispatchScope(bool shapekey);
unsigned SkTakeQueueOutcome(uint8_t* reason);

// The mod's queued lists replayed on its instance's first draw of the frame (sk_replay.cpp).
void RunShapeKeyPasses(const std::wstring& ns, int mod_id, ID3D12Resource* inst, reshade::api::command_list* cmd_list);
void SkNoteDrawVbs(int mod, ID3D12Resource* const* res, unsigned n); // the draw names the instance

// Resource lifetime: a destroyed resource must not survive in the chain's snapshots or its queue.
void SkOnDestroyed(ID3D12Resource* dead);

// A list submitted under g_debug: its place in the frame's submission order (SHAPEKEY-ORDER).
void SkNoteSubmitted(const void* list);
