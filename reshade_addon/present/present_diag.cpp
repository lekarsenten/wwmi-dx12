// present_diag.cpp -- see present_diag.h.
#include "../addon_common.h"
#include "present_diag.h"
#include "../mods/character_recognition.h"
#include "../rabbitfx/rabbitfx_bind.h"
#include "../hooks/pipeline_registry.h" // PipelineRegistry::TakeStats
#include "../draw/draw_toggles.h"
#include "frame_sync.h"
#include "present_toggles.h"
#include "../rabbitfx/pso_clone.h"
#include "../rabbitfx/rfx_patch.h"
#include "../rabbitfx/rfx_toggles.h"
#include "../shapekeys/shapekey_chain.h"
#include "../shapekeys/sk_toggles.h"
#include "../migoto/port_api.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/dx12_bridge.h"
#include "../textures/tex_toggles.h"
#include "../textures/mod_texture_store.h"
#include "../migoto/shim_unmet.h"
#include "../addon/addon.h"
#include "../mods/mod_set.h"

using namespace reshade::api;

PresentDiag g_present_diag;

void PresentDiag::MonitorConfig()
{
    if (g_addon.cfg_engine_path.empty()) return;
    int fl = (int)GetPrivateProfileIntW(
        L"SystemSettings", L"r.Streaming.FullyLoadUsedTextures", -1, g_addon.cfg_engine_path.c_str());
    int lod = (int)GetPrivateProfileIntW(L"SystemSettings", L"r.MipMapLODBias", -999, g_addon.cfg_engine_path.c_str());
    if (fl != cfg_fully_load_ || lod != cfg_lod_bias_) {
        logger::write("CFGWATCH Engine.ini: FullyLoadUsedTextures=%d MipMapLODBias=%d%s", fl, lod,
            (cfg_fully_load_ == -2) ? " (initial)" : " <-CHANGED");
        cfg_fully_load_ = fl;
        cfg_lod_bias_ = lod;
    }
    int img = -1; // DB is SQLite (no lib in-addon): raw-scan for the "ImageDetail" data row
    {
        FILE* f = _wfsopen(g_addon.cfg_db_path.c_str(), L"rb", _SH_DENYNO);
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0 && n < 8 * 1024 * 1024) {
                std::vector<char> b(n);
                if (fread(b.data(), 1, n, f) == (size_t)n)
                    for (long i = 0; i + 12 < n; ++i)
                        if (memcmp(b.data() + i, "ImageDetail", 11) == 0)
                            if (b[i + 11] >= '0' && b[i + 11] <= '9') img = b[i + 11] - '0';
            } // keep last (newest) match
            fclose(f);
        }
    }
    if (img != cfg_image_detail_) {
        logger::write(
            "CFGWATCH LocalStorage.db ImageDetail=%d%s", img, (cfg_image_detail_ == -2) ? " (initial)" : " <-CHANGED");
        cfg_image_detail_ = img;
    }
}

void PresentDiag::QueueDiag(command_queue* queue)
{
    if (g_debug) {
        const void* pq = queue ? (const void*)queue->get_native() : nullptr;
        const void* gq = g_frames.claim_exec_queue.load(std::memory_order_relaxed);
        if (pq && gq) {
            const bool same = (pq == gq);
            (same ? g_present_stats.presentq_same : g_present_stats.presentq_diff)
                .fetch_add(1, std::memory_order_relaxed);
            if (pq != presentq_queue_ || gq != presentq_claim_queue_ || (int)same != presentq_same_) {
                presentq_queue_ = pq;
                presentq_claim_queue_ = gq;
                presentq_same_ = (int)same;
                logger::write("PRESENTQ %s: [Present] executes on queue %p (this thread %u); the list "
                              "carrying the last claimed draw executed on queue %p (recorded on thread %u)%s",
                    same ? "SAME" : "DIFFERENT", pq, (unsigned)GetCurrentThreadId(), gq,
                    g_frames.claim_thread.load(std::memory_order_relaxed),
                    same ? ""
                         : " -- the [Present] copy of the merged skeleton is unordered against "
                           "the merges that write it and the draws that read it");
            }
        }
    }
}

void PresentDiag::ConfigWatchTick()
{
    if (g_debug) {
        if (++config_watch_frames_ >= 60) {
            config_watch_frames_ = 0;
            MonitorConfig();
        }
    } // ~1/sec config watch
}

void PresentDiag::SeedHeartbeat()
{
    if (!logger::enabled(3)) return;
    if (++seed_frames_ >= 60) {
        seed_frames_ = 0;
        const size_t mN = g_recognition.CharMeshCount();
        uint64_t s[3] = {};
        size_t ns = 0;
        g_recognition.ForEachCharMeshVb0([&](uint64_t va, const CharVbOrigin&) {
            s[ns++] = va;
            return ns == 3;
        });
        const size_t sN = g_recognition.SkinOutCount();
        uint64_t vmin;
        uint64_t vmax;
        uint32_t lg;
        uint32_t pl;
        g_recognition.TakeHeartbeat(&vmin, &vmax, &lg, &pl);
        logger::trace(
            "SEEDHB meshVB0=%zu[%llx %llx %llx] skinOut=%zu largeDraws=%u poolBand=%u vb0range=[%llx..%llx] band=[%llx..%llx]",
            mN, s[0], s[1], s[2], sN, lg, pl, (lg ? vmin : 0ull), vmax, g_recognition.SkinBandLo(),
            g_recognition.SkinBandHi());
    }
}

void PresentDiag::Heartbeat(uint32_t texinj_last)
{
    if (logger::enabled(1)) {
        if (g_frame_id >= heartbeat_next_) {
            heartbeat_next_ = g_frame_id + 300;
            std::string per;
            for (size_t i = 0; i < Mods().Count(); ++i) {
                const std::wstring& uns = Mods().At(i).wcl_ns;
                char b[288]; // widened with sect/skip/rep; the longest ns in the library is ~50 chars
                _snprintf_s_l(b, sizeof(b), _TRUNCATE, " [%zu %ls age=%u det=%g en=%g sect=%u skip=%u rep=%u]",
                    logger::utf8_locale(), i, uns.empty() ? L"(no-ns)" : uns.c_str(),
                    (uint32_t)(g_frame_id - Mods().At(i).last_char_frame), Mods().At(i).det_at_keys,
                    PortApi::GetVarNs(uns.c_str(), L"mod_enabled", -1.f), Mods().At(i).sect_runs.last.load(),
                    Mods().At(i).skip_orig.last.load(), Mods().At(i).replaced.last.load());
                per += b;
            }
            const size_t learned = g_recognition.CharMeshCount();
            const long grew = (long)learned - (long)heartbeat_learned_;
            heartbeat_learned_ = learned;
            const uint32_t rfx_now = g_rfx_stats.tex_count.load();
            const uint32_t rfx_win = rfx_now - heartbeat_rfx_;
            heartbeat_rfx_ = rfx_now;
            logger::report("HEARTBEAT f=%u replaced=%u texinj=%u learnedVB=%zu(%+ld) "
                           "portDisp=%llu portDispCall=%llu "
                           "sect=%u nocb=%u drewnothing=%u refusedNoCL=%u unmet=%u rfx=%u gmiss=%u%s",
                g_frame_id, g_draw_stats.draw_replace_count.last.load(), texinj_last, learned, grew,
                MigotoPort::DispatchesIssued(), MigotoPort::DispatchCalls(), g_draw_stats.port_sect_runs.last.load(),
                g_draw_stats.port_sect_nocb.last.load(), g_draw_stats.port_fellback_count.last.load(),
                PortApi::TakeRefusedNoCl(), MigotoPort::g_shim_unmet.TakeCount(), rfx_win,
                g_tex_stats.global_miss.load(), per.c_str());
            {
                const PipelineHookStats ph = g_pipelines.TakeStats();
                const RfxPatchStats cs = g_rfx_patch.Stats();
                MEMORYSTATUSEX ms{};
                ms.dwLength = sizeof ms;
                GlobalMemoryStatusEx(&ms);
                logger::report(
                    "PIPESTAT f=%u since the last: %llu pipeline(s) created, %.1f ms in our hook (slowest %.2f ms); "
                    "%llu destroyed, %.1f ms | patch totals: %u patched, %u not, %u variant(s) | RAM free %.1f GB, "
                    "commit %.1f of %.1f GB",
                    g_frame_id, (unsigned long long)ph.created, ph.created_us / 1000.0, ph.created_max_us / 1000.0,
                    (unsigned long long)ph.destroyed, ph.destroyed_us / 1000.0, cs.ps_patched, cs.ps_failed,
                    cs.variants, ms.ullAvailPhys / 1073741824.0,
                    (ms.ullTotalPageFile - ms.ullAvailPageFile) / 1073741824.0, ms.ullTotalPageFile / 1073741824.0);
            }
        }
    }
}

void PresentDiag::HealthHeartbeat()
{
    if (g_debug && g_present_toggles.health_heartbeat && (g_frame_id % 600) == 0 && g_frame_id) {
        std::string ages;
        for (int md = 0; md < (int)Mods().Count(); ++md) {
            const uint32_t a = Mods().ClaimAge(md);
            char b[48];
            snprintf(b, sizeof(b), "%s%d:%s", ages.empty() ? "" : " ", md,
                (a == 0xFFFFFFFFu) ? "never" : std::to_string(a).c_str());
            ages += b;
        }
        logger::write("HEARTBEAT frame=%llu srv_heap=%u/%u geomods=%zu | claim ages: %s",
            (unsigned long long)g_frame_id, g_mod_textures.SrvHeapSize(), g_mod_textures.SrvHeapCap(), Mods().Count(),
            ages.c_str());
        logger::write("BRIDGE foreign-list=%llu", MigotoPort::BridgeForeignListRefusals());
        logger::write("CHARVB claims=%u xmod=%u refused=%u (H17 refuse xmod: %d)", g_draw_stats.charvb_claims.load(),
            g_draw_stats.charvb_xmod.load(), g_draw_stats.charvb_refused.load(),
            (int)g_draw_toggles.charvb_refuse_xmod);
        {
            const PsoCloneStats ps = g_pso_clones.Stats();
            logger::write(
                "PSOCLONE held=%zu (%.1f MB) shared code: %zu shader(s), %.1f MB; forgotten=%u variants_released=%u",
                ps.clones, ps.bytes / (1024.0 * 1024.0), ps.code_blobs, ps.code_bytes / (1024.0 * 1024.0), ps.forgotten,
                ps.variants_released);
        }
        {
            const RfxPatchStats cs = g_rfx_patch.Stats();
            logger::write(
                "RFXPATCH (E2 on: %d) ps_patched=%u ps_failed=%u variants=%u draws=%u glow_draws=%u nobind=%u mirror_null_slots=%llu",
                (int)g_rfx_toggles.patch, cs.ps_patched, cs.ps_failed, cs.variants, g_rfx_stats.patch_draws.load(),
                g_rfx_stats.glow_count.load(), g_rfx_stats.patch_nobind.load(), g_rfx_stats.mirror_misses.load());
        }
        logger::write(
            "VBNULL-UNBIND count=%llu (executed `vbN = null` as a real D3D12 unbind)", MigotoPort::VbNullUnbinds());
        {
            const unsigned long long c = g_draw_stats.vb6_readers_claimed.load(std::memory_order_relaxed);
            const unsigned long long u = g_draw_stats.vb6_readers_unclaimed.load(std::memory_order_relaxed);
            logger::write("VB6-READERS since-last: claimed=%llu unclaimed=%llu (draws whose slot 6 is a buffer "
                          "we widened; unclaimed = the vanilla morph output drawn as-is, named once each above)",
                c - vb6_claimed_last_, u - vb6_unclaimed_last_);
            vb6_claimed_last_ = c;
            vb6_unclaimed_last_ = u;
        }
        {
            const unsigned long long s = g_present_stats.presentq_same.load(std::memory_order_relaxed);
            const unsigned long long q = g_present_stats.presentq_diff.load(std::memory_order_relaxed);
            const unsigned long long e = g_frames.claim_exec_seen.load(std::memory_order_relaxed);
            logger::write("PRESENTQ since-last: same=%llu different=%llu (presents whose queue is / is not the "
                          "queue that executed a list with a claimed draw; %llu such executes) present-thread=%u "
                          "draw-thread=%u",
                s - presentq_same_last_, q - presentq_diff_last_, e - claim_exec_last_, (unsigned)GetCurrentThreadId(),
                g_frames.claim_thread.load(std::memory_order_relaxed));
            presentq_same_last_ = s;
            presentq_diff_last_ = q;
            claim_exec_last_ = e;
        }
        {
            const unsigned long long f = g_draw_stats.freeze_binds.load(std::memory_order_relaxed);
            logger::write("FREEZE-BIND since-last: %llu (identity bones bound inside the section's draw for a "
                          "component with overlay F ticked; 0 with F ticked = the freeze is not reaching the draw)",
                f - freeze_binds_last_);
            freeze_binds_last_ = f;
        }
        {
            unsigned long long c0;
            unsigned long long o0;
            unsigned long long r0;
            unsigned long long c1;
            unsigned long long o1;
            unsigned long long r1;
            SkZeroStats(TargetClass::UnorderedAccessView, 0, &c0, &o0, &r0);
            SkZeroStats(TargetClass::UnorderedAccessView, 1, &c1, &o1, &r1);
            logger::write("UAVCLEAR-RATE u0 cleared=%llu not_ours=%llu not_ready=%llu | u1 cleared=%llu "
                          "not_ours=%llu not_ready=%llu",
                c0, o0, r0, c1, o1, r1);
        }
        { /* Which record answered the u0 table, since the last report. */
            const unsigned c = g_sk_stats.u0tab_compute.load(std::memory_order_relaxed);
            const unsigned f = g_sk_stats.u0tab_fallback.load(std::memory_order_relaxed);
            const unsigned n = g_sk_stats.u0tab_none.load(std::memory_order_relaxed);
            if ((c - u0tab_compute_last_) + (f - u0tab_fallback_last_) + (n - u0tab_none_last_))
                logger::write("U0TABLE since-last: compute=%u fallback=%u none=%u | totals %u/%u/%u "
                              "(fallback can hold another dispatch's or the graphics bind)",
                    c - u0tab_compute_last_, f - u0tab_fallback_last_, n - u0tab_none_last_, c, f, n);
            u0tab_compute_last_ = c;
            u0tab_fallback_last_ = f;
            u0tab_none_last_ = n;
        }
        { /* Totals AND the delta since the last report -- see the counters' declaration. */
            const unsigned ok = g_sk_stats.claim_ok.load(std::memory_order_relaxed);
            const unsigned nd = g_sk_stats.claim_nodecl.load(std::memory_order_relaxed);
            const unsigned of = g_sk_stats.claim_offscreen.load(std::memory_order_relaxed);
            const unsigned mu = g_sk_stats.claim_multi.load(std::memory_order_relaxed);
            const unsigned d = (ok - claim_ok_last_) + (nd - claim_nodecl_last_) + (of - claim_offscreen_last_) +
                (mu - claim_multi_last_);
            if (d)
                logger::write("CLAIMGATE since-last: ok=%u no-callback-declares=%u none-on-screen=%u "
                              "multi=%u | totals ok=%u/%u (%.0f%%)",
                    ok - claim_ok_last_, nd - claim_nodecl_last_, of - claim_offscreen_last_, mu - claim_multi_last_,
                    ok, ok + nd + of + mu,
                    100.0 * (double)ok / (double)((ok + nd + of + mu) ? (ok + nd + of + mu) : 1));
            claim_ok_last_ = ok;
            claim_nodecl_last_ = nd;
            claim_offscreen_last_ = of;
            claim_multi_last_ = mu;
        }
    }
}
