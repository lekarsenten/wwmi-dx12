#pragma once
// The dispatch path's parts, called in order by on_dispatch (dispatch_hook.cpp).
#include "../addon_common.h"
#include "../hooks/command_list_registry.h"
#include "../shapekeys/shapekey_chain.h" // SkU0Ident
#include "../instrumentation/diag_gate.h"

struct DispatchArgs {
    reshade::api::command_list* cmd_list;
    uint64_t cs_hash;
    uint32_t group_count_x;
    uint32_t group_count_y;
    uint32_t group_count_z;
};
struct DispatchVerdict {
    bool skip = false;
    unsigned sections_fired = 0;
};

void DispatchDiagShapekey(const DispatchArgs& da); // dispatch_diag.cpp
void DispatchDiagMorph(const DispatchArgs& da); // dispatch_diag.cpp
DispatchVerdict RunDispatchOverrides(const DispatchArgs& da, SkU0Ident& ck); // dispatch_overrides.cpp
WWMI_DEBUG_FN void DispatchCapture(const DispatchArgs& da, const SkU0Ident& ck, bool skip_dispatch,
    unsigned md_sections_fired) WWMI_DEBUG_BODY(); // dispatch_capture.cpp
void DispatchDiagSkinProbe(const DispatchArgs& da, const CmdState& state); // dispatch_diag.cpp
void LearnSkinCache(const DispatchArgs& da, const CmdState& state); // skin_cache_learn.cpp
void DispatchDiagSkinBlueprint(const DispatchArgs& da, const CmdState& state); // dispatch_diag.cpp
void DispatchDiagStalker(const DispatchArgs& da, const CmdState& state); // dispatch_diag.cpp

bool on_dispatch(
    reshade::api::command_list* cmd_list, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z);
