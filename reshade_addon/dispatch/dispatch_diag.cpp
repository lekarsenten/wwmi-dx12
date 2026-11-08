// dispatch_diag.cpp -- the dispatch path's diagnostics (g_debug / armed): SHAPEKEYPAIR, SHAPEKEYU0 + U0SCAN,
// the MORPH* hunt, SKINPROBE, SKINBLUEPRINT and the overlay's skin-cache discovery list. See dispatch_parts.h.
#include "../addon_common.h"
#include "../mods/character_recognition.h"
#include "../mods/layout_registry.h"
#include "../hooks/resource_tracker.h"
#include "../rt/rt_rings.h"
#include "../rt/rt_diag_dispatch.h" // RTBONES / RTSKINREAD
#include "dispatch_parts.h"
#include <set>
#include "dispatch_toggles.h"
#include "../migoto/port_api.h"
#include "../mods/game_cb_reader.h"
#include "../instrumentation/uav_stalker.h"

using namespace reshade::api;

void DispatchDiagShapekey(const DispatchArgs& da)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t cs_hash = da.cs_hash;
    const uint32_t group_count_x = da.group_count_x;
    const uint32_t group_count_y = da.group_count_y;
    const uint32_t group_count_z = da.group_count_z;
    if (g_debug && g_dispatch_toggles.morph_cs_hash) {
        static std::atomic<uint64_t> s_prev{ 0 };
        const uint64_t prev = s_prev.exchange(cs_hash);
        if (prev == g_dispatch_toggles.morph_cs_hash && cs_hash != g_dispatch_toggles.morph_cs_hash) {
            static ProbeBudget s_once;
            if (s_once.take(1))
                logger::write("SHAPEKEYPAIR: dispatch AFTER the identified Loader %016llx is %016llx "
                              "groups=%u,%u,%u -- this successor is the ShapeKeyMultiplier candidate "
                              "(the dispatch hook tests %016llx; they must agree)",
                    (unsigned long long)g_dispatch_toggles.morph_cs_hash, (unsigned long long)cs_hash, group_count_x,
                    group_count_y, group_count_z, 0xc1a7f1fec51b482fULL);
        }
    }

    if (g_debug && g_dispatch_toggles.morph_cs_hash && cs_hash == g_dispatch_toggles.morph_cs_hash && ProbeArmed()) {
        static std::atomic<uint32_t> s_ep{ 0xFFFFFFFFu };
        const uint32_t ep = g_diag_epoch.load(std::memory_order_relaxed);
        const bool first = (s_ep.exchange(ep) != ep);
        if (first) {
            uint64_t clay = 0;
            clay = g_command_lists.CmpLayout(cmd_list);
            const int u0root = clay ? g_layouts.CsU0Root(clay) : -1;
            ID3D12Resource* u0res = nullptr;
            const char* via = "none";
            uint32_t u0bytes = 0; // cached size of the resolved u0 buffer (never GetDesc a cached ptr)
            if (u0root >= 0 && u0root < 64) {
                auto& st = g_command_lists.Gfx(cmd_list);
                if (st.root[u0root].desc_type == 3) {
                    u0res = st.root[u0root].res;
                    via = "root";
                }
            }
            if (!u0res && clay) {
                U0CsTable u0t{};
                const bool have = g_layouts.U0Table(clay, &u0t);
                if (have && u0t.param < 64) {
                    descriptor_table dt = {};
                    {
                        auto& st = g_command_lists.Gfx(cmd_list);
                        dt = st.root[u0t.param].compute_table();
                    }
                    if (dt.handle) {
                        descriptor_heap uheap = { 0 };
                        uint32_t uoff = 0;
                        cmd_list->get_device()->get_descriptor_heap_offset(dt, u0t.offset, 0, &uheap, &uoff);
                        if (uheap.handle) {
                            ResInfo ri;
                            if (g_descriptors.Find({ uheap.handle, uoff }, ri) && ri.res) {
                                u0res = reinterpret_cast<ID3D12Resource*>(ri.res);
                                via = "table";
                                u0bytes = ri.size;
                            }
                        }
                    }
                }
            }
            // width from the CACHED SIZE, not from u0res->GetDesc(): the pointer may already be dead.
            const uint64_t width = u0bytes;
            if (clay) {
                U0CsTable st_{};
                const bool have_ = g_layouts.U0Table(clay, &st_);
                if (have_) {
                    for (uint32_t prm = 0; prm < 64; ++prm) {
                        descriptor_table dtab = {};
                        {
                            auto& s = g_command_lists.Gfx(cmd_list);
                            dtab = s.root[prm].compute_table();
                        }
                        if (!dtab.handle) continue;
                        char line[1024];
                        int off = 0;
                        int declared = 0;
                        off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "U0SCAN param=%u%s :", prm,
                            (prm == st_.param) ? "(COMPUTED)" : "");
                        for (uint32_t k = 0; k < 32 && off < 880; ++k) {
                            descriptor_heap hh = { 0 };
                            uint32_t so = 0;
                            cmd_list->get_device()->get_descriptor_heap_offset(dtab, k, 0, &hh, &so);
                            if (!hh.handle) continue;
                            ResInfo r2;
                            if (!g_descriptors.Find({ hh.handle, so }, r2) || !r2.res) continue;
                            if (r2.size == 0) continue; // not a buffer / nothing recorded
                            const unsigned res_bytes = r2.size;
                            const unsigned hk = PortApi::GameComputeUavBufferHash(res_bytes);
                            const bool dec = PortApi::TextureOverrideDeclaresHash(hk);
                            if (dec) ++declared;
                            off += _snprintf_s(line + off, sizeof(line) - off, _TRUNCATE, "  [%u]=%u%s", k, res_bytes,
                                dec ? "<<<DECLARED" : "");
                        }
                        if (declared > 0 || prm == st_.param || off > 40) logger::write("%s", line);
                    }
                }
            }
            const unsigned h_resolved = width ? PortApi::GameComputeUavBufferHash((unsigned)width) : 0u;
            // Data-driven, not a size constant: is this buffer one the loaded inis actually name?
            const bool plausible = PortApi::TextureOverrideDeclaresHash(h_resolved);
            logger::write("SHAPEKEYU0 cs=%016llx cmpLayout=%llx u0root=%d via=%s res=%p ByteWidth=%llu "
                          "descHash=%08x %s",
                (unsigned long long)cs_hash, (unsigned long long)clay, u0root, via, (void*)u0res,
                (unsigned long long)width, h_resolved,
                !u0res          ? "-- UNRESOLVED: cs-u0 still cannot be answered"
                    : plausible ? "-- descHash IS declared by a [TextureOverride]: this is the buffer the mod names"
                                : "*** descHash is declared by NO [TextureOverride]: the RESOLVE is wrong "
                                  "(see the U0SCAN line for which index in this table IS declared) ***");
        }
    }
}

void DispatchDiagMorph(const DispatchArgs& da)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t cs_hash = da.cs_hash;
    const uint32_t group_count_x = da.group_count_x;
    const uint32_t group_count_y = da.group_count_y;
    const uint32_t group_count_z = da.group_count_z;
    if (!g_debug) return;
    uint32_t morph_param = 64;
    uint32_t morph_sz = 0;
    uint64_t morph_va = 0;
    ID3D12Resource* morph_res = nullptr;
    {
        auto it = &g_command_lists.Cbv(cmd_list);
        if (it != nullptr)
            for (uint32_t p = 0; p < 64; ++p) {
                uint32_t s = it->binds[p].size;
                if (s >= 1024 && s <= 1152 && it->binds[p].res) {
                    morph_param = p;
                    morph_sz = s;
                    morph_va = it->binds[p].va;
                    morph_res = it->binds[p].res;
                    break;
                }
            }
    }
    bool morph_via_table = false;
    if (g_debug && morph_param >= 64) {
        auto* dev = cmd_list->get_device();
        descriptor_table dtabs[64];
        uint32_t ntab = 0;
        {
            auto it = &g_command_lists.Gfx(cmd_list);
            if (it != nullptr)
                for (uint32_t p = 0; p < 64; ++p)
                    if (it->root[p].table.handle) {
                        dtabs[ntab] = it->root[p].table;
                        if (++ntab >= 64) break;
                    }
        }
        for (uint32_t t = 0; t < ntab && !morph_via_table; ++t) {
            const TableStart ts = TableStartOf(dev, dtabs[t]);
            if (!ts) continue;
            for (uint32_t e = 0; e < 16; ++e) { // a CB table is small; 16 entries covers b0..b15
                HeapCbv hc;
                if (!g_descriptors.FindCbv({ ts.heap, ts.base + e }, hc) || !hc.res) continue;
                if (hc.size >= 1024 && hc.size <= 1152) {
                    morph_param = 64 + t;
                    morph_sz = hc.size;
                    morph_va = hc.va;
                    morph_res = hc.res;
                    morph_via_table = true;
                    break;
                }
            }
        }
    }
    if (g_debug && g_dispatch_toggles.morph_cs_hash && cs_hash == g_dispatch_toggles.morph_cs_hash) {
        static ProbeBudget s_md;
        if (s_md.take(1)) {
            const uint64_t clay = g_command_lists.CmpLayout(cmd_list);
            const int b0root = clay ? g_layouts.CsCb0Root(clay) : -1;
            logger::write("MORPHDISPATCH cs=%016llx groups=%u,%u,%u b0root=%d (rootB0found=%d tableB0found=%d "
                          "= the OLD size-band scan, expected 0) -- Loader ran",
                cs_hash, group_count_x, group_count_y, group_count_z, b0root, (int)(morph_param < 64),
                (int)morph_via_table);
            auto* dev = cmd_list->get_device();
            char buf[900];
            int o = 0;
            buf[0] = 0;
            auto ci = &g_command_lists.Cbv(cmd_list);
            if (ci != nullptr)
                for (int p = 0; p < 64 && o < 820; ++p)
                    if (ci->binds[p].size)
                        o += snprintf(buf + o, sizeof(buf) - o, "cbv[%d]=%uB ", p, ci->binds[p].size);
            auto si = &g_command_lists.Gfx(cmd_list);
            if (si != nullptr) {
                for (int p = 0; p < 64 && o < 820; ++p)
                    if (si->root[p].va) o += snprintf(buf + o, sizeof(buf) - o, "rootVA[%d] ", p);
                for (int p = 0; p < 64 && o < 780; ++p) {
                    if (!si->root[p].table.handle) continue;
                    const TableStart ts = TableStartOf(dev, si->root[p].table);
                    o +=
                        snprintf(buf + o, sizeof(buf) - o, "tbl[%d]@%llx+%u ", p, (unsigned long long)ts.heap, ts.base);
                }
            }
            logger::write("MORPHBIND %s", buf);
        }
    }
    if (g_debug && (morph_param < 64 || morph_via_table)) {
        if (ProbeArmed() && DiagOnce("MORPHHUNT", (int)(cs_hash ^ (cs_hash >> 32))))
            logger::write("MORPHHUNT cs=%016llx b0param=%u cbvSize=%u cbvVA=%llx groups=%u,%u,%u src=%s", cs_hash,
                morph_param, morph_sz, morph_va, group_count_x, group_count_y, group_count_z,
                morph_via_table ? "TABLE" : "root");
    }
}

void DispatchDiagSkinProbe(const DispatchArgs& da, const CmdState& state)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t cs_hash = da.cs_hash;
    const uint32_t group_count_x = da.group_count_x;
    const uint32_t group_count_y = da.group_count_y;
    const uint32_t group_count_z = da.group_count_z;
    if (g_debug) {
        const uint64_t pool_lo = g_recognition.SkinBandLo();
        const uint64_t pool_hi = g_recognition.SkinBandHi();
        static std::mutex s_pm;
        static std::set<uint64_t> s_probed;
        static std::atomic<bool> s_done{ false };
        static uint32_t s_gen = 0;
        {
            uint32_t g = g_diag_gen.load();
            if (g != s_gen) {
                std::lock_guard l(s_pm);
                s_probed.clear();
                s_done.store(false);
                s_gen = g;
            }
        }
        const size_t meshN = g_recognition.CharMeshCount();
        uint64_t meshSample = 0;
        g_recognition.ForEachCharMeshVb0([&](uint64_t va, const CharVbOrigin&) {
            meshSample = va;
            return true;
        });
        if (!s_done.load() && meshN > 0) {
            char buf[1000];
            int o = 0;
            bool anyPool = false;
            bool anyMesh = false;
            o += snprintf(buf + o, sizeof(buf) - o, "SKINPROBE cs=%016llx meshSet=%zu(s=%llx) g=%u,%u,%u ROOT[",
                cs_hash, meshN, meshSample, group_count_x, group_count_y, group_count_z);
            {
                for (int p = 0; p < 64 && o < 760; ++p) {
                    uint64_t rv = state.root[p].va;
                    if (!rv) continue;
                    uint32_t sz = g_resources.SizeAtVa(rv);
                    bool pool = (rv >= pool_lo && rv < pool_hi);
                    const bool mesh = g_recognition.IsCharMeshVb0(rv);
                    if (pool) anyPool = true;
                    if (mesh) anyMesh = true;
                    if (pool || mesh || sz >= 0x10000)
                        o += snprintf(buf + o, sizeof(buf) - o, "p%d:%llx/%u%s%s ", p, rv, sz, pool ? "<P" : "",
                            mesh ? "<M" : "");
                }
            }
            o += snprintf(buf + o, sizeof(buf) - o, "] TBL[");
            {
                auto* dev = cmd_list->get_device();
                for (uint32_t p = 0; p < 16 && o < 940; ++p) {
                    const auto tbl = state.root[p].compute_table();
                    const TableStart ts = TableStartOf(dev, tbl);
                    if (!ts) continue;
                    for (uint32_t i = 0; i < 64 && o < 940; ++i) {
                        ResInfo ri;
                        if (!g_descriptors.Find({ ts.heap, ts.base + i }, ri)) continue; // sharded
                        uint64_t va = ri.cached_va;
                        if (!va) continue;
                        bool pool = (va >= pool_lo && va < pool_hi);
                        const bool mesh = g_recognition.IsCharMeshVb0(va);
                        if (pool) anyPool = true;
                        if (mesh) anyMesh = true;
                        if (pool || mesh)
                            o += snprintf(buf + o, sizeof(buf) - o, "t%u.%u:%llx/%u%s%s ", p, i, va, ri.size,
                                pool ? "<P" : "", mesh ? "<M" : "");
                    }
                }
            }
            o += snprintf(buf + o, sizeof(buf) - o, "]");
            // Only log dispatches that touch the pool or the char mesh, first 8 distinct hashes.
            if (anyPool || anyMesh) {
                bool fresh;
                {
                    std::lock_guard l(s_pm);
                    fresh = s_probed.insert(cs_hash).second;
                    if (s_probed.size() >= 8) s_done.store(true);
                }
                if (fresh) logger::write("%s", buf);
            }
        }
    }
}

void DispatchDiagSkinBlueprint(const DispatchArgs& da, const CmdState& state)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t cs_hash = da.cs_hash;
    const uint32_t group_count_x = da.group_count_x;
    const uint32_t group_count_y = da.group_count_y;
    const uint32_t group_count_z = da.group_count_z;
    if (g_debug) {
        const bool isSkin = g_recognition.IsSkinCs(cs_hash);
        g_rt_skin_read.Drain(); // RTSKINREAD: log the bytes once the recorded copies have retired
        uint64_t rt_out = 0;
        if ((isSkin || g_rt_bones.WindowOpen()) && g_rt_rings.Count()) {
            for (int p = 0; p < 64 && !rt_out; ++p)
                if (state.root[p].va && g_rt_rings.Overlaps(state.root[p].va, 1)) rt_out = state.root[p].va;
            auto* dev = cmd_list->get_device();
            for (uint32_t p = 0; p < 16 && !rt_out; ++p) {
                const auto tbl = state.root[p].compute_table();
                const TableStart ts = TableStartOf(dev, tbl);
                if (!ts) continue;
                for (uint32_t i = 0; i < 24 && !rt_out; ++i) {
                    ResInfo ri;
                    if (!g_descriptors.Find({ ts.heap, ts.base + i }, ri)) continue;
                    if (ri.cached_va && g_rt_rings.Overlaps(ri.cached_va, ri.size)) rt_out = ri.cached_va;
                }
            }
        }
        if (rt_out)
            g_rt_bones.Log(cmd_list, state, group_count_x, cs_hash,
                isSkin); // every skin-cache dispatch of the mod character, two frames
        const bool want_blueprint = isSkin && (g_rt_rings.Count() == 0 || rt_out != 0);
        if (want_blueprint) {
            const uint64_t bk = cs_hash ^ (rt_out * 0x9E3779B97F4A7C15ull);
            if (DiagOnce("SKINCS-MESH", (int)(bk ^ (bk >> 32)))) {
                std::unordered_set<uint64_t> meshVbs;
                g_recognition.ForEachCharMeshVb0([&](uint64_t va, const CharVbOrigin&) {
                    meshVbs.insert(va);
                    return false;
                });
                char buf[4000];
                int o = 0;
                o += snprintf(buf + o, sizeof(buf) - o, "SKINBLUEPRINT cs=%016llx g=%u,%u,%u CBV[", cs_hash,
                    group_count_x, group_count_y, group_count_z);
                {
                    auto it = &g_command_lists.Cbv(cmd_list);
                    if (it != nullptr)
                        for (int p = 0; p < 64 && o < 1500; ++p)
                            if (it->binds[p].size || it->binds[p].va) {
                                o += snprintf(buf + o, sizeof(buf) - o, "r%d=%uB@%llx", p, it->binds[p].size,
                                    (unsigned long long)it->binds[p].va);
                                uint8_t cb[64];
                                const uint32_t got = it->binds[p].res
                                    ? g_game_cbs.Snapshot(it->binds[p].res, it->binds[p].va, cb, sizeof(cb), true)
                                    : 0;
                                if (got) {
                                    o += snprintf(buf + o, sizeof(buf) - o, "{");
                                    for (uint32_t d = 0; d + 4 <= got && o < 1500; d += 4) {
                                        uint32_t v;
                                        memcpy(&v, cb + d, 4);
                                        o += snprintf(buf + o, sizeof(buf) - o, "%s%x", d ? "," : "", v);
                                    }
                                    o += snprintf(buf + o, sizeof(buf) - o, "}");
                                }
                                o += snprintf(buf + o, sizeof(buf) - o, " ");
                            }
                }
                o += snprintf(buf + o, sizeof(buf) - o, "] ROOTbuf[");
                {
                    for (int p = 0; p < 64 && o < 2000; ++p) {
                        uint64_t rv = state.root[p].va;
                        if (!rv) continue;
                        uint32_t sz = g_resources.SizeAtVa(rv);
                        const char* role = g_rt_rings.Overlaps(rv, 1) ? "<RTOUT"
                            : g_recognition.InSkinBand(rv)    ? "<POOL"
                                                              : (meshVbs.count(rv) ? "<MESH" : "");
                        o += snprintf(buf + o, sizeof(buf) - o, "r%d:%llx/%u%s ", p, rv, sz, role);
                    }
                }
                o += snprintf(buf + o, sizeof(buf) - o, "] TBL[");
                {
                    auto* dev = cmd_list->get_device();
                    for (uint32_t p = 0; p < 16 && o < 3800; ++p) {
                        const auto tbl = state.root[p].compute_table(); // compute set
                        const TableStart ts = TableStartOf(dev, tbl);
                        if (!ts) continue;
                        for (uint32_t i = 0; i < 24 && o < 3800; ++i) {
                            ResInfo ri;
                            if (!g_descriptors.Find({ ts.heap, ts.base + i }, ri)) continue; // sharded
                            uint64_t va = ri.cached_va;
                            if (!va) continue;
                            const char* role = g_rt_rings.Overlaps(va, ri.size) ? "<RTOUT"
                                : g_recognition.InSkinBand(va)          ? "<POOL"
                                                                        : (meshVbs.count(va) ? "<MESH" : "");
                            o += snprintf(buf + o, sizeof(buf) - o, "t%u.%u:%llx/%u%s ", p, i, va, ri.size, role);
                        }
                    }
                }
                o += snprintf(buf + o, sizeof(buf) - o, "]");
                logger::write("%s", buf);
                if (rt_out) g_rt_skin_read.Record(cmd_list, state, cs_hash, group_count_x);
            }
        }
    }
}

void DispatchDiagStalker(const DispatchArgs& da, const CmdState& state)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t cs_hash = da.cs_hash;
    const bool is_skin_cs = (cs_hash == 0x099222f33b1f30e6ULL);
    if (is_skin_cs && g_debug) {
        for (uint32_t p = 0; p < 16; ++p) {
            // is_skin_cs path -- compute set.
            const auto tbl = state.root[p].compute_table();
            if (tbl.handle != 0) {
                const TableStart ts = TableStartOf(cmd_list->get_device(), tbl);
                if (ts) {
                    for (uint32_t i = 0; i < 64; ++i) {
                        ResInfo ri;
                        if (g_descriptors.Find({ ts.heap, ts.base + i }, ri)) { // sharded
                            g_uav_stalker.Probe(cs_hash, p, i, ri.cached_va);
                        }
                    }
                }
            }
        }
    }
}
