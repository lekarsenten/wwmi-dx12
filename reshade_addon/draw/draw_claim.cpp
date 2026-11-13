// draw_claim.cpp -- which mod component a game draw is: the [ShaderOverride*] dispatch by shader hash, the
// mesh-hash probe and suppress, the claim, and the LOD pin. See draw_parts.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/character_recognition.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../hooks/pipeline_registry.h"
#include "../rt/rt_rings.h"
#include "../mods/shader_alias.h" // RunShaderOverrides, t_cur_vs_hash
#include "draw_context.h"
#include "draw_parts.h"
#include <cstdio>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "../addon/settings.h"
#include "../dispatch/dispatch_toggles.h"
#include "draw_toggles.h"
#include "../mods/port_section.h"
#include "../shapekeys/sk_toggles.h"
#include "../mods/ib_hash.h"
#include "../rabbitfx/decensor_rest.h"
#include "../addon/addon.h"
#include "../mods/mod_set.h"
#include "../migoto/dx12_context_impl.h"
#ifdef WWMI_DEBUG
#include "../instrumentation/opacity_watch.h"
#endif

using namespace reshade::api;

bool RunDrawShaderOverrides(const DrawArgs& da)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t index_count = da.index_count;
    const uint32_t instance_count = da.instance_count;
    const uint32_t first_index = da.first_index;
    const int32_t vertex_offset = da.vertex_offset;
    const uint32_t first_instance = da.first_instance;
    const uint64_t vs_hash = da.vs_hash;
    if (g_dispatch_toggles.shader_override_dispatch) {
        PortApi::DrawInfo ddi;
        ddi.index_count = index_count;
        ddi.first_index = first_index;
        ddi.first_vertex = (unsigned)vertex_offset;
        ddi.instance_count = instance_count;
        ddi.first_instance = first_instance;
        ddi.indexed = true;
        uint64_t draw_ps_h = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &draw_ps_h);
        t_cur_vs_hash = vs_hash;
        t_cur_ps_hash = draw_ps_h; // for the bare ps/vs filter operand
        uint64_t draw_gs_h = 0;
        cmd_list->get_private_data(s_iid_gs_hash, &draw_gs_h);
        const uint64_t vs_ini = AliasShaderHash(vs_hash);
        const uint64_t ps_ini = AliasShaderHash(draw_ps_h);
        const uint64_t gs_ini = AliasShaderHash(draw_gs_h);
        std::optional<PortApi::EngineScope> published;
        std::optional<PortApi::DrawListScope> on_list;
        uint32_t srv_gen0 = 0;
        if (g_sk_toggles.publish_shader_state &&
            (PortApi::ShaderOverrideMatches(vs_ini) || PortApi::ShaderOverrideMatches(ps_ini) ||
                PortApi::ShaderOverrideMatches(gs_ini))) {
            published.emplace();
            PortApi::SetBoundShader('v', vs_ini);
            PortApi::SetBoundShader('p', ps_ini);
            PortApi::SetBoundShader('g', gs_ini);
            if (g_draw_toggles.port_draw_list) {
                on_list.emplace(da.d3d12_cl, RestoreOverrideDraw, cmd_list);
                InheritGameBindings(*on_list, cmd_list);
                srv_gen0 = MigotoPort::SrvWriteGen();
                reshade::api::device* const dev = cmd_list->get_device();
                const CmdRT& rt = da.draw_rt;
                GameBackBuffer(*on_list, cmd_list, rt);
                on_list->Targets(rt.valid && rt.rtv_count
                        ? (DXGI_FORMAT)dev->get_resource_view_desc({ rt.rtv[0] }).format
                        : DXGI_FORMAT_UNKNOWN,
                    rt.valid && rt.has_dsv ? (DXGI_FORMAT)dev->get_resource_view_desc({ rt.dsv }).format
                                           : DXGI_FORMAT_UNKNOWN);
            }
        }
        bool sk = false;
        if (vs_hash) sk |= RunShaderOverrides(vs_hash, &ddi, cmd_list);
        {
            const PortApi::LateStagePost late;
            if (draw_gs_h) sk |= RunShaderOverrides(draw_gs_h, &ddi, cmd_list);
            if (draw_ps_h) sk |= RunShaderOverrides(draw_ps_h, &ddi, cmd_list);
        }
        if (on_list) StampOverrideSrvs(cmd_list, srv_gen0);
        if (sk) {
            if (g_debug && DiagOnce("SHADEROVR-SKIP", (int)((vs_hash ^ draw_ps_h) ^ ((vs_hash ^ draw_ps_h) >> 32)))) {
                logger::write("SHADEROVR-SKIP vs=%016llx ps=%016llx (ini handling=skip)", vs_hash, draw_ps_h);
            }
            return true; // suppressed exactly as the ini asks
        }
    }
    return false;
}

static int PickSharedRangeSlot(command_list* cmd_list, uint32_t first_index, uint32_t index_count, int dflt)
{
    const std::vector<int>* sh = Comps().SharedSlots(first_index, index_count);
    if (!sh) return dflt;
    const std::vector<int>& cands = *sh;
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
    CharVbOrigin origin;
    if (vbva && g_recognition.CharMeshOrigin(vbva, &origin) && origin.ns_hash)
        for (int s : cands)
            if (Mods().At(Comps().At(s).mod_id).ns_hash == origin.ns_hash) return s;
    if (!ibr || !vbr || !vstride) return dflt;
    const uint32_t iboff = (uint32_t)(ibva - ibr->GetGPUVirtualAddress());
    const uint32_t vboff = (uint32_t)(vbva - vbr->GetGPUVirtualAddress());
    for (int s : cands) {
        const int md = Comps().At(s).mod_id;
        const WwmiMod::Component* wc = Mods().Comp(md, Comps().At(s).comp_idx);
        if (!wc || !wc->ib_hash) continue;
        const uint32_t max_end = Mods().IndexEnd(md, wc->ib_hash);
        if (max_end && IbHash::Lookup(ibr, iboff, max_end * idxb, idxb, vbr, vboff, vstride) == wc->ib_hash) {
            if (g_debug && DiagChanged("SHAREDRANGE", (int)(first_index ^ index_count), (unsigned long long)s))
                logger::write(
                    "SHAREDRANGE first=%u count=%u: %zu mods declare this range; the index buffer names mod %d comp %d",
                    first_index, index_count, cands.size(), md, Comps().At(s).comp_idx);
            return s;
        }
    }
    return dflt;
}

#ifdef WWMI_DEBUG
static const char* HeapName(ID3D12Resource* r)
{
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS hf = D3D12_HEAP_FLAG_NONE;
    if (!r) return "none";
    if (FAILED(r->GetHeapProperties(&hp, &hf))) return "unknown";
    return hp.Type == D3D12_HEAP_TYPE_DEFAULT ? "default" : hp.Type == D3D12_HEAP_TYPE_UPLOAD ? "upload" : "other";
}

struct SmallMeshSighting {
    uint64_t vs = 0;
    uint32_t first = 0;
    uint32_t count = 0;
    uint32_t instances = 0;
    int32_t base_vertex = 0;
    uint32_t stride = 0;
    uint32_t view = 0;
    const char* heap = "";
    const char* outcome = "";
    uint32_t hash = 0;
    uint32_t frame = 0;
};
static std::mutex g_small_seen_mtx;
static std::unordered_map<uint64_t, SmallMeshSighting> g_small_seen; // by vb0 VA

static void SmallMeshSeen(const DrawArgs& da, ID3D12Resource* vbr, uint64_t vbva, uint32_t stride, uint32_t vsize,
    const char* outcome, uint32_t hash)
{
    thread_local std::unordered_set<uint64_t> t_seen;
    if (!t_seen.insert(vbva ^ ((uint64_t)hash << 32) ^ (uint64_t)(uintptr_t)outcome).second) return;
    SmallMeshSighting s;
    s.vs = da.vs_hash;
    s.first = da.first_index;
    s.count = da.index_count;
    s.instances = da.instance_count;
    s.base_vertex = da.vertex_offset;
    s.stride = stride;
    s.view = vsize;
    s.heap = HeapName(vbr);
    s.outcome = outcome;
    s.hash = hash;
    s.frame = g_frame_id;
    {
        std::lock_guard<std::mutex> l(g_small_seen_mtx);
        g_small_seen[vbva] = s;
    }
    const uint64_t k = ((uint64_t)da.index_count << 32) ^ ((uint64_t)stride << 16) ^ (uint64_t)(uintptr_t)outcome;
    if (DiagOnce("SMALLMESH-SEEN", (int)(k ^ (k >> 32))))
        logger::write("SMALLMESH-SEEN vs=%016llx first=%u count=%u instances=%u base_vertex=%d vb0=%llx stride=%u "
                      "view=%u B heap=%s: %s %08x",
            (unsigned long long)da.vs_hash, da.first_index, da.index_count, da.instance_count, da.vertex_offset,
            (unsigned long long)vbva, stride, vsize, s.heap, outcome, hash);
}

std::string SmallMeshReport(uint32_t count)
{
    std::vector<std::pair<uint64_t, SmallMeshSighting>> rows;
    {
        std::lock_guard<std::mutex> l(g_small_seen_mtx);
        for (const auto& kv : g_small_seen)
            if (!count || kv.second.count == count) rows.push_back(kv);
    }
    std::string out;
    char line[320];
    for (const auto& [va, s] : rows) {
        std::snprintf(line, sizeof line,
            "vb0=%llx vs=%016llx first=%u count=%u instances=%u base_vertex=%d stride=%u view=%u heap=%s f=%u: %s %08x\n",
            (unsigned long long)va, (unsigned long long)s.vs, s.first, s.count, s.instances, s.base_vertex, s.stride,
            s.view, s.heap, s.frame, s.outcome, s.hash);
        out += line;
    }
    return out + std::to_string(rows.size()) + " small draw(s) seen" +
        (count ? " with " + std::to_string(count) + " indices" : std::string());
}
#endif

bool RunSmallMeshOverrides(const DrawArgs& da)
{
    const bool menu_shape = g_settings.decensor && da.index_count == kMenuBackdropIndices;
    if (da.index_count >= 256 || (!menu_shape && !g_recognition.SmallMeshAny())) return false;
    ID3D12Resource* ibr = nullptr;
    ID3D12Resource* vbr = nullptr;
    uint64_t ibva = 0;
    uint64_t vbva = 0;
    uint32_t idxb = 2;
    uint32_t vstride = 0;
    uint32_t vsize = 0;
    {
        const auto& st = g_command_lists.Gfx(da.cmd_list);
        ibr = st.ib.res;
        ibva = st.ib.va;
        if (st.ib.index_bytes) idxb = st.ib.index_bytes;
        vbr = st.vb[0].res;
        vbva = st.vb[0].va;
        vstride = st.vb[0].stride;
        vsize = st.vb[0].size;
    }
    const int64_t vb_off =
        !vbr ? -1 : (int64_t)(vbva - vbr->GetGPUVirtualAddress()) + (int64_t)da.vertex_offset * vstride;
    const char* const refused = !ibr || !vbr || !vstride ? "no buffers"
        : vb_off < 0                                     ? "base vertex before the buffer"
                                                         : nullptr;
#ifdef WWMI_DEBUG
    if (refused) SmallMeshSeen(da, vbr, vbva, vstride, vsize, refused, 0);
#endif
    if (refused) return false;
    const uint32_t ib_off = (uint32_t)(ibva - ibr->GetGPUVirtualAddress()) + da.first_index * idxb;
    const uint32_t got = IbHash::LookupSmall(ibr, ib_off, da.index_count * idxb, idxb, vbr, (uint32_t)vb_off, vstride);
#ifdef WWMI_DEBUG
    SmallMeshSeen(da, vbr, vbva, vstride, vsize, got ? "hash" : "pending or not hashed", got);
#endif
    if (!got) return false;
    if (got == kMenuBackdropMesh) {
        g_decensor_rest.MenuDrawn();
        if (g_debug && DiagOnce("DECENSOR-REST", 0))
            logger::write("DECENSOR rests while the game draws its character menu (backdrop mesh %08x)", got);
    }
    thread_local uint32_t t_gen = 0;
    thread_local std::shared_ptr<const std::unordered_set<uint32_t>> t_hashes;
    if (g_recognition.SmallMeshGen() != t_gen) t_hashes = g_recognition.SmallMeshHashes(&t_gen);
    if (!t_hashes || !t_hashes->count(got)) return false;
    PortApi::DrawInfo di;
    di.first_index = da.first_index;
    di.index_count = da.index_count;
    di.instance_count = da.instance_count;
    const int ran = PortApi::DispatchTextureOverride(got, &di);
    if (DiagOnce("SMALLMESH-RUN", (int)got))
        logger::write("SMALLMESH-RUN %08x: vs=%016llx first=%u count=%u instances=%u stride=%u -- %d section(s) run%s",
            got, (unsigned long long)da.vs_hash, da.first_index, da.index_count, da.instance_count, vstride, ran,
            di.skip ? ", the draw dropped" : "");
    return di.skip;
}

std::optional<bool> ClaimDraw(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    ID3D12GraphicsCommandList* const d3d12_cl = da.d3d12_cl;
    const uint32_t index_count = da.index_count;
    const uint32_t first_index = da.first_index;
    const uint64_t vs_hash = da.vs_hash;
    const CmdRT& draw_rt = da.draw_rt;
    if (Comps().Loaded() && g_draw_toggles.merged_bone_cb) {
        int mc_slot = Comps().SlotOf(first_index, index_count);
        if (mc_slot >= 0) mc_slot = PickSharedRangeSlot(cmd_list, first_index, index_count, mc_slot);
        const bool skip_any = g_recognition.SkipVbHashAny();
        const bool model_any = g_recognition.ModelHashAny();
        const bool hash_it = g_debug || g_draw_toggles.ibhash_suppress || skip_any || model_any;
        if ((hash_it || g_settings.decensor) && mc_slot < 0 && index_count >= 256) {
            uint64_t bvb0 = 0;
            bool vb6 = false;
            {
                const auto& cg = g_command_lists.Gfx(cmd_list);
                bvb0 = cg.vb[0].va;
                vb6 = cg.vb[6].res != nullptr;
            }
            const bool skinned = g_recognition.InSkinBand(bvb0) ||
                vb6; // a character mesh: in the skin-cache band, or reading the character's stream 6
            if (g_debug && skip_any && !skinned && DiagOnce("SKIPONLY-BAND", (int)(first_index * 31u + index_count)))
                logger::write(
                    "SKIPONLY-BAND vs=%016llx first=%u count=%u vb0=%llx outside the skin band %llx..%llx, no vb6 -- not hashed",
                    (unsigned long long)vs_hash, first_index, index_count, (unsigned long long)bvb0,
                    (unsigned long long)g_recognition.SkinBandLo(), (unsigned long long)g_recognition.SkinBandHi());
            const uint64_t draw_pso = g_command_lists.Gfx(cmd_list).gfx_pso;
            if (g_settings.decensor && (g_recognition.InSkinBand(bvb0) || g_pipelines.ReadsStream6(draw_pso))) {
                g_pipelines.NoteCharacterDraw(draw_pso);
#ifdef WWMI_DEBUG
                if (g_debug) {
                    uint64_t skin_ps = 0;
                    cmd_list->get_private_data(s_iid_ps_hash, &skin_ps);
                    g_opacity_watch.NoteDraw(cmd_list, skin_ps, -1, (int)index_count);
                }
#endif
            }
            if (hash_it && skinned) {
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
                if (ibr && vbr && vstride) {
                    uint32_t iboff = (uint32_t)(ibva - ibr->GetGPUVirtualAddress());
                    uint32_t vboff = (uint32_t)(vbva - vbr->GetGPUVirtualAddress());
                    uint32_t ib_bytes =
                        (first_index + index_count) * idxb; // drawn extent (single-section LOD = full LOD IB)
                    // Lookup is a cached hash-map hit per (vb bind VA) after the first async compute -> per-draw cheap.
                    uint32_t got = IbHash::Lookup(ibr, iboff, ib_bytes, idxb, vbr, vboff, vstride, /*grow*/ true);
                    if (got && model_any) g_recognition.NoteModelDrawn(got);
                    std::shared_ptr<const std::unordered_set<uint32_t>> skip_vbhash;
                    if (got && skip_any) skip_vbhash = g_recognition.SkipVbHashes();
                    if (g_debug && skip_any &&
                        DiagOnce("SKIPONLY-SEEN", (int)((first_index * 31u + index_count) ^ (got ? 0x40000000u : 0u))))
                        logger::write(
                            "SKIPONLY-SEEN vs=%016llx first=%u count=%u vb0=%llx ibBytes=%u stride=%u -> hash %08x%s",
                            (unsigned long long)vs_hash, first_index, index_count, (unsigned long long)bvb0, ib_bytes,
                            vstride, got,
                            !got ? " (pending)" : (skip_vbhash && skip_vbhash->count(got) ? " IN the skip set" : ""));
                    if (skip_vbhash && skip_vbhash->count(got)) {
                        PortApi::DrawInfo di;
                        di.first_index = first_index;
                        di.index_count = index_count;
                        PortApi::DispatchTextureOverride(got, &di);
                        if (di.skip) {
                            g_recognition.NoteSkipVbHashDrop();
                            if (g_debug && DiagOnce("SKIPONLY-DROP", (int)((vs_hash ^ got) ^ ((vs_hash ^ got) >> 32))))
                                logger::write(
                                    "SKIPONLY-DROP vs=%016llx first=%u count=%u hash=%08x -- the game's draw is cancelled",
                                    (unsigned long long)vs_hash, first_index, index_count, got);
                            return true;
                        }
                    }
                    int match_mod = -1;
                    int match_comp = -1;
                    uint32_t match_hash = 0;
                    if (got)
                        if (const int hs = Comps().SlotOfIbHash(got); hs >= 0) {
                            match_mod = Comps().At(hs).mod_id;
                            match_comp = Comps().At(hs).comp_idx;
                            match_hash = got;
                        }
                    const bool never_claims = match_mod >= 0 && !g_recognition.IbHashEverClaimed(got);
                    if (g_draw_toggles.ibhash_suppress && match_mod >= 0 && never_claims) {
                        ++g_draw_stats.ibhash_suppress_count;
                        if (g_debug && ProbeArmed() &&
                            DiagOnce("IBHASH-SUPPRESS", (int)((vs_hash ^ got) ^ ((vs_hash ^ got) >> 32)))) {
                            logger::trace(
                                "IBHASH-SUPPRESS vs=%016llx first=%u count=%u hash=%08x mod=%d comp=%d (LOD proxy dropped)",
                                (unsigned long long)vs_hash, first_index, index_count, got, match_mod, match_comp);
                        }
                        return true;
                    }
                    if (g_debug) {
                        static std::atomic<int> npend{ 0 };
                        bool fresh;
                        if (got) {
                            const uint64_t k = vs_hash ^ ((uint64_t)got << 32);
                            fresh = ProbeArmed() && DiagOnce("IBHASH-PROBE", (int)(k ^ (k >> 32)));
                        } else
                            fresh = (npend.fetch_add(1, std::memory_order_relaxed) < 20);
                        if (fresh) {
                            const RootCbvBind b4cb = ctx.roots.Get().b4cb;
                            uint32_t skh = HashBoneCB(b4cb.res, b4cb.va);
                            const bool inskel = skh && g_recognition.InFrameSkel(skh);
                            logger::trace(
                                "IBHASH-PROBE vs=%016llx first=%u count=%u vb0=%llx ourVbHash=%08x inSkel=%d -> %s",
                                (unsigned long long)vs_hash, first_index, index_count, (unsigned long long)bvb0, got,
                                (int)inskel,
                                got ? (match_mod >= 0 ? (never_claims ? "MATCH never-claims (proxy!)"
                                                                      : "MATCH but hash also claims (main pass)")
                                                      : "nomatch")
                                    : "pending");
                            if (match_mod >= 0)
                                logger::write("   IBHASH-PROBE MATCH mod=%d comp=%d iniHash=%08x", match_mod,
                                    match_comp, match_hash);
                        }
                    }
                }
            }
        }
        if (mc_slot >= 0) {
            PROF(PROF_DI_ClaimInject); // per-claimed-draw claim bookkeeping
            int ci = Comps().At(mc_slot).comp_idx;
            int md = Comps().At(mc_slot).mod_id; // which geometry mod owns this draw
            RootCbvBind cur = {};
            RootCbvBind prev = {};
            uint64_t vb0 = 0;
            int b4_root = -1;
            int b3_root = -1;
            uint64_t all_vb_va[8] = {}; // every slot the GAME has bound at this draw -- see VBSLOTS
            uint32_t all_vb_sz[8] = {};
            uint32_t all_vb_str[8] = {};
            uint64_t all_vb_res_w[8] = {}; // the RESOURCE's own width -- see VBSLOTS
            {
                auto& st = g_command_lists.Gfx(cmd_list);
                vb0 = st.vb[0].va;
                for (unsigned q = 0; q < 8; ++q) {
                    all_vb_va[q] = st.vb[q].va;
                    all_vb_sz[q] = st.vb[q].size;
                    all_vb_str[q] = st.vb[q].stride;
                    all_vb_res_w[q] = st.vb[q].res_width;
                }
                const BoneRoots& br = ctx.roots.Get();
                b4_root = br.b4;
                b3_root = br.b3;
                cur = br.b4cb;
                prev = br.b3cb;
            }
            dc.b4_cb.res = cur.res;
            dc.b4_cb.va = cur.va;
            dc.b3_cb.res = prev.res;
            dc.b3_cb.va = prev.va;
            CharVbOrigin char_vb_org; // who learned this VA, and when
            bool char_vb = vb0 && g_recognition.CharMeshOrigin(vb0, &char_vb_org);
            if (char_vb) {
                g_draw_stats.charvb_claims.fetch_add(1, std::memory_order_relaxed);
                if (char_vb_org.ns_hash && char_vb_org.ns_hash != Mods().At(md).ns_hash) {
                    g_draw_stats.charvb_xmod.fetch_add(1, std::memory_order_relaxed);
                    if (g_debug && LogBudget("CHARVB-XMOD", L"", 12))
                        logger::write(
                            "CHARVB-XMOD: draw names mod %d comp %d (first=%u count=%u vs=%016llx) on vb0=%llx "
                            "learned from mod %d %u frames ago%s",
                            md, ci, first_index, index_count, vs_hash, (unsigned long long)vb0, Mods().IdOfNsHash(char_vb_org.ns_hash),
                            g_frame_id - char_vb_org.frame,
                            g_draw_toggles.charvb_refuse_xmod ? " -- REFUSED (H17)" : "");
                    if (g_draw_toggles.charvb_refuse_xmod) {
                        char_vb = false;
                        g_draw_stats.charvb_refused.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            }
            if (char_vb && logger::enabled(1)) {
                const bool fr = DiagOnce((const void*)&on_draw_indexed_body, md * 4096 + (int)(first_index % 4096));
                if (fr) {
                    char sl[512];
                    int n = 0;
                    for (unsigned q = 0; q < 8; ++q)
                        n += snprintf(sl + n, sizeof(sl) - (size_t)n, " vb%u=%llx/%u/%u res=%llu", q,
                            (unsigned long long)all_vb_va[q], all_vb_sz[q], all_vb_str[q],
                            (unsigned long long)all_vb_res_w[q]);
                    logger::write("VBSLOTS mod=%d first_index=%u (va/size/stride):%s", md, first_index, sl);
                }
            }
            if (!char_vb) char_vb = vb0 && g_recognition.IsCharLodVb0(vb0);
            bool char_ib = false;
            bool has_ib_ref = false;
            uint32_t ib_expect = 0; // RTCLAIM: the ini's hash= and what we computed for it
            uint32_t ib_got = 0;
            if (!char_vb) {
                const WwmiMod::Component* wc2 = Mods().Comp(md, ci);
                has_ib_ref = (wc2 && wc2->ib_hash);
                if (has_ib_ref) {
                    ib_expect = wc2->ib_hash;
                    const uint32_t max_end = Mods().IndexEnd(md, wc2->ib_hash);
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
                    if (ibr && vbr && max_end && vstride) {
                        uint32_t iboff = (uint32_t)(ibva - ibr->GetGPUVirtualAddress());
                        uint32_t vboff = (uint32_t)(vbva - vbr->GetGPUVirtualAddress());
                        ib_got = IbHash::Lookup(ibr, iboff, max_end * idxb, idxb, vbr, vboff, vstride);
                        char_ib = (ib_got == wc2->ib_hash);
                    }
                }
            }
            bool char_tex = false;
            if (!char_vb && !char_ib && !has_ib_ref) char_tex = g_recognition.DrawBindsModTexture(cmd_list);
            bool is_mod_char = char_ib || char_tex; // a definitive identification this draw
            bool char_ibset = false;
            if (g_draw_toggles.ib_anchor_claim) {
                uint64_t claim_ib_va = 0;
                uint64_t claim_ib_bytes = 0;
                {
                    const auto& gs = g_command_lists.Gfx(cmd_list);
                    claim_ib_va = gs.ib.va;
                    claim_ib_bytes = gs.ib.size;
                }
                char_ibset = g_recognition.IbAnchorNoteAndAsk(claim_ib_va, claim_ib_bytes, md, ci, is_mod_char);
                if (char_ibset && !is_mod_char && !char_vb)
                    g_draw_stats.ib_anchor_claims.fetch_add(1, std::memory_order_relaxed);
            }
            if (g_debug && !is_mod_char && !char_vb && !char_ibset && DiagOnce("RTCLAIM", CompKey(md, ci)))
                logger::write(
                    "RTCLAIM mod=%d comp=%d first=%u count=%u NOT IDENTIFIED: "
                    "char_vb=0 (vb0=%llx stride=%u inSkinPool=%d -- never learned) | char_ib=0 (ini hash=%08x computed=%08x%s) | "
                    "char_tex %s | vb6=%llx",
                    md, ci, first_index, index_count, (unsigned long long)vb0, all_vb_str[0],
                    (int)g_recognition.InSkinBand(vb0), ib_expect, ib_got,
                    !has_ib_ref ? ", no hash= in the ini" : (ib_got == 0 ? ", NOT COMPUTED YET" : ", MISMATCH"),
                    has_ib_ref ? "skipped (the mod carries hash=)" : "probed and missed",
                    (unsigned long long)all_vb_va[6]);
            if (is_mod_char) {
                const uint32_t sh = HashBoneCB(cur.res, cur.va);
                if (vb0) g_recognition.NoteCharMeshVb0(vb0, CharVbOrigin{ g_frame_id, Mods().At(md).ns_hash }); // by THIS mod, now
                if (sh) g_recognition.NoteFrameSkel(sh);
                if (g_debug && char_tex && DiagOnce("CHARTEX", ci)) {
                    logger::write("CHARTEX comp=%d claimed via texture-identity (vs=%016llx no rule) first=%u count=%u",
                        ci, vs_hash, first_index, index_count);
                }
            }
            if (is_mod_char || char_vb || char_ibset) {
                Mods().At(md).last_char_frame = g_frame_id;
                for (const int s : Comps().At(mc_slot).with)
                    Mods().At(Comps().At(s).mod_id).last_char_frame = g_frame_id;
            }
            if (g_debug && (is_mod_char || char_vb || char_ibset)) { // RTDBG: see RtDrawVbs::Note
                RtDrawVb v;
                v.frame = g_frame_id;
                v.va = all_vb_va[0];
                v.size = all_vb_sz[0];
                v.stride = all_vb_str[0];
                v.vs = vs_hash;
                g_rt_draw_vbs.Note(md, ci, v);
            }
            if (is_mod_char || char_vb || char_ibset) {
                const WwmiMod::Component* wcc = Mods().Comp(md, ci);
                if (wcc && wcc->ib_hash) g_recognition.NoteClaimedIbHash(wcc->ib_hash);
            }
            if ((is_mod_char || char_vb || char_ibset) && (g_draw_toggles.wcl_draw || Mods().GeomEnabled(md))) {
                dc.mc = &Comps().At(mc_slot);
                if (g_settings.decensor) g_pipelines.NoteCharacterDraw(g_command_lists.Gfx(cmd_list).gfx_pso);
#ifdef WWMI_DEBUG
                if (g_debug && g_settings.decensor) {
                    uint64_t claim_ps = 0;
                    cmd_list->get_private_data(s_iid_ps_hash, &claim_ps);
                    g_opacity_watch.NoteDraw(cmd_list, claim_ps, md, ci);
                }
#endif
                RemovalPin::Mark(d3d12_cl, "game: claimed character draw"); // DRED breadcrumb context
                dc.rep_vs = vs_hash;
                dc.mc_b4 = (cur.res && cur.va);
                dc.b4_va = cur.va; // CURRENT skeleton CBV (b4 root)
                dc.b3_va = prev.va; // PREVIOUS skeleton CBV (b3 root)
                if (g_debug) g_recognition.NoteCharBoneVa(cur.va, prev.va); // POOLDRAW's boneNear (draw_probes.cpp)
                dc.b4_root = b4_root;
                dc.b3_root = b3_root;
                {
                    const CmdRT& rtg = draw_rt;
                    if (g_debug) {
                        bool is_depth = (rtg.rtv_count == 0 && rtg.has_dsv);
                        bool in_pool = g_recognition.InSkinBand(vb0);
                        const uint64_t k = ((uint64_t)is_depth << 62) ^ ((uint64_t)index_count << 8) ^ (uint64_t)ci;
                        if (DiagOnce("SKINSRC", (int)(k ^ (k >> 32))))
                            logger::trace(
                                "SKINSRC pass=%s comp=%d idx=%u vb0=%llx inSkinPool=%d b4root=%d boneVA=%llx rtv=%u dsv=%d",
                                is_depth ? "DEPTH" : "GBUF", ci, index_count, (unsigned long long)vb0, (int)in_pool,
                                b4_root, (unsigned long long)cur.va, rtg.rtv_count, (int)rtg.has_dsv);
                    }
                }
            }
            if (dc.mc && !g_draw_toggles.shadow_via_claim) {
                const CmdRT& rt = draw_rt;
                if (rt.rtv_count == 0 && rt.has_dsv) {
                    dc.depth_claim = dc.mc;
                    dc.mc = nullptr;
                }
            }
        }
    }
    return std::nullopt;
}

void LodPinNote(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t index_count = da.index_count;
    const uint32_t first_index = da.first_index;
    const uint64_t vs_hash = da.vs_hash;
    if (g_draw_toggles.lod_pin && !dc.mc && index_count >= 256) {
        const uint64_t vb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
        ID3D12Resource* const b4res = ctx.roots.Get().b4cb.res;
        const uint64_t b4va = ctx.roots.Get().b4cb.va;
        uint32_t sh = HashBoneCB(b4res, b4va);
        bool linked_now = false;
        const bool recognized = g_recognition.RecogniseLod(sh, vb0, &linked_now);
        if (linked_now)
            logger::write("LODPIN linked char LOD buffer vb0=%llx (vs=%016llx first=%u count=%u)", vb0, vs_hash,
                first_index, index_count);
        if (recognized) {
            uint64_t key = vs_hash ^ ((uint64_t)first_index << 3) ^ ((uint64_t)index_count << 17);
            if (DiagOnce("LODPIN", (int)(key ^ (key >> 32)))) {
                uint64_t ps_hash = 0;
                cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
                logger::write("LODPIN char lower-LOD: vs=%016llx ps=%016llx first=%u count=%u vb0=%llx", vs_hash,
                    ps_hash, first_index, index_count, vb0);
            }
        }
    }
}
