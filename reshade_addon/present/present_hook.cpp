// present_hook.cpp -- see present_hook.h.
#include "../addon_common.h"
#include "../instrumentation/removal_pin.h"
#include "../addon/debug_layer.h"
#include "../mods/character_recognition.h"
#include "../mods/mod_registry.h"
#include "../instrumentation/occlusion_probe.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/stamp_ledger.h"
#include "present_hook.h"
#include "present_engine.h"
#include "present_mod_load.h"
#include "present_diag.h"
#include "../instrumentation/watchdogs.h"
#include "../instrumentation/diag_gate.h"
#include "present_log.h"
#include "../addon/addon.h"
#include "../addon/settings.h"
#include "../draw/draw_toggles.h"
#include "frame_sync.h"
#include "present_toggles.h"
#include "../textures/tex_toggles.h"
#include "../ui/overlay.h" // DrawStoppedNotice
#include "../instrumentation/hot_path.h"
#include "../instrumentation/perf_monitor.h"
#include "../hud/uid_hide.h"
#include "../textures/tex_replace.h"
#include "../textures/tex_budget.h"
#include "../textures/tex_encode.h"
#include "../instrumentation/crash_handler.h"
#include "../shapekeys/shapekey_chain.h"
#include "../instrumentation/debug_pipe.h"
#include "../instrumentation/diag_arms.h"
#include "../instrumentation/shader_capture.h"
#include "../instrumentation/uav_stalker.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/dx12_skcap.h"
#include "../migoto/frame_analysis_driver.h"
#include "../mods/buffer_relocation.h"
#include "../mods/ib_hash.h"
#include "../textures/mod_texture_store.h"
#include "../textures/tex_select.h"
#include "../textures/tex_readback.h"
#include "../textures/tex_ref_take.h"
#include "../mods/mod_set.h"

FrameSync g_frames;
PresentLog g_present_log;

PresentToggles g_present_toggles;
PresentStats g_present_stats;

FrameStepper g_frame_step;

namespace {

bool GameKeyDown(int vk)
{
    DWORD pid = 0;
    if (HWND fg = GetForegroundWindow()) GetWindowThreadProcessId(fg, &pid);
    const bool mods =
        ((GetAsyncKeyState(VK_SHIFT) | GetAsyncKeyState(VK_CONTROL) | GetAsyncKeyState(VK_MENU)) & 0x8000) != 0;
    return pid == GetCurrentProcessId() && !mods && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

} // namespace

void FrameStepper::OnPresent(command_queue* queue, swapchain* sc)
{
    HotPath::FrameEnd();
    g_perf.FrameEnd();
#ifdef WWMI_DEBUG
    {
        static LARGE_INTEGER s_freq{};
        static LARGE_INTEGER s_last{};
        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        if (!s_freq.QuadPart) QueryPerformanceFrequency(&s_freq);
        const double ms =
            s_last.QuadPart ? (double)(now.QuadPart - s_last.QuadPart) * 1000.0 / (double)s_freq.QuadPart : 0.0;
        if (ms >= 100.0) logger::write("FRAME-SLOW f=%u: %.0f ms since the previous present", (unsigned)g_frame_id, ms);
        s_last = now;
    }
#endif
    PROF(PROF_Present);
    const uint32_t me = GetCurrentThreadId();
    g_frames.presenter_tid.store(me, std::memory_order_relaxed);
    g_frames.present_queue.store(reinterpret_cast<void*>(queue->get_native()), std::memory_order_relaxed);
    step_sc_.store(sc, std::memory_order_release);
    {
        const resource_desc bbd = sc->get_device()->get_resource_desc(sc->get_current_back_buffer());
        g_frames.bb_width.store(bbd.texture.width, std::memory_order_relaxed);
        g_frames.bb_height.store(bbd.texture.height, std::memory_order_relaxed);
    }
    // Presents from another thread than the game's (frame generation): the game's thread steps at its own frame end, and
    // a present steps only when no frame end came for a while, and then only after the game drew.
    const uint32_t game_tid = g_frames.game_submit_tid.load(std::memory_order_relaxed);
    const bool other_thread = g_present_toggles.frame_end_step && game_tid && game_tid != me;
    g_frames.frame_end_armed.store(other_thread, std::memory_order_relaxed);
    if (other_thread != other_thread_) {
        other_thread_ = other_thread;
        logger::write("FRAMESTEP: presents come from %s thread -- the per-frame step runs at %s",
            other_thread ? "another" : "the game's", other_thread ? "the game's frame end" : "the present");
    }
    // A frame the step already ran for at its end (presents just moved to the game's thread) gets only the redraw.
    const bool stepped_at_end = g_frames.frame_end_stepped.exchange(false, std::memory_order_relaxed);
    const bool step = other_thread
        ? GetTickCount64() - g_frames.frame_end_step_ms.load(std::memory_order_relaxed) >= 250 &&
            g_frames.game_frame_submitted.exchange(false, std::memory_order_relaxed)
        : !stepped_at_end;
#ifdef WWMI_DEBUG
    if (g_present_log.left.load(std::memory_order_relaxed) > 0) {
        static LARGE_INTEGER s_last{};
        LARGE_INTEGER now;
        LARGE_INTEGER freq;
        QueryPerformanceCounter(&now);
        QueryPerformanceFrequency(&freq);
        const double dt =
            s_last.QuadPart ? (double)(now.QuadPart - s_last.QuadPart) * 1000.0 / (double)freq.QuadPart : 0.0;
        s_last = now;
        g_present_log.qpc.store(now.QuadPart, std::memory_order_relaxed);
        std::string subs;
        {
            std::lock_guard l(g_present_log.mtx);
            g_present_log.bbs.clear();
            for (uint32_t i = 0; i < sc->get_back_buffer_count(); ++i)
                g_present_log.bbs.push_back(sc->get_back_buffer(i).handle);
            for (const PresentLogSubmitter& s : g_present_log.subs) {
                char e[96];
                snprintf(e, sizeof(e), " %u/%llx:%u%s@%.2f", s.tid, (unsigned long long)s.queue, s.lists,
                    s.claimed ? "c" : "", (double)(now.QuadPart - s.last_qpc) * 1000.0 / (double)freq.QuadPart);
                subs += e;
            }
            g_present_log.subs.clear();
        }
        logger::report(
            "PRESENTLOG f=%u tid=%lu dt=%.2f ms step=%d draws=%u execs=%u claimed=%u bb=%u queue=%p swapchain=%p bbres=%llx subs=%s",
            g_frame_id, GetCurrentThreadId(), dt, (int)step, g_present_log.draws.exchange(0),
            g_present_log.execs.exchange(0), g_draw_stats.draw_replace_count.n.load(),
            sc->get_current_back_buffer_index(), (void*)queue->get_native(), (void*)sc->get_native(),
            (unsigned long long)sc->get_current_back_buffer().handle, subs.c_str());
        if (g_present_log.left.fetch_sub(1) == 1) logger::report("PRESENTLOG: done");
    }
#endif
    if (g_addon.rt_blocked || g_addon.faulted) {
        if (!g_addon.disabled) SetAddonDisabled(true);
        if (!notice_until_) {
            reshade::register_overlay("OSD", DrawStoppedNotice);
            notice_until_ = GetTickCount64() + 60000;
        } else if (notice_until_ != ~0ull && GetTickCount64() > notice_until_) {
            reshade::unregister_overlay("OSD", DrawStoppedNotice);
            notice_until_ = ~0ull;
        }
    }
    { // F6 (no modifiers) toggles A1. Polled before the bypass return below, so it also turns the addon back on.
        static bool s_f6 = false;
        const bool f6 = GameKeyDown(VK_F6);
        if (f6 && !s_f6 && !g_addon.rt_blocked && !g_addon.faulted) SetAddonDisabled(!g_addon.disabled.load());
        s_f6 = f6;
    }
    VerboseTick();
    if (g_addon.disabled) {
        DrainModSets();
        return;
    }
#ifdef WWMI_DEBUG
    { // Numpad 7: the per-frame step at the game's frame end (frame generation) or at every present, as 2026.10.2
        static bool s_np7 = false;
        const bool np7 = GameKeyDown(VK_NUMPAD7);
        if (np7 && !s_np7) {
            g_present_toggles.frame_end_step = !g_present_toggles.frame_end_step;
            logger::write("HOTKEY: per-frame step at the game's frame end = %d", (int)g_present_toggles.frame_end_step);
        }
        s_np7 = np7;
    }
#endif
    if (g_settings.hide_uid) g_uid_hide.NoteBackBuffer(sc);
    RemovalPin::Check(g_addon.d3d12_device, "Present");

    g_present_diag.QueueDiag(queue);
    if (!step) {
        ModSetRef set_ref;
        if (!g_present_toggles.skip_engine_tick) g_engine_frame.Redraw(queue, sc);
        return;
    }
    Step(queue, sc, true);
}

void FrameStepper::Step(command_queue* queue, swapchain* sc, bool at_present)
{
    std::lock_guard frame_lock(g_engine_frame.Mutex());
    ModSetStepScope set_scope;
    g_engine_frame.ProbeTick(queue, sc);
    g_present_diag.ConfigWatchTick();
    g_recognition
        .UpdateSkinBand(); // keep the skin-pool VA band tracking the learned char VB0s (auto re-derive across patches)
    g_present_diag.SeedHeartbeat();
    g_mod_textures.Tick();
    if (!g_present_toggles.skip_engine_tick) g_engine_frame.Tick(queue, sc, at_present);
    DrainModSets();
    g_watchdogs.StartRemoval(sc);
    g_buffer_reloc.Sweep(sc->get_device());
    g_mod_textures_load.Tick();
    g_tex_replace.Tick((ID3D12CommandQueue*)queue->get_native());
    g_tex_budget.Tick();
    g_tex_encode_test.Tick((ID3D12CommandQueue*)queue->get_native());
    g_tex_encoder.Tick();
    if (g_draw_toggles.wwmi_geom) {
        uint32_t gc = g_draw_stats.wwmi_geom_count.load();
        if (geom_lines_ < 5) {
            geom_lines_++;
            logger::write("WwmiGeom: M4 ini-geometry drew %u direct-comp sub-draws this frame", gc);
        }
    }
    g_draw_stats.draw_replace_count.Roll();
    g_draw_stats.port_sect_runs.Roll();
    g_draw_stats.port_sect_nocb.Roll();
    g_draw_stats.port_fellback_count.Roll();
    for (GeoMod& gm : Mods().Units()) {
        gm.replaced.Roll();
        gm.skip_orig.Roll();
        gm.sect_runs.Roll();
    }
    const uint32_t texinj_last = g_tex_stats.inject_count.exchange(0);
    g_tex_stats.global_count.store(0);
    g_tex_stats.lineage_count.store(0);
    g_draw_stats.wwmi_geom_count.store(0);
    g_draw_stats.custom_blend_count.store(0);
    g_draw_stats.custom_blend_bound_count.store(0);
    g_recognition.BeginFrame();
    {
        PROF(PROF_DrainTexHash);
        g_tex_readback.Drain();
    } // hash streamed char textures (source-mip0 copies appended on the game's copy list)
    ++g_frame_id; // new frame -> allow re-capturing each component's native bones once
    g_draw_stats.sections_this_frame.store(0, std::memory_order_relaxed);

    g_present_diag.Heartbeat(texinj_last);
    // ARMED-DIAGNOSTIC COUNTDOWN, centrally: "run for N frames then stop" budgets set by overlay buttons.
    g_occl.PresentTick();
    {
        int t = g_diag_arms.vbdiag_frames.load(std::memory_order_relaxed);
        if (t > 0) {
            g_diag_arms.vbdiag_frames.store(t - 1, std::memory_order_relaxed);
            if (t == 1) logger::write("VBDIAG: capture window closed");
        }
    }
    g_debug_layer.Drain("frame");
    g_present_diag.HealthHeartbeat();
    {
        int r = g_diag_arms.shadow_recog_watch.load(std::memory_order_relaxed);
        if (r > 0) g_diag_arms.shadow_recog_watch.store(r - 1, std::memory_order_relaxed);
    }
    g_tex_select.TickFlickerWatch(); // B4, same rule: a budget that actually expires
    PROF(PROF_PresTail); // on_present's tail: shapekey / ibhash drain / diag
    MigotoPort::SkCapDrain();
    {
        PROF(PROF_IbHashDrain);
        IbHash::DrainQueue();
    } // GPU IB-hash anchor: hash queued char IBs (bounded, cached) -- needed in prod too
    DebugPipe::RunQueued();
    g_tex_ref_take.Tick();
    if (g_debug) {
        PROF(PROF_DiagPresent);
        if (g_diag_arms.dump_shaders_requested.exchange(false)) g_shader_capture.Dump();
        g_shader_capture.DiffTick();
        g_diag_arms.Tick();
        g_uav_stalker.ForgetCharVbs();
    }
    // Ticks the armed window and writes out once the GPU has run the copies. Never waits.
    MigotoPort::FrameAnalysisFrameEnd();
    if (g_debug && g_diag_arms.wwmi_parse.exchange(false)) {
        // Self-check: log every discovered geometry mod's parsed layout.
        for (auto& gm : Mods().Units()) WwmiMod::LogSelfCheck(gm.mod);
    }
}

void FrameStepper::FrameEnd(command_queue* queue)
{
    swapchain* const sc = step_sc_.load(std::memory_order_acquire);
    if (!sc || g_addon.disabled || g_addon.rt_blocked || g_addon.faulted) return;
    if (reinterpret_cast<ID3D12CommandQueue*>(queue->get_native())->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return;
    if (!g_frames.game_frame_submitted.exchange(false, std::memory_order_relaxed)) return;
    g_frames.frame_end_step_ms.store(GetTickCount64(), std::memory_order_relaxed);
    g_frames.frame_end_stepped.store(true, std::memory_order_relaxed);
#ifdef WWMI_DEBUG
    if (g_present_log.left.load(std::memory_order_relaxed) > 0)
        logger::report("FRAMEEND step f=%u tid=%lu queue=%p draws=%u", g_frame_id, GetCurrentThreadId(),
            (void*)queue->get_native(), g_present_log.frame_end_draws.exchange(0));
#endif
    Step(queue, sc, false);
}

void FrameStepper::ForgetSwapchain()
{
    step_sc_.store(nullptr, std::memory_order_release);
}

void FrameEndStepGuarded(command_queue* queue)
{
    WWMI_GUARD_BEGIN("frame end step")
    g_frame_step.FrameEnd(queue);
    WWMI_GUARD_END_VOID()
}
