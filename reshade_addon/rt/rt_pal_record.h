#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <reshade.hpp>
#include "../hooks/command_list_registry.h" // CmdState

// RTPAL: a dispatch that writes a mod's BLAS ring records its section's first vertex and bone palette. A shader seen
// writing a ring is examined every dispatch, any other one whole frame in 120.
class RtPalRecorder {
public:
    void Record(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash);

private:
    struct CsNote {
        uint32_t exam_frame = 0;
        uint32_t ring_gen = 0; // RtSkinRings::Gen() when last examined -- a new ring re-examines every shader
        bool writes_ring = false;
    };
    std::mutex mutex_;
    std::unordered_map<uint64_t, CsNote> by_cs_;
};
extern RtPalRecorder g_rt_pal_recorder;
