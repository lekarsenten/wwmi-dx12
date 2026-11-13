#pragma once
// Debug: what the decensor's fade switches (rfx_dxil.h RfxOpacitySite) read from the game's material CBs at claimed
// draws -- an OPACITY log line when a draw's values change, and the debug pipe's `opacity` for the latest ones.
#include <reshade.hpp>
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vector>
#include "../rabbitfx/rfx_dxil.h"
#include "diag_gate.h"

class OpacityWatch {
public:
    WWMI_DEBUG_FN void NoteShader(uint64_t ps_hash, const std::vector<RfxOpacitySite>& sites) WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN void NoteDraw(reshade::api::command_list* cmd_list, uint64_t ps_hash, int mod_id, int comp)
        WWMI_DEBUG_BODY();
    // opacity [frames]: each (shader, mod, component) drawn within that many frames (120), with what it read; an
    // unmodded skinned draw is mod -1 with its index count as the component.
    WWMI_DEBUG_FN std::string Command(const std::vector<std::string>& args) WWMI_DEBUG_BODY(return {};);

private:
    struct Seen {
        uint32_t frame = 0;
        uint32_t changes = 0;
        std::string text;
    };
    std::mutex mutex_;
    std::unordered_map<uint64_t, std::vector<RfxOpacitySite>> sites_;
    std::map<std::tuple<uint64_t, int, int>, Seen> seen_;
};
WWMI_DEBUG_VAR OpacityWatch g_opacity_watch;

// Debug pipe `cbrows`: rows of one game shader's constant buffer as its draws read them -- a CBROWS log line when
// they change, and the latest ones.
class CbRowsWatch {
public:
    WWMI_DEBUG_FN void NoteDraw(reshade::api::command_list* cmd_list) WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN std::string Command(const std::vector<std::string>& args) WWMI_DEBUG_BODY(return {};);

private:
    std::atomic<uint64_t> ps_{ 0 };
    std::mutex mutex_;
    unsigned reg_ = 0;
    unsigned first_ = 0;
    unsigned rows_ = 0;
    uint32_t changes_ = 0;
    uint32_t frame_ = 0;
    std::string last_;
};
WWMI_DEBUG_VAR CbRowsWatch g_cb_rows;
