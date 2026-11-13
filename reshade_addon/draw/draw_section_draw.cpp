// draw_section_draw.cpp -- a component section's `drawindexed`: the game's PSO and tables bent the way
// the section asks (blend variant, its ps-tN, RabbitFX maps and shader patch), the draw issued, and put
// back. See draw_inject.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/character_recognition.h"
#include "../instrumentation/occlusion_probe.h"
#include "../instrumentation/diag_arms.h"
#include "../instrumentation/gpu_timer.h"
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "../mods/port_section.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../rabbitfx/rabbitfx_bind.h"
#include "../hooks/pipeline_registry.h"
#include "../rt/rt_draw.h"
#include "../instrumentation/draw_dump_publish.h"
#include "draw_context.h"
#include "section_ps_stamp.h"
#include "draw_parts.h"
#include "draw_inject.h"
#include "../mods/wwmi_mod.h"
#include "../hooks/dxil_psv.h" // DxilSrvRegs -- A8-SKIP's PS texture count
#include "DrawCallInfo.h"
#include "../addon/settings.h"
#include "draw_toggles.h"
#include "../rabbitfx/pso_clone.h"
#include "../rabbitfx/pso_variant.h"
#include "../rabbitfx/decensor_rest.h"
#include "../rabbitfx/rfx_patch.h"
#include "../rabbitfx/rfx_patch_srvs.h"
#include "../rabbitfx/rfx_toggles.h"
#include "../textures/tex_toggles.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/dx12_wrap.h"
#include "../migoto/port_api.h"
#include "../instrumentation/morph_dump.h"
#include "../migoto/dx12_input_layout.h"
#include "../migoto/frame_analysis_driver.h"
#include "../addon/addon.h"
#include "DrawCallInfo.h"
#include "../mods/mod_set.h"

static RfxGlowState WithSectionRfxMaps(const RfxGlowState& g, int dm, int comp, uint32_t srv_gen0)
{
    if (g.fxmap.ptr || g.active) return g;
    float on[4];
    if (!PortApi::GetIniParam(kRfxIniOnParam, on) || on[1] == 0.f) return g;
    const MigotoPort::PipeState& st = MigotoPort::State();
    const int si = MigotoPort::StageIndex('p');
    auto view = [&](unsigned reg) {
        if ((int32_t)(st.srv_gen[si][reg] - srv_gen0) <= 0) return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        MigotoPort::WrapView* wv = st.srv[si][reg] ? MigotoPort::AsWrapView(st.srv[si][reg]) : nullptr;
        return (wv && wv->kind == 't') ? wv->cpu : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    };
    float shift[4];
    PortApi::GetIniParam(kRfxIniShiftParam, shift);
    RfxGlowState s = g;
    s.fxmap = view(kRfxCutoutReg);
    s.map = view(kRfxGlowReg);
    s.hue = shift[0];
    s.sat = shift[1];
    s.val = shift[2];
    s.brightness = shift[3];
    s.active = s.map.ptr != 0;
    if ((s.fxmap.ptr || s.active) && g_debug && DiagOnce("RFX-SECTION-MAPS", CompKey(dm, comp)))
        logger::write(
            "RFX-SECTION-MAPS mod=%d comp=%d: the section bound RabbitFX's ps-t%u / ps-t%u itself with y%u on -- "
            "fx map %s, glow map %s",
            dm, comp, kRfxGlowReg, kRfxCutoutReg, kRfxIniOnParam, s.fxmap.ptr ? "bound" : "none",
            s.active ? "bound" : "none");
    return s;
}

void SectionDrawIndexed(ComponentRun& run, uint32_t c, uint32_t s2, uint32_t b2)
{
    DrawContext& ctx = run.ctx;
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    const int dm = run.dm;
    const WwmiMod::Component* const wc = run.wc;
    const RfxGlowState rfx_glow = g_tex_toggles.disable
        ? RfxGlowState{}
        : WithSectionRfxMaps(run.rfx.glow, dm, wc ? (int)wc->index : -1, run.hk.srv_gen0);
    const RfxTexLatch rfx_tex = g_tex_toggles.disable ? RfxTexLatch{} : run.rfx.tex;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const d3d12_cl = da.d3d12_cl;
    const uint32_t index_count = da.index_count;
    const uint32_t first_index = da.first_index;
    const int32_t vertex_offset = da.vertex_offset;
    const uint32_t first_instance = da.first_instance;
    const uint64_t vs_hash = da.vs_hash;
    const CmdRT& draw_rt = da.draw_rt;
    MergedComp* const mc = dc.mc;
    PROF(PROF_DI_PsDraw);
    (void)b2;
    auto a8_note = [&](uint64_t pso) {
        uint64_t ps_hash = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
        static std::mutex s_mtx;
        static std::unordered_set<uint64_t> s_seen;
        {
            std::lock_guard l(s_mtx);
            if (s_seen.size() >= 256 || !s_seen.insert(vs_hash ^ (ps_hash * 0x9E3779B97F4A7C15ull)).second) return;
        }
        const DxilSrvRegs pr = g_pipelines.SrvRegs(pso, 'p');
        int srvs = 0;
        for (unsigned r = 0; r < 128; ++r) srvs += pr.has(r) ? 1 : 0;
        logger::write("A8-SKIP mod=%d comp=%d vs=%016llx ps=%016llx pso=%llx rtv=%u dsv=%d idx=%u+%u ps_srvs=%d%s%s",
            dm, mc ? mc->comp_idx : -1, (unsigned long long)vs_hash, (unsigned long long)ps_hash,
            (unsigned long long)pso, draw_rt.rtv_count, (int)draw_rt.has_dsv, s2, c, srvs, pr.parsed ? "" : " (no PSV)",
            pr.unbounded ? " +unbounded" : "");
    };
    if (c == 0xFFFFFFFFu) {
        c = index_count;
        s2 = first_index;
    } // drawindexed=auto
    if (mc && mc->hide) return;
    if (g_settings.hide_outline) { // A8: the outline shell is the front-culling pass
        reshade::api::cull_mode cm = reshade::api::cull_mode::none;
        const uint64_t pso = g_command_lists.Gfx(cmd_list).gfx_pso;
        if (g_pipelines.CullMode(pso, &cm) && cm == reshade::api::cull_mode::front) {
            a8_note(pso);
            return;
        }
    }
    MigotoPort::FlushUavBarriers(d3d12_cl);
    const VfRestore vf_restore = g_rt_draw.SkinDraw(cmd_list, d3d12_cl, dm, wc, c, s2, first_index, index_count, vertex_offset,
        vs_hash, dc.b4_va, dc.b4_root, run.rtpal_no_merge);
    const MigotoPort::LayoutOverride layout_ovr = MigotoPort::State().layout_override;
    ID3D12PipelineState* blend_pso = nullptr;
    uint64_t restore_pso = 0;
    MigotoPort::StateDesc* cs_blend = MigotoPort::AsStateDesc(MigotoPort::State().blend);
    if (cs_blend) {
        if (!g_draw_toggles.custom_subdraw) return;
        ++g_draw_stats.custom_blend_count;
        if (g_draw_toggles.custom_blend && !layout_ovr.count) {
            {
                restore_pso = g_command_lists.Gfx(cmd_list).gfx_pso;
            }
            blend_pso = g_pso_variants.BlendFromDesc(cmd_list->get_device(), restore_pso, &cs_blend->blend,
                MigotoPort::State().blend_factor, DecensorOn());
            if (blend_pso) {
                d3d12_cl->SetPipelineState(blend_pso);
                const FLOAT* bf = MigotoPort::State().blend_factor;
                float fac[4];
                for (int i2 = 0; i2 < 3; ++i2)
                    fac[i2] = (g_draw_toggles.custom_blend_factor >= 0.f) ? g_draw_toggles.custom_blend_factor : bf[i2];
                fac[3] = bf[3];
                d3d12_cl->OMSetBlendFactor(fac);
                ++g_draw_stats.custom_blend_bound_count;
            }
            if (g_debug) {
                static std::atomic<bool> once{ false };
                bool e = false;
                const FLOAT* dbf = MigotoPort::State().blend_factor;
                if (once.compare_exchange_strong(e, true))
                    logger::write("CUSTOMBLEND from PipeState: enable=%d op=%d src=%d dst=%d "
                                  "factor=%.3f,%.3f,%.3f,%.3f variant=%p",
                        (int)cs_blend->blend.RenderTarget[0].BlendEnable, (int)cs_blend->blend.RenderTarget[0].BlendOp,
                        (int)cs_blend->blend.RenderTarget[0].SrcBlend, (int)cs_blend->blend.RenderTarget[0].DestBlend,
                        dbf[0], dbf[1], dbf[2], dbf[3], (void*)blend_pso);
            }
        }
    }
    ID3D12PipelineState* layout_pso = nullptr;
    if (layout_ovr.count) {
        const uint64_t src = g_command_lists.Gfx(cmd_list).gfx_pso;
        if (layout_ovr.pso == src)
            layout_pso = g_pso_variants.InputLayout(
                cmd_list->get_device(), src, layout_ovr.elements, layout_ovr.count, DecensorOn(), false);
        else if (DiagOnce("ILAYOUT-PSO", CompKey(dm, wc ? (int)wc->index : 0)))
            logger::write(
                "ILAYOUT mod=%d comp=%d: the layout override was made for pso=%llx, this draw has pso=%llx -- drawn with the game's layout",
                dm, wc ? (int)wc->index : -1, layout_ovr.pso, (unsigned long long)src);
        if (layout_pso) d3d12_cl->SetPipelineState(layout_pso);
        if (cs_blend && g_draw_toggles.custom_blend && DiagOnce("ILAYOUT-BLEND", CompKey(dm, wc ? (int)wc->index : 0)))
            logger::write(
                "ILAYOUT mod=%d comp=%d: the section's blend is not applied -- the input-layout override takes the PSO",
                dm, wc ? (int)wc->index : -1);
    }
    const SectionPsStamps sect_ps =
        g_ps_stamps.Plan(cmd_list, run.hk.srv_gen0, run.hk.this_gen, dm, wc ? (int)wc->index : -1);
    sect_ps.QueueForShadow();
    if (!sect_ps.shared) g_ps_stamps.StampInPlace(sect_ps);
    ID3D12PipelineState* rfxp_pso = nullptr;
    uint64_t rfxp_restore = 0;
    bool rfxp_glow = false; // the variant carries the glow block AND this component armed it
    std::vector<WclSlotOvr> rfxp_binds;
    const bool rfx_censor = DecensorOn();
    if (g_rfx_toggles.patch && (rfx_glow.fxmap.ptr || rfx_glow.active) && !dc.full_shadow) {
        if (blend_pso) {
            if (DiagOnce("RFXPATCH-BLEND", CompKey(dm, wc ? (int)wc->index : 0)))
                logger::write(
                    "RFXPATCH mod=%d comp=%d: a blend variant is active on this draw -- the patch is not applied to it",
                    dm, wc ? (int)wc->index : -1);
        } else {
            rfxp_restore = g_command_lists.Gfx(cmd_list).gfx_pso;
            unsigned ps_max = 0;
            unsigned win_base = 0;
            unsigned win_count = 0;
            bool has_glow = false;
            rfxp_pso = g_rfx_patch.Variant(cmd_list->get_device(), rfxp_restore, &ps_max, &has_glow, rfx_censor);
            if (rfxp_pso && layout_pso) {
                rfxp_pso = g_pso_variants.InputLayout(
                    cmd_list->get_device(), rfxp_restore, layout_ovr.elements, layout_ovr.count, rfx_censor, true);
                if (!rfxp_pso && DiagOnce("RFXPATCH-LAYOUT", CompKey(dm, wc ? (int)wc->index : 0)))
                    logger::write(
                        "RFXPATCH mod=%d comp=%d: no input-layout variant carrying the patched PS -- drawn unpatched",
                        dm, wc ? (int)wc->index : -1);
            }
            const unsigned top_reg = has_glow ? kRfxParamsReg : kRfxCutoutReg;
            if (rfxp_pso && !g_rfx_bind.Param0Covers(cmd_list, ps_max > top_reg ? ps_max : top_reg, &win_base, &win_count)) {
                if (DiagOnce("RFXPATCH-WINDOW", (int)(((rfxp_restore ^ (rfxp_restore >> 32)) * 31u) & 0x7fffffff)))
                    logger::write(
                        "RFXPATCH mod=%d comp=%d pso=%llx: the PS reads up to t%u and the patch needs t%u, the draw's first SRV table is t%u x%u -- drawn unpatched",
                        dm, wc ? (int)wc->index : -1, (unsigned long long)rfxp_restore, ps_max, top_reg, win_base,
                        win_count);
                rfxp_pso = nullptr;
                ++g_rfx_stats.patch_nobind;
            }
            D3D12_CPU_DESCRIPTOR_HANDLE null_tex{ 0 };
            D3D12_CPU_DESCRIPTOR_HANDLE null_buf{ 0 };
            if (rfxp_pso && !g_rfx_patch_srvs.NullSrvs(&null_tex, &null_buf)) {
                rfxp_pso = nullptr;
                ++g_rfx_stats.patch_nobind;
            }
            if (rfxp_pso) {
                rfxp_binds.push_back({ (int)kRfxCutoutReg, rfx_glow.fxmap.ptr ? rfx_glow.fxmap : null_tex, false });
                if (has_glow) {
                    D3D12_CPU_DESCRIPTOR_HANDLE prm{ 0 };
                    if (rfx_glow.active)
                        prm = g_rfx_patch_srvs.ParamsSrv(rfx_glow.hue, rfx_glow.sat, rfx_glow.val, rfx_glow.brightness);
                    rfxp_glow = (prm.ptr != 0);
                    rfxp_binds.push_back({ (int)kRfxGlowReg, rfxp_glow ? rfx_glow.map : null_tex, false });
                    rfxp_binds.push_back({ (int)kRfxParamsReg, rfxp_glow ? prm : null_buf, false });
                }
            }
        }
    }
    RfxBindState tbind{};
    if (rfx_tex.d.ptr || rfx_tex.n.ptr || rfx_tex.l.ptr || rfx_tex.m.ptr) {
        PROF(PROF_DI_RfxDecide);
        const DxilSrvRegs ps_regs = g_pipelines.SrvRegs(g_command_lists.Gfx(cmd_list).gfx_pso, 'p');
        uint8_t ps_reads = 0;
        for (unsigned r = 0; r < 4; ++r)
            if (ps_regs.reads(r)) ps_reads |= (uint8_t)(1u << r);
        tbind = g_rfx_bind.DecideAndBind(cmd_list, d3d12_cl, dm, wc ? (int)wc->index : -1, rfx_tex.d, rfx_tex.n, rfx_tex.l,
            rfx_tex.m, draw_rt.rtv_count >= 4, ps_reads, rfxp_pso ? &rfxp_binds : nullptr);
    }
    if (rfxp_pso) {
        if (!tbind.applied) tbind = g_rfx_bind.Apply(cmd_list, d3d12_cl, rfxp_binds, /*guard_bindless*/ false);
        const bool bound = tbind.applied;
        if (DiagOnce("RFXPATCH-DRAW",
                (int)((((rfxp_restore ^ (rfxp_restore >> 32)) * 31u) + (uint64_t)(wc ? wc->index : 0)) & 0x7fffffff)))
            logger::write("RFXPATCH draw mod=%d comp=%d pso=%llx rtv=%u -> variant %p, fx map %s, glow %s: %s", dm,
                wc ? (int)wc->index : -1, (unsigned long long)rfxp_restore, draw_rt.rtv_count, (void*)rfxp_pso,
                rfx_glow.fxmap.ptr ? "bound" : "null",
                rfxp_glow ? "bound" : (rfx_glow.active ? "ARMED BUT NOT IN THIS VARIANT" : "null"),
                bound ? "BOUND" : "NOT bindable (no param0 window: RABBITFX-BAIL above) -- drawn unpatched");
        if (bound) {
            d3d12_cl->SetPipelineState(rfxp_pso);
            ++g_rfx_stats.patch_draws;
            if (rfxp_glow) ++g_rfx_stats.glow_count;
        } else {
            rfxp_pso = nullptr;
            rfxp_glow = false;
            ++g_rfx_stats.patch_nobind;
        }
    }
    if (sect_ps.shared && !tbind.applied) {
        tbind = g_rfx_bind.Apply(cmd_list, d3d12_cl, {}, /*guard_bindless*/ false);
        if (!tbind.applied) {
            if (DiagOnce("PS-SLOT-SHARED-INPLACE", CompKey(dm, wc ? (int)wc->index : 0)))
                logger::write(
                    "PS-SLOT-STAMP mod=%d comp=%d: no private table for this draw (RABBITFX-BAIL above) -- its "
                    "ps-tN stamped in place over the earlier draw's",
                    dm, wc ? (int)wc->index : -1);
            g_ps_stamps.StampInPlace(sect_ps);
        }
    }
    g_rfx_bind.SetExtraSlots(nullptr, 0); // RTFETCH's slots belonged to this draw's shadow bind only
    {
        auto* odev = reinterpret_cast<ID3D12Device*>(cmd_list->get_device()->get_native());
        const CmdRT& ort = draw_rt;
        const int oq = g_occl.Begin(d3d12_cl, odev, wc ? (int)wc->index : -1, c, ort.rtv_count, ort.has_dsv);
        if (g_diag_arms.vbdiag_frames.load(std::memory_order_relaxed) > 0) {
            uint64_t gva[8] = {};
            uint32_t gstr[8] = {};
            uint32_t gsz[8] = {};
            {
                auto& gs = g_command_lists.Gfx(cmd_list);
                for (unsigned s = 0; s < 8; ++s) {
                    gva[s] = gs.vb[s].va;
                    gstr[s] = gs.vb[s].stride;
                    gsz[s] = gs.vb[s].size;
                }
            }
            char vbs[1024];
            int off = 0;
            for (unsigned s = 0; s < 8 && off < (int)sizeof(vbs) - 128; ++s) {
                void* r = nullptr;
                unsigned long long va = 0;
                unsigned sz = 0;
                unsigned str = 0;
                unsigned stt = 0;
                const bool got = PortApi::GetShimVertexBuffer(s, &r, &va, &sz, &str, &stt);
                off += _snprintf_s(vbs + off, sizeof(vbs) - off, _TRUNCATE,
                    " vb%u[shim=%d va=%llx str=%u | game va=%llx sz=%u str=%u]", s, (int)got, va, str,
                    (unsigned long long)gva[s], gsz[s], gstr[s]);
            }
            logger::write("VBDIAG mod=%d comp=%d is_shadow=%d first=%u count=%u "
                          "vs=%016llx b4root=%d b4va=%llx |%s",
                dm, wc ? wc->index : -1, (int)dc.full_shadow, s2, c, vs_hash, dc.b4_root, (unsigned long long)dc.b4_va,
                vbs);
            D3D12_VIEWPORT vp0{};
            D3D12_RECT sc0{};
            uint32_t vpn = 0;
            uint32_t scn = 0;
            uint32_t topo = 0;
            uint32_t sref = 0;
            uint64_t pso = 0;
            char roots[256] = {};
            int ro = 0;
            {
                auto& gs = g_command_lists.Gfx(cmd_list);
                vpn = gs.vp_count;
                scn = gs.scissor_count;
                topo = gs.topology;
                sref = gs.stencil_ref;
                pso = gs.gfx_pso;
                if (vpn) vp0 = gs.vp[0];
                if (scn) sc0 = gs.scissor[0];
                for (int p = 0; p < 16 && ro < (int)sizeof(roots) - 24; ++p)
                    if (gs.root[p].va)
                        ro += _snprintf_s(roots + ro, sizeof(roots) - ro, _TRUNCATE, " r%d=%llx", p,
                            (unsigned long long)gs.root[p].va);
            }
            logger::write("VBDIAG2 mod=%d comp=%d is_shadow=%d vp=%u[%.0f,%.0f %.0fx%.0f "
                          "z=%.3f..%.3f] sc=%u[%ld,%ld %ldx%ld] topo=%u sref=%u pso=%llx |%s",
                dm, wc ? wc->index : -1, (int)dc.full_shadow, vpn, vp0.TopLeftX, vp0.TopLeftY, vp0.Width, vp0.Height,
                vp0.MinDepth, vp0.MaxDepth, scn, (long)sc0.left, (long)sc0.top, (long)(sc0.right - sc0.left),
                (long)(sc0.bottom - sc0.top), topo, sref, (unsigned long long)pso, roots);
        }
        if (MorphDump::Armed() || MigotoPort::FrameAnalysisArmed()) {
            X27Cb cbs[8];
            const uint64_t layh = X27ResolveVsCbs(cmd_list, cbs);
            char desc[420];
            X27Format(desc, sizeof desc, cbs);
            uint64_t pso_now = 0;
            {
                pso_now = g_command_lists.Gfx(cmd_list).gfx_pso;
            }
            logger::write("X27 CBMAP mod=%d comp=%d is_shadow=%d vs=%016llx list=%p layout=%llx pso=%llx "
                          "port: b4root=%d b3root=%d rfx_bound=%d glow=%d blend_pso=%d |%s",
                dm, wc ? wc->index : -1, (int)dc.full_shadow, (unsigned long long)vs_hash, (void*)d3d12_cl,
                (unsigned long long)layh, (unsigned long long)pso_now, dc.b4_root, dc.b3_root, (int)tbind.bound,
                (int)rfx_glow.active, (int)(blend_pso != nullptr), desc[0] ? desc : " (layout not mapped)");
            if (DiagOnce("X27-LAYOUT", (int)(layh ^ (layh >> 32)))) {
                std::vector<LayoutTableParam> lt;
                g_layouts.WithTables(layh, [&](const std::vector<LayoutTableParam>& t) { lt = t; });
                char conf[96] = "";
                size_t cn = 0;
                for (int r = 0; r < 8; ++r) {
                    if (cbs[r].kind != 1) continue;
                    for (auto const& t : lt) {
                        if (t.param != cbs[r].param || cn + 12 >= sizeof(conf)) continue;
                        const int k =
                            _snprintf_s(conf + cn, sizeof(conf) - cn, _TRUNCATE, " b%d/p%u", r, (unsigned)t.param);
                        if (k > 0) cn += (size_t)k;
                    }
                }
                logger::write("X27 LAYOUT %llx: %zu table params recorded; root-CBV params also listed as tables:%s",
                    (unsigned long long)layh, lt.size(), cn ? conf : " none");
            }
        }
        if (MigotoPort::FrameAnalysisArmed()) {
            uint64_t fd_pso = 0;
            {
                fd_pso = g_command_lists.Gfx(cmd_list).gfx_pso;
            }
            MigotoPort::SetCurrentInputLayout(fd_pso);
            PublishGameBoundStateForDump(cmd_list); // depth: always, 1/draw
            PublishGameTexturesForDump(cmd_list);
            logger::write("DUMPIDX %06u mod=%d comp=%d is_shadow=%d first=%u count=%u "
                          "vs=%016llx cb4=%p b4root=%d b4va=%llx b3root=%d b3va=%llx "
                          "ctx.cb_ok=%d",
                MigotoPort::FrameAnalysisDrawIndex(), dm, wc ? wc->index : -1, (int)dc.full_shadow, s2, c, vs_hash,
                (void*)dc.b4_cb.res, dc.b4_root, (unsigned long long)dc.b4_va, dc.b3_root, (unsigned long long)dc.b3_va,
                (int)ctx.cb_ok);
            DrawCallInfo fci{};
            fci.type = DrawCall::DrawIndexedInstanced;
            fci.IndexCount = c;
            fci.InstanceCount = 1;
            fci.FirstIndex = s2;
            fci.FirstInstance = first_instance;
            uint64_t fa_ps = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &fa_ps);
            PublishGameDrawForDump(
                cmd_list, /*injected*/ true, run.x24_vb6_res, run.x24_vb6_va, run.x24_vb6_bytes, run.x24_vb6_verdict);
            MigotoPort::FrameAnalysisAfterDraw(false, &fci, vs_hash, fa_ps, 0);
            PublishGameDrawForDumpEnd();
        }
        {
            uint64_t x20_pso = 0;
            {
                x20_pso = g_command_lists.Gfx(cmd_list).gfx_pso;
            }
            const int x20_key =
                (int)((((x20_pso ^ (x20_pso >> 32)) * 31u) + (uint64_t)(wc ? wc->index : 0)) & 0x7fffffff);
            if (DiagOnce("X20-LAYOUT", x20_key)) {
                char lay[1024];
                const unsigned ne = MigotoPort::DescribeInputLayout(x20_pso, lay, sizeof(lay));
                logger::write("X20 LAYOUT mod=%d comp=%d vs=%016llx pso=%llx is_shadow=%d elems=%u: %s", dm,
                    wc ? (int)wc->index : -1, (unsigned long long)vs_hash, (unsigned long long)x20_pso,
                    (int)dc.full_shadow, ne, ne ? lay : "(layout not recorded for this PSO)");
            }
            if (const wchar_t* fa_dir = MigotoPort::FrameAnalysisFolder()) {
                const unsigned nw = g_pso_clones.WriteShaders(x20_pso, fa_dir);
                if (nw)
                    logger::write(
                        "PSOSHADERS pso=%llx: %u shader file(s) added to the capture", (unsigned long long)x20_pso, nw);
            }
            if (DiagOnce("DSSTATE", (int)(((x20_pso ^ (x20_pso >> 32)) * 31u) & 0x7fffffff))) {
                char dss[512];
                const unsigned dn = g_pso_clones.DescribeDepthStencil(x20_pso, dss, sizeof(dss));
                logger::write("DSSTATE pso=%llx is_shadow=%d rtv=%u stencil_ref_at_draw=%u: %s",
                    (unsigned long long)x20_pso, (int)dc.full_shadow, draw_rt.rtv_count,
                    g_command_lists.Gfx(cmd_list).stencil_ref,
                    dn ? dss : "(PSO not captured -- no clone, or no depth_stencil_state subobject)");
            }
        }
        if (mc && mc->freeze && Comps().IdentityCbVa() && dc.b4_root >= 0 && dc.b4_root < 64) {
            d3d12_cl->SetGraphicsRootConstantBufferView((UINT)dc.b4_root, Comps().IdentityCbVa());
            if (dc.b3_root >= 0 && dc.b3_root < 64)
                d3d12_cl->SetGraphicsRootConstantBufferView((UINT)dc.b3_root, Comps().IdentityCbVa());
            g_draw_stats.freeze_binds.fetch_add(1, std::memory_order_relaxed);
            if (DiagOnce("X19-FREEZE", CompKey(dm, mc->comp_idx) * 64 + dc.b4_root))
                logger::write("X19: identity bones bound at root %d (b3 root %d) inside the section's "
                              "draw for mod=%d comp=%d -- F now reaches the draw",
                    dc.b4_root, dc.b3_root, dm, mc->comp_idx);
        }
        const int gt = g_gpu_timer.Begin(d3d12_cl, GT_SectionDraw);
        void* const hp = HotPath::GpuBracketBegin(d3d12_cl, GT_SectionDraw);
        d3d12_cl->DrawIndexedInstanced(c, 1, s2, 0, first_instance);
        ++t_x23_injected;
        HotPath::GpuBracketEnd(d3d12_cl, hp);
        g_gpu_timer.End(d3d12_cl, gt);
        // RTFETCH: the game's LocalVF back at b2 -- its next draw on this list may not rebind it.
        if (vf_restore.param != 0xFF) d3d12_cl->SetGraphicsRootConstantBufferView(vf_restore.param, vf_restore.va);
        RemovalPin::Ours(d3d12_cl, "x23-draw");
        ++run.did;
        g_occl.End(d3d12_cl, oq);
    }
    if (tbind.bound) g_rfx_bind.Restore(d3d12_cl, tbind);
    if (rfxp_pso && rfxp_restore) // the patched PS wrote the glow / cutout; back to the game's PSO
        d3d12_cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(g_command_lists.Gfx(cmd_list).gfx_restore()));
    if (layout_pso)
        d3d12_cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(g_command_lists.Gfx(cmd_list).gfx_restore()));
    if (blend_pso && restore_pso) {
        const CmdState& gs = g_command_lists.Gfx(cmd_list);
        d3d12_cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(gs.gfx_restore()));
        const float def[4] = { 1.f, 1.f, 1.f, 1.f };
        d3d12_cl->OMSetBlendFactor(gs.blend_factor_set ? gs.blend_factor : def);
    }
}
