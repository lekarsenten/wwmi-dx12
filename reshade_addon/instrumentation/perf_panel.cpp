// perf_panel.cpp -- the overlay's performance monitor (A17, A18 and the details table).
#include <imgui.h>
#include "../ui/overlay.h"
#include "../ui/panel_registry.h"
#include "perf_monitor.h"
#include "profiler.h"
#include <cstdint>

static ImVec4 MemoryColour(uint64_t used, uint64_t budget)
{
    const double pct = budget ? 100.0 * (double)used / (double)budget : 0.0;
    if (pct >= 100.0) return { 1, 0.4f, 0.3f, 1 };
    if (pct >= 90.0) return { 1, 0.8f, 0.3f, 1 };
    return ImGui::GetStyle().Colors[ImGuiCol_Text];
}

static void DrawPerformanceDetails(const PerfFigures& s)
{
    ImGui::PushTextWrapPos(-1.0f);
    if (ImGui::BeginTable(
            "perfareas", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Processor work");
        ImGui::TableSetupColumn("ms a frame");
        ImGui::TableSetupColumn("share");
        ImGui::TableHeadersRow();
        for (int a = 0; a < PA_COUNT; ++a) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kPerfAreaNames[a]);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", s.area_ms[a]);
            ImGui::TableNextColumn();
            ImGui::Text("%.0f%%", s.cpu_all_ms > 0.0 ? 100.0 * s.area_ms[a] / s.cpu_all_ms : 0.0);
        }
        ImGui::EndTable();
    }
    if (ImGui::BeginTable(
            "perfgpu", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("Graphics card work");
        ImGui::TableSetupColumn("ms a frame");
        ImGui::TableHeadersRow();
        for (int a = 0; a < kPerfGpuAreaCount; ++a) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kPerfGpuAreas[a].name);
            ImGui::TableNextColumn();
            ImGui::Text("%.3f", s.gpu_area_ms[a]);
        }
        ImGui::EndTable();
    }
    ImGui::PopTextWrapPos();
    ImGui::Text("Busiest thread %u: %.2f ms a frame; %.2f ms over the %d threads the add-on ran on", s.cpu_busiest_tid,
        s.cpu_busiest_ms, s.cpu_all_ms, s.cpu_threads);
    ImGui::TextDisabled(
        "Measuring costs %.2f ms a frame over all threads, left out of the figures above.", s.timers_ms);
    if (ImGui::TreeNode("Every measured step")) {
        ImGui::PushTextWrapPos(-1.0f);
        if (ImGui::BeginTable(
                "perfsteps", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
            ImGui::TableSetupColumn("step");
            ImGui::TableSetupColumn("with inner ms");
            ImGui::TableSetupColumn("own ms");
            ImGui::TableSetupColumn("slowest frame ms");
            ImGui::TableSetupColumn("calls a frame");
            ImGui::TableHeadersRow();
            for (int i = 0; i < PROF_COUNT; ++i) {
                const PerfStep& p = s.steps[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kProfSteps[i].name);
                ImGui::TableNextColumn();
                if (p.incl_ms >= 0.05)
                    ImGui::Text("%.3f", p.incl_ms);
                else
                    ImGui::TextDisabled("%.3f", p.incl_ms);
                ImGui::TableNextColumn();
                if (p.self_ms >= 0.05)
                    ImGui::Text("%.3f", p.self_ms);
                else
                    ImGui::TextDisabled("%.3f", p.self_ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.3f", p.max_ms);
                ImGui::TableNextColumn();
                ImGui::Text("%.0f", p.calls);
            }
            ImGui::EndTable();
        }
        ImGui::PopTextWrapPos();
        ImGui::TreePop();
    }
}

static void DrawPerformanceSummary()
{
    if (!g_perf.HaveShown()) {
        ImGui::TextDisabled("Measuring...");
    } else {
        const PerfFigures& s = g_perf.Shown();
        ImGui::Text("Frame rate: %.0f FPS (slowest frame %.0f FPS)", s.fps, s.slowest_fps);
        ImGui::Text("Add-on on the processor: %.2f ms a frame -- without it, up to %.0f FPS more", s.cpu_busiest_ms,
            PerfFpsGain(s.frame_ms, s.cpu_busiest_ms));
        if (s.gpu_read)
            ImGui::Text("Add-on on the graphics card: %.2f ms a frame -- without it, up to %.0f FPS more", s.gpu_ms,
                PerfFpsGain(s.frame_ms, s.gpu_ms));
        else
            ImGui::TextDisabled("Add-on on the graphics card: no reading yet");
    }
    if (g_perf.HaveMemory()) {
        constexpr double GB = 1024.0 * 1024.0 * 1024.0;
        const PerfMemory& m = g_perf.MemoryNow();
        const PerfMemory& pk = g_perf.MemoryPeak();
        ImGui::TextColored(MemoryColour(m.vram_used, m.vram_budget),
            "Video memory: %.1f of %.1f GB (%.0f%%), highest %.1f GB", m.vram_used / GB, m.vram_budget / GB,
            m.vram_budget ? 100.0 * (double)m.vram_used / (double)m.vram_budget : 0.0, pk.vram_used / GB);
        ImGui::Text("Shared memory (system memory the graphics card uses): %.1f GB", m.shared_used / GB);
        ImGui::Text("Game memory: %.1f GB", m.game_private / GB);
    }
}

void DrawPerfPanel()
{
    ImGui::Separator();
    {
        bool perf = g_prof_enabled;
        if (NCheckbox(
                "A17", "Performance monitor: what the add-on costs, live (measuring costs a little itself)", &perf))
            g_prof_enabled = perf;
    }
    if (g_prof_enabled) {
        DrawPerformanceSummary();
        if (g_perf.ReportRunning())
            ImGui::TextColored({ 1, 1, 0, 1 }, "Recording the report: %.0f s left", g_perf.ReportSecondsLeft());
        else if (NButton("A18", "Save a 10-second performance report next to wwmi_dx12_addon.log"))
            g_perf.RequestReport();
        if (!g_perf.LastReport().empty()) ImGui::TextDisabled("Saved: %s", g_perf.LastReport().c_str());
        if (g_perf.HaveShown() && ImGui::CollapsingHeader("Performance details"))
            DrawPerformanceDetails(g_perf.Shown());
    }
}
