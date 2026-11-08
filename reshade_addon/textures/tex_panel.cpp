// tex_panel.cpp -- the overlay's TEXTURE OVERRIDE section (C1..C13).
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "tex_toggles.h"
#include "tex_place.h"
#include "tex_rules.h"
#include "tex_replace.h" // the replace test (C8-C10)
#include "tex_encode.h" // the encode test (C11)
#include "../addon/settings.h"
#include "../draw/draw_toggles.h"
#include "../rabbitfx/rfx_toggles.h"

void DrawTexturePanel()
{
    ImGui::Separator();
    ImGui::TextColored({ 1, 0.6f, 1, 1 }, "*** TEXTURE OVERRIDE ***");
    const bool swaps_were = g_tex_toggles.placement_swap && !g_tex_toggles.disable;
    NCheckbox("C1",
        "Disable texture override: game textures on every draw, mod meshes included (no texture swaps, no mod textures, no RabbitFX maps)",
        &g_tex_toggles.disable);
    NCheckbox("C2", "Placement swap, pre-GPU (g_global_tex)", &g_tex_toggles.placement_swap);
    g_tex_place.SwapsSwitched(swaps_were);
    NCheckbox("C3", "Per-draw in-place stamp, live heap (g_tex_inplace_stamp)", &g_tex_toggles.inplace_stamp);
    NCheckbox("C3R", "C3 stamps only the slots the draw's shaders read (g_tex_read_slots)", &g_tex_toggles.read_slots);
    NCheckbox("C4", "RabbitFX SetTextures bind (g_rabbitfx_tex)", &g_rfx_toggles.tex);
    NCheckbox("C4S",
        "  -> only into shaders that read t0, t1 and t2 (RabbitFX's main shaders); outline and prepass shaders keep the game's textures",
        &g_rfx_toggles.tex_main);
    NCheckbox("C4G",
        "  -> only in the main passes (4+ render targets), as RabbitFX on DX11; off: every pass that draws the part, outlines included",
        &g_rfx_toggles.tex_gbuffer);
    NCheckbox("C6", "Section `ps-tN = ResourceX` stamped in place for the injected draw (g_ps_slot_stamp)",
        &g_draw_toggles.ps_slot_stamp);
    NCheckbox("C7", "  -> and `ps-tN = null` as a null descriptor (g_ps_slot_stamp_nulls)",
        &g_draw_toggles.ps_slot_stamp_nulls);
    {
        uint32_t sc = g_draw_stats.ps_slot_stamp_count.load();
        ImGui::TextColored(sc > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  ps-tN stamps: %u", sc);
    }
    if (NButton("C5", "Texture target audit -> log (TEXAUDIT: per mod, targets never seen / never stamped)"))
        g_tex_rules.TargetAudit();
    if (NButton("C8",
            "Replace test: load every texture of the mods on screen again and swap it in while they draw (TEXREPL) -- nothing on screen should change"))
        g_tex_replace.Request(TexReplace::Pass::Same);
    if (NButton("C9",
            "Replace test: swap every texture of the mods on screen that has mips for a half-size copy; pressed again, back to full size (TEXREPL) -- slightly soft, then sharp"))
        g_tex_replace.Request(g_tex_replace.Halved() ? TexReplace::Pass::Full : TexReplace::Pass::Half);
    if (g_tex_replace.Halved()) ImGui::TextDisabled("  some textures are at half size: C9 brings them back");
    {
        bool stress = g_tex_replace.Stress();
        if (NCheckbox("C10", "Replace test, repeated: C8 every 2 seconds", &stress)) g_tex_replace.SetStress(stress);
    }
    if (g_tex_replace.Busy()) ImGui::TextColored({ 1, 1, 0, 1 }, "  replacing textures...");
    if (NButton("C11",
            "Encode test: make a half-size BC7 copy of every single-mip BC7 texture of the mods on screen on the graphics card, and log how long it took and how far it is from a plain half-size average (TEXENC) -- nothing on screen changes"))
        g_tex_encode_test.Request();
    if (g_tex_encode_test.Busy()) ImGui::TextColored({ 1, 1, 0, 1 }, "  encoding textures...");
    {
        int watch = (int)g_settings.tex_watch_ms;
        if (NSliderInt("C12",
                "Auto: how long a lowered texture is drawn, with nothing loading, before it comes back sharp (ms)",
                &watch, 500, 5000)) {
            g_settings.tex_watch_ms = (unsigned)watch;
            g_settings.Save(L"tex_watch_ms", g_settings.tex_watch_ms);
        }
        int cap = (int)g_settings.tex_vram_cap_mb;
        if (NSliderInt("C13",
                "Auto: take the video memory budget as at most this (MB; 0 = what Windows gives), to try a smaller card",
                &cap, 0, 16384)) {
            g_settings.tex_vram_cap_mb = (unsigned)cap;
            g_settings.Save(L"tex_vram_cap_mb", g_settings.tex_vram_cap_mb);
        }
    }
    {
        uint32_t rc = g_rfx_stats.tex_count.load();
        ImGui::TextColored(rc > 0 ? ImVec4(0, 1, 0, 1) : ImVec4(0.6f, 0.6f, 0.6f, 1), "  slot binds: %u", rc);
    }
}
