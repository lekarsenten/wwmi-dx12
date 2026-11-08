// render_fixes_panel.cpp -- the overlay's RENDER FIXES section (D2..D16): the LOD proxy and shadow A/Bs, the
// texture lineage and alias toggles, the draw log and shader-diff arms.
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "draw_toggles.h"
#include "../dispatch/dispatch_toggles.h"
#include "../instrumentation/diag_arms.h"
#include "../textures/tex_toggles.h"

void DrawRenderFixesPanel()
{
    ImGui::Separator();
    ImGui::TextColored({ 0.9f, 0.6f, 1, 1 }, "*** RENDER FIXES ***");
    NCheckbox("D2", "IBHASH-suppress LOD proxies (g_ibhash_suppress)", &g_draw_toggles.ibhash_suppress);
    {
        uint32_t ic = g_draw_stats.ibhash_suppress_count.load();
        ImGui::TextColored(ic > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  suppressed: %u", ic);
    }
    NCheckbox("D3", "Vanilla shadow: release depth-only passes (g_shadow_vanilla)", &g_draw_toggles.shadow_vanilla);
    NCheckbox("D4", "  Shadow via claim -- needs D3 OFF (g_shadow_via_claim)", &g_draw_toggles.shadow_via_claim);
    if (NButton("D5", "  Watch shadow recognition (240 frames -> SHADOWRECOG)"))
        g_diag_arms.shadow_recog_watch.store(240);
    {
        int rw = g_diag_arms.shadow_recog_watch.load();
        if (rw > 0) ImGui::TextColored({ 0, 1, 1, 1 }, "    armed: %d frames left", rw);
    }
    ImGui::SetNextItemWidth(160);
    NSliderInt("D6", "Suppress comp5 rtv== (-1 off)", &g_draw_toggles.dbg_comp5_suppress_rtv, -1, 8);
    NCheckbox("D7", "  Skip modded comp translucent (2-RT) pass (g_skip_translucent_cloth)",
        &g_draw_toggles.skip_translucent_cloth);
    NCheckbox("D8", "ShaderOverride hash dispatch (g_shader_override_dispatch)",
        &g_dispatch_toggles.shader_override_dispatch);
    NCheckbox("D9", "Resource-lineage tex swap: mod textures at all LODs (g_tex_lineage)", &g_tex_toggles.lineage);
    {
        uint32_t lc = g_tex_stats.lineage_count.load();
        if (lc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "  lineage propagations this frame: %u", lc);
        else
            ImGui::TextDisabled("  lineage propagations this frame: 0");
    }
    NCheckbox(
        "D12", "A/B: IGNORE learned LOD aliases -- still recorded (g_texlod_alias_off)", &g_tex_toggles.lod_alias_off);
    NCheckbox("D13", "Mip-exact aliases: a target's mips hashed as the residents they would be (g_mip_exact)",
        &g_tex_toggles.mip_exact);
    NCheckbox("D15",
        "Dump degenerate LOD textures as they load or are found: <hash>.dds in DX12_Dumps\\degenerates, once each",
        &g_tex_toggles.dump_degenerates);
    NCheckbox("D16",
        "Hash every texture upload from a GPU copy, as with Resizable BAR, and check it against the processor's read (TEXSTREAM same/differ)",
        &g_tex_toggles.upload_gpu);
    {
        uint32_t tc = g_tex_stats.inject_count.load();
        if (tc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "Texture swaps this frame: %u", tc);
        else
            ImGui::TextDisabled("Texture swaps this frame: 0");
        uint32_t gc = g_tex_stats.global_count.load();
        if (gc > 0)
            ImGui::TextColored({ 0, 1, 1, 1 }, "Global descriptor swaps this frame: %u", gc);
        else
            ImGui::TextDisabled("Global descriptor swaps this frame: 0");
    }
    if (NButton("D10",
            "Log all draws (2 frames) + TEXFLIP (8 frames: a claimed register changing identity frame to frame)")) {
        g_diag_arms.log_all_draws.store(2);
        g_diag_arms.texflip_arm.store(8);
        ++g_diag_arms.texflip_gen;
    }
    if (g_diag_arms.log_all_draws.load() > 0)
        ImGui::TextColored({ 1, 1, 0, 1 }, "Armed: draw log (%d frames)", g_diag_arms.log_all_draws.load());
    if (NButton("D14",
            "Compare shaders: press in the normal state, then again in the changed one (e.g. a transformation); the log lists the shaders only the second state drew"))
        g_diag_arms.shaderdiff_arm.store(60);
    if (g_diag_arms.shaderdiff_arm.load() > 0)
        ImGui::TextColored({ 1, 1, 0, 1 }, "Armed: shader capture (%d frames)", g_diag_arms.shaderdiff_arm.load());
    if (g_diag_arms.texflip_arm.load() > 0)
        ImGui::TextColored({ 1, 1, 0, 1 }, "Armed: TEXFLIP (%d frames)", g_diag_arms.texflip_arm.load());
    {
        bool lod = g_diag_arms.log_lod.load();
        if (NCheckbox("D11", "Capture LOD/char draws -> CDRAW (walk close->far, then untick)", &lod))
            g_diag_arms.log_lod.store(lod);
        if (lod) ImGui::TextColored({ 1, 1, 0, 1 }, "  capturing");
    }
}
