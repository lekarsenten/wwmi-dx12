// draw_inject.cpp -- a claimed draw (or a recognised depth pass) replaced by the mod's geometry: the game's
// bindings snapshotted, the component's section run in its place, everything put back. See draw_parts.h.
#include "../addon_common.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/character_recognition.h"
#include "../instrumentation/gpu_timer.h"
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "../instrumentation/draw_dump_publish.h"
#include "draw_context.h"
#include "draw_parts.h"
#include "draw_inject.h"
#include "DrawCallInfo.h"
#include "draw_toggles.h"
#include "../migoto/dx12_input_layout.h"
#include "../migoto/frame_analysis_driver.h"
#include "../addon/addon.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/mod_set.h"

bool InjectClaimedDraw(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const d3d12_cl = da.d3d12_cl;
    const uint32_t index_count = da.index_count;
    const uint32_t instance_count = da.instance_count;
    const uint32_t first_index = da.first_index;
    const int32_t vertex_offset = da.vertex_offset;
    const uint32_t first_instance = da.first_instance;
    const uint64_t vs_hash = da.vs_hash;
    const CmdRT& draw_rt = da.draw_rt;
    MergedComp* const mc = dc.mc;
    if (mc && mc->vanilla) return false;

    if ((mc || dc.full_shadow) && !g_draw_toggles.disable_vb) {
        ProfScope _di_vbprep(PROF_DI_VbPrep);
        GpuBracket _gt(d3d12_cl, GT_ClaimedDraw);
        {
            auto& st = g_command_lists.Gfx(cmd_list);
            for (int s = 0; s < 8; ++s) {
                if (st.vb[s].va && st.vb[s].stride > 0) {
                    ctx.save_vb[s] = { st.vb[s].va, st.vb[s].size, st.vb[s].stride };
                    ctx.save_vb_set[s] = true;
                }
            }
            if (st.ib.va && st.ib.index_bytes > 0) {
                ctx.save_ib = { st.ib.va, st.ib.size,
                    st.ib.index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
                ctx.save_ib_set = true;
            }
        }
        uint64_t apply_vs = (mc && dc.rep_vs) ? dc.rep_vs : vs_hash;

        bool frozen = (mc && mc->freeze && Comps().IdentityCbVa() && dc.mc_b4);
        if (frozen) {
            if (dc.b4_root >= 0 && dc.b4_root < 64) {
                d3d12_cl->SetGraphicsRootConstantBufferView((UINT)dc.b4_root, Comps().IdentityCbVa());
                if (dc.b3_root >= 0 && dc.b3_root < 64)
                    d3d12_cl->SetGraphicsRootConstantBufferView((UINT)dc.b3_root, Comps().IdentityCbVa());
                ctx.cb_ok = true;
            }
        }

        if (mc && !dc.full_shadow) {
            const CmdRT& rtc = draw_rt;
            if (g_debug) { // COMPRTV: one-shot per (comp, rtv_count) -- shows each pass's real RT count at DRAW time
                if (DiagOnce("COMPRTV", (mc->comp_idx << 8) | (rtc.rtv_count & 0xff))) {
                    uint64_t psh = 0;
                    cmd_list->get_private_data(s_iid_ps_hash, &psh);
                    logger::write("COMPRTV comp=%d rtv_count=%u has_dsv=%d vs=%016llx ps=%016llx", mc->comp_idx,
                        rtc.rtv_count, (int)rtc.has_dsv, vs_hash, psh);
                }
            }
            bool color_pass = (rtc.rtv_count >= 1);
            bool cloth_hit =
                (color_pass && g_draw_toggles.skip_translucent_cloth && mc->comp_idx == 5 && rtc.rtv_count == 2);
            // slider = comp5(bow)-only so sweeping it pins the bow's pass without dropping the whole character.
            bool slider_hit = (color_pass && g_draw_toggles.dbg_comp5_suppress_rtv >= 1 && mc->comp_idx == 5 &&
                (int)rtc.rtv_count == g_draw_toggles.dbg_comp5_suppress_rtv);
            if (cloth_hit || slider_hit) {
                RestoreGameState(ctx);
                return true;
            } // drop this color pass only
        }
        _di_vbprep.End();

        if (g_draw_toggles.wwmi_geom && !(Mods().Count() == 0)) {
            if (dc.full_shadow) {
                return CastModShadows(ctx); // draw_shadow.cpp
            } else if (mc) {
                int dm = (mc->mod_id >= 0 && mc->mod_id < (int)Mods().Count()) ? mc->mod_id : 0;
                const WwmiMod::Component* wc = Mods().Comp(dm, mc->comp_idx);
                const int mc_slot = Comps().IndexOf(mc);
                RfxDrawState rfx_draw;
                bool drew = false;
                bool mc_drew = false;
                auto run_slot = [&](int s) {
                    const MergedComp& co = Comps().At(s);
                    const bool own = s == mc_slot;
                    if (!own && (co.vanilla || co.hide)) return;
                    if (drew) RestoreGameState(ctx);
                    const bool d = own ? DrawComponent(ctx, dm, wc)
                                       : DrawComponent(ctx, co.mod_id, Mods().Comp(co.mod_id, co.comp_idx));
                    if (own) mc_drew = d;
                    drew = d || drew;
                };
                ctx.rfx_shared = &rfx_draw;
                if (mc->run_order.empty()) run_slot(mc_slot);
                for (const int s : mc->run_order) run_slot(s);
                ctx.rfx_shared = nullptr;
                if (g_debug) { // SKELDBG: per (comp, skeleton-root) -- so depth pass (b4r=9) logs apart
                    if (DiagOnce("DEPTH-CB13", (mc->comp_idx << 8) | (dc.b4_root & 0xff))) {
                        uint64_t psh = 0;
                        cmd_list->get_private_data(s_iid_ps_hash, &psh);
                        logger::write("SKELDBG comp=%d cb13root=%d ps=%016llx remapped=%d drew=%d", mc->comp_idx,
                            dc.b4_root, psh, (int)(wc && wc->remapped), (int)mc_drew);
                    }
                }
                if (drew) {
                    RestoreGameState(ctx);
                    return true;
                }
                if (ctx.mod_switched_off) {
                    for (int s = 0; s < 8; ++s) {
                        const D3D12_VERTEX_BUFFER_VIEW none{ 0, 0, 0 };
                        d3d12_cl->IASetVertexBuffers(s, 1, ctx.save_vb_set[s] ? &ctx.save_vb[s] : &none);
                    }
                    if (ctx.save_ib_set) d3d12_cl->IASetIndexBuffer(&ctx.save_ib);
                    const unsigned unstamped = UnstampClaimedTextures(da);
                    if (g_debug &&
                        DiagChanged("SECTION-DECLINED", (mc->mod_id << 8) | (mc->comp_idx & 0xff), unstamped ? 1u : 0u))
                        logger::write("SECTION-DECLINED mod=%d comp=%d f=%u: the mod switched itself off "
                                      "($mod_enabled = 0) and its section drew nothing -- the game's own draw goes "
                                      "through, as on DX11; %u stamped slot(s) given the game's texture back",
                            mc->mod_id, mc->comp_idx, g_frame_id, unstamped);
                    return false;
                }
            }
        }

        PROF(PROF_DI_Tail); // the legacy tail, to the end of this block
        if (g_debug && g_draw_toggles.shadow_via_claim && mc) {
            const CmdRT& rtd = draw_rt;
            if (rtd.rtv_count == 0 && rtd.has_dsv) {
                const uint64_t k = apply_vs ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
                if (ProbeArmed() && DiagOnce("DEPTHINJ", (int)(k ^ (k >> 32))))
                    logger::write("DEPTHINJ comp=%d first=%u count=%u applyVS=%016llx cb_ok=%d cb13root=%d",
                        mc->comp_idx, first_index, index_count, apply_vs, (int)ctx.cb_ok, dc.b4_root);
            }
        }

        auto report_to_dump = [&](uint32_t count, uint32_t first, uint32_t insts) {
            if (!MigotoPort::FrameAnalysisArmed()) return;
            uint64_t fd_pso = 0;
            {
                fd_pso = g_command_lists.Gfx(cmd_list).gfx_pso;
            }
            MigotoPort::SetCurrentInputLayout(fd_pso);
            PublishGameBoundStateForDump(cmd_list);
            PublishGameTexturesForDump(cmd_list);
            DrawCallInfo fci{};
            fci.type = DrawCall::DrawIndexedInstanced;
            fci.IndexCount = count;
            fci.InstanceCount = insts;
            fci.FirstIndex = first;
            fci.FirstInstance = first_instance;
            uint64_t fa_ps = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &fa_ps);
            PublishGameDrawForDump(cmd_list, /*injected*/ true, nullptr, 0, 0, 0); // X24: no RELOCBIND on this path
            MigotoPort::FrameAnalysisAfterDraw(false, &fci, vs_hash, fa_ps, 0);
            PublishGameDrawForDumpEnd();
        };
        report_to_dump(index_count, first_index, instance_count ? instance_count : 1);
        d3d12_cl->DrawIndexedInstanced(
            index_count, instance_count ? instance_count : 1, first_index, vertex_offset, first_instance);
        RemovalPin::Ours(d3d12_cl, "mod-draw");
        g_draw_stats.last_replace_hash = vs_hash;
        g_draw_stats.last_replace_ib_count = index_count;
        ++g_draw_stats.draw_replace_count;
        RestoreGameState(ctx);
        return true; // we drew it; suppress the original
    }
    return false;
}
