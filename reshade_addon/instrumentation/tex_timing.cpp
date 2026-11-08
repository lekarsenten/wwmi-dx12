#include "tex_timing.h"
#include "../core/log.h"
#include "../core/ns_hash.h"
#include "../migoto/port_api.h"
#include <string>
#include <windows.h>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace TexTiming {
namespace {

struct Tex {
    double created = -1;
    double alloc_ms = 0;
    double queue_ms = 0;
    double flushed = -1;
    double swapped = -1;
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t mips = 0;
    uint32_t fmt = 0;
    uint32_t tid = 0;
    uint32_t rule = 0;
    unsigned long long bytes = 0;
};
struct Rule {
    double needed = -1;
    double swapped = -1;
    ID3D12Resource* res = nullptr;
};
struct Mod {
    double claimed = -1;
    std::wstring ns;
};

std::mutex g_mtx;
std::unordered_map<ID3D12Resource*, Tex> g_tex;
std::unordered_map<uint32_t, Rule> g_rule;
std::unordered_map<uint64_t, Mod> g_mod;
LARGE_INTEGER g_freq{};
LARGE_INTEGER g_t0{};
double g_window_ms = 0;
std::atomic<unsigned> g_gen{ 0 };

double Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)(t.QuadPart - g_t0.QuadPart) * 1000.0 / (double)g_freq.QuadPart;
}

void Report()
{
    std::vector<std::pair<ID3D12Resource*, Tex>> tex(g_tex.begin(), g_tex.end());
    std::sort(tex.begin(), tex.end(), [](const auto& a, const auto& b) {
        const double ta = a.second.created >= 0 ? a.second.created : a.second.flushed;
        const double tb = b.second.created >= 0 ? b.second.created : b.second.flushed;
        return ta < tb;
    });
    double create_ms = 0;
    unsigned long long mb = 0;
    std::unordered_map<uint32_t, double> by_tid;
    for (const auto& [res, t] : tex) {
        if (t.created < 0) continue;
        create_ms += t.alloc_ms + t.queue_ms;
        mb += t.bytes;
        by_tid[t.tid] += t.alloc_ms + t.queue_ms;
        logger::write(
            "TEXTIME res=%p %ux%u mips=%u fmt=%u %.1f MB | created @%.0f ms on tid %u: allocate %.1f ms + stage/copy %.1f ms"
            " | upload recorded %s%.0f ms | first swap %s%.0f ms%s",
            (void*)res, t.w, t.h, t.mips, t.fmt, t.bytes / 1048576.0, t.created, t.tid, t.alloc_ms, t.queue_ms,
            t.flushed >= 0 ? "+" : "never ", t.flushed >= 0 ? t.flushed - t.created : 0.0,
            t.swapped >= 0 ? "+" : "never ", t.swapped >= 0 ? t.swapped - t.created : 0.0,
            t.rule ? "" : " (not seen swapping a game texture)");
    }
    unsigned needed = 0;
    unsigned late = 0;
    double worst = 0;
    double sum = 0;
    for (const auto& [rule, r] : g_rule) {
        if (r.needed < 0) continue;
        ++needed;
        if (r.swapped < 0) {
            ++late;
            continue;
        }
        const double d = r.swapped - r.needed;
        sum += d;
        worst = (std::max)(worst, d);
        auto it = g_tex.find(r.res);
        logger::write("TEXWAIT rule=%08x needed @%.0f ms -> modded @%.0f ms: %.0f ms without its mod texture%s", rule,
            r.needed, r.swapped, d,
            it != g_tex.end() && it->second.created >= 0 ? "" : " (texture created before the window)");
    }
    for (const auto& [ns_hash, m] : g_mod) {
        unsigned n = 0;
        unsigned still = 0;
        double first = -1;
        double last = -1;
        for (const auto& [rule, r] : g_rule) {
            wchar_t sec[512] = {};
            PortApi::TextureOverrideSection(rule, sec, std::size(sec));
            const wchar_t* ns = sec[0] ? wcschr(sec, L'\\') : nullptr;
            if (!ns || _wcsnicmp(ns + 1, m.ns.c_str(), m.ns.size()) != 0) continue;
            ++n;
            if (r.swapped < 0) {
                ++still;
                continue;
            }
            if (first < 0 || r.swapped < first) first = r.swapped;
            if (r.swapped > last) last = r.swapped;
        }
        logger::write(
            "TEXMOD '%ls': mesh first claimed @%.0f ms | %u of its textures seen: first modded %s%.0f ms after the claim,"
            " last %s%.0f ms after | %u still vanilla at the end",
            m.ns.c_str(), m.claimed, n, first >= 0 ? "" : "never ", first >= 0 ? first - m.claimed : 0.0,
            last >= 0 ? "" : "never ", last >= 0 ? last - m.claimed : 0.0, still);
    }
    for (const auto& [tid, ms] : by_tid) logger::write("TEXTIME thread %u: %.0f ms creating textures", tid, ms);
    logger::write(
        "TEXTIME window %.0f ms: %zu texture(s) created, %.0f MB, %.0f ms in creation | %u game texture(s) waited for "
        "their mod texture: avg %.0f ms, worst %.0f ms, %u still waiting at the end",
        g_window_ms, (size_t)std::count_if(tex.begin(), tex.end(), [](const auto& e) { return e.second.created >= 0; }),
        mb / 1048576.0, create_ms, needed, needed > late ? sum / (needed - late) : 0.0, worst, late);
}

} // namespace

void Arm(double seconds)
{
    std::lock_guard l(g_mtx);
    if (g_armed.load()) return;
    QueryPerformanceFrequency(&g_freq);
    QueryPerformanceCounter(&g_t0);
    g_window_ms = seconds * 1000.0;
    g_tex.clear();
    g_rule.clear();
    g_mod.clear();
    g_gen.fetch_add(1);
    g_armed.store(true);
    logger::write("TEXTIME armed (stops on the next press, or after %.0f s) -- swap characters now", seconds);
}

void Stop()
{
    std::lock_guard l(g_mtx);
    if (!g_armed.load()) return;
    g_armed.store(false);
    g_window_ms = Now();
    Report();
}

void Tick()
{
    if (!Armed()) return;
    std::lock_guard l(g_mtx);
    if (Now() < g_window_ms) return;
    g_armed.store(false);
    Report();
}

bool SeenOnThisThread(uint64_t key)
{
    thread_local unsigned t_gen = 0;
    thread_local std::unordered_set<uint64_t> t_seen;
    const unsigned gen = g_gen.load(std::memory_order_relaxed);
    if (t_gen != gen) {
        t_gen = gen;
        t_seen.clear();
    }
    return !t_seen.insert(key).second;
}

void NoteClaimed(const wchar_t* ns)
{
    if (!Armed()) return;
    const uint64_t ns_hash = NsHash(ns);
    if (!ns_hash || SeenOnThisThread(0xC1A1000000000000ull ^ (ns_hash >> 16))) return;
    std::lock_guard l(g_mtx);
    Mod& m = g_mod[ns_hash];
    if (m.claimed < 0) {
        m.claimed = Now();
        m.ns = ns;
    }
}

void NoteNeeded(uint32_t rule)
{
    if (!Armed() || !rule || SeenOnThisThread(rule)) return;
    std::lock_guard l(g_mtx);
    Rule& r = g_rule[rule];
    if (r.needed < 0 && r.swapped < 0) r.needed = Now();
}

void NoteCreate(
    ID3D12Resource* res, uint32_t w, uint32_t h, uint32_t mips, uint32_t fmt, double alloc_ms, double queue_ms)
{
    if (!Armed() || !res) return;
    std::lock_guard l(g_mtx);
    Tex& t = g_tex[res];
    t.created = Now() - alloc_ms - queue_ms;
    t.alloc_ms = alloc_ms;
    t.queue_ms = queue_ms;
    t.w = w;
    t.h = h;
    t.mips = mips;
    t.fmt = fmt;
    t.tid = GetCurrentThreadId();
}

void NoteFlushed(ID3D12Resource* res, unsigned long long bytes)
{
    if (!Armed() || !res) return;
    std::lock_guard l(g_mtx);
    auto it = g_tex.find(res);
    if (it == g_tex.end() || it->second.flushed >= 0) return;
    it->second.flushed = Now();
    it->second.bytes = bytes;
}

void NoteSwapped(ID3D12Resource* res, uint32_t rule)
{
    if (!Armed() || !res || SeenOnThisThread((uint64_t)(uintptr_t)res ^ ((uint64_t)rule << 1) ^ 1ull)) return;
    std::lock_guard l(g_mtx);
    const double now = Now();
    auto it = g_tex.find(res);
    if (it != g_tex.end() && it->second.swapped < 0) {
        it->second.swapped = now;
        it->second.rule = rule;
    }
    if (rule) {
        Rule& r = g_rule[rule];
        if (r.swapped < 0) {
            r.swapped = now;
            r.res = res;
        }
    }
}

} // namespace TexTiming
