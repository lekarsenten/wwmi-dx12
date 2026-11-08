#pragma once
#include "draw_context.h"
#include "../mods/wwmi_mod.h"
#include "../mods/port_section.h" // PortHooks
#include "../instrumentation/morph_dump.h" // MorphDump::DrawRecord -- the I8 row a component run fills

struct RfxGlowState {
    bool active = false;
    float hue = 0;
    float sat = 0;
    float val = 0;
    float brightness = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE map{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE fxmap{ 0 };
};
struct RfxTexLatch {
    D3D12_CPU_DESCRIPTOR_HANDLE d{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE n{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE l{ 0 };
    D3D12_CPU_DESCRIPTOR_HANDLE m{ 0 };
}; // m: Materialmap

// What RabbitFX's calls leave behind for the draws that follow them: one per section run, or one for every
// section on a claimed draw, as DX11's bindings outlive the section that made them.
struct RfxDrawState {
    RfxGlowState glow;
    RfxTexLatch tex;
    std::wstring diffuse_fq;
    std::wstring normal_fq;
    std::wstring light_fq;
    std::wstring material_fq;
    std::wstring cutout_fq;
    std::wstring special_fq;
};

void RestoreGameState(DrawContext& ctx); // draw_restore.cpp
// The game's graphics root signature and its root arguments set again on the list; false = none recorded
// (draw_restore.cpp).
bool RestoreGameRoot(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl);
// One mod component's section run in place of the draw; true = it drew (draw_component.cpp).
bool DrawComponent(DrawContext& ctx, int dm, const WwmiMod::Component* wc);
// The mod components cast into a recognised character depth pass; true = suppress the game's (draw_shadow.cpp).
bool CastModShadows(DrawContext& ctx);

// One component's section run on one draw: what its hooks read and write, owned by DrawComponent for the run.
struct ComponentRun {
    ComponentRun(DrawContext& ctx, int dm, const WwmiMod::Component* wc);
    DrawContext& ctx;
    const int dm;
    const WwmiMod::Component* const wc;
    const std::wstring& ns; // the mod's namespace
    PortHooks hk;
    uint32_t did = 0; // draws the section issued
    RfxDrawState rfx_own;
    RfxDrawState& rfx; // ctx.rfx_shared when every section on this draw shares one, else rfx_own
    // X24: what slot 6 of the draw reads after RELOCBIND (the dump publish reads it)
    ID3D12Resource* x24_vb6_res = nullptr;
    uint64_t x24_vb6_va = 0;
    uint32_t x24_vb6_bytes = 0;
    uint8_t x24_vb6_verdict = 0;
    bool rtpal_no_merge = false; // RTPAL published a palette for a component that merges no skeleton
    uint64_t sh_restore_va = 0; // the game's bone CB to put back at b4 after a shadow draw on our own
    MorphDump::DrawRecord i8_row{};
    bool i8_pending = false;
    unsigned long long i8_nulls_before = 0;
};
void SectionDrawIndexed(ComponentRun& run, uint32_t c, uint32_t s2, uint32_t b2); // draw_section_draw.cpp
