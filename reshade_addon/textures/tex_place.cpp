// Placement: mod textures into the game's descriptor heaps at placement time. See tex_place.h.
#include "tex_place.h"
#include "tex_identity.h"
#include "tex_rules.h"
#include "tex_select.h"
#include "tex_lod.h"
#include "tex_slot_log.h"
#include "tex_toggles.h"
#include "mod_texture_store.h"
#include "../hooks/descriptor_shadow.h"
#include "../hooks/stamp_ledger.h"
#include "../instrumentation/tex_timing.h"
#include "../instrumentation/diag_gate.h"
#include "../draw/draw_toggles.h"
#include "../addon/addon.h"
#include "../core/frame_clock.h"
#include "../core/log.h"
#include "../migoto/port_api.h"
#include "../mods/mod_set.h"
#include <chrono>
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <vector>

TexPlacement g_tex_place;

static thread_local int t_queued_sel = -1; // ApplyQueued: the pick to stamp, no section run

void TexPlacement::NotePlacement(uint32_t rule, uint64_t heap, uint32_t slot, bool ok)
{
    if (!g_debug) return;
    std::lock_guard l(place_mutex_);
    PlaceStat& s = place_stats_[rule];
    ++s.placed;
    if (!ok) ++s.missed;
    s.slots.insert({ heap, slot });
}

void TexPlacement::Report()
{
    if (!g_debug) return;
    std::lock_guard l(place_mutex_);
    for (const auto& [rule, s] : place_stats_)
        if (s.missed)
            logger::write("TEXPLACE rule=%08x placed=%u missed=%u distinct_slots=%zu (last 600 frames)", rule, s.placed,
                s.missed, s.slots.size());
    place_stats_.clear();
}

int TexPlacement::SnapshotSelection(uint32_t rule)
{
    std::shared_lock l(sel_snap_mutex_);
    auto it = sel_snap_.find(rule);
    if (it == sel_snap_.end() || g_frame_id - it->second.frame > 120u) return -1;
    return it->second.sel;
}

static thread_local char t_tex_decline[96];

const char* TexPlacement::Decline()
{
    return t_tex_decline;
}

bool TexPlacement::Swap(ID3D12Device* dev, uint64_t heap_handle, uint32_t slot, ID3D12Resource* res)
{
    t_tex_decline[0] = 0;
    if (g_addon.disabled) {
        strcpy_s(t_tex_decline, "addon disabled");
        return false;
    }
    if (!g_tex_toggles.placement_swap || g_tex_toggles.disable || !g_tex_rules.Ready() || !dev || !res ||
        !heap_handle) {
        sprintf_s(t_tex_decline, "global_tex=%d disable_tex=%d texovr_ready=%d", (int)g_tex_toggles.placement_swap,
            (int)g_tex_toggles.disable, (int)g_tex_rules.Ready());
        return false;
    }
    if (g_tex_rules.Now().hash_to_rule.empty()) {
        strcpy_s(t_tex_decline, "no texture rules");
        return false;
    }
    bool candidate = false;
    const uint32_t hash =
        g_tex_identity.HashAndHashable(reinterpret_cast<uint64_t>(res), g_tex_toggles.lineage, &candidate);
    // any texture an upload can name, not BC only:
    //   a mod may replace an R8 mask
    if (candidate) g_stamps.NotePlacement(reinterpret_cast<uint64_t>(res), { heap_handle, slot });
    uint32_t rule_hash = g_tex_select.RuleHashFor(hash);
    constexpr bool sticky = false; // GTEXRATE log field, kept stable
    if (!rule_hash) { // not a mod texture / variant of one
        sprintf_s(t_tex_decline, "hash %08x has no rule", hash);
        return false;
    }
    ModTextureStore::Entry rep_e; // the SRV to stamp AND the resource it views
    int sel = -1;
    unsigned loading_gen = 0;
    const void* loading = nullptr;
    if (t_queued_sel >= 0) {
        sel = t_queued_sel;
        g_mod_textures.TouchAt(sel, g_tex_select.DrawnMip(hash, rule_hash));
    } else {
        sel = g_tex_select.SelectIdx(rule_hash, g_tex_select.DrawnMip(hash, rule_hash));
        loading = PortApi::SelectionLoading(&loading_gen);
    }
    const bool from_snapshot = sel < 0;
    if (from_snapshot) {
        sel = SnapshotSelection(rule_hash);
        if (sel >= 0 && g_debug && DiagOnce("TEXSEL-SNAP", (int)rule_hash))
            logger::write(
                "TEXSEL-SNAP rule=%08x: its section selects nothing at placement; used the selection %d made at its mod's draw-time state",
                rule_hash, sel);
    }
    if (!g_mod_textures.EntryAt(sel, &rep_e) ||
        rep_e.srv.ptr == 0) { // TexSelector::Replacement's test: honors $var/key conditionals
        g_descriptors.SetMiss({ heap_handle, slot }, g_frame_id);
        ++g_tex_stats.global_miss;
        TexTiming::NoteNeeded(rule_hash);
        if (g_debug) {
            const int wsel = g_mod_textures.IndexOf(PortApi::SelectTextureOverride(rule_hash));
            ModTextureStore::Entry we;
            const bool have = wsel >= 0 && g_mod_textures.EntryAt(wsel, &we);
            sprintf_s(t_tex_decline, "rule %08x: section selects %d, %s", rule_hash, wsel,
                wsel < 0         ? (loading ? "its pick loading: queued" : "no texture (conditional or no section)")
                    : !have      ? "entry missing"
                    : we.srv.ptr ? "entry has SRV"
                                 : "entry not loaded yet (no SRV)");
        }
        NotePlacement(rule_hash, heap_handle, slot, false);
#ifdef WWMI_DEBUG
        if (g_tex_trace.On(rule_hash))
            g_tex_trace.Line("%s res=%p heap=%llx slot=%u: its section selects nothing%s; the slot is left as it is",
                t_tex_swap_site ? t_tex_swap_site : "place", (void*)res, (unsigned long long)heap_handle, slot,
                loading ? ", its pick still loading" : "");
#endif
        if (g_debug && !t_tex_swap_site)
            g_tex_slot_log.Note("place", res, hash, rule_hash, nullptr, false);
        if (candidate) {
            const uint64_t held = reinterpret_cast<uint64_t>(res);
            std::lock_guard l(missed_mutex_); // its slot is recorded: retry it later
            missed_.insert(held);
            if (loading) queued_[held] = QueuedPick{ loading, loading_gen, rule_hash, ++queued_seq_ };
            queued_n_.store(queued_.size(), std::memory_order_release);
        }
        if (loading && candidate && g_debug && DiagOnce("TEXQUEUE", (int)rule_hash))
            logger::write("TEXQUEUE f=%u rule=%08x res=%p: its section picked a texture still loading -- stamped "
                          "when it is described",
                g_frame_id, rule_hash, (void*)res);
        if (g_debug) {
            static ProbeBudget missed;
            const int n = missed.index(2000);
            if (n >= 0)
                logger::write("GTEXMISS f=%u n=%u rule=%08x boundHash=%08x heap=%llx slot=%u sections_run=%u",
                    g_frame_id, (unsigned)n, rule_hash, hash, (unsigned long long)heap_handle, slot,
                    g_draw_stats.sections_this_frame.load(std::memory_order_relaxed));
            else if (n == 2000)
                logger::write("GTEXMISS capped at 2000");
        }
        return false;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE rep = rep_e.srv;
    auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(heap_handle);
    uint32_t inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    D3D12_CPU_DESCRIPTOR_HANDLE dst = { heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)slot * inc };
    g_stamps.BackupBeforeStamp(heap_handle, slot, rep.ptr);
    dev->CopyDescriptorsSimple(1, dst, rep, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_descriptors.SetSwapped({ heap_handle, slot }, rep_e.res); // the frame dump publishes what the GPU reads
    g_stamps.StampedStoreTexture(heap_handle, slot, rep_e.res);
    ++g_tex_stats.global_count;
    TexTiming::NoteSwapped(rep_e.res, rule_hash);
    NotePlacement(rule_hash, heap_handle, slot, true);
#ifdef WWMI_DEBUG
    if (g_tex_trace.On(rule_hash))
        g_tex_trace.Line("%s res=%p heap=%llx slot=%u: stamped %ls%s", t_tex_swap_site ? t_tex_swap_site : "place",
            (void*)res, (unsigned long long)heap_handle, slot, g_tex_trace.ModText(rep_e.res).c_str(),
            from_snapshot ? " -- from the selection snapshot" : "");
#endif
    if (queued_n_.load(std::memory_order_acquire)) {
        std::lock_guard l(missed_mutex_);
        queued_.erase(reinterpret_cast<uint64_t>(res));
        queued_n_.store(queued_.size(), std::memory_order_release);
    }
    if (g_debug)
        g_tex_slot_log.Note(
            t_tex_swap_site ? t_tex_swap_site : "place", res, hash, rule_hash, rep_e.res, from_snapshot);
    g_tex_rules.NoteSwap(rule_hash);
    if (g_debug && ProbeArmed() && DiagOnce("GTEXSWAP", (int)rule_hash))
        logger::write("GTEXSWAP rule_hash=%08x boundHash=%08x slot=%u", rule_hash, hash, slot);

    if (g_debug) {
        static ProbeBudget emitted;
        const int n = emitted.index(4000);
        if (n >= 0)
            logger::write("GTEXRATE f=%u n=%u rule=%08x boundHash=%08x slot=%u sticky=%d", g_frame_id, (unsigned)n,
                rule_hash, hash, slot, sticky ? 1 : 0);
        else if (n == 4000)
            logger::write("GTEXRATE capped at 4000 stamps -- the volume IS the finding");
    }
    return true;
}

void TexPlacement::HoldForRetry(uint64_t res)
{
    if (!res) return;
    std::lock_guard l(g_tex_place.missed_mutex_);
    g_tex_place.missed_.insert(res);
}

void TexPlacement::ForgetMissed(uint64_t res)
{
    std::lock_guard l(missed_mutex_);
    missed_.erase(res);
    queued_.erase(res);
    queued_n_.store(queued_.size(), std::memory_order_release);
}

void TexPlacement::RetryMissed(const char* why, bool quiet_if_none)
{
    if (!g_tex_toggles.placement_swap || g_tex_toggles.disable) return;
    std::unordered_set<uint64_t> held;
    {
        std::lock_guard l(missed_mutex_);
        held.swap(missed_);
    }
    if (held.empty()) return;
    uint32_t swapped = 0;
    const char* const site_was = t_tex_swap_site;
    t_tex_swap_site = "retry";
    for (uint64_t r : held)
        if (g_stamps.Restamp(r)) ++swapped; // a miss again re-enters the set
    t_tex_swap_site = site_was;
    if (g_debug && (swapped || !quiet_if_none))
        logger::write("TEXRETRY f=%u: %s -- %zu held placement(s) retried, %u now swapped", g_frame_id, why,
            held.size(), swapped);
}

void TexPlacement::SnapshotSelections(const wchar_t* ns)
{
    if (!ns || !*ns) return;
    std::vector<std::pair<uint32_t, int>> sel;
    for (uint32_t r : g_tex_rules.RulesOfNs(ns))
        sel.emplace_back(r, g_tex_select.SelectIdx(r, ModTextureStore::kMipUnknown));
    std::unique_lock l(sel_snap_mutex_);
    for (const auto& [r, s] : sel)
        if (s >= 0)
            sel_snap_[r] = SelSnap{ s, g_frame_id };
        else
            sel_snap_.erase(r);
}

void TexPlacement::SnapshotsMove(const std::unordered_map<int, int>& index_to)
{
    if (index_to.empty()) return;
    std::unique_lock l(sel_snap_mutex_);
    for (auto& [rule, snap] : sel_snap_) {
        const auto it = index_to.find(snap.sel);
        if (it != index_to.end()) snap.sel = it->second;
    }
}

void TexPlacement::UnstampSwitchedOff(const wchar_t* ns)
{
    if (g_addon.disabled || !ns || !*ns) return;
    const std::unordered_set<uint32_t> rules = g_tex_rules.RulesOfNs(ns);
    if (rules.empty()) return;
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<std::pair<HeapKey, uint64_t>> stamped = g_stamps.StampedCpuOnlySlots();
    std::erase_if(stamped,
        [&](const auto& s) { return !rules.count(g_tex_select.RuleHashFor(g_tex_identity.HashOf(s.second))); });
    unsigned restored = 0;
    for (const auto& [key, res] : stamped) {
        if (g_stamps.UnstampSlot(key.heap, key.offset)) {
            ++restored;
            HoldForRetry(res);
            continue;
        }
        ResInfo ri{};
        HeapKey src{};
        if (!g_descriptors.Find(key, ri)) continue;
        const bool tracked = g_descriptors.SlotOfCpu(ri.view_handle, &src);
        logger::write(
            "TEXOFF-KEPT f=%u heap %llx slot %u res=%p: source view %llx (%s heap %llx slot %u), slot written by src %u",
            g_frame_id, (unsigned long long)key.heap, key.offset, (void*)res, (unsigned long long)ri.view_handle,
            tracked ? "tracked" : "untracked", (unsigned long long)src.heap, src.offset, (unsigned)ri.wrote_src);
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logger::write("TEXOFF f=%u ns='%ls': switched itself off -- %u of %zu stamped slot(s) in CPU-only heaps under its "
                  "%zu texture rule(s) given the game's descriptor back (%.2f ms)",
        g_frame_id, ns, restored, stamped.size(), rules.size(), ms);
}

void TexPlacement::SwapsSwitched(bool were_live)
{
    const bool live = g_tex_toggles.placement_swap && !g_tex_toggles.disable;
    if (live == were_live || g_addon.disabled) return;
    if (live) {
        RetryMissed("texture swaps back on (C1/C2)");
        return;
    }
    const UnstampTally t = g_stamps.UnstampAll(HoldForRetry);
    logger::write("C1/C2: texture swaps off -- %u stamped slot(s) given the game's texture back, and %u of %zu still "
                  "stamped in CPU-only heaps",
        t.recorded, t.cpu_only, t.cpu_only_stamped);
}

void TexPlacement::LookAlikesSwitched(bool on)
{
    std::vector<uint64_t> carriers;
    g_tex_identity.ForEachHash([&](uint64_t res, uint32_t hash) {
        if (g_tex_lod.Linked(hash) && !g_tex_lod.Known(hash)) carriers.push_back(res);
    });
    unsigned changed = 0;
    for (uint64_t r : carriers) changed += on ? g_stamps.Restamp(r) : g_stamps.Unstamp(r, false);
    logger::write("TexLod: A13 %s -- %zu resident(s) carry a look-alike, %u slot(s) %s", on ? "on" : "off",
        carriers.size(), changed, on ? "now hold the mod texture" : "given back to the game");
}

void TexPlacement::WatchSwitchedOff(const wchar_t* ns)
{
    if (!g_debug || !ns || !*ns) return;
    std::unordered_set<uint32_t> rules = g_tex_rules.RulesOfNs(ns);
    const size_t n = rules.size();
    {
        std::lock_guard l(off_.mutex);
        off_.rules.swap(rules);
    }
    g_tex_stats.off_leaks.store(0);
    g_tex_stats.off_kept.store(0);
    off_.watching.store(n != 0);
    logger::write("OFFWATCH f=%u ns='%ls': switched itself off -- every draw is checked for a slot still stamped "
                  "under one of its %zu texture rule(s)",
        g_frame_id, ns, n);
}

void TexPlacement::WatchSwitchedOn()
{
    if (!off_.watching.exchange(false)) return;
    logger::write("OFFWATCH f=%u: switched back on -- %u slot read(s) of its textures by draws not handed back, "
                  "%u hand-back(s) that left its texture in place",
        g_frame_id, g_tex_stats.off_leaks.load(), g_tex_stats.off_kept.load());
}

bool TexPlacement::OffWatching() const
{
    return off_.watching.load(std::memory_order_relaxed);
}

uint32_t TexPlacement::OffRuleOf(ID3D12Resource* res)
{
    const uint32_t rule = g_tex_select.RuleHashFor(g_tex_identity.HashOf(reinterpret_cast<uint64_t>(res)));
    if (!rule) return 0;
    std::lock_guard l(off_.mutex);
    return off_.rules.count(rule) ? rule : 0;
}

namespace {
struct RuleVar {
    const float* var = nullptr;
    uint32_t bits = 0;
    std::vector<uint32_t> rules;
};
struct RuleVarWatch : SetState {
    bool built = false;
    unsigned gen = 0;
    std::vector<RuleVar> vars;
};

std::wstring LowerNs(const wchar_t* b, const wchar_t* e)
{
    std::wstring s(b, e);
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}
} // namespace

void TexPlacement::RestampOnVarChange()
{
    RuleVarWatch& w = CurrentSet().State<RuleVarWatch>();
    const unsigned gen = PortApi::LoadGeneration();
    if (!w.built || w.gen != gen) {
        if (!g_tex_rules.Ready()) return;
        w.built = true;
        w.gen = gen;
        w.vars.clear();
        struct Ctx {
            std::unordered_set<std::wstring> units;
            std::unordered_map<const float*, size_t> at;
            std::vector<RuleVar>* vars;
        } ctx{ {}, {}, &w.vars };
        for (const GeoMod& gm : Mods().Units())
            if (!gm.wcl_ns.empty()) ctx.units.insert(LowerNs(gm.wcl_ns.c_str(), gm.wcl_ns.c_str() + gm.wcl_ns.size()));
        PortApi::ForEachTextureRuleVar(
            [](uint32_t hash, const wchar_t* fq, const float* var, void* user) {
                auto* c = (Ctx*)user;
                const wchar_t* a = fq ? wcschr(fq, L'\\') : nullptr;
                const wchar_t* z = fq ? wcsrchr(fq, L'\\') : nullptr;
                if (a && z > a && c->units.count(LowerNs(a + 1, z))) return;
                auto it = c->at.find(var);
                if (it == c->at.end()) {
                    it = c->at.emplace(var, c->vars->size()).first;
                    c->vars->push_back(RuleVar{ var, 0, {} });
                }
                std::vector<uint32_t>& rules = (*c->vars)[it->second].rules;
                if (std::find(rules.begin(), rules.end(), hash) == rules.end()) rules.push_back(hash);
            },
            &ctx);
        for (RuleVar& rv : w.vars) memcpy(&rv.bits, rv.var, sizeof rv.bits);
        logger::write("TEXVARS: %zu variable(s) read by the texture sections of mods that are not characters -- a "
                      "change seen between their [Present] passes runs those swaps again",
            w.vars.size());
        return;
    }
    std::unordered_set<uint32_t> changed;
    for (RuleVar& rv : w.vars) {
        uint32_t now = 0;
        memcpy(&now, rv.var, sizeof now);
        if (now == rv.bits) continue;
        rv.bits = now;
        changed.insert(rv.rules.begin(), rv.rules.end());
    }
    if (changed.empty()) return;
    std::vector<std::pair<uint64_t, uint32_t>> carriers;
    g_tex_identity.ForEachHash([&](uint64_t res, uint32_t h) {
        if (changed.count(h)) carriers.push_back({ res, h });
    });
    for (const auto& [res, rule] : carriers) {
        const bool ok = g_stamps.Restamp(res);
        if (logger::enabled(1) && DiagOnce("TEXVARS-RESTAMP", (int)rule))
            logger::write("TEXVARS-RESTAMP f=%u rule=%08x res=%p: a variable its section reads changed -- %s", g_frame_id,
                rule, (void*)res, ok ? "swapped again" : "not swapped (no selection now, or no slot recorded)");
    }
}

void TexPlacement::RetryMissedBetweenPasses(const char* why)
{
    retry_why_.store(why, std::memory_order_release);
}

void TexPlacement::RunRetryBetweenPasses()
{
    const char* const why = retry_why_.exchange(nullptr, std::memory_order_acq_rel);
    if (why) RetryMissed(why);
}

void TexPlacement::ApplyQueued()
{
    if (!queued_n_.load(std::memory_order_acquire)) return;
    std::vector<std::pair<uint64_t, QueuedPick>> picks;
    {
        std::lock_guard l(missed_mutex_);
        picks.assign(queued_.begin(), queued_.end());
    }
    struct Done {
        uint64_t res;
        uint64_t seq;
        bool stamped;
    };
    std::vector<Done> done;
    unsigned stamped = 0;
    unsigned waiting = 0;
    for (const auto& [res, q] : picks) {
        bool gone = false;
        ID3D12Resource* const tex = PortApi::LoadedSelection(q.custom, q.gen, &gone);
        const int sel = tex ? g_mod_textures.IndexOf(tex) : -1;
        ModTextureStore::Entry e;
        const bool ready = sel >= 0 && g_mod_textures.EntryAt(sel, &e) && e.srv.ptr;
        if (!ready && !gone) {
            ++waiting;
            continue;
        }
        bool ok = false;
        if (ready) {
            const char* const site_was = t_tex_swap_site;
            t_tex_swap_site = "queued";
            t_queued_sel = sel;
            ok = g_stamps.Restamp(res);
            t_queued_sel = -1;
            t_tex_swap_site = site_was;
        }
        if (ok) ++stamped;
        done.push_back(Done{ res, q.seq, ok });
        if (g_debug && DiagOnce("TEXQUEUE-STAMP", (int)q.rule))
            logger::write("TEXQUEUE-STAMP f=%u rule=%08x res=%p: the texture its section picked while it loaded %s",
                g_frame_id, q.rule, (void*)res,
                ok         ? "is described -- stamped"
                    : gone ? "will not load -- dropped"
                           : "is described, but the slot no longer holds this texture -- dropped");
    }
    if (done.empty()) return;
    {
        std::lock_guard l(missed_mutex_);
        for (const Done& d : done) {
            const auto it = queued_.find(d.res);
            if (it != queued_.end() && it->second.seq == d.seq) queued_.erase(it);
            if (d.stamped) missed_.erase(d.res);
        }
        queued_n_.store(queued_.size(), std::memory_order_release);
    }
    if (g_debug)
        logger::write("TEXQUEUE f=%u: new mod textures described -- %zu queued pick(s): %u stamped, %u still "
                      "loading, %zu dropped",
            g_frame_id, picks.size(), stamped, waiting, done.size() - stamped);
}
