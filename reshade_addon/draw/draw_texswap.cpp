// draw_texswap.cpp -- mod textures into the game's descriptor tables, by texture hash on a claimed draw.
// See draw_parts.h.
#include "../addon_common.h"
#include "../mods/character_recognition.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/stamp_ledger.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../hooks/pipeline_registry.h"
#include "draw_context.h"
#include "draw_parts.h"
#include "read_slots.h"
#include "../instrumentation/tex_timing.h"
#include <bitset>
#include <optional>
#include "../textures/tex_toggles.h"
#include "../textures/tex_rules.h"
#include "../textures/tex_select.h"
#include "../textures/tex_identity.h"
#include "../textures/tex_place.h"
#include "../textures/tex_slot_log.h"
#include "../textures/mod_texture_store.h"
#include "../migoto/port_api.h"
#include "../instrumentation/diag_arms.h"
#include "../addon/addon.h"
#include "../mods/mod_set.h"

using namespace reshade::api;

namespace {
void TableReadSlots(reshade::api::command_list* cmd_list, ReadSlots& rs)
{
    for (bool& k : rs.known) k = false;
    if (!g_tex_toggles.read_slots) return;
    const uint64_t pso = g_command_lists.Gfx(cmd_list).gfx_pso;
    uint64_t vsh = 0;
    uint64_t psh = 0;
    uint64_t gsh = 0;
    g_pipelines.BoundHashes(pso, &vsh, &psh, &gsh);
    if (gsh) return;
    const DxilSrvRegs pr = g_pipelines.SrvRegs(pso, 'p');
    const DxilSrvRegs vr = g_pipelines.SrvRegs(pso, 'v');
    if (!pr.parsed || pr.unbounded || !vr.parsed || vr.unbounded) return;
    const uint64_t layh = g_command_lists.GfxLayout(cmd_list);
    uint32_t declared[64] = {};
    uint32_t covered[64] = {};
    const bool tabled = g_layouts.WithTables(layh, [&](const std::vector<LayoutTableParam>& tabs) {
        for (const auto& t : tabs)
            if (t.param < 64 && t.is_srv && t.count) declared[t.param] = t.count;
    });
    if (!tabled) return;
    for (int p = 0; p < 64; ++p) {
        rs.len[p] = 0;
        rs.want[p].reset();
    }
    const bool mapped = g_layouts.WithRegMap(layh, [&](const LayoutRegMap& m) {
        for (unsigned r = 0; r < RegisterFileWidth(TargetClass::ShaderResource); ++r) {
            const RegBind& b = m.t_gfx[r];
            if (b.param >= 64 || !b.is_table) continue;
            ++covered[b.param];
            if (!pr.has(r) && !vr.has(r)) continue;
            if (b.offset >= kTexScanMax) continue;
            rs.want[b.param].set(b.offset);
            if (b.offset + 1 > rs.len[b.param]) rs.len[b.param] = b.offset + 1;
        }
    });
    if (!mapped) return;
    for (int p = 0; p < 64; ++p) rs.known[p] = declared[p] && covered[p] == declared[p];
}
} // namespace

const ReadSlots& DrawReadSlots(reshade::api::command_list* cmd_list)
{
    struct Memo {
        uint64_t pso = 0;
        uint64_t layout = 0;
        uint32_t frame = 0;
        bool on = false;
        ReadSlots rs;
    };
    thread_local Memo t_memo[4];
    thread_local unsigned t_next = 0;
    const uint64_t pso = g_command_lists.Gfx(cmd_list).gfx_pso;
    const uint64_t layout = g_command_lists.GfxLayout(cmd_list);
    for (Memo& m : t_memo)
        if (m.frame == g_frame_id && m.pso == pso && m.layout == layout && m.on == g_tex_toggles.read_slots)
            return m.rs;
    Memo& m = t_memo[t_next++ & 3];
    TableReadSlots(cmd_list, m.rs);
    m.pso = pso;
    m.layout = layout;
    m.frame = g_frame_id;
    m.on = g_tex_toggles.read_slots;
    return m.rs;
}

// A mod a claimed draw runs: detected (`$object_detected`) and its textures wanted now.
static void NoteClaimedMod(int mod_id)
{
    if (mod_id < 0 || mod_id >= (int)Mods().Count()) return;
    const GeoMod& gm = Mods().At(mod_id);
    if (gm.wcl_ns.empty()) return;
    PROF(PROF_TH_Detect);
    const unsigned gen = Mods().ParseGen();
    if (gm.det_slot && !PortApi::VarSlotHolds(gm.det_slot, gen, 1.f)) {
        std::optional<PortApi::EngineScope> det_scope;
        {
            PROF(PROF_TH_DetLock);
            det_scope.emplace();
        }
        PortApi::SetVarSlot(gm.det_slot, gen, 1.f);
    }
    TexTiming::NoteClaimed(gm.wcl_ns.c_str());
}

void StampClaimedTextures(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t index_count = da.index_count;
    const uint32_t first_index = da.first_index;
    const uint64_t vs_hash = da.vs_hash;
    if (dc.mc && g_tex_rules.Ready() && !g_tex_toggles.disable) {
        PROF(PROF_DI_TexHash);
        const wchar_t* own_ns = nullptr;
        uint64_t own_ns_hash = 0;
        if (dc.mc->mod_id >= 0 && dc.mc->mod_id < (int)Mods().Count() && !Mods().At(dc.mc->mod_id).wcl_ns.empty()) {
            own_ns = Mods().At(dc.mc->mod_id).wcl_ns.c_str();
            own_ns_hash = Mods().At(dc.mc->mod_id).ns_hash;
        }
        NoteClaimedMod(dc.mc->mod_id);
        for (const int s : dc.mc->with) NoteClaimedMod(Comps().At(s).mod_id);
        ProfScope _th_prep(PROF_TH_Prep);
        auto* dev = reinterpret_cast<ID3D12Device*>(cmd_list->get_device()->get_native());
        uint32_t inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const auto& root = g_command_lists.Gfx(cmd_list).root;
        if (g_debug) {
            uint64_t lkey = vs_hash ^ ((uint64_t)index_count << 1) ^ ((uint64_t)dc.mc->comp_idx << 40);
            if (DiagOnce("DRAW-LKEY", (int)(lkey ^ (lkey >> 32)))) {
                uint64_t psh = 0;
                cmd_list->get_private_data(s_iid_ps_hash, &psh);
                for (int p = 0; p < 64; ++p) {
                    const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
                    if (!ts) continue;
                    D3D12_DESCRIPTOR_HEAP_DESC hd = ts.Heap()->GetDesc();
                    logger::trace(
                        "TBLLAYOUT comp=%d vs=%016llx ps=%016llx count=%u first=%u | p=%d heap=%llx type=%d num=%u base=%u",
                        dc.mc->comp_idx, vs_hash, psh, index_count, first_index, p, (unsigned long long)ts.heap,
                        (int)hd.Type, hd.NumDescriptors, ts.base);
                }
            }
        }
        uint32_t scan_len[64];
        g_layouts.TexScanLens(g_command_lists.GfxLayout(cmd_list), scan_len);
        const ReadSlots& rs = DrawReadSlots(cmd_list);
        _th_prep.End();
        int pr_res = 0;
        int pr_hash = 0;
        int pr_match = 0;
        uint32_t pr_h0 = 0; // TEXPROBE
        uint32_t pr_w = 0;
        uint32_t pr_hh = 0;
        uint32_t pr_fmt = 0;
        int texflip_ord = -1;
        uint64_t texflip_ps = 0; // TEXFLIP: this draw's identity, resolved on its first slot
        for (int p = 0; p < 64; ++p) {
            if (root[p].table.handle == 0) continue;
            if (rs.known[p] && !rs.len[p]) continue;
            ProfScope _th_walk(PROF_TH_Walk);
            const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
            if (!ts) continue;
            auto* heap = ts.Heap();
            const uint32_t base = ts.base;
            uint32_t num = heap->GetDesc().NumDescriptors;
            ID3D12Resource* local_res[kTexScanMax];
            uint32_t local_hash[kTexScanMax];
            const uint32_t local_max_s = g_tex_select.ResolveBound(ts.heap, base, num, local_res, local_hash,
                rs.known[p] ? rs.len[p] : scan_len[p], rs.known[p] ? &rs.want[p] : nullptr);
            uint32_t far_hit = 0;
            _th_walk.End();

            PROF(PROF_TH_Select); // per table, not per slot: a per-slot scope would cost more than the work
            for (uint32_t s = 0; s < local_max_s; ++s) {
                uint32_t slot = base + s;
                ID3D12Resource* res = local_res[s];
                if (!res) continue;
                if (rs.known[p] && !rs.want[p].test(s)) continue;
                const int sel = local_hash[s] ? g_tex_select.SelectIdxNs(local_hash[s], own_ns, own_ns_hash, 0) : -1;
                if (g_diag_arms.texflip_arm.load(std::memory_order_relaxed) > 0) {
                    struct Seen {
                        ID3D12Resource* res;
                        ID3D12Resource* sw;
                        uint32_t hash;
                        uint32_t frame;
                        unsigned logged;
                        int sel;
                    };
                    static std::unordered_map<uint64_t, Seen> s_seen;
                    static std::mutex s_mtx;
                    static unsigned s_flips = 0;
                    static unsigned s_gen = 0;
                    static std::unordered_map<uint64_t, std::pair<uint32_t, int>>
                        s_ord; // (mod, comp, ps) -> (frame, draws so far)
                    ResInfo fri{};
                    g_descriptors.Find({ ts.heap, slot }, fri);
                    std::lock_guard lf(s_mtx);
                    if (const unsigned gen = g_diag_arms.texflip_gen.load(); gen != s_gen) {
                        s_gen = gen;
                        s_seen.clear();
                        s_ord.clear();
                        s_flips = 0;
                    }
                    if (texflip_ord < 0) {
                        cmd_list->get_private_data(s_iid_ps_hash, &texflip_ps);
                        const uint64_t dkey = ((uint64_t)(uint32_t)dc.mc->mod_id << 56) |
                            ((uint64_t)(uint32_t)dc.mc->comp_idx << 48) | (texflip_ps & 0xFFFFFFFFFFFFull);
                        auto& o = s_ord[dkey];
                        if (o.first != g_frame_id) {
                            o.first = g_frame_id;
                            o.second = 0;
                        }
                        texflip_ord = o.second++;
                    }
                    const uint64_t key = ((uint64_t)(uint32_t)dc.mc->mod_id << 56) |
                        ((uint64_t)(uint32_t)dc.mc->comp_idx << 48) |
                        ((texflip_ps ^ (texflip_ps >> 24)) & 0xFFFFFFull) << 24 |
                        ((uint64_t)(texflip_ord & 0xFF) << 16) | ((uint64_t)p << 8) | s;
                    auto it = s_seen.find(key);
                    unsigned logged = 0;
                    if (it != s_seen.end()) {
                        logged = it->second.logged;
                        const bool hash_flip = it->second.hash && local_hash[s] && it->second.hash != local_hash[s];
                        const bool stamp_flip = (it->second.sw != nullptr) != (fri.swapped != nullptr);
                        const bool sel_flip = it->second.sel != sel;
                        if (g_frame_id > it->second.frame && g_frame_id - it->second.frame <= 2 &&
                            (hash_flip || stamp_flip || sel_flip)) {
                            if (++s_flips <= 256 && ++logged <= 8)
                                logger::write(
                                    "TEXFLIP mod=%d comp=%d ps=%016llx #%d idx=%u+%u p=%d t%u heap=%p slot=%u: f%u res=%p hash=%08x %s%p sel=%d -> f%u res=%p hash=%08x %s%p sel=%d (inplace=%d, flip #%u)",
                                    dc.mc->mod_id, dc.mc->comp_idx, (unsigned long long)texflip_ps, texflip_ord,
                                    first_index, index_count, p, s, (void*)ts.Heap(), slot, it->second.frame,
                                    (void*)it->second.res, it->second.hash, it->second.sw ? "STAMPED modres=" : "game ",
                                    (void*)it->second.sw, it->second.sel, g_frame_id, (void*)res, local_hash[s],
                                    fri.swapped ? "STAMPED modres=" : "game ", (void*)fri.swapped, sel,
                                    (int)g_tex_toggles.inplace_stamp, s_flips);
                            else if (s_flips == 257)
                                logger::write("TEXFLIP capped at 256");
                        }
                    }
                    s_seen[key] = Seen{ res, fri.swapped, local_hash[s], g_frame_id, logged, sel };
                }
                D3D12_CPU_DESCRIPTOR_HANDLE rep = { 0 };
                ID3D12Resource* rep_res = nullptr; // the resource behind `rep`, for the slot's stamp record
                uint32_t hash = local_hash[s];
                ModTextureStore::Entry sel_entry;
                const bool direct =
                    hash && g_mod_textures.EntryAt(sel, &sel_entry); // `sel` above: one selection per slot
                if (direct) {
                    rep = sel_entry.srv;
                    rep_res = sel_entry.res;
                }
                uint32_t swap_rule = direct ? hash : 0;
                // pr_* feed only the g_debug TEXPROBE line.
                ++pr_res;
                if (hash) {
                    ++pr_hash;
                    if (g_debug && !pr_h0) {
                        pr_h0 = hash;
                        TexInfo di{};
                        if (g_tex_identity.DimsOf(reinterpret_cast<uint64_t>(res), &di)) {
                            pr_w = di.width;
                            pr_hh = di.height;
                            pr_fmt = di.format;
                        }
                    }
                }
                if (rep.ptr) ++pr_match;
                if (g_debug) {
                    uint32_t tw = 0;
                    uint32_t th = 0;
                    uint32_t tf = 0;
                    {
                        TexInfo di{};
                        if (g_tex_identity.DimsOf(reinterpret_cast<uint64_t>(res), &di)) {
                            tw = di.width;
                            th = di.height;
                            tf = di.format;
                        }
                    }
                    bool bc = (tf >= 70 && tf <= 99);
                    if (bc && (uint64_t)tw * th >= 256ull * 256ull) {
                        const bool byptr = g_tex_identity.HasModSrv(reinterpret_cast<uint64_t>(res));
                        if (ProbeArmed() &&
                            DiagOnce("MIDTEX",
                                (int)(reinterpret_cast<uint64_t>(res) ^ (reinterpret_cast<uint64_t>(res) >> 32))))
                            logger::trace("MIDTEX comp=%d res=%llx %ux%u fmt=%u hashable=%d byptr=%d count=%u",
                                dc.mc->comp_idx, reinterpret_cast<uint64_t>(res), tw, th, tf, (int)(hash != 0),
                                (int)byptr, index_count);
                    }
                }
                if (rep.ptr == 0) {
                    const uint32_t tgt = g_tex_select.LodTargetOf(hash);
                    ModTextureStore::Entry alias_entry;
                    if (tgt &&
                        g_mod_textures.EntryAt(
                            g_tex_select.SelectIdxNs(tgt, own_ns, own_ns_hash, g_tex_select.DrawnMip(hash, tgt)), &alias_entry)) {
                        rep = alias_entry.srv;
                        rep_res = alias_entry.res;
                        swap_rule = tgt;
                    }
                    if (rep.ptr == 0) continue;
                }
                if (s >= kTexScanMin) far_hit = s;
                if (g_debug) g_tex_identity.NoteModSrv(reinterpret_cast<uint64_t>(res), rep.ptr);

                if (g_tex_toggles.inplace_stamp) {
                    D3D12_CPU_DESCRIPTOR_HANDLE dst = { heap->GetCPUDescriptorHandleForHeapStart().ptr +
                        (SIZE_T)slot * inc };
                    dev->CopyDescriptorsSimple(1, dst, rep, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
                    g_descriptors.SetSwapped({ ts.heap, slot }, rep_res); // the frame dump publishes what the GPU reads
                    g_stamps.StampedStoreTexture(ts.heap, slot, rep_res);
                    ++g_tex_stats.inject_count;
                    TexTiming::NoteSwapped(rep_res, swap_rule);
                    if (g_debug) g_tex_slot_log.Note("draw", res, hash, swap_rule, rep_res, false);
#ifdef WWMI_DEBUG
                    if (g_tex_trace.On(swap_rule))
                        g_tex_trace.Line("claimed draw mod=%d comp=%d res=%p heap=%llx slot=%u: stamped %ls",
                            dc.mc->mod_id, dc.mc->comp_idx, (void*)res, (unsigned long long)ts.heap, slot,
                            g_tex_trace.ModText(rep_res).c_str());
#endif
                }
            }
            if (g_debug && far_hit &&
                DiagChanged("TEXSCANFAR", CompKey(dc.mc->mod_id, dc.mc->comp_idx) ^ (p << 20), far_hit))
                logger::write(
                    "TEXSCANFAR mod=%d comp=%d p=%d: a mod texture matched at t%u, past the old 128-slot window (scan %u)",
                    dc.mc->mod_id, dc.mc->comp_idx, p, far_hit, scan_len[p]);
        }
        // TEXPROBE (one-shot per comp): why textures don't swap -- res-bearing vs hashed vs target-matched.
        if (g_debug && DiagOnce("TEXPROBE", CompKey(dc.mc->mod_id, dc.mc->comp_idx))) {
            logger::write(
                "TEXPROBE mod=%d comp=%d slotsWithRes=%d slotsWithHash=%d targetMatches=%d hSample=%08x (%ux%u fmt=%u) targets=%zu",
                dc.mc->mod_id, dc.mc->comp_idx, pr_res, pr_hash, pr_match, pr_h0, pr_w, pr_hh, pr_fmt,
                g_tex_rules.Now().targets.size());
        }
    }
}

unsigned UnstampClaimedTextures(const DrawArgs& da)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const auto& root = g_command_lists.Gfx(cmd_list).root;
    uint32_t scan_len[64];
    g_layouts.TexScanLens(g_command_lists.GfxLayout(cmd_list), scan_len);
    unsigned restored = 0;
    for (int p = 0; p < 64; ++p) {
        if (root[p].table.handle == 0) continue;
        const TableStart ts = TableStartOf(cmd_list->get_device(), root[p].table);
        if (!ts) continue;
        const uint32_t num = ts.Heap()->GetDesc().NumDescriptors;
        for (uint32_t s = 0; s < scan_len[p] && ts.base + s < num; ++s) {
            ResInfo ri;
            if (!g_descriptors.Find({ ts.heap, ts.base + s }, ri) || !ri.res || !ri.swapped) continue;
            const bool ok = g_stamps.UnstampSlot(ts.heap, ts.base + s);
            DrawProbeUnstamp(ri, ok);
            if (!ok) continue;
            ++restored;
            g_tex_place.HoldForRetry(reinterpret_cast<uint64_t>(ri.res));
        }
    }
    return restored;
}
