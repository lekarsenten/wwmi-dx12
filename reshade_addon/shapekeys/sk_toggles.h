#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct ShapekeyToggles {
    bool freeze = false; // run the chain, zero the deltas: the morph blocker
    bool strict_owner = true;
    bool cb0_snapshot = true;
    bool u0_size_recover = true;
    bool u0_clear = true; // the Loader accumulates: u0 must start zeroed
    bool u1_clear = true; // else u1 wraps negative and the Multiplier snaps
    bool clear_barrier = true; // a UAV barrier does not order the zero copy
    bool u1_by_identity = true; // resolve u1 by declared ShapeKeyScale identity
    bool publish_cs_u0 = true;
    bool publish_shader_state = true;
    bool shader_hash_alias = true;
};
extern ShapekeyToggles g_sk_toggles;

struct ShapekeyStats {
    std::atomic<unsigned> clear_cancelled{ 0 }; // clears cancelled so far (overlay I18)
    // The claim gate (CLAIMGATE) and the u0 table source (U0TABLE), counted under g_debug.
    std::atomic<unsigned> claim_ok{ 0 }; // exactly one on-screen mod declared this buffer
    std::atomic<unsigned> claim_nodecl{ 0 }; // no shapekey CALLBACK section declares this hash
    std::atomic<unsigned> claim_offscreen{ 0 }; // declared, but no claimant passed the 4-frame filter
    std::atomic<unsigned> claim_multi{ 0 }; // two on-screen mods declare it -- refused, correctly
    std::atomic<unsigned> u0tab_compute{ 0 }; // this dispatch's own compute table answered
    std::atomic<unsigned> u0tab_fallback{ 0 }; // no compute table at that param -- used the union
    std::atomic<unsigned> u0tab_none{ 0 }; // neither had anything
};
extern ShapekeyStats g_sk_stats;
