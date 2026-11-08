#pragma once
#include <cstdint>
#include <reshade.hpp>
#include "../instrumentation/diag_gate.h"

// The present path's diagnostics, called by FrameStepper in order: the PRESENTQ queue check, the config watch and
// the heartbeats (SEEDHB, HEARTBEAT, PIPESTAT, the health heartbeat).
class PresentDiag {
public:
    void QueueDiag(reshade::api::command_queue* queue);
    void ConfigWatchTick();
    void SeedHeartbeat();
    void Heartbeat(uint32_t texinj_last);
    void HealthHeartbeat();

private:
    void MonitorConfig();

    int cfg_fully_load_ = -2;
    int cfg_lod_bias_ = -999;
    int cfg_image_detail_ = -2;
    int config_watch_frames_ = 0;
    const void* presentq_queue_ = nullptr;
    const void* presentq_claim_queue_ = nullptr;
    int presentq_same_ = -1;
    int seed_frames_ = 0;
    uint32_t heartbeat_next_ = 0;
    size_t heartbeat_learned_ = 0;
    uint32_t heartbeat_rfx_ = 0;
    unsigned long long vb6_claimed_last_ = 0;
    unsigned long long vb6_unclaimed_last_ = 0;
    unsigned long long presentq_same_last_ = 0;
    unsigned long long presentq_diff_last_ = 0;
    unsigned long long claim_exec_last_ = 0;
    unsigned long long freeze_binds_last_ = 0;
    unsigned u0tab_compute_last_ = 0;
    unsigned u0tab_fallback_last_ = 0;
    unsigned u0tab_none_last_ = 0;
    unsigned claim_ok_last_ = 0;
    unsigned claim_nodecl_last_ = 0;
    unsigned claim_offscreen_last_ = 0;
    unsigned claim_multi_last_ = 0;
};
extern PresentDiag g_present_diag;
