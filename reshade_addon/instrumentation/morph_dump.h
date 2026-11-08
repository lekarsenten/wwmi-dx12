// MORPH DUMP -- an UNCONDITIONAL record of compute dispatches, modded and unmodded alike.
//
// WHY THIS EXISTS. Every other instrument for the shapekey chain was gated on something that made it
// blind to the case we needed: CSU0-CHAIN is counts only, with no per-dispatch row to correlate; frame
// analysis dumps only what we PUBLISH into PipeState, and nothing is published for a buffer no mod
// declares, so an unmodded character's morph dispatch emits ZERO files; SHAPEKEY-ORDER is scoped to mods
// by construction. So "are we touching unmodded characters?" could not be answered by any capture.
//
// THE FILTER IS THE BUG: each of those was gated on OWNERSHIP, and ownership is the thing under
// investigation -- an instrument that assumes the answer cannot measure it. This records EVERY compute
// dispatch while armed: no mod filter, no on-screen filter, no shader filter, not even a morph filter. A
// dispatch we cannot identify is exactly the dispatch worth seeing. Filtering happens at ANALYSIS time,
// over a complete record, where it is reversible.
//
// METADATA ONLY, DELIBERATELY. No views are created, no GPU work is issued, no resource contents are
// read. Every device removal in this project came from those three things at a dispatch: raw-buffer UAVs
// built over descriptor tables, unaligned CBVs, and readbacks on a still-recording list.
//
// WHO RESOLVES WHAT: the caller resolves and passes the fields, and this module only buffers and writes.
#pragma once

#include <cstdint>
#include "diag_gate.h"

namespace MorphDump {

// One dispatch, fully resolved by the caller.
struct Record {
    uint32_t frame = 0;
    uint64_t cs_hash = 0;
    uint32_t gx = 0;
    uint32_t gy = 0;
    uint32_t gz = 0;
    uint64_t cmd_list = 0;
    uint64_t cmp_layout = 0; // the bound compute root signature

    uint64_t u0_res = 0;
    uint32_t u0_size = 0;
    uint32_t u0_desc_hash = 0; // crc32c of the reconstructed D3D11_BUFFER_DESC -- their matcher's key
    int u0_declared = -1; // 1 = a [TextureOverride] declares it, 0 = none, -1 = unresolved
    int u0_param = -1;
    int owner = -1; // the mod the claim gate named, so dispatch/drain/pass rows JOIN
    int u0_slot_age = -1;
    uint8_t u0_slot_src = 0; // 0 = never written by a hook we own, 1 = update_, 2 = copy_
    uint8_t u0_why = 0;
    int u0_erased = -1;
    unsigned res_map_n = 0;

    int ct_match = -1;
    int ct_age = -1;
    int ct_seqd = -1;
    int ck_why = -1;
    unsigned ck_size = 0;
    int ck_decl = 0;
    int ck_live = 0;
    int ck_push = -1;
    int ck_claims = 0;
    int ck_buf = -1;
    int ck_tex = -1;
    unsigned ck_off = 0;
    uint64_t ck_heap = 0;
    unsigned ck_slot = 0;
    int ck_age = -1;
    int ck_src = 0;
    unsigned geo_lo = 0;
    unsigned geo_hi = 0;
    int geo_verdict = 0;

    // cs-b0 -- the morph weight CB, by root parameter from the same layout.
    uint64_t cb0_res = 0;
    uint64_t cb0_va = 0;
    uint32_t cb0_size = 0; // from the SAME RootCbvBind as res and va -- see the capture site
    int b0_param = -1;

    bool skip = false; // what on_dispatch returns: true = the game's dispatch was CANCELLED
    unsigned sections_fired = 0; // sections OUR dispatch-firing loop INVOKED (not what they queued)
    // The half sections_fired never measured.
    unsigned queued = 0; // command lists the observer actually captured this dispatch
    uint8_t queue_reason = 0; // 0 queued, 1 not-shapekey, 2 unused, 3 no-run, 4 dup
    const wchar_t* so_section = nullptr; // the [ShaderOverride*] their engine matched, null if none
    int so_match = -1;
};

struct PassRecord {
    uint32_t frame = 0;
    int mod_id = -1;
    int owner_mod = -1; // whom the QUEUING dispatch named, -1 if it named nobody
    unsigned pass_index = 0; // 1-based
    unsigned pass_count = 0;
    uint64_t queued_cs = 0; // the game dispatch whose callback queued this list
    unsigned queued_tgy = 0; // its thread_group_count_y -- the batch selector
    unsigned emitted_n = 0;
    uint32_t emitted_x = 0;
    uint32_t emitted_y = 0;
    uint32_t emitted_z = 0;
    // The registers the trace names, per pass. 1 = bound, 0 = not.
    int t0 = -1;
    int t1 = -1;
    int t6 = -1;
    int t33 = -1;
    int u5 = -1;
    int u6 = -1;
    // The three WWMIv1 variables the shaders loop on.
    float checksum = 0;
    float shapekey_vtx = 0;
    float custom_vtx = 0;
    uint64_t cb0_res = 0; // the PipeState's b0. NOT what the pass runs on -- see snap_* below.

    float x0 = 0;
    float y0 = 0;
    float z0 = 0;
    float x1 = 0;
    float y1 = 0;
    float z1 = 0;

    int snap_hit = -1;
    uint64_t snap_res = 0;
    uint64_t snap_va = 0;
    unsigned snap_bytes = 0;

    // IDENTITY -- the same questions as on DrainRecord, sampled at pass time.
    uint64_t ps_addr = 0;
    uint64_t vb_last_ps = 0;
    unsigned vb_binds = 0;
    unsigned vb_nonnull = 0;

    uint64_t vb0_state = 0;
    uint64_t vb6_state = 0;
    uint64_t vb0_shim = 0;
    uint64_t vb6_shim = 0;
    uint32_t vb0_shim_size = 0;
    uint32_t vb0_shim_stride = 0;

    const wchar_t* ns = nullptr;
};

struct DrainRecord {
    uint32_t frame = 0;
    int mod_id = -1;
    uint8_t exit = 0; // 0 completed, 1 empty-ns, 2 same-frame-repeat (a later draw)
    unsigned pending_in = 0;
    unsigned matched = 0; // lists this mod claimed (lists.size())
    unsigned left = 0; // left_for_others: still queued for other mods
    unsigned dropped = 0; // sk_dropped_unowned: queued by a dispatch that named no mod
    unsigned ran = 0; // passes actually executed by this call
    uint8_t disp_seen = 0; // 1 = the game's shapekey dispatch was recorded this frame before us
    uint64_t u0_res = 0; // what the drain republished as cs-u0
    uint32_t u0_bytes = 0;
    uint64_t vb0_state = 0;
    uint64_t vb0_shim = 0;
    uint64_t vb6_state = 0;
    uint64_t vb6_shim = 0;

    uint64_t ps_addr = 0;
    uint64_t vb_last_ps = 0;
    unsigned vb_binds = 0;
    unsigned vb_nonnull = 0;

    uint8_t u0_clear = 0;
    uint8_t u1_clear = 0;
    uint32_t u0_identity = 0; // g_buffer_reloc.IdentityBytes(): what the buffer was BORN as
    uint32_t reloc_from = 0;
    uint32_t reloc_to = 0;

    const wchar_t* ns = nullptr;
};

WWMI_DEBUG_FN void Arm(unsigned max_records) WWMI_DEBUG_BODY();

// J1's capture, from now: these tables, the buffer contents (SKCAP) and the one-shot probes.
WWMI_DEBUG_FN void ArmCapture() WWMI_DEBUG_BODY();
// J4: ArmCapture at the next relocation of a mod's buffer -- its next new instance.
WWMI_DEBUG_FN void ArmCaptureAtNewInstance() WWMI_DEBUG_BODY();
WWMI_DEBUG_FN bool CaptureWaiting() WWMI_DEBUG_BODY(return false;);
WWMI_DEBUG_FN void NoteNewInstance(uint32_t hash) WWMI_DEBUG_BODY();

// One drain attempt. Recorded whenever armed, on every exit path.
WWMI_DEBUG_FN void RecordDrain(const DrainRecord& r) WWMI_DEBUG_BODY();

struct DrawRecord {
    uint32_t frame = 0;
    int mod_id = -1;
    int comp = -1; // [TextureOverrideComponentN]
    uint64_t list = 0; // the native command list the draw was recorded on
    uint32_t index_count = 0;
    uint32_t first_index = 0;
    int32_t vertex_offset = 0;
    uint8_t is_depth = 2; // 1 = no RTV and a DSV bound (depth/shadow pass), 0 = colour, 2 = unknown
    uint64_t vb6_res = 0; // slot 6 as the GAME bound it, before any substitution
    uint64_t vb6_va = 0;
    uint32_t vb6_bytes = 0;
    uint32_t vb6_stride = 0;
    uint8_t vb6_verdict = 0; // 0 not relocated (reads it AS BOUND), 1 SUBSTITUTED, 2 DECLINED, 3 unbound
    uint64_t sub_va = 0; // what it reads after substitution (verdict 1)
    uint32_t sub_bytes = 0;
    uint64_t my_u0 = 0; // this mod's u0 per the store (0 = none published)
    uint64_t vb0_res = 0; // slot 0 as the game bound it
    uint32_t ini_null_unbinds = 0;
    uint32_t mid_restores = 0;
    uint32_t mid_ia_skipped = 0;
    const wchar_t* ns = nullptr;
};
WWMI_DEBUG_FN void RecordDraw(const DrawRecord& r) WWMI_DEBUG_BODY();

struct GameDrawRecord {
    uint32_t frame = 0;
    int mod_id = -1;
    int comp = -1;
    uint64_t list = 0;
    uint32_t thread = 0;
    uint32_t index_count = 0;
    uint32_t first_index = 0;
    uint32_t instance_count = 0;
    uint64_t vs = 0;
    uint64_t ps = 0;
    uint64_t pso = 0;
    uint32_t rtv_count = 0;
    uint8_t has_dsv = 0;
    uint64_t dsv = 0;
    uint32_t pass_gen = 0;
    float vp_w = 0.f;
    float vp_h = 0.f;
    uint32_t stencil_ref = 0;
    uint8_t stencil_ref_set = 0;
    uint64_t vb0_va = 0;
    uint64_t vb6_va = 0;
    uint32_t vb6_stride = 0;
    uint8_t claimed = 0; // mc still set after every gate (the component path ran for it)
    uint8_t shadow = 0; // is_full_shadow_pass recognised it (depth pass, bone VA matched)
    uint8_t cast = 0; // (shadow) this draw triggered the cast of every component into its target
    uint8_t hidden = 0; // dropped at entry: H (per-component hide) or hide-by-VS
    uint32_t injected = 0; // DrawIndexedInstanced calls the port issued inside this hook
    uint8_t ret = 0; // hook return: 1 = game draw suppressed, 0 = game draw proceeds
    uint64_t ib_va = 0;
    uint8_t same_mesh = 0;
    const wchar_t* ns = nullptr;
    uint64_t cb3_va = 0; // the game's VS b3 / b4 at entry: each instance's bones have their own
    uint64_t cb4_va = 0;
    uint8_t merges = 0; // SkeletonMerger runs its section entered: this draw merged the bones
};
WWMI_DEBUG_FN void RecordGameDraw(const GameDrawRecord& r) WWMI_DEBUG_BODY();

WWMI_DEBUG_FN void NoteResourceErased(uint64_t handle) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN int WasResourceErased(uint64_t handle) WWMI_DEBUG_BODY(return -1;);

// One of our passes. Recorded whenever armed, with no ownership filter -- same rule as the dispatch rows.
WWMI_DEBUG_FN void RecordPass(const PassRecord& r) WWMI_DEBUG_BODY();

// True while a capture is running. An atomic load, so the dispatch hook can gate on it for free.
WWMI_DEBUG_FN bool Armed() WWMI_DEBUG_BODY(return false;);

// Called from on_dispatch for EVERY compute dispatch, with NO filtering whatsoever.
WWMI_DEBUG_FN void RecordDispatch(const Record& r) WWMI_DEBUG_BODY();

// Flush to disk and disarm. Safe when not armed. Called on reaching the cap, on a re-arm and by the overlay's J2.
WWMI_DEBUG_FN void Close() WWMI_DEBUG_BODY();

} // namespace MorphDump
