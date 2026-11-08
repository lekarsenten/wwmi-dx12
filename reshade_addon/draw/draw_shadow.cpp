// draw_shadow.cpp -- a character's depth / shadow pass recognised, so the mod components are cast into it.
// See draw_parts.h.
#include "../addon_common.h"
#include "draw_hook.h"
#include "../mods/character_recognition.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "draw_context.h"
#include "draw_parts.h"
#include "draw_inject.h"
#include <set>
#include "draw_toggles.h"
#include "../hooks/command_list_registry.h"
#include "../instrumentation/diag_arms.h"
#include "../mods/mod_set.h"

using namespace reshade::api;

namespace {
struct ShadowTarget {
    uint64_t dsv;
    uint32_t pass;
    float vx;
    float vy;
    float vw;
    float vh;
    bool Same(const ShadowTarget& t) const
    {
        return t.dsv == dsv && t.pass == pass && t.vx == vx && t.vy == vy && t.vw == vw && t.vh == vh;
    }
};

// The depth targets the mod components were cast into this frame, so each is replaced once, and the SHADOWRECOG /
// SHADOWDRAW watch counters.
class ShadowCastTracker {
public:
    // The pass recognised to the given stage: 0 = a character depth pass, 1 = its mesh claimed, 2 = replaced.
    void Recognised(int stage);
    // True = this draw casts the components into the target; else `replaced` says whether an earlier one did.
    bool Claim(const ShadowTarget& t, bool* replaced);
    void NoteReplaced(const ShadowTarget& t, bool replaced);
    bool WatchFresh(uint64_t key); // SHADOWDRAW: a (target, outcome) not logged in the last 120 frames

private:
    std::mutex mutex_;
    uint32_t frame_ = 0xFFFFFFFFu;
    std::vector<std::pair<ShadowTarget, bool>> done_; // depth targets handled this frame, and whether replaced
    std::mutex recog_mutex_;
    uint32_t recog_frame_ = 0xFFFFFFFFu;
    uint32_t recog_frames_ = 0;
    uint32_t n_pass_ = 0;
    uint32_t n_mesh_ = 0;
    uint32_t n_ok_ = 0;
    std::mutex watch_mutex_;
    std::set<uint64_t> watch_seen_;
    uint32_t watch_frame_ = 0xFFFFFFFFu;
};
ShadowCastTracker g_shadow_casts;

void ShadowCastTracker::Recognised(int stage)
{
    if (g_diag_arms.shadow_recog_watch.load(std::memory_order_relaxed) <= 0) return;
    std::lock_guard l(recog_mutex_);
    if (recog_frame_ != g_frame_id) {
        recog_frame_ = g_frame_id;
        if (++recog_frames_ % 120 == 0) {
            logger::write("SHADOWRECOG last-120-frames: char-depth-passes=%u meshClaimed=%u fullyRecognised=%u"
                          "  (passes>recognised => some frames fall back to the VANILLA shadow = flicker)",
                n_pass_, n_mesh_, n_ok_);
            n_pass_ = n_mesh_ = n_ok_ = 0;
        }
    }
    ++n_pass_;
    if (stage >= 1) ++n_mesh_;
    if (stage >= 2) ++n_ok_;
}

bool ShadowCastTracker::Claim(const ShadowTarget& t, bool* replaced)
{
    std::lock_guard l(mutex_);
    if (frame_ != g_frame_id) {
        frame_ = g_frame_id;
        done_.clear();
    }
    for (const auto& d : done_)
        if (d.first.Same(t)) {
            *replaced = d.second;
            return false;
        }
    done_.emplace_back(t, false);
    return true;
}

void ShadowCastTracker::NoteReplaced(const ShadowTarget& t, bool replaced)
{
    std::lock_guard l(mutex_);
    for (auto& d : done_)
        if (d.first.Same(t)) d.second = replaced;
}

bool ShadowCastTracker::WatchFresh(uint64_t key)
{
    std::lock_guard l(watch_mutex_);
    if (watch_frame_ != g_frame_id && (g_frame_id % 120) == 0) {
        watch_frame_ = g_frame_id;
        watch_seen_.clear();
    } // re-report every 120 frames so a LATER failure is visible
    return ProbeArmed() && watch_seen_.size() < 64 && watch_seen_.insert(key).second;
}
} // namespace

void RecogniseShadowPass(DrawContext& ctx)
{
    const DrawArgs& da = ctx.args;
    DrawClaim& dc = ctx.claim;
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint32_t index_count = da.index_count;
    const uint32_t first_index = da.first_index;
    const uint64_t vs_hash = da.vs_hash;
    if (index_count >= 64 && !dc.mc && Comps().Loaded() &&
        !g_draw_toggles.shadow_vanilla) { // OFF = draw mod silhouette; ON = let game draw vanilla
        const CmdRT& rt = da.draw_rt;
        if (rt.rtv_count == 0 && rt.has_dsv) { // depth/shadow caster
            const int b4r = ctx.roots.Get().b4;
            const int b3r = ctx.roots.Get().b3;
            const RootCbvBind b4_cb = ctx.roots.Get().b4cb;
            // The claim recognised this depth draw (its mesh + range) and released it: that is the identity.
            const bool char_mesh = dc.depth_claim != nullptr;
            g_shadow_casts.Recognised(char_mesh ? 1 : 0); // 1 = mesh claimed, 0 = not ours (see the counters above)
            bool any_mod_onscreen = false;
            for (int dm = 0; dm < (int)Mods().Count(); ++dm)
                if (Mods().OnScreen(dm) && Mods().GeomEnabled(dm)) {
                    any_mod_onscreen = true;
                    break;
                } // ENABLED mod (a disabled mod keeps its vanilla shadow)
            int shadow_comp = -1;
            if (dc.depth_claim)
                shadow_comp = Comps().IndexOf(dc.depth_claim);
            else
                shadow_comp = Comps().SlotOf(first_index, index_count);
            const bool comp_range = shadow_comp >= 0 && Mods().OnScreen(Comps().At(shadow_comp).mod_id) &&
                Mods().GeomEnabled(Comps().At(shadow_comp).mod_id);
            if (char_mesh && any_mod_onscreen && comp_range) {
                dc.full_shadow = true;
                t_x23_shadow = 1; // X23: the game-draw table's `shadow` column
                dc.shadow_dsv = rt.dsv; // which shadow map this pass casts into (per light / per cascade)
                dc.shadow_pass = rt.pass_gen; // ...and which pass wrote it, in case the target is reused
                dc.b4_root = b4r;
                dc.b3_root = -1;
                g_shadow_casts.Recognised(2); // 2 = fully recognised, this pass will be replaced
                if (g_debug) {
                    const uint64_t k = vs_hash ^ ((uint64_t)index_count << 24);
                    if (DiagOnce("DRAW-VSCOUNT", (int)(k ^ (k >> 32)))) {
                        logger::write("SHADOWCLAIM FULL vs=%016llx first=%u count=%u b4root=%d b3root=%d boneVA=%llx",
                            vs_hash, first_index, index_count, b4r, b3r, (unsigned long long)b4_cb.va);
                        uint64_t dvb0 = 0;
                        {
                            dvb0 = g_command_lists.Gfx(cmd_list).vb[0].va;
                        }
                        bool d_pool = g_recognition.InSkinBand(dvb0);
                        logger::trace("SKINSRC pass=DEPTHCAST idx=%u vb0=%llx inSkinPool=%d b4root=%d boneVA=%llx",
                            index_count, (unsigned long long)dvb0, (int)d_pool, b4r, (unsigned long long)b4_cb.va);
                    }
                }
            }
        }
    }
}

bool CastModShadows(DrawContext& ctx)
{
    reshade::api::command_list* const cmd_list = ctx.args.cmd_list;
    const DrawClaim& dc = ctx.claim;
    PROF(PROF_DI_ShadowCast); // N mods x N components per depth target, all inside one game draw
    ShadowTarget target{ dc.shadow_dsv, dc.shadow_pass, 0.0f, 0.0f, 0.0f, 0.0f };
    {
        auto& gs = g_command_lists.Gfx(cmd_list);
        if (gs.vp_count) {
            target.vx = gs.vp[0].TopLeftX;
            target.vy = gs.vp[0].TopLeftY;
            target.vw = gs.vp[0].Width;
            target.vh = gs.vp[0].Height;
        } // never set -> one region, old behaviour
    }
    bool already_replaced = false;
    const bool cast_here = g_shadow_casts.Claim(target, &already_replaced);
    if (cast_here) {
        t_x23_cast = 1; // X23
        int comp_n = 0;
        int drew_n = 0;
        for (int dm = 0; dm < (int)Mods().Count(); ++dm) {
            if (!Mods().OnScreen(dm)) continue;
            if (!Mods().GeomEnabled(dm)) continue; // WWMI: disabled mod casts its vanilla shadow, not ours
            for (const auto& c : Mods().At(dm).mod.comps) {
                ++comp_n;
                if (DrawComponent(ctx, dm, &c)) ++drew_n;
            }
        }
        already_replaced = (drew_n > 0);
        g_shadow_casts.NoteReplaced(target, already_replaced);
        if (g_diag_arms.shadow_recog_watch.load(std::memory_order_relaxed) > 0) {
            const uint64_t vkey = ((uint64_t)(uint32_t)target.vx << 32) ^ (uint64_t)(uint32_t)target.vy ^
                ((uint64_t)(uint32_t)target.vw << 16) ^ (uint64_t)(uint32_t)target.vh;
            if (g_shadow_casts.WatchFresh(
                    (dc.shadow_dsv ^ ((uint64_t)dc.shadow_pass << 32)) ^ ((uint64_t)(drew_n > 0) << 63) ^ vkey)) {
                logger::write("SHADOWDRAW dsv=%llx pass=%u vp=[%.0f,%.0f %.0fx%.0f] comps=%d drew=%d "
                              "cb13root=%d cb14root=%d -> %s",
                    (unsigned long long)dc.shadow_dsv, dc.shadow_pass, target.vx, target.vy, target.vw, target.vh,
                    comp_n, drew_n, dc.b4_root, dc.b3_root,
                    drew_n > 0 ? "REPLACED (suppress vanilla)" : "NOT replaced (vanilla kept)");
            }
        }
    }
    RestoreGameState(ctx);
    return already_replaced; // suppress only what we actually replaced
}
