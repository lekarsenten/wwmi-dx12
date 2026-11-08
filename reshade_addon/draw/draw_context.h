#pragma once
// One game draw on its way through the draw path: what the hook was called with, what the claim decided, the
// game's bindings saved before the mod's geometry goes in their place, and the bone roots of the bound layout.
#include "../addon_common.h"
#include "../mods/layout_registry.h" // LayoutRegistry::BoneRoots
#include "../mods/component_table.h" // MergedComp
#include "../mods/game_cb_reader.h"
#include "../hooks/command_list_registry.h"

struct DrawArgs {
    reshade::api::command_list* cmd_list;
    ID3D12GraphicsCommandList* d3d12_cl;
    uint32_t index_count;
    uint32_t instance_count;
    uint32_t first_index;
    int32_t vertex_offset;
    uint32_t first_instance;
    uint64_t vs_hash;
    CmdRT draw_rt; // stable for this draw: only the game's bind hook writes it
};

// Set by the claim (draw_claim.cpp) and the depth-pass recognition (draw_shadow.cpp); read by the inject path.
// b4 / b3 are the skeleton registers of WWMI's vertex shaders: the current and the previous pose.
struct DrawClaim {
    MergedComp* mc = nullptr; // the component this draw is, when claimed
    MergedComp* depth_claim = nullptr; // the claim a DEPTH draw earned and then released -- the shadow pass's identity
    bool mc_b4 = false; // did the game bind a CBV at the b4 root for this draw?
    uint64_t b4_va = 0; // the game's current (b4) / previous (b3) skeleton CBVs, of a claimed draw
    uint64_t b3_va = 0;
    RootCbvBind b4_cb = {}; // the same CBVs as bound at this draw, handed to the section as vs-cb4 / vs-cb3
    RootCbvBind b3_cb = {};
    int b4_root = -1; // b4 (current) / b3 (previous) root params for this pass
    int b3_root = -1;
    uint64_t rep_vs = 0; // VS hash whose mod.ini rules are applied
    bool full_shadow = false; // a recognised character depth pass: the mod components are cast into it
    uint64_t shadow_dsv = 0; // which shadow map it casts into
    uint32_t shadow_pass = 0; // render-pass instance (CmdRT::pass_gen) -- distinguishes reuse of one target
};

struct BoneRoots {
    reshade::api::command_list* cl;
    bool done = false;
    int b4 = -1;
    int b3 = -1;
    RootCbvBind b4cb = {};
    RootCbvBind b3cb = {};
    const BoneRoots& Get()
    {
        if (done) return *this;
        done = true;
        const uint64_t lay = g_command_lists.GfxLayout(cl);
        g_layouts.BoneRoots(lay, &b4, &b3);
        const CmdCbvBinds& cb = g_command_lists.Cbv(cl);
        if (b4 >= 0 && b4 < 64) b4cb = cb.binds[b4];
        if (b3 >= 0 && b3 < 64) b3cb = cb.binds[b3];
        return *this;
    }
};

// The per-object CB (vs-cb2) the game bound at this draw.
inline RootCbvBind ObjectCbOf(reshade::api::command_list* cl)
{
    const int root = g_layouts.ObjectCbRoot(g_command_lists.GfxLayout(cl));
    return (root >= 0 && root < 64) ? g_command_lists.Cbv(cl).binds[root] : RootCbvBind{};
}

struct RelocXition {
    ID3D12Resource* res;
    D3D12_RESOURCE_STATES back;
};

struct RfxDrawState;

struct DrawContext {
    const DrawArgs args;
    BoneRoots roots;
    DrawClaim claim;
    // The game's bindings before the mod's geometry went in their place; RestoreGameState puts them back.
    D3D12_VERTEX_BUFFER_VIEW save_vb[8] = {};
    bool save_vb_set[8] = {};
    unsigned reloc_vb_mask = 0;
    RelocXition reloc_xit[9] = {};
    unsigned reloc_xit_n = 0;
    D3D12_INDEX_BUFFER_VIEW save_ib = {};
    bool save_ib_set = false;
    bool cb_ok = false; // we bound the bone CB at b4/b3 ourselves (the frozen identity pose)
    bool x32_mid = false;
    unsigned x32_mid_restores = 0;
    unsigned x32_mid_skipped = 0;
    bool mod_switched_off = false; // its section drew nothing with $mod_enabled = 0: the game's draw
    RfxDrawState* rfx_shared = nullptr; // set: every section on this draw reads and writes this one
};
