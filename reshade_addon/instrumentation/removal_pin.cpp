// removal_pin.cpp -- see removal_pin.h.
#include "../addon_common.h"
#include "removal_pin.h"
#include "../core/sharded_map.h"
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include "../addon/addon.h"

#ifdef WWMI_DEBUG
namespace {
struct Tally {
    static constexpr unsigned kKinds = 12;
    struct Kind {
        const char* what;
        uint32_t n;
    };
    Kind kinds[kKinds] = {};
    unsigned nkinds = 0;
    uint32_t total = 0;
    void add(const char* what)
    {
        ++total;
        for (unsigned i = 0; i < nkinds; ++i)
            if (strcmp(kinds[i].what, what) == 0) {
                ++kinds[i].n;
                return;
            }
        if (nkinds < kKinds) kinds[nkinds++] = Kind{ what, 1 };
    }
    // "ours=0" or "ours=37 [port-draw x26, port-uav-barrier x9, reloc-seed x2]"
    void describe(char* out, size_t cap) const
    {
        int n = snprintf(out, cap, "ours=%u", total);
        if (!total || n < 0 || (size_t)n >= cap) return;
        size_t p = (size_t)n;
        for (unsigned i = 0; i < nkinds && p < cap; ++i) {
            n = snprintf(
                out + p, cap - p, "%s%s x%u%s", i ? ", " : " [", kinds[i].what, kinds[i].n, i + 1 == nkinds ? "]" : "");
            if (n < 0) break;
            p += (size_t)n;
        }
    }
};
struct Exec {
    uint64_t seq = 0;
    uint32_t frame = 0;
    const void* list = nullptr;
    int type = -1;
    int qtype = -1;
    const char* who = "";
    unsigned long tid = 0; // the calling thread (a Close that fails asserts on its thread)
    ULONGLONG tick = 0; // GetTickCount64 at the entry
    char ours[200] = {};
};
constexpr unsigned kRing = 48;
constexpr unsigned kCloseRing = 48;

std::mutex g_mtx;
ShardedMap<const void*, Tally> g_tally; // by native list; an entry lives one recording; its own locks
struct Ref {
    const void* list;
    uint32_t frame;
    uint64_t exec_seq_then;
    const char* what;
};
std::unordered_map<const void*, Ref> g_refs;
struct LastExec {
    uint32_t frame;
    uint64_t seq;
};
std::unordered_map<const void*, LastExec> g_last_exec; // by native list: its last execute
constexpr size_t kRefsSweepAt = 4096;
uint32_t g_refs_swept_frame = 0;
Exec g_ring[kRing];
uint64_t g_seq = 0; // entries seen; g_ring[(g_seq - 1) % kRing] is the newest
Exec g_closes[kCloseRing];
uint64_t g_close_seq = 0;
std::atomic<bool> g_reported{ false };

// One ring, newest first. Under g_mtx.
void WriteRing(const char* label, const Exec* ring, unsigned cap, uint64_t seq, unsigned long mark_tid)
{
    const ULONGLONG now = GetTickCount64();
    const unsigned n = seq < cap ? (unsigned)seq : cap;
    for (unsigned i = 0; i < n; ++i) {
        const Exec& e = ring[(seq - 1 - i) % cap];
        logger::report("REMOVAL-PIN:   %s #%llu frame=%u -%llu ms tid=%lu%s %s list=%p type=%d queue=%d %s", label,
            (unsigned long long)e.seq, e.frame, (unsigned long long)(now - e.tick), e.tid,
            (mark_tid && e.tid == mark_tid) ? " <-stalled-thread" : "", e.who, e.list, e.type, e.qtype, e.ours);
    }
}
void WriteUnexecuted()
{
    unsigned pending = 0;
    g_tally.for_each([&](const void* const& list, const Tally& t) {
        if (!t.total || pending > 8) return;
        if (++pending > 8) {
            logger::report("REMOVAL-PIN:   ... more lists with our commands");
            return;
        }
        char buf[200];
        t.describe(buf, sizeof buf);
        auto le = g_last_exec.find(list);
        if (le == g_last_exec.end())
            logger::report("REMOVAL-PIN:   recorded list=%p %s -- never executed", list, buf);
        else
            logger::report("REMOVAL-PIN:   recorded list=%p %s -- last executed #%llu frame=%u", list, buf,
                (unsigned long long)le->second.seq, le->second.frame);
    });
    if (!pending) logger::report("REMOVAL-PIN:   no recorded list carries a command of ours");
}

// The report. Called under g_mtx. `hr` is the removal reason the check just read.
void Report(HRESULT hr, const char* where)
{
    logger::report("REMOVAL-PIN: the device is already removed (hr=%08x) at the entry of %s (frame %u, "
                   "%llu ExecuteCommandLists entries seen): the removing call lies between the previous "
                   "entry below and this one. Entries newest first, each with what OUR addon recorded "
                   "onto that list in its recording:",
        (unsigned)hr, where, g_frame_id, (unsigned long long)g_seq);
    WriteRing("exec", g_ring, kRing, g_seq, 0);
    logger::report("REMOVAL-PIN: the last closes, newest first:");
    WriteRing("close", g_closes, kCloseRing, g_close_seq, 0);
    WriteUnexecuted();
}
// Under g_mtx. What a new ring entry holds, from the calling thread.
void Fill(Exec& e, uint64_t seq, const void* native_list, int list_type, int queue_type, const char* who)
{
    e.seq = seq;
    e.frame = g_frame_id;
    e.list = native_list;
    e.type = list_type;
    e.qtype = queue_type;
    e.who = who ? who : "";
    e.tid = GetCurrentThreadId();
    e.tick = GetTickCount64();
    if (!g_tally.with(native_list, [&](const Tally& t) { t.describe(e.ours, sizeof e.ours); }))
        snprintf(e.ours, sizeof e.ours, "ours=0");
}

// Reads the device; on the first failure writes the report. Under g_mtx.
void CheckLocked(ID3D12Device* dev, const char* where)
{
    if (!dev || g_reported.load(std::memory_order_relaxed)) return;
    if (!RemovalPin::g_device_removed.load(std::memory_order_acquire)) return;
    const HRESULT hr = dev->GetDeviceRemovedReason();
    if (SUCCEEDED(hr)) return;
    g_reported.store(true, std::memory_order_relaxed);
    Report(hr, where);
}
} // namespace

namespace RemovalPin {

void Mark(const void* native_list, const char* what)
{
    if (!g_debug || !native_list || !what) return;
    auto* cl = reinterpret_cast<ID3D12GraphicsCommandList*>(const_cast<void*>(native_list));
    UINT64 blob[16] = {};
    blob[0] = (0x008ull & 0x3FFull) << 10;
    blob[1] = 0xFF00FF00ull;
    blob[2] = (8ull << 55) | (1ull << 54);
    const size_t len = strlen(what) + 1;
    size_t chunks = (len + 7) / 8;
    if (chunks > 13) chunks = 13;
    memcpy(&blob[3], what, len < chunks * 8 ? len : chunks * 8);
    reinterpret_cast<char*>(&blob[3])[chunks * 8 - 1] = 0;
    cl->SetMarker(2u, blob, (UINT)((3 + chunks) * sizeof(UINT64))); // the DRED breadcrumb context
}

void Ours(const void* native_list, const char* what)
{
    if (!g_debug || !native_list || !what) return;
    Mark(native_list, what);
    g_tally.update(native_list, [&](Tally& t) { t.add(what); });
}

void OnReset(const void* native_list)
{
    if (!g_debug || !native_list) return;
    g_tally.erase(native_list);
}

void OnClose(const void* native_list)
{
    if (!g_debug || !native_list) return;
    auto* nl = reinterpret_cast<ID3D12CommandList*>(const_cast<void*>(native_list));
    std::lock_guard<std::mutex> l(g_mtx);
    const uint64_t seq = ++g_close_seq;
    Fill(g_closes[(seq - 1) % kCloseRing], seq, nl, (int)nl->GetType(), -1, "Close");
}

void Refs(const void* native_list, const void* resource, const char* what)
{
    if (!g_debug || !native_list || !resource) return;
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_refs.size() >= kRefsSweepAt && g_refs_swept_frame != g_frame_id) { // a list never lives 64 frames
        g_refs_swept_frame = g_frame_id;
        for (auto it = g_refs.begin(); it != g_refs.end();)
            if (g_frame_id - it->second.frame > 64u)
                it = g_refs.erase(it);
            else
                ++it;
    }
    g_refs[resource] = Ref{ native_list, g_frame_id, g_seq, what ? what : "" };
}

void OnDestroyResource(const void* resource)
{
    if (!g_debug || !resource) return;
    std::lock_guard<std::mutex> l(g_mtx);
    auto it = g_refs.find(resource);
    if (it == g_refs.end()) return;
    const Ref r = it->second;
    g_refs.erase(it);
    const uint32_t age = g_frame_id - r.frame;
    if (age > 8u) return; // executed and retired long ago: not a hazard
    auto le = g_last_exec.find(r.list);
    const bool executed = le != g_last_exec.end() && le->second.frame >= r.frame;
    logger::write("REMOVAL-PIN: DEAD-REF %p destroyed by the game %u frame(s) after we named it (%s) on list %p, "
                  "which %s -- the kernel rejects a submission naming a freed allocation (INVALID_CALL, no breadcrumb)",
        resource, age, r.what, r.list,
        executed ? "HAS executed since (GPU may still be on it)" : "has NOT executed since");
}

void OnExecute(ID3D12Device* dev, const void* native_list, int list_type, int queue_type, const char* who)
{
    if (!g_debug) return;
    std::lock_guard<std::mutex> l(g_mtx);
    CheckLocked(dev, who);
    const uint64_t seq = ++g_seq; // sequenced before the slot: newest = ring[(seq - 1) % cap]
    Fill(g_ring[(seq - 1) % kRing], seq, native_list, list_type, queue_type, who);
    g_last_exec[native_list] = LastExec{ g_frame_id, seq };
}

void DumpOnStall(unsigned long stalled_tid)
{
    std::lock_guard<std::mutex> l(g_mtx);
    logger::report("REMOVAL-PIN (stall, thread %lu): the last closes, newest first -- a Close that failed asserts "
                   "the engine on its thread, so the newest entry marked <-stalled-thread is that list:",
        stalled_tid);
    WriteRing("close", g_closes, kCloseRing, g_close_seq, stalled_tid);
    logger::report("REMOVAL-PIN (stall): the last ExecuteCommandLists entries, newest first:");
    WriteRing("exec", g_ring, kRing, g_seq, stalled_tid);
    WriteUnexecuted();
}

void Describe(const void* native_list, char* out, size_t cap)
{
    if (!out || !cap) return;
    std::lock_guard<std::mutex> l(g_mtx);
    const unsigned rn = g_seq < kRing ? (unsigned)g_seq : kRing;
    for (unsigned i = 0; i < rn; ++i) {
        const Exec& e = g_ring[(g_seq - 1 - i) % kRing];
        if (e.list != native_list) continue;
        snprintf(out, cap, "at execute #%llu frame=%u: %s", (unsigned long long)e.seq, e.frame, e.ours);
        return;
    }
    if (!g_tally.with(native_list, [&](const Tally& t) { t.describe(out, cap); }))
        snprintf(out, cap, "ours=0 (no recording of ours on it)");
    auto le = g_last_exec.find(native_list);
    const size_t n = strlen(out);
    if (le != g_last_exec.end() && n < cap)
        snprintf(out + n, cap - n, " -- last executed #%llu frame=%u (before the ring)",
            (unsigned long long)le->second.seq, le->second.frame);
    else if (n < cap)
        snprintf(out + n, cap - n, " -- never executed");
}

void Check(ID3D12Device* dev, const char* where)
{
    if (!g_debug || g_reported.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> l(g_mtx);
    CheckLocked(dev, where);
}

} // namespace RemovalPin
#endif
