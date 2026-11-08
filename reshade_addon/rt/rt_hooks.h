#pragma once
// The RT module's entry points that are not a class: its overlay section, and whether the module is in this build.
// The rest: rt_rings.h, rt_pal_record.h, rt_diag_dispatch.h, rt_blas.h, rt_draw.h -- each stubbed by rt_stub.cpp
// when WWMI_RT=0.

// ---- overlay (rt/rt_overlay.cpp) ----
void RtDrawOverlay();

// Is the RT module in this build? false = rt_stub.cpp: mods cannot be drawn while the game ray traces.
bool RtModulePresent();
