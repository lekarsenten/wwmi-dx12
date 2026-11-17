// The ray-tracing hooks: the RT module's feed, and the RT BLOCK without it. See rt_block_hooks.h.
#include "../addon_common.h"
#include "rt_block_hooks.h"
#include "../addon/addon.h"
#include "../addon/settings.h"
#include "../core/frame_clock.h"
#include "../core/log.h"
#include "../instrumentation/removal_pin.h"
#include "../mods/mod_set.h"
#include "../rt/rt_hooks.h" // RtModulePresent
#include "../rt/rt_blas.h"

static void RtBlockWithoutModule()
{
    if (RtModulePresent()) return;
    if (g_settings.rt_experimental) {
        // Experimental (ini rt_experimental = 1): the mods stay on. Draws are claimed by their index buffer (H18),
        // since the game animates vb0 with ray tracing on; the game's acceleration structures keep its own models.
        static std::atomic<bool> s_told{ false };
        if (!s_told.exchange(true))
            logger::report(
                "RT EXPERIMENTAL: the game is ray tracing and rt_experimental = 1 -- the mods stay on (frame %u). "
                "Ray-traced shadows and reflections still show the game's own models.",
                g_frame_id);
        return;
    }
    if (g_addon.rt_blocked.load(std::memory_order_relaxed) || g_addon.rt_blocked.exchange(true)) return;
    logger::report(
        "RT BLOCK: the game is ray tracing and this build has no RT module -- the add-on is off until the game "
        "restarts (frame %u). Turn ray tracing off in the game's graphics settings.",
        g_frame_id);
}

bool on_build_acceleration_structure(reshade::api::command_list* cmd_list,
    reshade::api::acceleration_structure_type type, reshade::api::acceleration_structure_build_flags build_flags,
    uint32_t input_count, const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource,
    uint64_t, reshade::api::resource_view, reshade::api::resource_view dest,
    reshade::api::acceleration_structure_build_mode mode)
{
    RtBlockWithoutModule();
    static std::atomic<unsigned long long> s_n{ 0 };
    const unsigned long long n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || n == 1000 || n == 100000)
        logger::report(
            "RT ACTIVE: BuildRaytracingAccelerationStructure #%llu (type %d, mode %d, %u input(s)) at frame %u -- "
            "the game builds acceleration structures, so ray tracing is on and running",
            n + 1, (int)type, (int)mode, input_count, g_frame_id);
    std::optional<ModSetScope> set_scope;
    if (!g_addon.disabled) set_scope.emplace(cmd_list);
    // RT (rt/rt_blas.cpp): the rings RTPAL finds the mod character's skin cache by, H19 FREEZE, and the RTGEO / RTDBG lines.
    if (g_rt_blas.OnBuild(cmd_list, type, build_flags, input_count, inputs, dest, mode, n)) return true;
    if (cmd_list) RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "game-as-build");
    return false;
}

bool on_dispatch_rays(reshade::api::command_list* cmd_list, reshade::api::resource, uint64_t, uint64_t,
    reshade::api::resource, uint64_t, uint64_t, uint64_t, reshade::api::resource hit_group, uint64_t hit_group_offset,
    uint64_t hit_group_size, uint64_t hit_group_stride, reshade::api::resource, uint64_t, uint64_t, uint64_t,
    uint32_t width, uint32_t height, uint32_t depth)
{
    RtBlockWithoutModule();
    if (g_debug)
        RtNoteDispatchRays(reinterpret_cast<ID3D12Resource*>(hit_group.handle), hit_group_offset, hit_group_size,
            hit_group_stride, width, height);
    static std::atomic<unsigned long long> s_n{ 0 };
    const unsigned long long n = s_n.fetch_add(1, std::memory_order_relaxed);
    if (n == 0 || n == 1000 || n == 100000)
        logger::report(
            "RT ACTIVE: DispatchRays #%llu (%u x %u x %u) at frame %u", n + 1, width, height, depth, g_frame_id);
    if (cmd_list) RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "game-dispatch-rays");
    return false;
}
