// rfx_panel.cpp -- the overlay's RABBITFX GLOW section (E2).
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "rfx_toggles.h"
#include "rfx_patch.h" // RfxPatchWorker::Stats (E2)

void DrawRabbitFxPanel()
{
    ImGui::Separator();
    ImGui::TextColored({ 0.9f, 0.6f, 1, 1 }, "*** RABBITFX GLOW ***");
    {
        uint32_t gp = g_rfx_stats.glow_params.load();
        ImGui::TextColored(
            gp > 0 ? ImVec4(0, 1, 1, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  comps armed by the ini: %u", gp);
    }
    NCheckbox("E2",
        "RabbitFX shader patch: draw on the game's PS patched with the ini's t51 cutout and t50 glow / hue (g_rfx_patch)",
        &g_rfx_toggles.patch);
    {
        const RfxPatchStats cs = g_rfx_patch.Stats();
        const uint32_t cd = g_rfx_stats.patch_draws.load();
        const uint32_t cn = g_rfx_stats.patch_nobind.load();
        ImGui::TextColored(cs.ps_failed ? ImVec4(1, 0.5f, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
            "  shaders patched this instance: %u  failed: %u  PSO variants: %u  (see RFXPATCH in the log)",
            cs.ps_patched, cs.ps_failed, cs.variants);
        ImGui::TextColored(cd > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1),
            "  patched draws: %u (with glow: %u)  maps not bindable: %u", cd, g_rfx_stats.glow_count.load(), cn);
    }
}
