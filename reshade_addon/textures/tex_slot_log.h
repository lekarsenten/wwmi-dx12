#pragma once
#include <d3d12.h>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <atomic>
#include <mutex>
#include <vector>
#include "../instrumentation/diag_gate.h"

// What fired TexPlacement::Swap when it is not the placement itself ("retry", "restamp").
inline thread_local const char* t_tex_swap_site = nullptr;

// TEXSLOT (debug builds): which mod texture a game texture's slots get, logged when it changes for that texture.
class TexSlotLog {
public:
    WWMI_DEBUG_FN void Note(const char* site, ID3D12Resource* game_res, uint32_t bound, uint32_t rule,
        ID3D12Resource* mod_res, bool snapshot) WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN void Forget(uint64_t game_res) WWMI_DEBUG_BODY();

private:
    struct Logged {
        long long value;
        unsigned changes;
    };
    static constexpr unsigned kChanges = 8; // a texture's changes logged before it goes quiet
    static std::string BoundHow(uint32_t bound, uint32_t rule);

    std::shared_mutex mutex_;
    std::unordered_map<uint64_t, Logged> logged_; // a game texture (| 1 at a claimed draw) -> what was logged for it
};
WWMI_DEBUG_VAR TexSlotLog g_tex_slot_log;

// TEXTRACE (debug builds, debug pipe `textrace <hash> [frames]`): for a few frames, every write to one game
// texture's slots, who made it and on which thread, with the variables its sections read that are set.
class TexTrace {
public:
    WWMI_DEBUG_FN std::string Arm(uint32_t hash, uint32_t frames) WWMI_DEBUG_BODY(return {};);
    WWMI_DEBUG_FN bool Armed() const WWMI_DEBUG_BODY(return false;);
    WWMI_DEBUG_FN bool On(uint32_t hash) const WWMI_DEBUG_BODY(return false;);
    WWMI_DEBUG_FN void Line(_Printf_format_string_ const char* fmt, ...) WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN std::wstring ModText(ID3D12Resource* mod_res) WWMI_DEBUG_BODY(return {};);
    WWMI_DEBUG_FN void FrameEnd() WWMI_DEBUG_BODY();

private:
    struct Var {
        std::wstring name;
        const float* slot = nullptr;
    };
    std::atomic<uint32_t> hash_{ 0 };
    std::atomic<uint32_t> left_{ 0 };
    std::mutex mutex_;
    std::vector<Var> vars_;
    unsigned gen_ = 0;
};
WWMI_DEBUG_VAR TexTrace g_tex_trace;
