// The compute path: every dispatch the game issues, and the shapekey work we run off it.
// See dispatch_parts.h.
#include "../addon_common.h"
#include "../mods/shader_alias.h"
#include "../rt/rt_pal_record.h" // RT: the RTPAL recorder
#include "../instrumentation/gpu_timer.h"
#include "dispatch_parts.h"
#include "dispatch_toggles.h"
#include "../addon/addon.h"
#include "../shapekeys/sk_toggles.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../migoto/frame_analysis_driver.h"
#include "../mods/mod_set.h"

DispatchToggles g_dispatch_toggles;

bool on_dispatch(command_list* cmd_list, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
    PROF(PROF_Dispatch);
    if (g_addon.disabled || !g_addon.is_dx12) return false;
    MigotoPort::FrameAnalysisDrawScope
        fa_scope; // see on_draw_indexed_body: g_fa_mtx first, one hook at a time while armed
    uint64_t cs_hash = 0;
    cmd_list->get_private_data(s_iid_cs_hash, &cs_hash);
    if (!cs_hash) return false;
    ModSetScope set_scope(cmd_list);
    GpuBracket _gt((ID3D12GraphicsCommandList*)cmd_list->get_native(), GT_DispatchHook);
    const DispatchArgs da{ cmd_list, cs_hash, group_count_x, group_count_y, group_count_z };

    if (g_sk_toggles.publish_shader_state) PortApi::SetBoundShader('c', AliasShaderHash(cs_hash));

    DispatchDiagShapekey(da);

    SkU0Ident ck;
    ck.shapekey = (g_dispatch_toggles.morph_cs_hash &&
        (cs_hash == g_dispatch_toggles.morph_cs_hash || cs_hash == 0xc1a7f1fec51b482fULL));

    {
        PROF(PROF_Disp_Resolve);
        SkResolveU0(cmd_list, cs_hash, group_count_x, group_count_y, group_count_z, ck);
    }

    const CmdState& state = g_command_lists.Gfx(cmd_list);
    g_rt_pal_recorder.Record(cmd_list, state,
        cs_hash); // RTPAL: a character's skin-cache sections' bone palettes (rt/rt_toggles.h palette)

    DispatchDiagMorph(da);
    DispatchVerdict verdict;
    {
        PROF(PROF_Disp_Ovr);
        verdict = RunDispatchOverrides(da, ck);
    }
    const bool skip_dispatch = verdict.skip;
    const unsigned md_sections_fired = verdict.sections_fired;

    DispatchCapture(da, ck, skip_dispatch, md_sections_fired);

    DispatchDiagSkinProbe(da, state);

    {
        PROF(PROF_Disp_Learn);
        LearnSkinCache(da, state);
    }

    DispatchDiagSkinBlueprint(da, state);

    DispatchDiagStalker(da, state);
    return skip_dispatch;
}
