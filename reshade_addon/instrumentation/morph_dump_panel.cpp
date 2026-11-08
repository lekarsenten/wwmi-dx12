// morph_dump_panel.cpp -- the overlay's MORPH DUMP section (J1..J4), the shader bytecode cache (K6) and the
// command pipe (K7).
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "debug_pipe.h"
#include "diag_gate.h"
#include "diag_arms.h"
#include "morph_dump.h"
#include "shader_capture.h"
#include "sk_ring.h"
#include "../addon/addon.h"
#include "../migoto/dx12_skcap.h" // SkCapArmed

void DrawMorphDumpPanel()
{
    ImGui::Separator();
    ImGui::TextColored({ 0.5, 1, 0.5, 1 }, "*** MORPH DUMP ***");
    ImGui::TextDisabled("Every compute dispatch, unfiltered, metadata only -> morph_dump-<time>.tsv beside\n"
                        "the exe. Answers: of the dispatches no mod owns, how many did we skip or fire\n"
                        "sections for? Both must be zero.");
    if (NButton("J1", "ARM SHAPEKEY CAPTURE -- tables + buffer contents + probes")) MorphDump::ArmCapture();
    if (NButton("J4", "...at the next new instance (an ult's copies)")) MorphDump::ArmCaptureAtNewInstance();
    if (NButton("J2", "Close now")) MorphDump::Close();
    if (NButton("J3", "Dump the shapekey decision ring (last ~15 s; Numpad 1 in capture mode)")) SkRing::Dump("J3");
    ImGui::Text("capture mode %s (Numpad 0)", g_addon.capture_mode ? "ON" : "off");
    ImGui::Text("tables %s | contents %s | probes %s",
        MorphDump::Armed()                ? "ARMED"
            : MorphDump::CaptureWaiting() ? "WAITING"
                                          : "idle",
        MigotoPort::SkCapArmed() ? "ARMED" : "idle", ProbeArmed() ? "ARMED" : "idle");
    ImGui::TextDisabled("morph_dump / morph_pass / morph_drain-<stamp>.tsv beside the exe: which dispatch\n"
                        "the chain claimed, what cs-u0 resolved to, why a pass was refused.\n"
                        "DX12_Dumps\\SKCAP-<stamp>\\: per-frame ledger, slots <1 MB in full, digests above.\n"
                        "Read the tables first to confirm the chain RAN, then the .buf files for what it\n"
                        "computed. Clean tables + a wrong character means the answer is in the .buf files.");

    ImGui::Separator();
    ImGui::TextColored({ 0, 1, 1, 1 }, "Shader Bytecode Cache:");
    ImGui::Text("Cached shaders (all vs/ps/cs): %zu", g_shader_capture.Count());
    if (NButton("K6", "Dump captured shaders to disk (shader_<stage>_<hash>.bin)"))
        g_diag_arms.dump_shaders_requested.store(true);

    ImGui::Separator();
    ImGui::TextColored({ 0, 1, 1, 1 }, "Command Pipe:");
    {
        static bool s_pipe = false;
        if (NCheckbox("K7",
                "Take commands from tools\\debug_pipe\\pipe.py: they read and change the game's state while this is on",
                &s_pipe))
            DebugPipe::SetListening(s_pipe);
    }
}
