// skin_cache_learn.cpp -- the game's skin-cache dispatch for a mod character found by data flow, and the
// pool buffers it writes recorded (CharacterRecognition::LearnSkinCs). See dispatch_parts.h.
#include "../addon_common.h"
#include "../mods/character_recognition.h"
#include "../mods/layout_registry.h"
#include "../hooks/resource_tracker.h"
#include "../rt/rt_rings.h" // RT: the BLAS rings
#include "dispatch_parts.h"

using namespace reshade::api;

void LearnSkinCache(const DispatchArgs& da, const CmdState& state)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t cs_hash = da.cs_hash;
    const uint32_t group_count_x = da.group_count_x;
    const uint32_t group_count_y = da.group_count_y;
    const uint32_t group_count_z = da.group_count_z;
    if (!g_debug) return;
    {
        const uint64_t pool_lo = g_recognition.SkinBandLo();
        const uint64_t pool_hi = g_recognition.SkinBandHi();
        const bool consider =
            (g_recognition.AnyCharMesh() || g_rt_rings.Count() != 0) && g_recognition.SkinCsAccepts(cs_hash);
        if (consider) {
            struct Cand {
                uint64_t va;
                uint32_t size;
                bool root;
            };
            constexpr uint32_t kRootCount = 64;
            constexpr uint32_t kTableCount = 16;
            constexpr uint32_t kSlotCount = 64;
            constexpr uint32_t kCandCap = kRootCount + kTableCount * kSlotCount;
            static thread_local Cand cand[kCandCap]; // 17 KB: per thread, not on the stack (C6262)
            uint32_t nc = 0;
            for (uint32_t p = 0; p < kRootCount; ++p)
                if (state.root[p].va) cand[nc++] = { state.root[p].va, 0u, true };
            {
                auto* dev = cmd_list->get_device();
                for (uint32_t p = 0; p < kTableCount; ++p) {
                    const auto tbl = state.root[p].compute_table();
                    const TableStart ts = TableStartOf(dev, tbl);
                    if (!ts) continue;
                    for (uint32_t i = 0; i < kSlotCount; ++i) {
                        ResInfo ri;
                        if (!g_descriptors.Find({ ts.heap, ts.base + i }, ri)) continue; // lock-free
                        if (ri.cached_va) cand[nc++] = { ri.cached_va, ri.size, false };
                    }
                }
            }
            bool reads_mesh = false;
            bool writes_rt_out = false;
            for (uint32_t k = 0; k < nc && !reads_mesh; ++k) reads_mesh = g_recognition.IsCharMeshVb0(cand[k].va);
            const bool rt_on = g_rt_rings.Count() != 0; // no ring recorded: Overlaps is false for all
            bool rt_out[kCandCap] = {};
            if (rt_on)
                for (uint32_t k = 0; k < nc; ++k)
                    if ((rt_out[k] = g_rt_rings.Overlaps(cand[k].va, cand[k].root ? 1u : cand[k].size))) writes_rt_out = true;
            if (reads_mesh || writes_rt_out) {
                std::vector<SkinOut> out_bufs;
                {
                    for (uint32_t k = 0; k < nc; ++k) {
                        const uint64_t va = cand[k].va;
                        const bool pool = va >= pool_lo && va < pool_hi;
                        if (cand[k].root) {
                            if (rt_out[k]) out_bufs.push_back({ va, 0u });
                            if (pool) out_bufs.push_back({ va, g_resources.SizeAtVa(va) });
                        } else if (rt_out[k] || pool)
                            out_bufs.push_back({ va, cand[k].size });
                    }
                }
                int added = 0;
                size_t total = 0;
                if (g_recognition.LearnSkinCs(cs_hash, out_bufs, &added, &total))
                    logger::write(
                        "SKINCACHE: cs=%016llx skins our char (%s); poolOutputs=%zu newOutputs=%d totalOut=%zu groups=%u,%u,%u",
                        cs_hash,
                        writes_rt_out ? "WRITES the vertex buffer a character's BLAS is built from -- RT"
                                      : "binds char mesh VB0",
                        out_bufs.size(), added, total, group_count_x, group_count_y, group_count_z);
            }
        }
    }
}
