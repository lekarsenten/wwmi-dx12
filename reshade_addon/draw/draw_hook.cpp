// The draw path: every indexed draw the game issues, and the mod geometry we put in its place.
// See draw_hook.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../mods/character_recognition.h"
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "../mods/layout_registry.h"
#include "draw_context.h"
#include "draw_parts.h"
#include "draw_sectex.h"
#include "../instrumentation/hot_path.h"
#include <optional>
#include "draw_toggles.h"
#include "../addon/addon.h"
#include "../instrumentation/shader_capture.h"
#include "../present/frame_sync.h"
#include "../migoto/frame_analysis_driver.h"
#include "../migoto/port_api.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/mod_set.h"

DrawToggles g_draw_toggles;
DrawStats g_draw_stats;

thread_local uint8_t t_x23_claimed = 0;
thread_local uint8_t t_x23_shadow = 0;
thread_local uint8_t t_x23_cast = 0;
thread_local uint8_t t_x23_hidden = 0;
thread_local uint32_t t_x23_injected = 0;
thread_local uint8_t t_x23_merges = 0;

bool on_draw_indexed_body(command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index,
    int32_t vertex_offset, uint32_t first_instance)
{
    PROF(PROF_DrawIndexed);
    if (g_addon.disabled || g_addon.faulted || !g_addon.is_dx12) return false;
    MigotoPort::FrameAnalysisDrawScope fa_scope;
    uint64_t vs_hash = 0;
    cmd_list->get_private_data(s_iid_vs_hash, &vs_hash);
    if (!vs_hash) return false;
    if (g_draw_toggles.hide_vs_hash && vs_hash == g_draw_toggles.hide_vs_hash) {
        t_x23_hidden = 1;
        return true;
    }

    auto* d3d12_cl = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    if (!d3d12_cl) return false;
    const CmdRT draw_rt = g_command_lists.Rt(cmd_list); // stable for this draw: only the game's bind hook writes it
    DrawContext ctx{ DrawArgs{ cmd_list, d3d12_cl, index_count, instance_count, first_index, vertex_offset,
                         first_instance, vs_hash, draw_rt },
        BoneRoots{ cmd_list } };
    const DrawArgs& args = ctx.args;

    if (g_debug) g_shader_capture.DiffNote(cmd_list, vs_hash, index_count);
    g_section_textures.NoteMeshPlaces(args);
    g_section_textures.CarryPsReads(args);
    g_tex_draw_watch.Note(args);

    if (index_count >= 256 && logger::enabled(3)) { // feeds only the SEEDHB trace line
        uint64_t hbvb0 = 0;
        {
            hbvb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
        }
        if (hbvb0) g_recognition.NoteHeartbeatDraw(hbvb0);
    }

    if (Comps().Loaded()) {
        const int hit = Comps().SlotOf(first_index, index_count);
        if (hit >= 0 && Comps().At(hit).hide) {
            t_x23_hidden = 1;
            return true;
        }
    }

    struct PostAfterDraw {
        ~PostAfterDraw() { PortApi::RunPendingPost(); }
    } post_after_draw;
    if (RunDrawShaderOverrides(args)) return true;
    if (RunSmallMeshOverrides(args)) return true;

    if (const std::optional<bool> r = ClaimDraw(ctx)) return *r;
    MergedComp*& mc = ctx.claim.mc;

    DrawProbesClaim(ctx);

    PROF(PROF_DI_Inject);
    ProfScope _di_prologue(PROF_DI_Prologue);

    LodPinNote(ctx);

    DrawProbesPasses(ctx);

    RecogniseShadowPass(ctx);
    if (g_draw_toggles.skip_no_bone13 && mc && (g_game_cbs.BoneRootMask(cmd_list) & 0x2000u) == 0) mc = nullptr;

    if (mc) {
        HotPath::NoteClaimed();
        t_x23_claimed = 1; // X23: mc survived every gate above
        g_frames.claim_last_list.store((const void*)cmd_list->get_native(), std::memory_order_relaxed);
        if (g_debug) g_frames.claim_thread.store((uint32_t)GetCurrentThreadId(), std::memory_order_relaxed);
    }

    DrawProbeVb6Reader(ctx);
    DrawProbeOffLeak(ctx);

    if (g_draw_toggles.hide_unclaimed_char && !mc && index_count >= 2000) {
        uint64_t vb0 = 0;
        {
            vb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
        }
        if (g_recognition.InSkinBand(vb0)) return true;
    }

    if (g_debug) DrawDiagIndexed(ctx);

    // Prologue ends HERE: everything above ran for all 1035 draws, everything below is claimed-draw work.
    _di_prologue.End();
    StampClaimedTextures(ctx);

    return InjectClaimedDraw(ctx);
}
