// The present path: the frame's own bookkeeping, the [Present] command list, and everything that
// only makes sense once per frame. See present_parts.h.
#include "../addon_common.h"
#include "../instrumentation/removal_pin.h"
#include "../addon_core.h"
#include "../mods/claim_state.h"
#include "../instrumentation/occlusion_probe.h"
#include "../hooks/resource_tracker.h"
#include "present_parts.h"
#include "../instrumentation/device_watch.h"
#include "../config.h"     // g_skip_engine_tick (A9)
#include "../ui/overlay.h" // DrawStoppedNotice
#include "../instrumentation/hot_path.h"
#include "../instrumentation/perf_monitor.h"
#include "../hud/uid_hide.h"

void on_present_body(command_queue* queue, swapchain* sc)
{
    HotPath::FrameEnd();
    g_perf.FrameEnd();
    PROF(PROF_Present);
    if (g_rt_blocked || g_faulted) {
        static ULONGLONG s_notice_until = 0;
        if (!g_addon_disabled) SetAddonDisabled(true);
        if (!s_notice_until) {
            reshade::register_overlay("OSD", DrawStoppedNotice);
            s_notice_until = GetTickCount64() + 60000;
        } else if (s_notice_until != ~0ull && GetTickCount64() > s_notice_until) {
            reshade::unregister_overlay("OSD", DrawStoppedNotice);
            s_notice_until = ~0ull;
        }
    }
    { // F6 (no modifiers) toggles A1. Polled before the bypass return below, so it also turns the addon back on.
        static bool s_f6 = false;
        DWORD       pid = 0;
        if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
        const bool mods = ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) != 0;
        const bool f6 = pid == GetCurrentProcessId() && !mods && (GetAsyncKeyState(VK_F6) & 0x8000) != 0;
        if (f6 && !s_f6 && !g_rt_blocked && !g_faulted) SetAddonDisabled(!g_addon_disabled.load());
        s_f6 = f6;
    }
    VerboseTick();
    if (g_addon_disabled) return;
    if (g_hide_uid) g_uid_hide.NoteBackBuffer(sc);
    RemovalPin::Check(g_d3d12_device, "Present");

    PresentQueueDiag(queue);

    PortProbeTick(queue, sc);
    ConfigWatchTick();
    UpdateSkinBand(); // keep the skin-pool VA band tracking the learned char VB0s (auto re-derive across patches)
    SeedHeartbeat();
    // Manual reload (B1) runs on the render thread, at the frame boundary.
    if (g_reload_requested.exchange(false)) {
        ModLoader::Reload(); // -> g_reload_pending, applied by Tick() just below
        g_texovr_ready = false;
        logger::write("RELOAD: engine state cleared (g_texovr_ready=0) -- inis will be re-parsed");
    }
    ModLoader::Tick();
    if (!g_skip_engine_tick) EngineFrameTick(queue, sc);
    if (g_faulted) return;
    StartRemovalWatchdog(sc);
    g_buffer_reloc.Sweep(sc->get_device());
    ModTexturesTick();
    if (g_wwmi_geom) {
        static int s_gl = 0;
        uint32_t   gc = g_wwmi_geom_count.load();
        if (s_gl < 5) {
            s_gl++;
            logger::write("WwmiGeom: M4 ini-geometry drew %u direct-comp sub-draws this frame", gc);
        }
    }
    g_draw_replace_count.Roll();
    g_port_sect_runs.Roll();
    g_port_sect_nocb.Roll();
    g_port_fellback_count.Roll();
    for (GeoMod& gm : g_geomods) {
        gm.replaced.Roll();
        gm.skip_orig.Roll();
        gm.sect_runs.Roll();
    }
    const uint32_t texinj_last = g_tex_inject_count.exchange(0);
    g_global_tex_count.store(0);
    g_tex_lineage_count.store(0);
    g_wwmi_geom_count.store(0);
    g_custom_blend_count.store(0);
    g_custom_blend_bound_count.store(0);
    {
        std::lock_guard l(g_char_skel_mtx);
        g_frame_skel.clear();
    } // per-frame skeleton scope (LOD-pin)
    {
        std::lock_guard l(g_skincache_mtx);
        g_char_bone_va.clear();
    } // per-frame bone-arena scope (POOLDRAW boneNear)
    {
        PROF(PROF_DrainTexHash);
        DrainTexReadback();
    } // hash streamed char textures (source-mip0 copies appended on the game's copy list)
    ++g_frame_id; // new frame -> allow re-capturing each component's native bones once
    g_sections_this_frame.store(0, std::memory_order_relaxed);

    Heartbeat(texinj_last);
    // ARMED-DIAGNOSTIC COUNTDOWN, centrally: "run for N frames then stop" budgets set by overlay buttons.
    g_occl.PresentTick();
    {
        int t = g_vbdiag_frames.load(std::memory_order_relaxed);
        if (t > 0) {
            g_vbdiag_frames.store(t - 1, std::memory_order_relaxed);
            if (t == 1) logger::write("VBDIAG: capture window closed");
        }
    }
    if (g_infoq) DrainDebugLayer("frame");
    HealthHeartbeat();
    {
        int r = g_shadow_recog_watch.load(std::memory_order_relaxed);
        if (r > 0) g_shadow_recog_watch.store(r - 1, std::memory_order_relaxed);
    }
    {
        extern std::atomic<int> g_tex_flicker_watch; // B4, same rule: a budget that actually expires
        int                     f = g_tex_flicker_watch.load(std::memory_order_relaxed);
        if (f > 0) g_tex_flicker_watch.store(f - 1, std::memory_order_relaxed);
    }
    LoadGeometryMods();
    PROF(PROF_PresTail); // on_present's tail: shapekey / ibhash drain / diag
    SkRefreshModHashes();
    MigotoPort::SkCapDrain();
    {
        PROF(PROF_IbHashDrain);
        IbHash::DrainQueue();
    } // GPU IB-hash anchor: hash queued char IBs (bounded, cached) -- needed in prod too
    if (g_debug) {
        PROF(PROF_DiagPresent);
        DiagOnPresent(reinterpret_cast<ID3D12CommandQueue*>(queue->get_native()));
    }
    // Ticks the armed window and writes out once the GPU has run the copies. Never waits.
    MigotoPort::FrameAnalysisFrameEnd();
    if (g_debug && g_wwmi_parse.exchange(false)) {
        // Self-check: log every discovered geometry mod's parsed layout.
        for (auto& gm : g_geomods) WwmiMod::LogSelfCheck(gm.mod);
    }
}
