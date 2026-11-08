#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct RfxToggles {
    bool patch = true;
    bool blend_debug_zero = false; // src=0,dst=1: show the background behind the piece
    bool tex = true;
    bool tex_main = true; // C4S: bind only into shaders that declare t0, t1 and t2
    bool tex_gbuffer = true; // C4G: bind only in GBuffer passes (4+ render targets), as E2
};
extern RfxToggles g_rfx_toggles;

struct RfxStats {
    std::atomic<uint32_t> glow_count{ 0 }; // patched draws that ran with the glow block's maps bound
    std::atomic<uint32_t> glow_params{ 0 }; // components the ini armed (h/brightness/GlowMap)
    std::atomic<uint32_t> patch_draws{ 0 }; // draws issued on a patched variant
    std::atomic<uint32_t> patch_nobind{ 0 }; // variant existed, its maps could not be bound: drawn unpatched
    std::atomic<uint32_t> tex_count{ 0 };
    std::atomic<unsigned long long> mirror_misses{ 0 }; // slots mirrored as null for want of a source record
};
extern RfxStats g_rfx_stats;
