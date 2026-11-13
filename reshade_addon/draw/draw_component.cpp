// draw_component.cpp -- one mod component drawn in place of a game draw: its [TextureOverride] section run
// by their engine, with this draw's hooks (RabbitFX latches, the section's drawindexed, the restore
// callback), in phases. See draw_inject.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../mods/character_recognition.h"
#include "../instrumentation/gpu_timer.h"
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "../mods/port_section.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../mods/shader_alias.h"
#include "../hooks/pipeline_registry.h"
#include "../rt/rt_draw.h"
#include "draw_context.h"
#include "draw_inject.h"
#include "draw_sectex.h"
#include "draw_skel_copies.h"
#include "../mods/wwmi_mod.h"
#include "../migoto/dx12_bridge.h"
#include "../addon/settings.h"
#include "draw_parts.h"
#include "draw_toggles.h"
#include "../rabbitfx/decensor_rest.h"
#include "../rabbitfx/rfx_patch.h"
#include "../rabbitfx/rfx_toggles.h"
#include "../shapekeys/shapekey_chain.h"
#include "../shapekeys/sk_toggles.h"
#include "../addon/addon.h"
#include "../textures/tex_select.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/dx12_skcap.h"
#include "../mods/skeleton_dump.h"
#include "../mods/buffer_relocation.h"
#include "../mods/mod_set.h"

static std::wstring s_rfx_glowmap_fq; // last GlowMap ref no Run has consumed yet
static std::wstring s_rfx_fxmap_fq; // last FXMap ref: the cutout's input
static thread_local int t_b4_root = -1; // the roots their engine asks for while the section runs (captureless)
static thread_local int t_b3_root = -1;

ComponentRun::ComponentRun(DrawContext& c, int mod_id, const WwmiMod::Component* comp)
    : ctx(c),
      dm(mod_id),
      wc(comp),
      ns(Mods().At(mod_id).wcl_ns),
      rfx(c.rfx_shared ? *c.rfx_shared : rfx_own)
{}

static void PublishDrawShaders(reshade::api::command_list* cmd_list, uint64_t vs_hash)
{
    if (!g_sk_toggles.publish_shader_state) return;
    uint64_t ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    const uint64_t ps = AliasShaderHash(ps_hash);
    PortApi::SetBoundShader('v', AliasShaderHash(vs_hash));
    PortApi::SetBoundShader('p', ps);
    if (!g_rfx_toggles.patch || !ps || PortApi::ShaderRegexLinked(ps) ||
        !PortApi::ShaderRegexGroupLoaded(kRfxNamespace, kRfxRegexGroup))
        return;
    unsigned ps_max = 0;
    bool has_glow = false;
    if (g_rfx_patch.Variant(
            cmd_list->get_device(), g_command_lists.Gfx(cmd_list).gfx_pso, &ps_max, &has_glow, DecensorOn()))
        PortApi::LinkShaderRegexGroup(ps, kRfxNamespace, kRfxRegexGroup);
}

// The section's RabbitFX hooks: the `ref` lines that name its maps, and the `run` that binds them.
static void InstallRfxHooks(ComponentRun& run)
{
    run.hk.resource_ref = [&run](const std::wstring& lhs_fq, const std::wstring& rhs_fq) {
        if (lhs_fq.find(L"rabbitfx") == std::wstring::npos) return;
        RfxDrawState& rfx = run.rfx;
        if (lhs_fq.find(L"glowmap") != std::wstring::npos)
            s_rfx_glowmap_fq = rhs_fq;
        else if (lhs_fq.find(L"fxmap") != std::wstring::npos)
            s_rfx_fxmap_fq = rhs_fq;
        else if (lhs_fq.find(L"diffuse") != std::wstring::npos)
            rfx.diffuse_fq = rhs_fq;
        else if (lhs_fq.find(L"normalmap") != std::wstring::npos)
            rfx.normal_fq = rhs_fq;
        else if (lhs_fq.find(L"lightmap") != std::wstring::npos)
            rfx.light_fq = rhs_fq;
        else if (lhs_fq.find(L"materialmap") != std::wstring::npos)
            rfx.material_fq = rhs_fq;
        else if (lhs_fq.find(L"cutoutmap") != std::wstring::npos)
            rfx.cutout_fq = rhs_fq;
        else if (lhs_fq.find(L"specialmap") != std::wstring::npos)
            rfx.special_fq = rhs_fq;
    };
    run.hk.run_cmd = [&run](const std::wstring& /*n*/, const std::wstring& target_fq) {
        if (target_fq.find(L"rabbitfx") == std::wstring::npos) return;
        const int dm = run.dm;
        const WwmiMod::Component* const wc = run.wc;
        RfxDrawState& rfx = run.rfx;
        // SetTextures (texture-binding half).
        if (target_fq.find(L"settextures") != std::wstring::npos) {
            auto srv = [&](const std::wstring& fq) { return g_tex_select.RfxMapSrv(fq, run.ns.c_str()); };
            D3D12_CPU_DESCRIPTOR_HANDLE d = srv(rfx.diffuse_fq);
            D3D12_CPU_DESCRIPTOR_HANDLE n = srv(rfx.normal_fq);
            D3D12_CPU_DESCRIPTOR_HANDLE l = srv(rfx.light_fq);
            D3D12_CPU_DESCRIPTOR_HANDLE m = srv(rfx.material_fq);
            const bool pending = (!rfx.diffuse_fq.empty() && !d.ptr) || (!rfx.normal_fq.empty() && !n.ptr) ||
                (!rfx.light_fq.empty() && !l.ptr) || (!rfx.material_fq.empty() && !m.ptr);
            rfx.tex = pending ? RfxTexLatch{} : RfxTexLatch{ d, n, l, m }; // the draws below read the latch
            if (g_debug) {
                const D3D12_CPU_DESCRIPTOR_HANDLE cut = srv(rfx.cutout_fq);
                const D3D12_CPU_DESCRIPTOR_HANDLE spc = srv(rfx.special_fq);
                const int k = CompKey(dm, wc ? wc->index : 0);
                const int mask = (d.ptr ? 1 : 0) | (n.ptr ? 2 : 0) | (l.ptr ? 4 : 0) | (m.ptr ? 8 : 0) |
                    (cut.ptr ? 16 : 0) | (spc.ptr ? 32 : 0);
                if (DiagChanged("RABBITFX-TEX", k, (unsigned long long)(unsigned)mask))
                    logger::write("RABBITFX-TEX f=%u mod=%d comp=%d diffuse=%ls(srv=%llx) "
                                  "normal=%ls(srv=%llx) light=%ls(srv=%llx) material=%ls(srv=%llx) "
                                  "cutout=%ls(srv=%llx, not bound on DX12) special=%ls(srv=%llx, not bound on DX12)%s",
                        g_frame_id, dm, wc ? wc->index : -1, rfx.diffuse_fq.c_str(), (unsigned long long)d.ptr,
                        rfx.normal_fq.c_str(), (unsigned long long)n.ptr, rfx.light_fq.c_str(),
                        (unsigned long long)l.ptr, rfx.material_fq.c_str(), (unsigned long long)m.ptr,
                        rfx.cutout_fq.c_str(), (unsigned long long)cut.ptr, rfx.special_fq.c_str(),
                        (unsigned long long)spc.ptr,
                        pending ? "  -- a named map is not loaded: the game's textures stay" : "");
            }
            return;
        }
        float h = PortApi::GetVar(L"rabbitfx\\h", 0.f);
        float br = PortApi::GetVar(L"rabbitfx\\brightness", 0.f);
        float ip = PortApi::GetVar(L"rabbitfx\\interpolate", 0.f);
        float s = PortApi::GetVar(L"rabbitfx\\s", 0.f);
        float v = PortApi::GetVar(L"rabbitfx\\v", 0.f);
        D3D12_CPU_DESCRIPTOR_HANDLE glow = s_rfx_glowmap_fq.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{ 0 }
                                                                    : g_tex_select.ModSrvByResourceFq(s_rfx_glowmap_fq);
        D3D12_CPU_DESCRIPTOR_HANDLE fxm =
            s_rfx_fxmap_fq.empty() ? D3D12_CPU_DESCRIPTOR_HANDLE{ 0 } : g_tex_select.ModSrvByResourceFq(s_rfx_fxmap_fq);
        RfxGlowState& rfx_glow = rfx.glow;
        rfx_glow.hue = (h / 360.0f) * ip;
        rfx_glow.sat = (s / 100.0f) * ip;
        rfx_glow.val = (v / 100.0f) * ip;
        rfx_glow.brightness = br;
        rfx_glow.map = glow;
        rfx_glow.fxmap = fxm;
        rfx_glow.active = (glow.ptr != 0); // brightness==0 is RECOLOUR, not "off" -- the ini writes o3 either way
        if (rfx_glow.active) ++g_rfx_stats.glow_params;
        if (g_debug &&
            DiagOnce("RABBITFX-INVOKE",
                CompKey(dm, wc ? wc->index : 0) * 8 +
                    (int)((glow.ptr ? 1u : 0u) | (fxm.ptr ? 2u : 0u) | (br != 0.f ? 4u : 0u)))) {
            logger::write(
                "RABBITFX invoke mod=%d comp=%d h=%.1f s=%.1f v=%.1f brightness=%.2f interpolate=%.2f glowmap=%ls srv=%llx fxmap=%ls srv=%llx -> glow %s%s",
                dm, wc ? wc->index : -1, h, s, v, br, ip, s_rfx_glowmap_fq.c_str(), (unsigned long long)glow.ptr,
                s_rfx_fxmap_fq.c_str(), (unsigned long long)fxm.ptr,
                rfx_glow.active ? "ARMED" : "inactive(no glow map)", fxm.ptr ? "; FX map present" : "");
        }
    };
}

// What their engine's restore callback puts back after each of the section's own dispatches and draws.
struct PortRestore {
    reshade::api::command_list* game = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12DescriptorHeap* srv = nullptr;
    ID3D12DescriptorHeap* smp = nullptr;
    std::vector<std::pair<UINT, uint64_t>> tables;
    DrawContext* ctx = nullptr; // X32: x32_mid raised around the inner call so RestoreGameState knows it is mid-section
    ID3D12PipelineState* pso = nullptr;
};
struct PushCbGuard {
    uint16_t vs = 0; // the vs-cbN pushed from the game's draw, but the skeleton and object ones
    uint16_t ps = 0; // the ps-cbN pushed from the game's draw
    ~PushCbGuard()
    {
        PortApi::PushGameConstantBuffer('v', 4, nullptr, 0, 0);
        PortApi::PushGameConstantBuffer('v', 3, nullptr, 0, 0);
        PortApi::PushGameConstantBuffer('v', 2, nullptr, 0, 0);
        for (unsigned reg = 0; reg < 16; ++reg) {
            if (vs & (1u << reg)) PortApi::PushGameConstantBuffer('v', reg, nullptr, 0, 0);
            if (ps & (1u << reg)) PortApi::PushGameConstantBuffer('p', reg, nullptr, 0, 0);
        }
        if (g_draw_toggles.push_game_vb) PortApi::PushGameVertexBuffer(0, nullptr, 0, 0, 0, 0);
    }
};
// The scopes a section runs in, in the order they are entered: their engine's lock, the draw's command list
// handed to it, and the game CBs unpushed when the run ends.
struct SectionScopes {
    std::optional<PortApi::EngineScope> engine;
    PortRestore port_restore;
    std::optional<PortApi::DrawListScope> list;
    std::optional<PushCbGuard> push_guard;
};

static void EnterSectionScopes(ComponentRun& run, SectionScopes& sc)
{
    DrawContext& ctx = run.ctx;
    reshade::api::command_list* const cmd_list = ctx.args.cmd_list;
    {
        PROF(PROF_DI_EngLock);
        sc.engine.emplace();
    }
    PROF(PROF_CS_Restore);
    PortRestore& port_restore = sc.port_restore;
    {
        const auto& root = g_command_lists.Gfx(cmd_list).root;
        const uint64_t layh = g_command_lists.GfxLayout(cmd_list);
        std::vector<LayoutTableParam> ltabs;
        if (layh) g_layouts.WithTables(layh, [&](const std::vector<LayoutTableParam>& t) { ltabs = t; });
        for (const auto& t : ltabs) {
            const TableStart ts = TableStartOf(cmd_list->get_device(), root[t.param].table);
            if (!ts) continue;
            auto* hp = ts.Heap();
            if (t.kind == 1 && !port_restore.srv) port_restore.srv = hp;
            if (t.kind == 0 && !port_restore.smp) port_restore.smp = hp;
            port_restore.tables.emplace_back((UINT)t.param, (uint64_t)root[t.param].table.handle);
        }
        if (ID3D12DescriptorHeap* gh = GameHeap(cmd_list, 0)) port_restore.srv = gh;
        if (ID3D12DescriptorHeap* gh = GameHeap(cmd_list, 1)) port_restore.smp = gh;
        port_restore.game = cmd_list;
        port_restore.cl = ctx.args.d3d12_cl;
        port_restore.ctx = &ctx;
        port_restore.pso = reinterpret_cast<ID3D12PipelineState*>(g_command_lists.Gfx(cmd_list).gfx_restore());
    }
    t_b4_root = ctx.claim.b4_root;
    t_b3_root = ctx.claim.b3_root;
    if (g_draw_toggles.port_draw_list)
        sc.list.emplace(
            ctx.args.d3d12_cl,
            [](void* u) {
                auto* r = static_cast<PortRestore*>(u);
                if (r->pso) r->cl->SetPipelineState(r->pso);
                if (MigotoPort::TakeHeapsChanged()) {
                    if (r->srv) {
                        ID3D12DescriptorHeap* h[2] = { r->srv, r->smp };
                        r->cl->SetDescriptorHeaps(r->smp ? 2 : 1, h);
                    } else if (g_debug && DiagOnce("HEAPRESTORE-NOSRV", 0)) {
                        logger::write(
                            "HEAPRESTORE: our heaps were set on this list and the game's CBV_SRV_UAV heap is unknown -- ours stay set");
                    }
                }
                const bool root = MigotoPort::g_touched_root_sig && RestoreGameRoot(r->game, r->cl);
                MigotoPort::g_touched_root_sig = false;
                if (!root)
                    for (const auto& t : r->tables)
                        r->cl->SetGraphicsRootDescriptorTable(t.first, D3D12_GPU_DESCRIPTOR_HANDLE{ t.second });
                r->ctx->x32_mid = true;
                RestoreGameState(*r->ctx);
                r->ctx->x32_mid = false;
            },
            &port_restore,
            [](void*, unsigned slot) -> int { return slot == 4 ? t_b4_root
                                                  : slot == 3  ? t_b3_root
                                                               : -1; }, nullptr);
    if (sc.list) {
        InheritGameBindings(*sc.list, cmd_list);
        GameBackBuffer(*sc.list, cmd_list, ctx.args.draw_rt);
    }
    sc.push_guard.emplace();
}

// The game's skeleton CBs (vs-cb4 / vs-cb3), the per-object CB (vs-cb2) and vb0 pushed to their engine, as the
// section's `ref` lines read them on DX11.
static void PushGameBindings(ComponentRun& run)
{
    DrawContext& ctx = run.ctx;
    const DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = ctx.args.cmd_list;
    const int dm = run.dm;
    const WwmiMod::Component* const wc = run.wc;
    ++g_draw_stats.port_sect_runs;
    if (dc.b4_cb.res)
        g_skeleton_dump.NoteDrawCbs(dc.b4_cb.res, dc.b4_cb.va, dc.b4_root, dc.b3_cb.res, dc.b3_cb.va, dc.b3_root);
    if (!dc.b4_cb.res) ++g_draw_stats.port_sect_nocb;
    if (logger::enabled(1) && DiagOnce("PORTCB", CompKey(dm, wc->index))) {
        logger::write("PORTCB mod=%d comp=%d cb13res=%p va=%llx cb13root=%d  "
                      "cb14res=%p va=%llx cb14root=%d %s",
            dm, wc->index, (void*)dc.b4_cb.res, (unsigned long long)dc.b4_cb.va, dc.b4_root, (void*)dc.b3_cb.res,
            (unsigned long long)dc.b3_cb.va, dc.b3_root,
            dc.b4_cb.res ? "" : "*** NO BONE CB -- vs-cb4 will read UNBOUND ***");
    }
    const uint64_t draw_pso = g_command_lists.Gfx(cmd_list).gfx_pso;
    auto cb_width = [&](unsigned reg) -> unsigned {
        uint32_t decl = 0;
        if (!g_pipelines.VsCbufferBytes(draw_pso, reg, decl)) return kDx11SkeletonCbByteWidth;
        return decl == kBoneCBBytes ? kDx11SkeletonCbByteWidth : decl;
    };
    const RtPalResult pal = g_rt_draw.PalPublish(draw_pso, dm, wc, dc.b4_cb.va);
    if (pal.published) {
        t_b4_root = -1;
        run.rtpal_no_merge = pal.no_merge;
    }
    constexpr unsigned kNotSkeletonWidth = 256;
    if (!pal.published)
        PortApi::PushGameConstantBuffer('v', 4, dc.b4_cb.res, dc.b4_cb.va, pal.tried ? kNotSkeletonWidth : cb_width(4));
    unsigned w3 = cb_width(3);
    if (pal.tried && w3 == kDx11SkeletonCbByteWidth) {
        uint32_t decl = 0;
        const bool parsed = g_pipelines.VsCbufferBytes(draw_pso, 3, decl);
        if (DiagOnce("RTPAL-CB3", (int)(draw_pso ^ (draw_pso >> 32))))
            logger::write("RTPAL-CB3 pso=%llx mod=%d comp=%d: b3 would have been published as a skeleton "
                          "(declared %u B, %s) -- published at %u B instead",
                (unsigned long long)draw_pso, dm, wc->index, decl, parsed ? "read" : "declarations NOT read",
                kNotSkeletonWidth);
        w3 = kNotSkeletonWidth;
    }
    PortApi::PushGameConstantBuffer('v', 3, dc.b3_cb.res, dc.b3_cb.va, w3);
    {
        const RootCbvBind obj = ObjectCbOf(cmd_list);
        if (obj.res && obj.va) PortApi::PushGameConstantBuffer('v', 2, obj.res, obj.va, kDx11ObjectCbByteWidth);
    }

    if (g_draw_toggles.push_game_vb) {
        uint64_t gva[8] = {};
        uint32_t gsz[8] = {};
        uint32_t gstr[8] = {};
        ID3D12Resource* gres[8] = {};
        {
            auto const& gs = g_command_lists.Gfx(cmd_list);
            for (unsigned s = 0; s < 8; ++s) {
                gva[s] = gs.vb[s].va;
                gsz[s] = gs.vb[s].size;
                gstr[s] = gs.vb[s].stride;
                gres[s] = gs.vb[s].res;
            }
        }
        for (unsigned s = 0; s < 1; ++s) {
            if (!gres[s] || !gva[s]) {
                PortApi::PushGameVertexBuffer(s, nullptr, 0, 0, 0, 0);
                continue;
            }
            const uint64_t base = gres[s]->GetGPUVirtualAddress();
            const unsigned off = (gva[s] >= base) ? (unsigned)(gva[s] - base) : 0u;
            PortApi::PushGameVertexBuffer(s, gres[s], base, gsz[s] + off, gstr[s], off);
        }
        if (logger::enabled(1) && ProbeArmed() && DiagOnce("PORTVB", CompKey(dm, wc ? wc->index : 0))) {
            logger::write("PORTVB mod=%d comp=%d vb0res=%p va=%llx size=%u stride=%u %s", dm, wc ? wc->index : -1,
                (void*)gres[0], (unsigned long long)gva[0], gsz[0], gstr[0],
                gres[0] ? "" : "*** NO vb0 -- `ref vb0` will capture NOTHING ***");
        }
    }
}

// A shadow draw inherits the pass's bone CB; the component's own, noted at its colour pass, goes in its place.
static void SwapShadowBoneCb(ComponentRun& run)
{
    DrawContext& ctx = run.ctx;
    const DrawClaim& dc = ctx.claim;
    const int dm = run.dm;
    const WwmiMod::Component* const wc = run.wc;
    if (!dc.full_shadow && wc && dc.b4_va) g_skel_copies.NoteComponentBone(dm, wc->index, dc.b4_va);
    if (dc.full_shadow && wc && dc.b4_root >= 0 && dc.b4_root < 64) {
        const uint64_t own = g_skel_copies.ComponentBone(dm, wc->index);
        if (!dc.b4_va) {
            if (DiagOnce("SHADOWBONE-REFUSED", CompKey(dm, wc->index)))
                logger::write("SHADOWBONE mod=%d comp=%d REFUSED: the game bound NO CB "
                              "at root %d on this pass, so we cannot know the register "
                              "is a bone CB here -- and would have nothing to restore",
                    dm, wc->index, dc.b4_root);
        }
        if (own && dc.b4_va && own != dc.b4_va) {
            ctx.args.d3d12_cl->SetGraphicsRootConstantBufferView((UINT)dc.b4_root, own);
            run.sh_restore_va = dc.b4_va; // put the game's back after our draw
            if (logger::enabled(1)) {
                if (DiagOnce("SHADOWBONE-RESTORED", CompKey(dm, wc->index)))
                    logger::write("SHADOWBONE mod=%d comp=%d root=%d inherited=%llx "
                                  "-> own=%llx (per-component CB restored)",
                        dm, wc->index, dc.b4_root, (unsigned long long)dc.b4_va, (unsigned long long)own);
            }
        }
    }
}

// RELOCBIND: the widened buffers the chain writes bound in place of the game's, and the capture taps (I8, I9).
static void BindRelocatedBuffers(ComponentRun& run, ID3D12Resource* sk_inst)
{
    DrawContext& ctx = run.ctx;
    const DrawArgs& da = ctx.args;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const d3d12_cl = da.d3d12_cl;
    const int dm = run.dm;
    const WwmiMod::Component* const wc = run.wc;
    PROF(PROF_CS_Reloc);
    if (!d3d12_cl) return;
    uint64_t gva[8] = {};
    uint32_t gstr[8] = {};
    ID3D12Resource* gres[8] = {};
    uint32_t ifmt = 0;
    ID3D12Resource* ires = nullptr;
    {
        auto const& gs = g_command_lists.Gfx(cmd_list);
        for (unsigned q = 0; q < 8; ++q) {
            gva[q] = gs.vb[q].va;
            gstr[q] = gs.vb[q].stride;
            gres[q] = gs.vb[q].res;
        }
        ifmt = gs.ib.index_bytes;
        ires = gs.ib.res;
    }
    SkNoteDrawVbs(dm, gres, 8);
    auto xit_in = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES to) -> bool {
        if (!r || ctx.reloc_xit_n >= 9) return false;
        ctx.reloc_xit[ctx.reloc_xit_n++] = { r, MigotoPort::OwnedTransition(d3d12_cl, r, to) };
        return true;
    };
    SkBufferBinding my_u0b{};
    const bool have_my_u0 = SkGetBinding(run.ns, sk_inst, TargetClass::UnorderedAccessView, 0, &my_u0b);
    ID3D12Resource* const my_u0 = my_u0b.res;
    uint8_t v6 = 3;
    uint64_t v6_sub_va = 0;
    uint32_t v6_sub_bytes = 0;
    ID3D12Resource* v6_sub_res = nullptr;
    uint64_t v6_sub_off = 0;
    bool v6_xit = false;
    for (unsigned q = 0; q < 8; ++q) {
        RelocBuf rb{};
        if (!gstr[q] || !g_buffer_reloc.Resolve(gres[q], &rb)) {
            if (q == 6) v6 = gstr[q] ? 0 : 3;
            continue;
        }
        if (!have_my_u0 || rb.res != my_u0) {
            if (q == 6) v6 = 2;
            if (logger::enabled(1)) {
                static const char kRelocBindSite = 0;
                if (DiagOnce((const void*)&kRelocBindSite, dm * 8 + (int)q))
                    logger::write("RELOCBIND mod=%d vb%u: DECLINED -- the widened "
                                  "buffer for this resource (%p) is not this mod's "
                                  "u0 (%p, have=%d); binding it would hand this "
                                  "draw another character's morphs",
                        dm, q, (void*)rb.res, (void*)my_u0, (int)have_my_u0);
            }
            continue;
        }
        const bool xin = xit_in(rb.res, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
        D3D12_VERTEX_BUFFER_VIEW nv{ rb.va, (UINT)rb.bytes, gstr[q] };
        d3d12_cl->IASetVertexBuffers(q, 1, &nv);
        ctx.reloc_vb_mask |= (1u << q);
        if (q == 6) {
            v6 = 1;
            v6_sub_va = rb.va;
            v6_sub_bytes = (uint32_t)rb.bytes;
            v6_sub_res = rb.res;
            v6_xit = xin;
            const uint64_t base = rb.res->GetGPUVirtualAddress();
            v6_sub_off = (rb.va >= base) ? (rb.va - base) : 0;
            run.x24_vb6_res = rb.res;
            run.x24_vb6_va = rb.va;
            run.x24_vb6_bytes = (uint32_t)rb.bytes;
            run.x24_vb6_verdict = 1; // X24
        }
        if (logger::enabled(1)) {
            if (DiagOnce("RELOCBIND", dm * 8 + (int)q))
                logger::write("RELOCBIND mod=%d vb%u: %llx -> %llx/%llu (stride %u)"
                              " -- the draw now reads the WIDENED buffer the chain "
                              "writes",
                    dm, q, (unsigned long long)gva[q], (unsigned long long)rb.va, (unsigned long long)rb.bytes,
                    gstr[q]);
        }
    }
    RelocBuf rb{};
    if (ires && ifmt && ctx.save_ib_set && g_buffer_reloc.Resolve(ires, &rb)) {
        xit_in(rb.res, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        D3D12_INDEX_BUFFER_VIEW ni{ rb.va, (UINT)rb.bytes, ifmt == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
        d3d12_cl->IASetIndexBuffer(&ni);
    }
    const bool i8 = MorphDump::Armed();
    const bool i9 = (v6 == 1) && v6_sub_res && MigotoPort::SkCapArmed();
    if (i8 || i9) {
        const uint8_t is_depth = (da.draw_rt.rtv_count == 0 && da.draw_rt.has_dsv) ? 1 : 0;
        const int comp_ix = wc ? wc->index : -1;
        if (i9) {
            static std::atomic<unsigned> s_dseq{ 0 };
            char prov[72];
            sprintf_s(prov, "D f=%u m=%d c=%d l=%llx d=%u x=%d n=%u fi=%u", g_frame_id, dm, comp_ix,
                (unsigned long long)reinterpret_cast<uintptr_t>(d3d12_cl), (unsigned)is_depth, (int)v6_xit,
                da.index_count, da.first_index);
            MigotoPort::SkCapSnapDraw(g_addon.d3d12_device, d3d12_cl, "vb6", v6_sub_res, v6_sub_off, v6_sub_bytes,
                v6_xit ? D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER : D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                1000u + s_dseq.fetch_add(1, std::memory_order_relaxed), prov);
        }
        if (i8) {
            MorphDump::DrawRecord& w = run.i8_row;
            w.frame = g_frame_id;
            w.mod_id = dm;
            w.comp = comp_ix;
            w.list = reinterpret_cast<uint64_t>(d3d12_cl);
            w.index_count = da.index_count;
            w.first_index = da.first_index;
            w.vertex_offset = da.vertex_offset;
            w.is_depth = is_depth;
            w.vb6_res = reinterpret_cast<uint64_t>(gres[6]);
            w.vb6_va = gva[6];
            w.vb6_stride = gstr[6];
            if (gres[6]) {
                ResInfo ri{};
                if (g_resources.InfoOf(reinterpret_cast<uint64_t>(gres[6]), &ri)) w.vb6_bytes = ri.size;
            }
            w.vb6_verdict = v6;
            w.sub_va = v6_sub_va;
            w.sub_bytes = v6_sub_bytes;
            w.my_u0 = reinterpret_cast<uint64_t>(my_u0);
            w.vb0_res = reinterpret_cast<uint64_t>(gres[0]);
            w.ns = run.ns.c_str();
            run.i8_pending = true;
            run.i8_nulls_before = MigotoPort::VbNullUnbinds();
        }
    }
}

// The section's draws are issued; what it did to the slots recorded, the game's bone CB put back after a shadow
// draw on our own, the counters kept. True = the game's draw is replaced.
static bool FinishSection(ComponentRun& run, bool ran, bool skip_orig, ID3D12Resource* sk_inst)
{
    DrawContext& ctx = run.ctx;
    const DrawArgs& da = ctx.args;
    const int dm = run.dm;
    const WwmiMod::Component* const wc = run.wc;
    PROF(PROF_DI_CompPost); // to the end of this block, every return included
    if (ctx.x32_mid_restores) {
        g_draw_stats.x32_mid_total.fetch_add(ctx.x32_mid_restores, std::memory_order_relaxed);
        g_draw_stats.x32_mid_skipped_total.fetch_add(ctx.x32_mid_skipped, std::memory_order_relaxed);
        if (logger::enabled(1) && DiagOnce("X32-MID", CompKey(dm, wc->index)))
            logger::write("X32 MID-RESTORE mod=%d comp=%d vs=%016llx: the section issued %u engine "
                          "dispatch/draw(s) before its drawindexed and each one's restore callback "
                          "ran restore_state (reloc mask %u); IA rebinds skipped on %u of them. %s",
                dm, wc->index, (unsigned long long)da.vs_hash, ctx.x32_mid_restores, ctx.reloc_vb_mask,
                ctx.x32_mid_skipped,
                ctx.x32_mid_skipped == ctx.x32_mid_restores
                    ? "The injected draw keeps the RELOCBIND substitution."
                    : "A restore path skipped the mid-section guard: slot 6 went back to "
                      "the GAME's vanilla morph buffer -- RE-OPEN.");
    }
    if (run.i8_pending) { // X9: what the section did to the slots after the verdict
        const unsigned long long nb = MigotoPort::VbNullUnbinds();
        run.i8_row.ini_null_unbinds = (nb > run.i8_nulls_before) ? (uint32_t)(nb - run.i8_nulls_before) : 0u;
        run.i8_row.mid_restores = ctx.x32_mid_restores; // X32
        run.i8_row.mid_ia_skipped = ctx.x32_mid_skipped;
        MorphDump::RecordDraw(run.i8_row);
    }
    if (run.sh_restore_va) da.d3d12_cl->SetGraphicsRootConstantBufferView((UINT)ctx.claim.b4_root, run.sh_restore_va);
    if (logger::enabled(1)) {
        if (DiagOnce("COMP-NS", CompKey(dm, wc->index))) {
            const std::wstring& u2 = run.ns;
            logger::write("CHAINPROF post-section mod=%d comp=%d ns='%ls' "
                          "object_detected=%g state_id=%g mod_enabled=%g ran=%d",
                dm, wc->index, u2.c_str(), PortApi::GetVarNs(u2.c_str(), L"object_detected", -1.f),
                PortApi::GetVarNs(u2.c_str(), L"state_id", -1.f), PortApi::GetVarNs(u2.c_str(), L"mod_enabled", -1.f),
                (int)ran);
        }
    }
    if (logger::enabled(1) && ran) {
        const float en = PortApi::GetVarNs(run.ns.c_str(), L"mod_enabled", -1.f);
        const unsigned long long v =
            ((unsigned long long)(en * 16.f + 64.f) << 8) | (run.did ? 2u : 0u) | (skip_orig ? 1u : 0u);
        const int key = (int)(((uint64_t)(unsigned)dm << 24) ^ ((uint64_t)(unsigned)wc->index << 20) ^
            (reinterpret_cast<uintptr_t>(sk_inst) >> 4));
        if (DiagChanged("MODDRAW", key, v))
            logger::write("MODDRAW f=%u mod=%d comp=%d inst=%p: mod_enabled=%g drew=%u skip=%d", g_frame_id, dm,
                wc->index, (void*)sk_inst, en, run.did, (int)skip_orig);
    }
    GeoMod& unit = Mods().At(dm);
    ++unit.sect_runs;
    if (ran && skip_orig) ++unit.skip_orig;
    if (ran) unit.replaced += run.did;
    if (!ran) return false;
    g_draw_stats.draw_replace_count += run.did;
    g_draw_stats.wwmi_geom_count += run.did;
    if (skip_orig || run.did > 0) return true;
    ++g_draw_stats.port_fellback_count;
    RestoreGameState(ctx);
    ctx.mod_switched_off = PortApi::GetVarNs(run.ns.c_str(), L"mod_enabled", -1.f) == 0.f;
    return false;
}

bool DrawComponent(DrawContext& ctx, int dm, const WwmiMod::Component* wc)
{
    const DrawArgs& da = ctx.args;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const d3d12_cl = da.d3d12_cl;
    PROF(PROF_DI_DrawComp);
    if (dm < 0 || dm >= (int)Mods().Count()) return false;
    if (!g_draw_toggles.wcl_draw || !wc) return false;
    // DI:CompSetup: everything before PortSect; ended right before it (a span, not a block).
    ProfScope _di_compsetup(PROF_DI_CompSetup);
    ProfScope _cs_prep(PROF_CS_Prep);
    const std::wstring& uns = Mods().At(dm).wcl_ns;
    const std::wstring fq = PortComponentSection(dm, wc->index, wc->tail, uns);
    if (fq.empty()) return false;
    bool skip_orig = false;
    ComponentRun run(ctx, dm, wc);
    run.hk.handling_skip = [&skip_orig] { skip_orig = true; };
    InstallRfxHooks(run);
    run.hk.drawindexed = [&run](uint32_t c, uint32_t s2, uint32_t b2) { SectionDrawIndexed(run, c, s2, b2); };
    {
        static std::atomic<bool> once{ false };
        if (!once.exchange(true))
            logger::write("PORTDRAW first component dispatch: mod=%d comp=%d fq='%ls'", dm, wc->index, fq.c_str());
    }
    _cs_prep.End();
    SectionScopes scopes;
    EnterSectionScopes(run, scopes);
    ID3D12Resource* sk_inst = nullptr;
    unsigned sk_copy = 0;
    bool sk_first = false;
    {
        PROF(PROF_CS_Push);
        PushGameBindings(run);
        constexpr uint16_t kSkeletonAndObjectCbs = (1u << 2) | (1u << 3) | (1u << 4);
        scopes.push_guard->vs = PushGameConstantBuffers(cmd_list, 'v', kSkeletonAndObjectCbs);
        scopes.push_guard->ps = PushGameConstantBuffers(cmd_list, 'p', 0);
        SwapShadowBoneCb(run);
        {
            auto const& gs = g_command_lists.Gfx(cmd_list);
            if (gs.vb[6].stride) sk_inst = gs.vb[6].res;
        }
        sk_copy = g_skel_copies.CopySlot(
            cmd_list, dm, wc->index, ctx.claim.b4_cb.res, ctx.claim.b4_cb.va, ctx.claim.full_shadow, &sk_first);
        if (sk_copy) {
            const RootCbvBind obj = ObjectCbOf(cmd_list);
            if (obj.res && obj.va)
                PortApi::PushGameConstantBuffer('v', 2, obj.res, obj.va, kDx11ObjectCbByteWidth, sk_copy);
            if (logger::enabled(1) && DiagOnce("OBJCB-COPY", CompKey(dm, (int)sk_copy)))
                logger::write("OBJCB-COPY mod=%d skeleton #%u: vs-cb2 is published through this copy's own wrapper, "
                              "so `@vs-cb2` names the copy",
                    dm, sk_copy);
        }
        if (g_draw_toggles.sk_run_passes) {
            GpuBracket _gt(d3d12_cl, GT_ShapeKeys);
            RunShapeKeyPasses(uns, dm, sk_inst, cmd_list);
        }
    }
    BindRelocatedBuffers(run, sk_inst);
    ProfScope _cs_secttex(PROF_CS_SectTex);
    const uint16_t sect_tex = g_section_textures.Publish(cmd_list, dm, wc->index, da.draw_rt.rtv_count);
    if (MorphDump::Armed())
        run.hk.cs_enter = [](const std::wstring&, const std::wstring& t) {
            if (t.find(L"skeletonmerger") != std::wstring::npos) ++t_x23_merges;
        };
    _cs_secttex.End();
    _di_compsetup.End();
    bool ran;
    {
        PROF(PROF_DI_PortSect);
        g_draw_stats.sections_this_frame.fetch_add(1, std::memory_order_relaxed); // GTEXMISS: sections before a miss
        std::optional<PortApi::SkelCopyScope> sk_scope;
        if (sk_copy || sk_first) sk_scope.emplace(uns.c_str(), sk_copy);
        PublishDrawShaders(cmd_list, da.vs_hash);
#ifdef WWMI_DEBUG
        const CmdState& bt_gs = g_command_lists.Gfx(cmd_list);
        if (logger::enabled(1) && g_recognition.IbAnchorJustLearned(bt_gs.ib.va)) {
            char bt_ia[768] = "";
            int bt_n = 0;
            for (unsigned s = 0; s < 16 && bt_n >= 0 && bt_n < (int)sizeof(bt_ia); ++s)
                if (bt_gs.vb[s].va)
                    bt_n += snprintf(bt_ia + bt_n, sizeof(bt_ia) - bt_n, " vb%u=%llx+%u(%p)", s,
                        (unsigned long long)bt_gs.vb[s].va, bt_gs.vb[s].size, (void*)bt_gs.vb[s].res);
            logger::write(
                "BINDTRACE mod=%d comp=%d f=%u: the first frame claimed on this index buffer -- the game's IA:%s "
                "ib=%llx+%u(%p); the section's commands and binds follow as CHAIN lines",
                dm, wc->index, g_frame_id, bt_ia, (unsigned long long)bt_gs.ib.va, bt_gs.ib.size, (void*)bt_gs.ib.res);
            t_trace_binds = true;
        }
#endif
        ran = PortRunSection(fq, run.hk, da.index_count, da.first_index, da.first_instance,
            g_command_lists.Gfx(cmd_list).gfx_pso, skip_orig);
        t_trace_binds = false;
    }
    for (unsigned r = 0; r < kSectionTexRegs; ++r)
        if (sect_tex & (1u << r)) PortApi::UnpushGameTexture('p', r);
    return FinishSection(run, ran, skip_orig, sk_inst);
}
