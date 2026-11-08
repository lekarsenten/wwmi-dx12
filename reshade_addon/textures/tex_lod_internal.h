#pragma once
// The LOD cache's table entries and the record helpers its three files share.
#include "tex_lod.h"
#include "../core/log.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include "tex_toggles.h"
#include <windows.h>
#include <algorithm>

struct TexLodCache::Entry {
    bool exact = false;
    uint32_t mip = 0; // an exact low's: the target's mip it is; 0 = not known
    std::vector<uint32_t> highs;
};
struct TexLodCache::Slot {
    std::atomic<uint32_t> low{ 0 };
    std::atomic<const Entry*> e{ nullptr };
};
struct TexLodCache::EntryTab {
    uint32_t bits;
    Slot* s;
};

inline constexpr float kTexLodSsimMargin =
    0.03f; // a look-alike under several targets swaps as the best by at least this

inline bool TexLodExact(const TexLodCache::Rec& r)
{
    return !r.links.empty() && r.links.front().exact;
}
inline TexLodCache::Link* TexLodLinkTo(TexLodCache::Rec& r, uint32_t high)
{
    for (TexLodCache::Link& k : r.links)
        if (k.high == high) return &k;
    return nullptr;
}
inline void TexLodMergeLink(TexLodCache::Records& into, uint32_t low, const TexLodCache::Link& k)
{
    if (!low || !k.high || low == k.high) return;
    TexLodCache::Rec& r = into[low];
    if (TexLodCache::Link* e = TexLodLinkTo(r, k.high))
        *e = k;
    else
        r.links.push_back(k);
}
inline uint32_t TexLodBelow(uint32_t mip, uint32_t steps)
{
    return mip && steps ? mip + steps : 0;
}
