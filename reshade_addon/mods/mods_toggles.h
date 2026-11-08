#pragma once
#include <atomic>
#include <cstdint>
#include <string>
#include "../instrumentation/diag_gate.h"

struct ModsToggles {
    bool ibhash_gpu_skip = false;
};
extern ModsToggles g_mods_toggles;
