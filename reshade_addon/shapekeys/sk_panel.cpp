// sk_panel.cpp -- the overlay's SHAPE KEYS / MORPHS section (I1..I19).
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "sk_toggles.h"
#include "../dispatch/dispatch_toggles.h"
#include "../draw/draw_toggles.h"
#include "../hooks/hooks_toggles.h"
#include "../present/present_toggles.h"
#include "../migoto/dx12_bridge.h" // X30 counters
#include "../migoto/port_toggles.h" // g_blend_write_all_rt
#include <cstdio>
#include <cstdlib>

void DrawShapekeyPanel()
{
    ImGui::Separator();
    ImGui::TextColored({ 0.6f, 0.9f, 1, 1 }, "*** SHAPE KEYS / MORPHS (eyelids, facial) ***");
    NCheckbox("I1", "Run the mod's shapekey passes (g_sk_run_passes)", &g_draw_toggles.sk_run_passes);
    ImGui::TextDisabled("  OFF is a diagnostic: it explodes some mods' characters by design -- for them\n"
                        "  these passes are the only writer of vb0.");
    NCheckbox("I2", "Forget relocations on resource destroy (g_reloc_forget_on_destroy)",
        &g_hooks_toggles.reloc_forget_on_destroy);
    ImGui::TextDisabled("  the maps are keyed on the POINTER and Windows recycles addresses");
    NCheckbox("I3", "Freeze shapekeys: zero the deltas before the Applier (g_sk_freeze)", &g_sk_toggles.freeze);
    ImGui::TextDisabled("  the morph blocker -- unlike I1 OFF, it does not explode anyone");
    NCheckbox("I5", "Health heartbeat every 600 frames (g_health_heartbeat)", &g_present_toggles.health_heartbeat);
    {
        static char s_hex[19] = "";
        static bool s_init = false;
        if (!s_init) {
            s_init = true;
            if (g_dispatch_toggles.morph_cs_hash)
                snprintf(s_hex, sizeof(s_hex), "%016llx", (unsigned long long)g_dispatch_toggles.morph_cs_hash);
        }
        if (NInputText("I6", "morph cs_hash (hex)", s_hex, sizeof(s_hex), ImGuiInputTextFlags_CharsHexadecimal))
            g_dispatch_toggles.morph_cs_hash = _strtoui64(s_hex, nullptr, 16);
        if (g_dispatch_toggles.morph_cs_hash)
            ImGui::TextColored(
                { 0, 1, 0, 1 }, "morph cs set: %016llx", (unsigned long long)g_dispatch_toggles.morph_cs_hash);
        else
            ImGui::TextDisabled("morph cs unset (capture inactive)");
    }
    NCheckbox("I7", "DX11->DX12 shader-hash alias for [ShaderOverride] (g_shader_hash_alias)",
        &g_sk_toggles.shader_hash_alias);

    NCheckbox("I8", "Publish bound shader into their HackerContext (g_publish_shader_state)",
        &g_sk_toggles.publish_shader_state);
    ImGui::TextDisabled("  without it the bare cs/vs/ps operand reads -0.0 and every bare shader filter is dead");

    NCheckbox(
        "I9", "Publish the GAME's compute UAV at cs-u0 -- needs I8 (g_publish_cs_u0)", &g_sk_toggles.publish_cs_u0);
    ImGui::TextDisabled("  lets `if cs-u0 >= 1` answer. Gated on the desc hash a [TextureOverride] declares,\n"
                        "  so a wrong resolve publishes nothing and logs REFUSED. The mesh WILL change.");

    NCheckbox("I10", "Shapekey: zero u0 before the chain runs (g_sk_u0_clear)", &g_sk_toggles.u0_clear);
    ImGui::TextDisabled("  the Loader accumulates with atomic_iadd, so u0 must start zeroed. OFF = the\n"
                        "  kaleidoscope (integers added into the previous frame's float bit patterns).");
    NCheckbox("I11", "Shapekey: zero u1, per-vertex weight sums (g_sk_u1_clear)", &g_sk_toggles.u1_clear);
    ImGui::TextDisabled("  OFF = u1 accumulates until it wraps negative and the Multiplier's normalize\n"
                        "  snaps: the flinch. Check the drain table's u1_clear column.");
    NCheckbox("I12", "Shapekey: COPY_DEST barriers around the u0/u1 zero copy (g_sk_clear_barrier)",
        &g_sk_toggles.clear_barrier);
    ImGui::TextDisabled("  a UAV barrier does not order a copy write against the Loader's atomics");
    NCheckbox("I13", "Shapekey: resolve u1 by declared ShapeKeyScale identity (g_sk_u1_by_identity)",
        &g_sk_toggles.u1_by_identity);
    ImGui::TextDisabled("  for when the register has no record; refuses when several buffers fit (U1IDENT)");
    NCheckbox("I14", "Shapekey: honour `handling = skip` for the game's dispatch (g_sk_honour_skip)",
        &g_dispatch_toggles.honour_skip);
    NCheckbox("I19",
        "Shapekey: fire the callbacks through the engine's own hash lookup, as the mesh skip does (g_sk_engine_dispatch)",
        &g_dispatch_toggles.engine_dispatch);
    ImGui::TextDisabled("  DX11 BeforeDispatch semantics. Faithfulness, not a known fix. DISPATCH-SKIP in log.");
    NCheckbox("I18", "Shapekey: cancel the game's u0 clear on a buffer the chain writes (g_sk_cancel_game_clear)",
        &g_dispatch_toggles.cancel_game_clear);
    ImGui::TextDisabled(
        "  draw lists submitted before the pass list read last frame's deltas, not zeros (frozen face /\n"
        "  flinch). cancelled so far: %u",
        g_sk_stats.clear_cancelled.load());

    ImGui::TextDisabled("bridge: displaced=%llu  wrong-list draws=%llu  dispatches=%llu",
        (unsigned long long)MigotoPort::BridgeDisplacedCount(), (unsigned long long)MigotoPort::BridgeWrongListDraws(),
        (unsigned long long)MigotoPort::BridgeWrongListDispatches());
    ImGui::TextDisabled("mid-section restores=%llu  IA rebinds skipped=%llu  (must be equal)",
        (unsigned long long)g_draw_stats.x32_mid_total.load(),
        (unsigned long long)g_draw_stats.x32_mid_skipped_total.load());

    NCheckbox("I15", "Shapekey: recover u0's extent when the descriptor record has none (g_sk_u0_size_recover)",
        &g_sk_toggles.u0_size_recover);
    ImGui::TextDisabled("  a size-0 record hashes to nothing any [TextureOverride] declares, so the whole\n"
                        "  declared region is skipped. Proves liveness before dereferencing; never guesses.");
    NCheckbox(
        "I16", "Shapekey: snapshot the game's cb0 at its dispatch (g_sk_cb0_snapshot)", &g_sk_toggles.cb0_snapshot);
    ImGui::TextDisabled("  OFF holds a VA into the game's constant ring, which is reused before the chain\n"
                        "  replays: scale row 64 reads garbage and every delta rounds to zero (frozen face).");
    NCheckbox("I17", "Blend: write all channels on all 8 render targets (g_blend_write_all_rt)",
        &MigotoPort::g_blend_write_all_rt);
    ImGui::TextDisabled("  only affects the full-PSO path, which this corpus takes once. Unproven in game.");
}
