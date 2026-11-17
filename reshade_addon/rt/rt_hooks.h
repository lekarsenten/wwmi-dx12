#pragma once
#include <cstddef>
#include <cstdint>
struct ID3D12Resource;
// The RT module's entry points that are not a class: its overlay section, and whether the module is in this build.
// The rest: rt_rings.h, rt_pal_record.h, rt_diag_dispatch.h, rt_blas.h, rt_draw.h -- each stubbed by rt_stub.cpp
// when WWMI_RT=0.

// ---- overlay (rt/rt_overlay.cpp) ----
void RtDrawOverlay();

// Is the RT module in this build? false = rt_stub.cpp: mods cannot be drawn while the game ray traces.
bool RtModulePresent();

// A compute shader is being created (pipeline_registry.cpp). The portable module keeps a copy of the game's
// skin-cache shaders for offline study (debug only); the stub ignores it.
void RtNoteComputeShader(uint64_t cs_hash, const void* code, size_t code_size);
// A graphics pipeline was created; debug: saves the vertex shader of the skin cache's pass-through draws.
void RtNoteGraphicsPipeline(uint64_t pso, uint64_t vs_hash, const void* vs_code, size_t vs_size);
// One range of indices a mod's component section drew.
struct RtDrawnRange {
    uint32_t count;
    uint32_t first;
};
// A mod's component section ran for one of the game's draws. The module learns from it which of the game's own
// geometry the mod hides, to cut the same out of the game's ray-tracing geometry; the stub ignores it.
void RtNoteComponentDraw(int mod, ID3D12Resource* game_ib, uint64_t game_ib_va, uint32_t game_ib_bytes,
    uint32_t game_first_index, uint32_t game_index_count, bool skip_original, const RtDrawnRange* drawn,
    uint32_t drawn_count);
// The device is being destroyed: the module releases everything it created on it and forgets what it learned
// about the device's buffers and pipelines; the stub ignores it.
void RtOnDestroyDevice();
// The game dispatches rays; debug: logs the hit-group records of a mod character's instance.
void RtNoteDispatchRays(ID3D12Resource* hit_groups, uint64_t offset, uint64_t size, uint64_t stride, uint32_t width,
    uint32_t height);
