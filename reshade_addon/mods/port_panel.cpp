// port_panel.cpp -- the overlay's 3DMIGOTO PORT section (H1..H18, the RT module's H19..H27, H17).
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "../addon/addon.h"
#include "../draw/draw_toggles.h"
#include "../hooks/hooks_toggles.h"
#include "../migoto/port_api.h"
#include "../instrumentation/diag_arms.h"
#include "../rabbitfx/rfx_toggles.h"
#include "../rt/rt_hooks.h" // RtDrawOverlay

void DrawPortPanel()
{
    ImGui::Separator();
    ImGui::TextColored({ 0.6f, 0.9f, 1, 1 }, "*** 3DMIGOTO PORT ***");
    NCheckbox("H1", "Give their engine the draw's command list (g_port_draw_list)", &g_draw_toggles.port_draw_list);
    ImGui::TextDisabled("  off = the mod's GPU work is dropped and it stays vanilla; watch portDisp in HEARTBEAT");
    NCheckbox("H2", "Crash reporter -> wwmi_dx12_crash.log (next launch) (g_crash_handler)", &g_addon.crash_handler);
    ImGui::TextDisabled("  resolve +0x offsets against wwmi_dx12.map");
    NCheckbox("H16",
        "Stall watchdog: no present for 1 s while focused -> STALL + the present thread's stack (g_stall_watchdog)",
        &g_diag_arms.stall_watchdog);
    ImGui::TextDisabled("  stalls so far: %u", g_diag_arms.stall_count.load());
    if (NButton("H3", "Trace the chain for 10 frames -> wwmi_dx12_chain_trace.log"))
        g_diag_arms.chaintrace_req.store(true);
    ImGui::TextDisabled("  every executed line with its section, plus all engine variables per frame");
    if (NButton("H4", "Dump the whole skeleton chain (inputs, merger output, remaps, vars)"))
        g_diag_arms.skelchain_req.store(true);
    ImGui::TextDisabled("  char on screen; results ~4 frames later");
    if (NButton("H5", "Dump merged skeleton (what SkeletonMerger wrote)")) g_diag_arms.skeldump_req.store(true);
    ImGui::TextDisabled("  all-zero rows = merger ran with wrong inputs; plausible rows = look downstream");
    NCheckbox("H6", "Frame dump includes the GAME's textures -- ~1.3 s/draw (g_dump_game_textures)",
        &g_diag_arms.dump_game_textures);
    if (NButton("H7", "Log each mod's component layout (self-check -> log)")) g_diag_arms.wwmi_parse.store(true);
    NCheckbox("H8", "LOD pin: identify char lower-LOD draws, log only (g_lod_pin)", &g_draw_toggles.lod_pin);
    if (NCheckbox("H9", "Original-ini geometry for DIRECT comps (g_wwmi_geom)", &g_draw_toggles.wwmi_geom))
        PortApi::RequestReload();
    {
        uint32_t gc = g_draw_stats.wwmi_geom_count.load();
        if (gc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "  ini-geometry draws this frame: %u", gc);
        else
            ImGui::TextDisabled("  ini-geometry draws this frame: 0");
    }

    NCheckbox("H10", "Custom-material sub-draws: run = [CustomShader*] parts (g_custom_subdraw)",
        &g_draw_toggles.custom_subdraw);
    NCheckbox("H11", "  -> derived blend PSO for transparency (g_custom_blend)", &g_draw_toggles.custom_blend);
    {
        uint32_t cc = g_draw_stats.custom_blend_count.load();
        if (cc > 0)
            ImGui::TextColored({ 0, 1, 1, 1 }, "  sub-draws this frame: %u", cc);
        else
            ImGui::TextDisabled("  sub-draws this frame: 0");
    }
    {
        uint32_t bc = g_draw_stats.custom_blend_bound_count.load();
        if (bc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "  blend PSO bound: %u", bc);
        else if (g_draw_toggles.custom_blend)
            ImGui::TextColored({ 1, 0.5, 0, 1 }, "  blend PSO bound: 0 -- see CUSTOMBLEND-MISS in log");
        else
            ImGui::TextDisabled("  blend PSO bound: 0");
    }
    {
        static bool s_override = false;
        static float s_f = 0.5f;
        if (NCheckbox("H12", "  override blend factor (lower = more transparent)", &s_override))
            g_draw_toggles.custom_blend_factor = s_override ? s_f : -1.0f;
        if (s_override) {
            if (NSliderFloat("H13", "  blend factor (0.9 = ini default)", &s_f, 0.05f, 1.0f, "%.2f"))
                g_draw_toggles.custom_blend_factor = s_f;
        }
    }
    NCheckbox(
        "H14", "  debug: show background behind piece (g_custom_blend_debug_zero)", &g_rfx_toggles.blend_debug_zero);
    NCheckbox("H15",
        "Command-list Reset clears our per-list binding record, as D3D12 does the list's (g_cmd_reset_clears)",
        &g_hooks_toggles.cmd_reset_clears);
    ImGui::TextDisabled("  resets so far: %u", g_hooks_stats.cmd_resets.load());
    NCheckbox("H18", "Claim by the INDEX BUFFER when a mod's whole component set lands on one (g_ib_anchor_claim)",
        &g_draw_toggles.ib_anchor_claim);
    ImGui::TextDisabled("  needed with ray tracing on, where the game animates vb0 and every content hash dies");
    ImGui::TextDisabled("  index buffers anchored: %u   draws claimed by one: %u",
        g_draw_stats.ib_anchor_learned.load(), g_draw_stats.ib_anchor_claims.load());
    RtDrawOverlay(); // H19..H27 -- the RT module's section (rt/rt_overlay.cpp)
    NCheckbox("H17", "Refuse a char_vb claim whose VB0 was learned from ANOTHER mod (g_charvb_refuse_xmod)",
        &g_draw_toggles.charvb_refuse_xmod);
    ImGui::TextDisabled("  char_vb claims: %u  cross-mod: %u  refused: %u", g_draw_stats.charvb_claims.load(),
        g_draw_stats.charvb_xmod.load(), g_draw_stats.charvb_refused.load());
}
