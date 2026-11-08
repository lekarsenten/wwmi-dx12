#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct HooksToggles {
    bool reloc_forget_on_destroy = true;
    bool cmd_reset_clears = true;
    bool shader_replace = false; // swap a game shader for one of ours by hash
};
extern HooksToggles g_hooks_toggles;

struct HooksStats {
    std::atomic<uint32_t> cmd_resets{ 0 };
    std::atomic<uint32_t> init_res_all{ 0 }; // init_resource, every type
    std::atomic<uint32_t> init_res_buf{ 0 }; // ... of which buffers WITH a GPU VA (what the map takes)
    std::atomic<uint32_t> init_view_all{ 0 }; // init_resource_view, every type
};
extern HooksStats g_hooks_stats;
