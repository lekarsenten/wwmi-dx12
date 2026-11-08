#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <imgui.h>
#include <reshade.hpp>

// The add-on's ImGui control panel: the frame, the MASTER section and the module panels (panel_registry.h), in
// that order. It only reads and writes the shared flag globals. (Overlay is their engine's class.)
class OverlayPanel {
public:
    // The panel's contents.
    void Draw(reshade::api::effect_runtime* runtime);
    // The panel's window, while ReShade's overlay is open.
    void DrawPanel(reshade::api::effect_runtime* runtime);
    void SetOpen(bool open) { open_.store(open, std::memory_order_relaxed); }
    // Once per install: the panel docked beside ReShade's tabs, in ReShade's saved layout.
    void DockOnce();
    // Why the add-on is off: the RT block or a fault.
    static const char* StoppedText();

private:
    // The MASTER section; false = the add-on is disabled (A1) and nothing below it is drawn.
    bool DrawMaster();

    std::atomic<bool> open_{ false };
    bool clear_requested_ = false; // A11 pressed this run
    uint32_t live_frame_ = 0; // the liveness line: the frame id last seen, and how many draws it has not moved
    int live_stalls_ = 0;
};
extern OverlayPanel g_overlay;

// ReShade's callbacks: registered in wwmi_addon.cpp; the OSD notice while g_addon.rt_blocked or g_addon.faulted is
// fresh in present_hook.cpp.
void DrawOverlayPanel(reshade::api::effect_runtime* runtime);
bool OnOverlayOpen(reshade::api::effect_runtime* runtime, bool open, reshade::api::input_source source);
void DockOverlayPanelOnce();
void DrawStoppedNotice(reshade::api::effect_runtime* runtime);

// Overlay controls that carry their id: "H20. label". Shared with the module panels and the RT module's section.
bool NCheckbox(const char* id, const char* label, bool* v);
bool NButton(const char* id, const char* label);
bool NSliderInt(const char* id, const char* label, int* v, int lo, int hi, const char* fmt = "%d");
bool NSliderFloat(const char* id, const char* label, float* v, float lo, float hi, const char* fmt = "%.3f");
bool NInputInt(const char* id, const char* label, int* v);
bool NInputText(const char* id, const char* label, char* v, size_t n, ImGuiInputTextFlags f = 0);
