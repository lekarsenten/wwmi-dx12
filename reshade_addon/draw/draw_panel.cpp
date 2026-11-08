// draw_panel.cpp -- the overlay's INJECTION section (B2..B23): the stalker's findings, the draw path's toggles,
// the per-component isolate list, the present log.
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "draw_toggles.h"
#include "../instrumentation/diag_gate.h"
#include "../instrumentation/diag_arms.h"
#include "../instrumentation/uav_stalker.h"
#include "../core/log.h"
#include "../mods/component_table.h"
#include "../mods/mod_registry.h" // ModRegistry::OnScreen: the per-component isolate list shows the mods on screen
#include "../present/present_log.h"
#include "../textures/tex_select.h"
#include "../migoto/port_api.h" // the loaded engine: variables for the PANELGATE dump
#include "../migoto/port_toggles.h" // g_inherit_game_viewport, g_port_post_split, g_lazy_restore
#include "../migoto/dx12_pipeline.h" // ArmPresentDrawProbe (PRESENTDRAW)
#include <cstdio>
#include <cstdlib>
#include <set>
#include <string>
#include "../mods/mod_set.h"

void DrawInjectionPanel()
{
    ImGui::Separator();

    ImGui::Separator();
    ImGui::TextColored({ 1, 0.9f, 0.6f, 1 }, "*** INJECTION ***");

    ImGui::Separator();
    ImGui::TextColored({ 0, 1, 1, 1 }, "UAV Stalker Findings:");
    ImGui::BeginChild("StalkerList", ImVec2(0, 200), true);
    g_uav_stalker.ForEachResult([](const StalkerResult& res) {
        ImGui::BulletText("CS %llx -> Table %u[%u]", res.cs_hash, res.root, res.slot);
    });
    ImGui::EndChild();
    if (NButton("B2", "Clear Discovery List")) g_uav_stalker.Clear();
    if (NButton("B3", "Re-arm distance capture (SKINPROBE/POOLDRAW)")) g_diag_gen.fetch_add(1);
    ImGui::TextDisabled("Travel to the spot (e.g. a character at far LOD), then click to start a fresh capture there.");
    if (NButton("B4", "Watch texture flicker for 600 frames (TEXFLICKER)")) {
        g_tex_select.ArmFlickerWatch(600);
        logger::write("TEXFLICKER: armed for 600 frames");
    }
    if (NButton("B5", "Dump panel gate vars (PANELGATE)")) {
        static const wchar_t* kGates[] = { L"object_detected", L"mod_enabled", L"menu", L"ActiveCharacter", L"mod_id",
            L"show_menu", L"enable_mods" };
        std::set<std::wstring> nss;
        PortApi::ForEachVariable(
            [](const wchar_t* fq, float, bool, void* user) {
                std::wstring s(fq ? fq : L"");
                const size_t bs = s.rfind(L'\\');
                if (bs != std::wstring::npos) ((std::set<std::wstring>*)user)->insert(s.substr(0, bs));
            },
            &nss);
        logger::write("PANELGATE: %zu namespaces", nss.size());
        for (const auto& ns : nss) {
            std::wstring line;
            for (const wchar_t* g : kGates) {
                std::wstring key = ns + L"\\" + g;
                for (auto& c : key) c = (wchar_t)towlower(c);
                wchar_t b[64];
                swprintf_s(b, L" %ls=%g", g, PortApi::GetVar(key.c_str(), -999.f));
                line += b;
            }
            logger::write("PANELGATE ns='%ls'%ls", ns.c_str(), line.c_str());
        }
    }
    if (NButton("B6", "Trace [Present] draws for 3 frames (PRESENTDRAW)")) MigotoPort::ArmPresentDrawProbe(3);

    ImGui::Separator();
    {
        uint32_t rc = g_draw_stats.draw_replace_count.n.load();
        if (rc > 0)
            ImGui::TextColored({ 0, 1, 0, 1 }, "Draw replacements this frame: %u  (last: %016llx  sub_draws=%u)", rc,
                g_draw_stats.last_replace_hash, g_draw_stats.last_replace_ib_count);
        else
            ImGui::TextDisabled("Draw replacements this frame: 0  (last: %016llx  sub_draws=%u)",
                g_draw_stats.last_replace_hash, g_draw_stats.last_replace_ib_count);
    }
    // Merged-skeleton (WWMI-faithful bone CB injection) status
    {
        NCheckbox("B7", "FULL-PORT draw path (execute ini command lists -- off = none run)", &g_draw_toggles.wcl_draw);
        NCheckbox("B8", "Merged-skeleton bone CB injection", &g_draw_toggles.merged_bone_cb);
        NCheckbox("B9", "Restore VB/IB after draw", &g_draw_toggles.restore_vbib);
        NCheckbox("B10", "Restore bone CB after draw", &g_draw_toggles.restore_cb);
        NCheckbox("B11", "Restore viewport/scissor/topology/blend/stencil after draw", &g_draw_toggles.restore_nonpso);
        NCheckbox("B12", "Inherit game viewport on injected draws (off = old swap-chain-sized viewport)",
            &MigotoPort::g_inherit_game_viewport);
        NCheckbox("B13", "Pre/post SPLIT: run post lists after the draw (off = old inline behaviour)",
            &MigotoPort::g_port_post_split);
        NCheckbox("B21",
            "Put the game's state back once after a run of mod compute work, not after every dispatch and copy",
            &MigotoPort::g_lazy_restore);
        if (NButton("B15", "Log merged-blend draws (8 frames)")) g_diag_arms.log_merged.store(8);
        NCheckbox(
            "B16", "Hide UNCLAIMED char draws (shadow/LOD proxy) [clone test]", &g_draw_toggles.hide_unclaimed_char);
        {
            static char vsbuf[24] = "";
            ImGui::SetNextItemWidth(180);
            if (ImGui::InputText(
                    "HIDE draw by VS hash (hex)", vsbuf, sizeof(vsbuf), ImGuiInputTextFlags_CharsHexadecimal))
                g_draw_toggles.hide_vs_hash = _strtoui64(vsbuf, nullptr, 16);
            if (g_draw_toggles.hide_vs_hash) {
                ImGui::SameLine();
                if (ImGui::SmallButton("clear##hidevs")) {
                    g_draw_toggles.hide_vs_hash = 0;
                    vsbuf[0] = 0;
                }
                ImGui::SameLine();
                ImGui::TextColored({ 1, 1, 0, 1 }, "hiding %016llx", (unsigned long long)g_draw_toggles.hide_vs_hash);
            }
        }
        NCheckbox("B17", "Skip passes w/o bones@root13 (draw vanilla) [spiral test]", &g_draw_toggles.skip_no_bone13);
        if (ImGui::CollapsingHeader("Per-component isolate (Hide / Vanilla / Freeze)")) {
            if (!Comps().Loaded() || (Comps().Count() == 0)) {
                ImGui::TextDisabled("  no components loaded");
            } else {
                if (NButton("B18", "Clear all component isolation"))
                    for (auto& e : Comps().All()) e.hide = e.vanilla = e.freeze = false;
                ImGui::TextDisabled("  H = hide draw   V = vanilla (skip mod)   F = freeze (bind pose)");
                size_t shown = 0;
                for (size_t i = 0; i < Comps().Count(); ++i) {
                    MergedComp& mc = Comps().At(i);
                    if (!Mods().OnScreen(mc.mod_id) && !(mc.hide || mc.vanilla || mc.freeze))
                        continue; // an isolated one stays visible
                    ++shown;
                    ImGui::PushID((int)i);
                    ImGui::Checkbox("H", &mc.hide);
                    ImGui::SameLine();
                    ImGui::Checkbox("V", &mc.vanilla);
                    ImGui::SameLine();
                    ImGui::Checkbox("F", &mc.freeze);
                    ImGui::SameLine();
                    ImGui::Text(
                        "mod %d comp %d  (fi=%u  n=%u)", mc.mod_id, mc.comp_idx, mc.first_index, mc.match_count);
                    ImGui::PopID();
                }
                if (!shown) ImGui::TextDisabled("  no mod on screen");
            }
        }
        if (Comps().Loaded())
            ImGui::TextColored({ 0, 1, 1, 1 }, "  comps=%zu", Comps().Count());
        else
            ImGui::TextDisabled("  merged remap not loaded (no geometry mod loaded)");
    }
    NCheckbox("B19", "Disable VB Injection", &g_draw_toggles.disable_vb);
    NCheckbox("B20",
        "One merged skeleton per copy of a character drawn at once (off: the copies share one and can mix)",
        &g_draw_toggles.skel_copies);
    if (NButton("B23",
            "Log the next 240 presents: thread, time since the previous one, the game's draws and submissions in between (PRESENTLOG), and each submission that hands a full-screen target to present (FRAMEEND)")) {
        g_present_log.draws.store(0);
        g_present_log.execs.store(0);
        g_present_log.frame_end_draws.store(0);
        {
            std::lock_guard l(g_present_log.mtx);
            g_present_log.subs.clear();
        }
        g_present_log.left.store(240);
    }
}
