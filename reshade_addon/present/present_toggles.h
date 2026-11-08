#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct PresentToggles {
    bool skip_engine_tick = false; // A9: perf A/B only -- no [Present] lists, no present pass; mods stop updating
    bool frame_end_step =
        true; // presents from another thread (frame generation): the per-frame step at the game's own frame end (Numpad 7)
    bool on_game_queue = false;
    bool flush_immediate_first = true;
    bool allow_buffer_resize = true;
    bool health_heartbeat = true; // log the counters that GROW, every 600 frames
};
extern PresentToggles g_present_toggles;

struct PresentStats {
    std::atomic<uint64_t> presentq_same{ 0 };
    std::atomic<uint64_t> presentq_diff{ 0 };
};
extern PresentStats g_present_stats;
