#pragma once
// The overlay's module panels, drawn after the MASTER section in this order, so the control ids and labels keep
// their sequence. tools/gen_overlay_doc.py reads the panel files from this table.
struct PanelEntry {
    const char* file;
    void (*draw)();
};

void DrawPerfPanel();
#ifdef WWMI_DEBUG
void DrawInjectionPanel();
void DrawTexturePanel();
void DrawRabbitFxPanel();
void DrawRenderFixesPanel();
void DrawCapturePanel();
void DrawPortPanel();
void DrawShapekeyPanel();
void DrawMorphDumpPanel();
#endif

inline constexpr PanelEntry kOverlayPanels[] = {
    { "instrumentation/perf_panel.cpp", DrawPerfPanel },
#ifdef WWMI_DEBUG
    { "draw/draw_panel.cpp", DrawInjectionPanel },
    { "textures/tex_panel.cpp", DrawTexturePanel },
    { "rabbitfx/rfx_panel.cpp", DrawRabbitFxPanel },
    { "draw/render_fixes_panel.cpp", DrawRenderFixesPanel },
    { "instrumentation/capture_panel.cpp", DrawCapturePanel },
    { "mods/port_panel.cpp", DrawPortPanel },
    { "shapekeys/sk_panel.cpp", DrawShapekeyPanel },
    { "instrumentation/morph_dump_panel.cpp", DrawMorphDumpPanel },
#endif
};
