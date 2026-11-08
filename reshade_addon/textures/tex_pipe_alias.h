#pragma once
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../instrumentation/diag_gate.h"

// Debug pipe `texalias`: game texture hashes taken as mod targets, ahead of the texture cache's links. The selector
// asks it first; the texture cache never learns from it (TexSelector::ProvenRuleFor).
class TexPipeAlias {
public:
    // The mod target `hash` is aliased to from the pipe, else 0.
    WWMI_DEBUG_FN uint32_t Of(uint32_t hash) WWMI_DEBUG_BODY(return 0;);
    // texalias: lists every alias; <game> <rule> sets one; <game> off removes it. The live textures carrying the
    // game hash swap again at once.
    WWMI_DEBUG_FN std::string Command(const std::vector<std::string>& args) WWMI_DEBUG_BODY(return {};);

private:
    static unsigned Restamp(uint32_t hash, bool on);

    std::shared_mutex mutex_;
    std::unordered_map<uint32_t, uint32_t> alias_; // game texture hash -> mod target
    std::atomic<bool> any_{ false };
};
WWMI_DEBUG_VAR TexPipeAlias g_tex_pipe_alias;
