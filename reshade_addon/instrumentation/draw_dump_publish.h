#pragma once
// Frame-analysis publishing for a draw (armed captures only).
#include <reshade.hpp>
#include <d3d12.h>
#include <cstddef>
#include <cstdint>
#include "diag_gate.h"

// The game's depth / render targets, and (g_diag_arms.dump_game_textures) the textures its shaders read.
WWMI_DEBUG_FN void PublishGameBoundStateForDump(reshade::api::command_list* cmd_list) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void PublishGameTexturesForDump(reshade::api::command_list* cmd_list) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void PublishGameDrawForDump(reshade::api::command_list* cmd_list, bool injected, ID3D12Resource* sub6_res,
    uint64_t sub6_va, uint32_t sub6_bytes, uint8_t sub6_verdict) WWMI_DEBUG_BODY();
WWMI_DEBUG_FN void PublishGameDrawForDumpEnd() WWMI_DEBUG_BODY();

struct X27Cb {
    uint8_t kind = 0;
    uint8_t param = 0xFF;
    uint32_t off = 0;
    ID3D12Resource* res = nullptr;
    uint64_t va = 0;
    uint32_t size = 0;
};
WWMI_DEBUG_FN uint64_t X27ResolveVsCbs(reshade::api::command_list* cmd_list, X27Cb out[8])
    WWMI_DEBUG_BODY(return 0;); // returns the layout
WWMI_DEBUG_FN void X27Format(char* buf, size_t cap, const X27Cb cbs[8]) WWMI_DEBUG_BODY(if (cap) buf[0] = 0;);
