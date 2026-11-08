// draw_probes.cpp -- the draw path's debug-only lines (g_debug, most armed by ProbeArm). Nothing here changes
// what is drawn. See draw_parts.h.
#include "../addon_common.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/character_recognition.h"
#include "../hooks/resource_tracker.h"
#include "../mods/layout_registry.h"
#include "draw_context.h"
#include "draw_parts.h"
#include <set>
#include "draw_toggles.h"
#include "../mods/buffer_relocation.h"
#include "../textures/tex_place.h"
#include "../textures/tex_toggles.h"
#include "../mods/ib_hash.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/shader_alias.h"
#include "../mods/mod_set.h"

using namespace reshade::api;

void DrawProbesClaim(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t index_count = da.index_count;
    const uint32_t first_index = da.first_index;
    const uint64_t vs_hash = da.vs_hash;
    if (g_debug) {
        const int it = Comps().SlotOf(first_index, index_count);
        if (it >= 0) {
            const uint64_t k = ((uint64_t)first_index << 24) ^ index_count;
            if (ProbeArmed() && DiagOnce("DRAW-FIRSTCOUNT", (int)(k ^ (k >> 32)))) {
                uint64_t vb0dbg = 0;
                {
                    vb0dbg = g_command_lists.Gfx(cmd_list).vb[0].va;
                }
                logger::trace("CLAIMHIT comp first=%u count=%u vs=%016llx vb0=%llx claimed=%d", first_index,
                    index_count, vs_hash, vb0dbg, (int)(dc.mc != nullptr));
                {
                    int md2 = Comps().At(it).mod_id;
                    const WwmiMod::Component* wc2 = Mods().Comp(md2, Comps().At(it).comp_idx);
                    const uint32_t maxEnd = Mods().IndexEnd(md2, wc2 ? wc2->ib_hash : 0u);
                    ID3D12Resource* ibr = nullptr;
                    ID3D12Resource* vbr = nullptr;
                    uint64_t ibva = 0;
                    uint64_t vbva = 0;
                    uint32_t idxb = 2;
                    uint32_t vstride = 0;
                    {
                        auto& st = g_command_lists.Gfx(cmd_list);
                        ibr = st.ib.res;
                        ibva = st.ib.va;
                        if (st.ib.index_bytes) idxb = st.ib.index_bytes;
                        vbr = st.vb[0].res;
                        vbva = st.vb[0].va;
                        vstride = st.vb[0].stride;
                    }
                    if (ibr && vbr && maxEnd && vstride) {
                        uint32_t iboff = (uint32_t)(ibva - ibr->GetGPUVirtualAddress());
                        uint32_t vboff = (uint32_t)(vbva - vbr->GetGPUVirtualAddress());
                        uint32_t got = IbHash::Lookup(ibr, iboff, maxEnd * idxb, idxb, vbr, vboff, vstride);
                        logger::trace("IBHASH-ANCHOR mod=%d ibBytes=%u vstride=%u ourVbHash=%08x iniHash=%08x %s", md2,
                            maxEnd * idxb, vstride, got, wc2 ? wc2->ib_hash : 0,
                            (got && wc2 && got == wc2->ib_hash) ? "*** MATCH ***" : (got ? "nomatch" : "pending"));
                    }
                }
            }
        }
    }

#ifdef WWMI_DEBUG
    if (g_debug && dc.mc) {
        uint64_t ps_hash = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
        const int md = dc.mc->mod_id;
        const int ci = dc.mc->comp_idx;
        const uint64_t k = (vs_hash ^ (ps_hash * 1099511628211ULL)) ^ ((uint64_t)(uint32_t)CompKey(md, ci) << 40);
        const unsigned epoch = g_diag_epoch.load(std::memory_order_relaxed);
        thread_local std::unordered_set<uint64_t> t_seen;
        thread_local unsigned t_epoch = ~0u;
        if (t_epoch != epoch) {
            t_seen.clear();
            t_epoch = epoch;
        }
        if (t_seen.insert(k).second) {
            static std::mutex m;
            static std::unordered_set<uint64_t> seen;
            static unsigned seen_epoch = ~0u;
            bool fresh;
            {
                std::lock_guard l(m);
                if (seen_epoch != epoch) {
                    seen.clear();
                    seen_epoch = epoch;
                }
                fresh = seen.insert(k).second;
            }
            if (fresh) {
                const uint64_t dx11 = AliasShaderHashAlways(ps_hash);
                char partner[64];
                if (ShaderOverrideMatches(ps_hash, nullptr))
                    std::snprintf(partner, sizeof partner, "a [ShaderOverride] has this hash");
                else if (dx11 != ps_hash)
                    std::snprintf(partner, sizeof partner, "dx11 %016llx", (unsigned long long)dx11);
                else
                    std::snprintf(partner, sizeof partner, "NO DX11 PARTNER");
                const wchar_t* ns = md >= 0 && md < (int)Mods().Count() ? Mods().At(md).wcl_ns.c_str() : L"?";
                logger::report("CLAIMPS mod=%d '%ls' comp=%d vs=%016llx ps=%016llx: %s (first=%u count=%u)", md, ns, ci,
                    (unsigned long long)vs_hash, (unsigned long long)ps_hash, partner, first_index, index_count);
                const WwmiMod::Component* wc = Mods().Comp(md, ci);
                const uint32_t mesh = wc ? wc->ib_hash : 0u;
                static std::mutex pm;
                static std::unordered_set<uint64_t> paired_on;
                for (const uint64_t h : { vs_hash, ps_hash }) {
                    const uint32_t from = ShaderPairMesh(h);
                    if (!from || !mesh || from == mesh) continue;
                    {
                        std::lock_guard l(pm);
                        if (!paired_on.insert(h ^ ((uint64_t)mesh * 0x9E3779B97F4A7C15ull)).second) continue;
                    }
                    logger::report("PAIR-FOREIGN dx12 %016llx -> dx11 %016llx, paired on mesh %08x, applies to mod=%d "
                                  "'%ls' comp=%d mesh %08x",
                        (unsigned long long)h, (unsigned long long)AliasShaderHashAlways(h), from, md, ns, ci, mesh);
                }
            }
        }
    }
#endif
}

void DrawProbesPasses(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t index_count = da.index_count;
    const uint32_t first_index = da.first_index;
    const uint64_t vs_hash = da.vs_hash;
    const CmdRT& draw_rt = da.draw_rt;
    if (g_debug && index_count >= 64 && ProbeArmed()) {
        const uint64_t vb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
        const int b4r = ctx.roots.Get().b4;
        const RootCbvBind b4cb = ctx.roots.Get().b4cb;
        bool charish_vb = (vb0 >= 0x300000000ULL && vb0 < 0x400000000ULL);
        if (!charish_vb && vb0 && g_recognition.IsCharMeshVb0(vb0)) charish_vb = true;
        uint32_t sh = HashBoneCB(b4cb.res, b4cb.va);
        const bool inskel = sh && g_recognition.InFrameSkel(sh);
        if (charish_vb || inskel) { // character draws only -- no world depth flood
            uint64_t pprobe = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &pprobe);
            const CmdRT& rt = draw_rt;
            const uint64_t k =
                vs_hash ^ (pprobe * 1099511628211ULL) ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
            const bool fresh = ProbeArmed() && DiagOnce("DEPTHPASS-PROBE", (int)(k ^ (k >> 32)));
            // rtv=0 + dsv=1 => DEPTH-ONLY pass (shadow caster / prepass) = the pass we're hunting.
            if (fresh)
                logger::trace(
                    "SHADOWPROBE vs=%016llx ps=%016llx first=%u count=%u vb0=%llx b4r=%d claimed=%d rtv=%u dsv=%d charVB=%d inSkel=%d",
                    vs_hash, pprobe, first_index, index_count, vb0, b4r, (int)(dc.mc != nullptr), rt.rtv_count,
                    (int)rt.has_dsv, (int)charish_vb, (int)inskel);
            if (fresh && rt.rtv_count == 0 && rt.has_dsv) {
                ID3D12Resource* pres[64];
                uint64_t pva[64];
                int pp[64];
                int npv = 0;
                {
                    auto it = &g_command_lists.Cbv(cmd_list);
                    if (it != nullptr)
                        for (int p = 0; p < 64; ++p) {
                            auto& b = it->binds[p];
                            if (b.res && b.va) {
                                pp[npv] = p;
                                pres[npv] = b.res;
                                pva[npv] = b.va;
                                ++npv;
                            }
                        }
                }
                char cbs[460];
                int n = 0;
                cbs[0] = 0;
                for (int pi = 0; pi < npv && n < 410; ++pi) {
                    uint32_t hp = HashBoneCB(pres[pi], pva[pi]);
                    const bool ins = hp && g_recognition.InFrameSkel(hp);
                    n += snprintf(cbs + n, sizeof(cbs) - n, "b%d=%llx%s ", pp[pi], (unsigned long long)pva[pi],
                        ins ? "*SKEL*" : "");
                }
                logger::write("SHADOWCBV vs=%016llx first=%u b4r=%d binds:[%s]", vs_hash, first_index, b4r, cbs);
            }
        }
    }


    if (g_debug) {
        PROF(PROF_DI_Probe);
        static std::mutex s_pd;
        static std::set<uint64_t> s_char;
        static std::set<uint64_t> s_other;
        // On-demand RE-ARM: when the overlay bumps g_diag_gen, wipe this capture so a fresh one starts HERE.
        static uint32_t s_gen = 0;
        {
            uint32_t g = g_diag_gen.load();
            if (g != s_gen) {
                std::lock_guard l(s_pd);
                s_char.clear();
                s_other.clear();
                s_gen = g;
            }
        }
        // ARM ONLY AFTER A MOD CHARACTER APPEARS (pool is shared by all actors -- see SKINPROBE): require its mesh known.
        const bool charUp = g_recognition.AnyCharMesh();
        if (charUp && index_count >= 32) {
            const uint64_t vb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
            const RootCbvBind b4cb = ctx.roots.Get().b4cb;
            bool boneNear = false;
            uint64_t bdist = ~0ull;
            if (b4cb.va) {
                bdist = g_recognition.CharBoneDistance(b4cb.va);
                boneNear = (bdist < 0x80000ULL);
            }
            bool inPool = g_recognition.InSkinBand(vb0);
            if (boneNear || inPool) {
                uint64_t k = vs_hash ^ ((uint64_t)first_index << 3) ^ ((uint64_t)index_count << 20);
                bool fresh = false;
                {
                    std::lock_guard l(s_pd);
                    if (boneNear) {
                        if (s_char.size() < 96) fresh = s_char.insert(k).second;
                    } else {
                        if (s_other.size() < 24) fresh = s_other.insert(k).second;
                    }
                }
                if (fresh) {
                    uint64_t psh = 0;
                    cmd_list->get_private_data(s_iid_ps_hash, &psh);
                    uint32_t bh = HashBoneCB(b4cb.res, b4cb.va);
                    const bool inSkel = bh && g_recognition.InFrameSkel(bh);
                    // rtv/dsv: rtv=0 + dsv=1 => DEPTH/SHADOW caster; rtv>0 => VISIBLE pass.
                    const CmdRT& rt = draw_rt;
                    logger::trace(
                        "POOLDRAW %s vs=%016llx ps=%016llx first=%u count=%u vb0=%llx claimed=%d rtv=%u dsv=%d b4va=%llx boneNear=%d bdist=%llx inSkel=%d",
                        boneNear ? "CHAR " : "world", vs_hash, psh, first_index, index_count, vb0,
                        (int)(dc.mc != nullptr), rt.rtv_count, (int)rt.has_dsv, b4cb.va, (int)boneNear, bdist,
                        (int)inSkel);
                }
            }
        }
    }

    if (g_debug && index_count >= 32) {
        uint64_t vb0 = 0;
        {
            vb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
        }
        const bool skinlod = vb0 && g_recognition.InSkinOut(vb0);
        if (skinlod) {
            const uint64_t k = vs_hash ^ ((uint64_t)first_index << 1) ^ ((uint64_t)index_count << 20);
            if (ProbeArmed() && DiagOnce("DRAW-VSRANGE", (int)(k ^ (k >> 32)))) {
                uint64_t psh = 0;
                cmd_list->get_private_data(s_iid_ps_hash, &psh);
                logger::write(
                    "SKINLOD vs=%016llx ps=%016llx first=%u count=%u vb0=%llx claimed=%d (char via skin-cache lineage)",
                    vs_hash, psh, first_index, index_count, vb0, (int)(dc.mc != nullptr));
            }
        }
    }
}

void DrawProbeVb6Reader(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const d3d12_cl = da.d3d12_cl;
    const uint32_t index_count = da.index_count;
    const uint32_t instance_count = da.instance_count;
    const uint32_t first_index = da.first_index;
    const uint64_t vs_hash = da.vs_hash;
    const CmdRT& draw_rt = da.draw_rt;
    if (g_debug) {
        PROF(PROF_DI_Vb6Reader);
        ID3D12Resource* r6 = nullptr;
        uint32_t s6 = 0;
        uint64_t va0 = 0;
        const CmdRT& rt6 = draw_rt;
        {
            const auto& cg = g_command_lists.Gfx(cmd_list);
            r6 = cg.vb[6].res;
            s6 = cg.vb[6].stride;
            va0 = cg.vb[0].va;
        }
        RelocBuf rb6{};
        if (r6 && s6 && g_buffer_reloc.Resolve(r6, &rb6)) {
            if (dc.mc)
                g_draw_stats.vb6_readers_claimed.fetch_add(1, std::memory_order_relaxed);
            else {
                g_draw_stats.vb6_readers_unclaimed.fetch_add(1, std::memory_order_relaxed);
                RemovalPin::Mark(d3d12_cl, "game: unclaimed draw reading a relocated morph buffer"); // DRED context
                const int key =
                    (int)(((vs_hash ^ ((uint64_t)first_index * 0x9E3779B97F4A7C15ull) ^ (uint64_t)index_count) >> 1) &
                        0x7fffffff);
                if (DiagOnce((const void*)&g_draw_stats.vb6_readers_unclaimed, key)) {
                    const unsigned ob = g_buffer_reloc.IdentityBytes(rb6.res, 0);
                    const bool in_pool = g_recognition.InSkinBand(va0);
                    logger::write("UNCLAIMED-VB6-READER vs=%016llx first=%u count=%u inst=%u rtv=%u dsv=%d "
                                  "vb0=%llx inPool=%d slot6=%p (%u B vanilla, widened to %llu B): a GAME draw "
                                  "reading the vanilla morph buffer -- vanilla geometry, drawn as-is",
                        (unsigned long long)vs_hash, first_index, index_count, instance_count, rt6.rtv_count,
                        (int)rt6.has_dsv, (unsigned long long)va0, (int)in_pool, (void*)r6, ob,
                        (unsigned long long)rb6.bytes);
                }
            }
        }
    }
}

void DrawProbeOffLeak(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    if (!g_debug || dc.mc || !g_tex_place.OffWatching() || !ProbeArmed()) return;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const auto& root = g_command_lists.Gfx(cmd_list).root;
    uint32_t scan_len[64];
    g_layouts.TexScanLens(g_command_lists.GfxLayout(cmd_list), scan_len);
    for (int p = 0; p < 64; ++p) {
        if (root[p].table.handle == 0) continue;
        const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
        if (!ts) continue;
        const uint32_t num = ts.Heap()->GetDesc().NumDescriptors;
        for (uint32_t s = 0; s < scan_len[p] && ts.base + s < num; ++s) {
            ResInfo ri;
            if (!g_descriptors.Find({ ts.heap, ts.base + s }, ri) || !ri.res || !ri.swapped) continue;
            const uint32_t rule = g_tex_place.OffRuleOf(ri.res);
            if (!rule) continue;
            g_tex_stats.off_leaks.fetch_add(1, std::memory_order_relaxed);
            uint64_t ps = 0;
            cmd_list->get_private_data(s_iid_ps_hash, &ps);
            const uint64_t k = da.vs_hash ^ (ps * 0x9E3779B97F4A7C15ull) ^ rule;
            if (DiagOnce("OFFLEAK", (int)((k ^ (k >> 32)) & 0x7fffffff)))
                logger::write(
                    "OFFLEAK f=%u vs=%016llx ps=%016llx first=%u count=%u rtv=%u p%d t%u: rule %08x res=%p "
                    "still stamped with %p (slot written %u frame(s) ago, src %u) -- a draw we do not hand back "
                    "reads the switched-off mod's texture",
                    g_frame_id, (unsigned long long)da.vs_hash, (unsigned long long)ps, da.first_index, da.index_count,
                    da.draw_rt.rtv_count, p, s, rule, (void*)ri.res, (void*)ri.swapped, g_frame_id - ri.wrote_frame,
                    (unsigned)ri.wrote_src);
        }
    }
}

void DrawProbeUnstamp(const ResInfo& ri, bool restored)
{
    if (!g_debug || !g_tex_place.OffWatching()) return;
    const uint32_t rule = g_tex_place.OffRuleOf(ri.res);
    if (!rule) return;
    if (!restored) {
        g_tex_stats.off_kept.fetch_add(1, std::memory_order_relaxed);
        if (DiagOnce("UNSTAMP-REFUSED", (int)rule))
            logger::write("UNSTAMP-REFUSED f=%u rule %08x res=%p: the hand-back has no CPU-only source for the game's "
                          "descriptor (view %llx) -- the switched-off mod's texture %p stays",
                g_frame_id, rule, (void*)ri.res, (unsigned long long)ri.view_handle, (void*)ri.swapped);
        return;
    }
    HeapKey src{};
    ResInfo sri{};
    if (!g_descriptors.SlotOfCpu(ri.view_handle, &src) || !g_descriptors.Find(src, sri) || !sri.swapped) return;
    g_tex_stats.off_kept.fetch_add(1, std::memory_order_relaxed);
    if (DiagOnce("UNSTAMP-NOOP", (int)rule))
        logger::write("UNSTAMP-NOOP f=%u rule %08x res=%p: the hand-back copied the game's descriptor from heap %llx "
                      "slot %u, itself stamped with %p -- the switched-off mod's texture stays",
            g_frame_id, rule, (void*)ri.res, (unsigned long long)src.heap, src.offset, (void*)sri.swapped);
}
