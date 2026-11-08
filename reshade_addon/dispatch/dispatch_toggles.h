#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct DispatchToggles {
    uint64_t morph_cs_hash = 0x4e91ecddc203a6cfULL;
    uint64_t clear_cs_hash = 0x099222f33b1f30e6ULL;
    bool honour_skip = false; // cancel the game's dispatch when the section asked
    bool engine_dispatch = false;
    bool cancel_game_clear = true;
    bool shader_override_dispatch = true;
};
extern DispatchToggles g_dispatch_toggles;
