#include "mod_set.h"
#include "../addon/addon.h"
#include "../core/frame_clock.h"
#include "../core/gpu_progress.h"
#include "../core/log.h"
#include "../draw/draw_toggles.h"
#include "../hooks/command_list_registry.h"
#include "../migoto/dx12_context_impl.h"
#include "../textures/tex_defer.h"
#include <chrono>
#include <intrin.h>
#ifdef WWMI_DEBUG
#include <mutex>
#include <unordered_set>
#endif

SetLife<ModSet> g_mod_sets;

static std::atomic<bool> s_sets_closed{ false };

struct SetsCloser {
    ~SetsCloser() { s_sets_closed.store(true); }
};
static SetsCloser s_sets_closer;

struct ThreadHazard {
    SetLife<ModSet>::Hazard* const h = g_mod_sets.RegisterHazard();
    ~ThreadHazard()
    {
        if (!s_sets_closed.load()) g_mod_sets.UnregisterHazard(h);
    }
};

static thread_local ModSet* t_set = nullptr;
static thread_local bool t_step = false;

static SetLife<ModSet>::Hazard& ThisThreadHazard()
{
    static thread_local ThreadHazard hazard;
    return *hazard.h;
}

static std::vector<SetState* (*)()>& SetStateMakers()
{
    static std::vector<SetState* (*)()> makers;
    return makers;
}

unsigned RegisterSetState(SetState* (*make)())
{
    SetStateMakers().push_back(make);
    return (unsigned)SetStateMakers().size() - 1;
}

ModSet::ModSet()
{
    static unsigned s_next_id = 0;
    id = s_next_id++;
    for (SetState* (*make)() : SetStateMakers()) states_.emplace_back(make());
}

ModSet::~ModSet()
{
    logger::report("MODSET: #%u died at frame %u, retired at frame %u, pinned by %u lists", id, g_frame_id,
        retired_frame, pinned_by.load(std::memory_order_relaxed));
    PortApi::EngineScope engine;
    states_.clear();
    for (const PortApi::HeldBuffer& h : held) MigotoPort::ReleaseShimObject(h.wrapper);
}

void ModSet::BuildStates()
{
    for (const std::unique_ptr<SetState>& s : states_) s->Build(*this);
}

void InitModSets()
{
    if (!g_mod_sets.Current()) g_mod_sets.Publish(new ModSet, 0);
}

void BuildModSet(void*)
{
    const auto t0 = std::chrono::steady_clock::now();
    ModSet* const set = new ModSet;
    ModSet* const prev = t_set;
    t_set = set;
    if (g_draw_toggles.wwmi_geom && g_addon.d3d12_device && !g_addon.wwmi_dir.empty()) set->mods.Build();
    set->BuildStates();
    t_set = prev;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logger::report("MODSET: built #%u: %zu units, %zu components, %zu buffers, %.0f ms", set->id, set->mods.Count(),
        set->comps.Count(), set->held.size(), ms);
    logger::report("RELOAD: parse %.0f ms, build %.0f ms, %zu units, set #%u", PortApi::LastParseMs(), ms,
        set->mods.Count(), set->id);
    const uint64_t stamp = GpuStamp();
    if (ModSet* const old = g_mod_sets.Publish(set, stamp)) old->retired_frame = g_frame_id;
    g_reload_gate.Published(stamp);
}

ReloadGate g_reload_gate;

const char* ReloadGate::Waiting() const
{
    if (published_ && !GpuPassed(published_)) return "the GPU is still drawing frames from before the last reload";
    if (g_tex_defer.Loading()) return "mod textures are still loading";
    return nullptr;
}

bool ReloadGate::MayReload(void* self)
{
    constexpr std::chrono::seconds kLongestHold{ 5 };
    ReloadGate& g = *static_cast<ReloadGate*>(self);
    const auto now = std::chrono::steady_clock::now();
    const double held_ms = std::chrono::duration<double, std::milli>(now - g.since_).count();
    const char* const why = g.Waiting();
    if (!why) {
        if (g.holding_) logger::report("RELOAD-HOLD: released after %.0f ms", held_ms);
        g.holding_ = false;
        return true;
    }
    if (!g.holding_) {
        g.holding_ = true;
        g.since_ = now;
        logger::report("RELOAD-HOLD: the reload waits: %s; key presses until then make one reload", why);
        return false;
    }
    if (now - g.since_ < kLongestHold) return false;
    logger::report("RELOAD-HOLD: %s after %.0f ms; reloading anyway", why, held_ms);
    g.holding_ = false;
    return true;
}

void DrainModSets()
{
    g_mod_sets.Drain(&GpuPassed);
}

ModSetScope::ModSetScope(const void* cmd_list) : prev_(t_set), hazard_(false)
{
    if (CmdBlock* const block = cmd_list ? g_command_lists.Block(cmd_list) : nullptr) {
        t_set = block->mod_pins.Pin(g_mod_sets);
        return;
    }
    if (prev_) return;
    t_set = g_mod_sets.Protect(ThisThreadHazard());
    hazard_ = true;
}

ModSetScope::~ModSetScope()
{
    if (hazard_) SetLife<ModSet>::Unprotect(ThisThreadHazard());
    t_set = prev_;
}

ModSetStepScope::ModSetStepScope() : prev_(t_set), prev_step_(t_step)
{
    t_set = nullptr;
    t_step = true;
}

ModSetStepScope::~ModSetStepScope()
{
    t_set = prev_;
    t_step = prev_step_;
}

ModSetRef::ModSetRef() : prev_(t_set)
{
    if (prev_) {
        SetLife<ModSet>::AddRef(prev_);
        set_ = prev_;
    } else {
        set_ = g_mod_sets.AcquireCurrent();
    }
    t_set = set_;
}

ModSetRef::~ModSetRef()
{
    t_set = prev_;
    g_mod_sets.Retire(set_, 0);
}

#ifdef WWMI_DEBUG
static void NoteUnscoped(void* caller)
{
    static std::mutex s_mtx;
    static std::unordered_set<void*> s_said;
    std::lock_guard<std::mutex> l(s_mtx);
    if (s_said.insert(caller).second) logger::report("MODSET-UNSCOPED %p", caller);
}
#endif

static ModSet& Resolve(void* caller)
{
    if (ModSet* const set = t_set) return *set;
#ifdef WWMI_DEBUG
    if (!t_step) NoteUnscoped(caller);
#else
    (void)caller;
#endif
    return *g_mod_sets.Protect(ThisThreadHazard());
}

ModSet& CurrentSet()
{
    return Resolve(_ReturnAddress());
}

ModRegistry& Mods()
{
    return Resolve(_ReturnAddress()).mods;
}

ComponentTable& Comps()
{
    return Resolve(_ReturnAddress()).comps;
}
