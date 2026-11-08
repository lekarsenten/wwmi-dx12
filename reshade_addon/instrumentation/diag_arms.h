#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "diag_gate.h"

// The diagnostics the overlay arms: requests, and the frame budgets Tick() counts down once a frame.
struct DiagArms {
    std::atomic<bool> skeldump_req{ false }; // overlay button -> dump the merged skeleton
    std::atomic<bool> skelchain_req{ false }; // overlay button -> dump the whole skeleton chain
    std::atomic<bool> chaintrace_req{ false };
    std::atomic<int> chaintrace_frames{ 0 };
    bool dump_game_textures = true; // frame dump includes the game's bindless textures
    bool stall_watchdog = true;
    unsigned stall_ms = 1000;
    std::atomic<uint32_t> stall_count{ 0 };
    std::atomic<int> log_all_draws{ 0 };
    std::atomic<int> texflip_arm{ 0 };
    std::atomic<unsigned> texflip_gen{ 0 };
    std::atomic<int> texnoswap_arm{ 0 }; // frames left: TEXNOSWAP logs target-shaped placements left vanilla
    std::atomic<int> texnoswap_n{ 0 }; // lines this arm (capped in TexNoSwapNote)
    std::atomic<unsigned> texnoswap_gen{ 0 }; // bumped per arm: TexNoSwapNote's once-per-resource set restarts
    std::atomic<int> log_merged{ 0 };
    std::atomic<int> occl_arm{ 0 };
    std::atomic<bool> wwmi_parse{ false }; // overlay button: log each mod's parsed layout (self-check)
    std::atomic<bool> log_lod{ false }; // persistent CDRAW capture: walk through LODs, log each char draw
    std::atomic<int> shaderdiff_arm{ 0 }; // frames left in a SHADERDIFF capture
    std::atomic<bool> dump_shaders_requested{ false };
    std::atomic<int> shadow_recog_watch{ 0 }; // frames left on the SHADOWRECOG watch
    std::atomic<int> sectex_watch{ 0 }; // frames left logging every texture a section sees (SECTEX)
    std::atomic<int> vbdiag_frames{ 0 };

    // The draw-log, TEXFLIP, TEXNOSWAP and merged-draw budgets, one frame down.
    void Tick();
};
extern DiagArms g_diag_arms;
