#pragma once
// Diagnostics gating: the debug master and log level, per-epoch dedupe, and probes armed by button.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <map>
#include <utility>
#include "../core/log.h"
#include "../core/frame_clock.h"

// Instrumentation only Debug builds compile: declared through these, Release gets empty inline bodies.
#ifdef WWMI_DEBUG
#define WWMI_DEBUG_FN
#define WWMI_DEBUG_BODY(...)
#define WWMI_DEBUG_VAR extern
#else
#define WWMI_DEBUG_FN __pragma(warning(push)) __pragma(warning(disable : 4100)) inline
#define WWMI_DEBUG_BODY(...) { __VA_ARGS__ } __pragma(warning(pop))
#define WWMI_DEBUG_VAR inline
#endif

inline bool g_debug = false; // diagnostics + logging master (overlay A2)
inline int g_log_verbosity = 2; // 1 events/errors, 2 +info
// The logger's level is the verbosity while debug is on, else 0. Set the three only through this.
inline void SetDiagnostics(bool on, int verbosity)
{
    g_debug = on;
    g_log_verbosity = verbosity;
    logger::set_level(on ? (verbosity > 2 ? 2 : verbosity) : 0);
}

// The verbose capture (debug overlay button): level 3 and no rate limit until the frame it ends at.
inline std::atomic<uint32_t> g_verbose_until{ 0 };
inline void VerboseArm(uint32_t frames)
{
    g_verbose_until.store(g_frame_id + frames, std::memory_order_relaxed);
    logger::set_unlimited(true);
    logger::set_level(3);
}
inline void VerboseTick()
{
    if (!logger::unlimited() || g_frame_id < g_verbose_until.load(std::memory_order_relaxed)) return;
    logger::set_unlimited(false);
    SetDiagnostics(g_debug, g_log_verbosity);
}

// Bumped by the overlay's re-arm button so each bounded probe resets its statics and captures HERE.
inline std::atomic<uint32_t> g_diag_gen{ 0 };

inline std::atomic<unsigned> g_diag_epoch{ 0 };

// One (mod, component) as an int key, for the diagnostics' maps and DiagOnce / DiagChanged. Components < 256.
inline int CompKey(int mod, int comp)
{
    return mod * 256 + comp;
}
inline int CompKeyMod(int key)
{
    return key / 256;
}
inline int CompKeyComp(int key)
{
    return key % 256;
}

inline bool DiagOnce(const void* site, int key)
{
    if (!g_debug) return false;
    static std::mutex m;
    static std::map<std::pair<const void*, int>, unsigned> seen;
    const unsigned e = g_diag_epoch.load(std::memory_order_relaxed) + 1;
    std::lock_guard<std::mutex> l(m);
    auto& slot = seen[{ site, key }];
    if (slot == e) return false;
    slot = e;
    return true;
}
inline bool DiagChanged(const void* site, int key, unsigned long long value)
{
    if (!g_debug) return false; // see DiagOnce: no global lock, no map, with debug off
    static std::mutex m;
    static std::map<std::pair<const void*, int>, unsigned long long> last;
    std::lock_guard<std::mutex> l(m);
    auto it = last.find({ site, key });
    if (it != last.end() && it->second == value) return false;
    last[{ site, key }] = value;
    return true;
}

inline std::atomic<uint32_t> g_probe_until{ 0 }; // probes may fire while g_frame_id < this
inline bool ProbeArmed()
{
    return g_frame_id < g_probe_until.load(std::memory_order_relaxed);
}
inline void ProbeArm(uint32_t frames)
{
    g_diag_epoch.fetch_add(1, std::memory_order_relaxed);
    g_probe_until.store(g_frame_id + frames, std::memory_order_relaxed);
}

/* Fires at most `k` times PER ARM PRESS. */
struct ProbeBudget {
    std::atomic<uint32_t> ep{ 0xFFFFFFFFu };
    std::atomic<int> n{ 0 };
    // This take's index within the current arm press, or -1 when unarmed or over budget.
    int index(int k)
    {
        if (!ProbeArmed()) return -1;
        const uint32_t e = g_diag_epoch.load(std::memory_order_relaxed);
        uint32_t cur = ep.load(std::memory_order_relaxed);
        if (cur != e && ep.compare_exchange_strong(cur, e)) n.store(0, std::memory_order_relaxed);
        const int i = n.fetch_add(1, std::memory_order_relaxed);
        return i < k ? i : -1;
    }
    bool take(int k) { return index(k) >= 0; }
};
