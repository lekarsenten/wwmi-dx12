#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct DrawToggles {
    bool disable_vb = false;
    uint64_t hide_vs_hash = 0; // hide every draw with this VS hash (0 = off)
    bool wwmi_geom = true; // bind the mod's shipped buffers for DIRECT comps
    bool port_draw_list = true; // give their engine the draw's command list
    bool lod_pin = false; // recognise the char's lower-LOD draws (log only)
    bool custom_subdraw = true; // render `run = [CustomShader*]` parts
    bool custom_blend = true; // clone the material PSO with the ini's blend
    bool wcl_draw = true; // execute ini command lists (off = none run)
    float custom_blend_factor = -1.0f; // -1 = use the ini's factor
    bool shadow_vanilla = false; // release depth-only passes to the game
    bool shadow_via_claim = true; // claim them instead; needs g_shadow_vanilla off
    bool push_game_vb = true;
    bool skel_copies = true; // one merged skeleton per copy of a character in a frame (B20)
    int dbg_comp5_suppress_rtv = -1; // bow A/B: drop comp5 passes with this rtv count
    bool skip_translucent_cloth = false; // bow A/B: drop the modded comp's 2-RT pass
    bool charvb_refuse_xmod = false;
    bool ib_anchor_claim = true;
    bool ps_slot_stamp = true;
    bool ps_slot_stamp_nulls = true;
    bool ibhash_suppress = false;
    bool sk_run_passes = true; // OFF explodes Applier mods: these passes write vb0
    bool restore_vbib = true; // restore VB0-4 + IB after our draws (3dmigoto auto-restore)
    bool restore_cb = true; // restore bone CB root 13/14 after our draws
    bool restore_nonpso = true;
    bool hide_unclaimed_char = false;
    bool skip_no_bone13 = false;
    bool merged_bone_cb = true; // B8: merged-skeleton bone CB injection
};
extern DrawToggles g_draw_toggles;

struct DrawStats {
    std::atomic<uint32_t> sections_this_frame{ 0 };
    FrameCounter port_fellback_count;
    FrameCounter port_sect_runs;
    FrameCounter port_sect_nocb;
    std::atomic<uint32_t> wwmi_geom_count{ 0 };
    std::atomic<uint32_t> custom_blend_count{ 0 };
    std::atomic<uint32_t> custom_blend_bound_count{ 0 };
    std::atomic<uint64_t> vb6_readers_claimed{ 0 };
    std::atomic<uint64_t> vb6_readers_unclaimed{ 0 };
    std::atomic<uint64_t> freeze_binds{ 0 };
    std::atomic<uint64_t> x32_mid_total{ 0 };
    std::atomic<uint64_t> x32_mid_skipped_total{ 0 };
    std::atomic<uint32_t> charvb_claims{ 0 };
    std::atomic<uint32_t> charvb_xmod{ 0 };
    std::atomic<uint32_t> charvb_refused{ 0 };
    std::atomic<uint32_t> ib_anchor_learned{ 0 }; // index buffers anchored to a mod this session
    std::atomic<uint32_t> ib_anchor_claims{ 0 }; // draws claimed because of one
    std::atomic<uint32_t> ps_slot_stamp_count{ 0 };
    std::atomic<uint32_t> ibhash_suppress_count{ 0 };
    FrameCounter draw_replace_count;
    uint64_t last_replace_hash = 0;
    uint32_t last_replace_ib_count = 0;
};
extern DrawStats g_draw_stats;
