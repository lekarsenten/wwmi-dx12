// The compute path: every dispatch the game issues, and the shapekey work we run off it.
// See dispatch_parts.h.
#include "../addon_common.h"
#include "../mods/shader_alias.h"
#include "../rt/rt_pal_record.h" // RT: the RTPAL recorder
#include "../instrumentation/gpu_timer.h"
#include "dispatch_parts.h"
#include "dispatch_toggles.h"
#include "../addon/addon.h"
#include "../shapekeys/sk_toggles.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../migoto/frame_analysis_driver.h"
#include "../mods/mod_set.h"
#include "../mods/game_cb_reader.h"
#include "../mods/layout_registry.h"
#include <map>
#include <string>
#include <mutex>
#include <set>
#include <utility>

DispatchToggles g_dispatch_toggles;

/**
 * Debug only: logs each compute shader the game dispatches, once per group count, to find the shaders a render
 * path uses (the morph shaders with ray tracing on).
 *
 * @param cs_hash       The compute shader's hash.
 * @param group_count_x The dispatch's group counts.
 * @param group_count_y
 * @param group_count_z
 * @return Nothing.
 */
static void LogDispatchOnce(uint64_t cs_hash, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
    if (!g_debug) return;
    static std::mutex s_mutex;
    static std::set<std::pair<uint64_t, uint64_t>> s_seen;
    std::lock_guard<std::mutex> lock(s_mutex);
    if (s_seen.size() >= 600) return;
    const uint64_t groups = ((uint64_t)group_count_x << 40) ^ ((uint64_t)group_count_y << 20) ^ group_count_z;
    if (!s_seen.insert({ cs_hash, groups }).second) return;
    logger::write("CSSEEN cs=%016llx groups=%u,%u,%u frame=%u", (unsigned long long)cs_hash, group_count_x,
        group_count_y, group_count_z, g_frame_id);
}

/**
 * Debug only: logs the shape key weights the game hands its morph shader (cb0: 128 list offsets, 128 weights,
 * two rows of scales), once per mesh each time they change. Shows whether they are fixed for a character.
 *
 * @param cmd_list      The command list of the dispatch.
 * @param group_count_y The dispatch's second group count (the mesh's shape key entries / 32).
 * @return Nothing.
 */
static void LogMorphWeights(command_list* cmd_list, uint32_t group_count_y)
{
    if (!g_debug) return;
    const uint64_t layout = g_command_lists.CmpLayout(cmd_list);
    const int root = layout ? g_layouts.CsCb0Root(layout) : -1;
    if (root < 0 || root >= 64) return;
    const auto& constant_buffers = g_command_lists.Cbv(cmd_list);
    ID3D12Resource* const res = reinterpret_cast<ID3D12Resource*>(constant_buffers.binds[root].res);
    const uint64_t va = constant_buffers.binds[root].va;
    uint32_t cb0[66 * 4] = {};
    const uint32_t got = (res && va) ? g_game_cbs.Snapshot(res, va, reinterpret_cast<uint8_t*>(cb0), sizeof(cb0), true) : 0;
    if (got < sizeof(cb0)) {
        static std::atomic<int> s_failures{ 0 };
        if (s_failures.fetch_add(1) < 4)
            logger::write("MORPHWEIGHTS: cb0 not readable (root %d, res %p, va %llx, %u bytes read)", root, (void*)res,
                (unsigned long long)va, got);
        return;
    }
    const uint32_t checksum = cb0[0] + cb0[1] + cb0[2] + cb0[3];
    uint64_t weights_hash = 14695981039346656037ull;
    std::string weights;
    for (int i = 0; i < 128; ++i) {
        const uint32_t bits = cb0[32 * 4 + i];
        weights_hash = (weights_hash ^ bits) * 1099511628211ull;
        if (!bits) continue;
        float value = 0;
        memcpy(&value, &bits, sizeof(value));
        char part[40];
        snprintf(part, sizeof(part), " %d=%.3f", i, value);
        weights += part;
    }
    static std::mutex s_mutex;
    struct Logged {
        uint64_t weights_hash = 0;
        uint32_t frame = 0;
        int lines = 0;
    };
    static std::map<uint64_t, Logged> s_last; // (checksum, group count) -> what was logged last
    std::lock_guard<std::mutex> lock(s_mutex);
    const uint64_t key = ((uint64_t)checksum << 32) | group_count_y;
    Logged& logged = s_last[key];
    if (logged.lines && logged.weights_hash == weights_hash) return;
    // Per mesh: the first two sets, then one every 120 frames, 12 lines at most.
    if (logged.lines >= 12 || (logged.lines >= 2 && g_frame_id - logged.frame < 120)) return;
    logged.weights_hash = weights_hash;
    logged.frame = g_frame_id;
    logged.lines += 1;
    logger::write("MORPHWEIGHTS checksum=%u groups_y=%u frame=%u: non-zero weights:%s", checksum, group_count_y,
        g_frame_id, weights.empty() ? " none" : weights.c_str());
}

bool on_dispatch(command_list* cmd_list, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z)
{
    PROF(PROF_Dispatch);
    if (g_addon.disabled || !g_addon.is_dx12) return false;
    MigotoPort::FrameAnalysisDrawScope
        fa_scope; // see on_draw_indexed_body: g_fa_mtx first, one hook at a time while armed
    uint64_t cs_hash = 0;
    cmd_list->get_private_data(s_iid_cs_hash, &cs_hash);
    if (!cs_hash) return false;
    ModSetScope set_scope(cmd_list);
    GpuBracket _gt((ID3D12GraphicsCommandList*)cmd_list->get_native(), GT_DispatchHook);
    const DispatchArgs da{ cmd_list, cs_hash, group_count_x, group_count_y, group_count_z };

    if (g_sk_toggles.publish_shader_state) PortApi::SetBoundShader('c', AliasShaderHash(cs_hash));

    DispatchDiagShapekey(da);
    LogDispatchOnce(cs_hash, group_count_x, group_count_y, group_count_z);

    SkU0Ident ck;
    ck.shapekey = (g_dispatch_toggles.morph_cs_hash &&
        (cs_hash == g_dispatch_toggles.morph_cs_hash || cs_hash == 0xc1a7f1fec51b482fULL));

    if (cs_hash == g_dispatch_toggles.morph_cs_hash) LogMorphWeights(cmd_list, group_count_y);

    {
        PROF(PROF_Disp_Resolve);
        SkResolveU0(cmd_list, cs_hash, group_count_x, group_count_y, group_count_z, ck);
    }

    const CmdState& state = g_command_lists.Gfx(cmd_list);
    g_rt_pal_recorder.Record(cmd_list, state,
        cs_hash); // RTPAL: a character's skin-cache sections' bone palettes (rt/rt_toggles.h palette)

    DispatchDiagMorph(da);
    DispatchVerdict verdict;
    {
        PROF(PROF_Disp_Ovr);
        verdict = RunDispatchOverrides(da, ck);
    }
    const bool skip_dispatch = verdict.skip;
    const unsigned md_sections_fired = verdict.sections_fired;

    DispatchCapture(da, ck, skip_dispatch, md_sections_fired);

    DispatchDiagSkinProbe(da, state);

    {
        PROF(PROF_Disp_Learn);
        LearnSkinCache(da, state);
    }

    DispatchDiagSkinBlueprint(da, state);

    DispatchDiagStalker(da, state);
    return skip_dispatch;
}
