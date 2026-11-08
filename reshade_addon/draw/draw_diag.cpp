// draw_diag.cpp -- the draw path's debug lines that need the claim's verdict: the frame-analysis hand-off of a
// game draw, CDRAW, DRAW, the UAV stalker's character vb0s and LODDIAG. See draw_parts.h.
#include "../addon_common.h"
#include "draw_parts.h"
#include "../instrumentation/draw_dump_publish.h"
#include "../instrumentation/diag_arms.h"
#include "../instrumentation/uav_stalker.h"
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "../mods/character_recognition.h" // InSkinBand
#include "../core/log.h"
#include "../migoto/frame_analysis_driver.h"
#include "DrawCallInfo.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../hooks/command_list_registry.h"
#include <cstdio>
#include "../mods/mod_set.h"

using namespace reshade::api;

static void NoteGameDrawCalls(command_list* cmd_list, const DrawCallInfo& ci, uint64_t vs_hash, uint64_t ps_hash)
{
    void* const pso = reinterpret_cast<void*>(g_command_lists.Gfx(cmd_list).gfx_pso);
    const CmdRT& rt = g_command_lists.Rt(cmd_list);
    char line[256];
    std::snprintf(line, sizeof line,
        "OMSetRenderTargets(NumViews:%u, ppRenderTargetViews:0x%p, pDepthStencilView:0x%p)",
        rt.valid ? rt.rtv_count : 0u, reinterpret_cast<void*>(rt.valid && rt.rtv_count ? rt.rtv[0] : 0),
        reinterpret_cast<void*>(rt.valid && rt.has_dsv ? rt.dsv : 0));
    MigotoPort::FrameAnalysisNote(line);
    std::snprintf(line, sizeof line,
        "VSSetShader(pVertexShader:0x%p, ppClassInstances:0x0000000000000000, NumClassInstances:0) hash=%016llx", pso,
        (unsigned long long)vs_hash);
    MigotoPort::FrameAnalysisNote(line);
    std::snprintf(line, sizeof line,
        "PSSetShader(pPixelShader:0x%p, ppClassInstances:0x0000000000000000, NumClassInstances:0) hash=%016llx", pso,
        (unsigned long long)ps_hash);
    MigotoPort::FrameAnalysisNote(line);
    if (ci.type == DrawCall::DrawIndexedInstanced)
        std::snprintf(line, sizeof line,
            "DrawIndexedInstanced(IndexCountPerInstance:%u, InstanceCount:%u, StartIndexLocation:%u, "
            "BaseVertexLocation:%d, StartInstanceLocation:%u)",
            ci.IndexCount, ci.InstanceCount, ci.FirstIndex, (int)ci.FirstVertex, ci.FirstInstance);
    else
        std::snprintf(line, sizeof line, "DrawIndexed(IndexCount:%u, StartIndexLocation:%u, BaseVertexLocation:%d)",
            ci.IndexCount, ci.FirstIndex, (int)ci.FirstVertex);
    MigotoPort::FrameAnalysisNote(line);
}

void DrawDiagIndexed(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t vs_hash = da.vs_hash;
    const uint32_t first_index = da.first_index;
    const uint32_t index_count = da.index_count;
    const uint32_t instance_count = da.instance_count;
    const uint32_t first_instance = da.first_instance;
    const int32_t vertex_offset = da.vertex_offset;
    MergedComp* const mc = ctx.claim.mc;
    bool fd_claimed = (mc != nullptr);
    bool fd_charband = false;
    if (MigotoPort::FrameAnalysisArmed() && !fd_claimed) {
        uint64_t vb0 = 0;
        {
            vb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
        }
        fd_charband = g_recognition.InSkinBand(vb0);
    }
    if (MigotoPort::FrameAnalysisWantGameDraw(fd_claimed, fd_charband) && index_count >= 3) {
        DrawCallInfo fci{};
        fci.type = (instance_count > 1) ? DrawCall::DrawIndexedInstanced : DrawCall::DrawIndexed;
        fci.IndexCount = index_count;
        fci.InstanceCount = instance_count;
        fci.FirstIndex = first_index;
        fci.FirstVertex = (UINT)vertex_offset;
        fci.FirstInstance = first_instance;
        uint64_t fa_ps = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &fa_ps);
        NoteGameDrawCalls(cmd_list, fci, vs_hash, fa_ps);
        // The GAME's own geometry and targets, not the last injected draw's leftovers.
        PublishGameDrawForDump(cmd_list, /*injected*/ false, nullptr, 0, 0, 0);
        MigotoPort::FrameAnalysisAfterDraw(false, &fci, vs_hash, fa_ps, 0);
        PublishGameDrawForDumpEnd();
    }

    if ((g_diag_arms.log_merged.load() > 0 || g_diag_arms.log_lod.load()) && index_count >= 64) {
        uint64_t vb0 = 0;
        {
            vb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
        }
        bool char_pool = (vb0 >= 0x300000000ULL && vb0 < 0x400000000ULL);
        bool at_comp = Comps().SlotOf(first_index, index_count) >= 0;
        if (char_pool || at_comp || index_count >= 3000) {
            uint64_t ps_hash = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
            uint32_t bone_roots = g_game_cbs.BoneRootMask(cmd_list);
            uint64_t key = vs_hash ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
            const bool fresh = DiagOnce("CLONEDIAG", (int)(key ^ (key >> 32)));
            uint64_t bone_va = 0;
            {
                auto it = &g_command_lists.Cbv(cmd_list);
                if (it != nullptr)
                    for (int rp = 0; rp < 16; ++rp)
                        if (bone_roots & (1u << rp)) {
                            bone_va = it->binds[rp].va;
                            break;
                        }
            }
            if (fresh)
                logger::write(
                    "CDRAW vs=%016llx ps=%016llx first=%u count=%u vb0=%llx pool=%d atcomp=%d claimed=%d comp=%d boneRoots=%04x boneVA=%llx",
                    vs_hash, ps_hash, first_index, index_count, vb0, (int)char_pool, (int)at_comp, (int)(mc != nullptr),
                    mc ? mc->comp_idx : -1, bone_roots, bone_va);
        }
    }

    // --- Diagnostic: log all substantial draws (find LOD hashes at distance) ---
    if (g_diag_arms.log_all_draws.load() > 0 && index_count >= 500) {
        // Tag character-pool draws: VB[0] VA inside the known skin-cache pool range
        uint64_t vb0 = 0;
        {
            vb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
        }
        bool charPool = (vb0 >= 0x3f0000000ULL && vb0 < 0x400000000ULL); // CS output pool band
        logger::write("DRAW vs=%016llx first=%u count=%u vb0=%llx%s", vs_hash, first_index, index_count, vb0,
            charPool ? "  <<CHAR-POOL" : "");
    }

    if (vs_hash == 0xbd50930e536be3eaULL || vs_hash == 0x1835840f4e3ff12aULL || vs_hash == 0xec783ffd78365180ULL) {
        auto const& state = g_command_lists.Gfx(cmd_list);
        g_uav_stalker.NoteCharVb(state.vb[0].va);
    }

    if (vs_hash == 0x1758b5d5982ddd63ULL || vs_hash == 0xbb0ac53ff09064e6ULL) {
        const uint64_t lk = vs_hash ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
        if (DiagOnce("LODDIAG", (int)(lk ^ (lk >> 32)))) {
            auto const& st = g_command_lists.Gfx(cmd_list);
            logger::write("LODDIAG vs=%016llx first=%u count=%u ib_va=%llx ib_bytes=%u", vs_hash, first_index,
                index_count, st.ib.va, st.ib.index_bytes);
            for (int s = 0; s < 8; ++s)
                if (st.vb[s].va)
                    logger::write(
                        "  LODVB[%d] va=%llx stride=%u size=%u", s, st.vb[s].va, st.vb[s].stride, st.vb[s].size);
            for (int rp = 0; rp < 64; ++rp)
                if (st.root[rp].table.handle)
                    logger::write("  LODTABLE root[%d] handle=%llx", rp, st.root[rp].table.handle);
            auto cit = &g_command_lists.Cbv(cmd_list);
            if (cit != nullptr) {
                for (int rp = 0; rp < 64; ++rp) {
                    auto& b = cit->binds[rp];
                    if (!b.res || !b.va) continue;
                    // Existing mappings only, as before: a diagnostic does not map new resources.
                    float m[12];
                    const bool bonelike =
                        g_game_cbs.Snapshot(b.res, b.va, (uint8_t*)m, sizeof(m), false) == sizeof(m) &&
                        LooksLikeBone(m);
                    logger::write("  LODCBV root[%d] va=%llx size=%u bonelike=%d", rp, b.va, b.size, (int)bonelike);
                }
            }
        }
    }
}
