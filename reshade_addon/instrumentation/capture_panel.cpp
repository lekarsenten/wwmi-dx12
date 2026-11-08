// capture_panel.cpp -- the overlay's CAPTURE section (G2..G12): frame dumps, SKCAP, the shapekey and present A/Bs,
// the VB and occlusion probes.
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "diag_gate.h"
#include "diag_arms.h"
#include "../draw/draw_toggles.h"
#include "../mods/mods_toggles.h"
#include "../present/present_toggles.h"
#include "../shapekeys/sk_toggles.h"
#include "../migoto/frame_analysis_driver.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/dx12_skcap.h"

void DrawCapturePanel()
{
    ImGui::Separator();
    ImGui::TextColored({ 0.6f, 1, 0.6f, 1 }, "*** CAPTURE ***");
    // WHAT a G2 / G3 dump writes comes from d3dx.ini [Hunting] analyse_options, not from here; G13 writes log.txt alone.
    if (NButton("G2", "CHAR DUMP: one frame, char draws + all of ours (wwmi-tools shape)"))
        MigotoPort::FrameAnalysisArm(1, /*char_only*/ true, /*log_only*/ false);
    if (NButton("G3", "FULL DUMP: one frame, every draw (~1600; not importable)"))
        MigotoPort::FrameAnalysisArm(1, /*char_only*/ false, /*log_only*/ false);
    if (NButton("G13", "CHAR DUMP, log only: one frame, char draws + all of ours, log.txt alone (shaders, draws)"))
        MigotoPort::FrameAnalysisArm(1, /*char_only*/ true, /*log_only*/ true);
    if (NButton("G4", "SKCAP BYTES: 8 dispatches in full (J1 covers shapekeys)")) {
        MigotoPort::SkCapArm(8);
        // BRACES ARE LOAD-BEARING: unbraced, this ran every frame and re-armed every DiagOnce site.
        g_diag_epoch.fetch_add(1, std::memory_order_relaxed);
    }
    ImGui::TextDisabled("  what is IN a buffer; ~146 MB per chain, cannot span a frame boundary");
    if (NButton("G5", "SKCAP FRAMES: 10 frames (J1 covers shapekeys)")) MigotoPort::SkCapArmFrames(10);
    ImGui::TextDisabled("  WHICH frames the chain ran on; slots >1 MB reduced to stats");
    NCheckbox(
        "G6", "Shapekey: drop passes whose dispatch named no mod (g_sk_strict_owner)", &g_sk_toggles.strict_owner);
    ImGui::TextDisabled("  off = guess the owner by hash/name. Fully un-morphed with this ON means\n"
                        "  provenance is missing at the queuing site.");
    NCheckbox("G7", "Publish the game's vertex buffers at the draw (g_push_game_vb)", &g_draw_toggles.push_game_vb);
    ImGui::TextDisabled("  off = the ini's vb0 restore becomes a real NULL unbind; watch VBNULL-UNBIND");
    NCheckbox("G8", "[Present] on the game's draw queue, not the present queue (g_present_on_game_queue)",
        &g_present_toggles.on_game_queue);
    ImGui::TextDisabled("  queue-orders the mod's [Present] copies against the frame's draws, as on DX11");
    NCheckbox("G9", "[Present]: flush ReShade's immediate list first (g_present_flush_immediate_first)",
        &g_present_toggles.flush_immediate_first);
    ImGui::TextDisabled("  off = RenoDX's back-buffer copy erases the mod panels");
    NCheckbox("G10", "Skip the GPU IB/VB hash dispatch (g_ibhash_gpu_skip)", &g_mods_toggles.ibhash_gpu_skip);
    ImGui::TextDisabled("  A/B for the skill freeze: no freeze with this ON = the hash dispatch is the hang.\n"
                        "  Mods recognised only by their ini's hash= stay vanilla. Log: IBHASH-SLOW / -RATE / -SKIP.");
    if (NButton("G11", "VB DIAG (3 frames, log only): which vertex view each injected draw reads"))
        g_diag_arms.vbdiag_frames.store(3);
    ImGui::TextDisabled("  shim=0 on vb0 = ResourceShapeKeyedPosition did not bind for that pass");
    if (NButton("G12", "Occlusion probe: did the injected mesh rasterise? (30 frames -> OCCL)"))
        g_diag_arms.occl_arm.store(30);
    ImGui::TextDisabled("  NO FRAGMENTS = never rasterised (culled/degenerate/state). RASTERISED = it is\n"
                        "  material or texture, not geometry. Render the character first.");
}
