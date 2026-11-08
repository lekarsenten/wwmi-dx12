// The LOD cache's table: each low's entry for the lock-free lookups. See tex_lod.h.
#include "tex_lod_internal.h"

TexLodCache g_tex_lod;

TexLodCache::EntryTab* TexLodCache::TabNew(uint32_t bits)
{
    return new EntryTab{ bits, new Slot[size_t(1) << bits]() };
}

static uint32_t TabSlot(uint32_t low, uint32_t bits)
{
    return (low * 2654435761u) >> (32 - bits);
}

const TexLodCache::Entry* TexLodCache::EntryOf(uint32_t low) const
{
    if (!low) return nullptr;
    const EntryTab* tb = tab_.load(std::memory_order_acquire);
    const uint32_t mask = (1u << tb->bits) - 1;
    for (uint32_t i = TabSlot(low, tb->bits), n = 0; n <= mask; ++n, i = (i + 1) & mask) {
        const uint32_t k = tb->s[i].low.load(std::memory_order_acquire);
        if (!k) return nullptr;
        if (k == low) return tb->s[i].e.load(std::memory_order_acquire);
    }
    return nullptr;
}

void TexLodCache::TabGrow()
{ // caller holds mutex_
    const EntryTab* old = tab_.load(std::memory_order_relaxed);
    EntryTab* tb = TabNew(old->bits + 1);
    const uint32_t mask = (1u << tb->bits) - 1;
    uint32_t used = 0;
    for (uint32_t k = 0; k < (1u << old->bits); ++k) {
        const uint32_t low = old->s[k].low.load(std::memory_order_relaxed);
        const Entry* e = old->s[k].e.load(std::memory_order_relaxed);
        if (!low || !e) continue; // empty or a tombstone: not carried over
        uint32_t i = TabSlot(low, tb->bits);
        while (tb->s[i].low.load(std::memory_order_relaxed)) i = (i + 1) & mask;
        tb->s[i].e.store(e, std::memory_order_relaxed);
        tb->s[i].low.store(low, std::memory_order_relaxed);
        ++used;
    }
    tab_used_ = used;
    tab_.store(tb, std::memory_order_release);
    if (g_debug) logger::write("TexLod: table grown to %u slots (%u lows)", mask + 1, used);
}

// Caller holds mutex_. A null entry takes the low out; its slot keeps the key as a tombstone.
void TexLodCache::TabSet(uint32_t low, const Entry* e)
{
    if (!low) return;
    if (e && (uint64_t)(tab_used_ + 1) * 2 > (1ull << tab_.load(std::memory_order_relaxed)->bits)) TabGrow();
    const EntryTab* tb = tab_.load(std::memory_order_relaxed);
    const uint32_t mask = (1u << tb->bits) - 1;
    for (uint32_t i = TabSlot(low, tb->bits), n = 0; n <= mask; ++n, i = (i + 1) & mask) {
        const uint32_t k = tb->s[i].low.load(std::memory_order_relaxed);
        if (k && k != low) continue;
        if (!k && !e) return;
        tb->s[i].e.store(e, std::memory_order_release);
        if (!k) {
            tb->s[i].low.store(low, std::memory_order_release);
            ++tab_used_;
        }
        return;
    }
}

void TexLodCache::PublishDegenerate()
{ // caller holds mutex_
    degen_snap_.store(new std::vector<uint32_t>(degenerate_.begin(), degenerate_.end()), std::memory_order_release);
}

bool TexLodCache::ExactRecord(uint32_t low) const
{
    auto it = records_.find(low);
    return it != records_.end() && TexLodExact(it->second);
}

uint32_t TexLodCache::SingleHigh(uint32_t low) const
{
    auto it = records_.find(low);
    return it == records_.end() || it->second.links.size() != 1 ? 0 : it->second.links.front().high;
}

// Of two hashes that are one texture at two resolutions (one an exact low of the other), the higher; else 0.
uint32_t TexLodCache::OneTexture(uint32_t a, uint32_t b) const
{
    if (ExactRecord(b) && SingleHigh(b) == a) return a;
    if (ExactRecord(a) && SingleHigh(a) == b) return b;
    return 0;
}

// Of a look-alike record's links, the one that beats every other by the margin; null for one link or a near-tie.
const TexLodCache::Link* TexLodCache::ClearBest(const Rec& r)
{
    const Link* best = nullptr;
    float second = 0;
    for (const Link& k : r.links) {
        if (best && k.ssim <= best->ssim) {
            second = (std::max)(second, k.ssim);
            continue;
        }
        if (best) second = (std::max)(second, best->ssim);
        best = &k;
    }
    return r.links.size() > 1 && best && best->ssim - second >= kTexLodSsimMargin ? best : nullptr;
}

// A low's entry from its record, for the lock-free readers; the lookups decide what it swaps. Caller holds mutex_.
void TexLodCache::Publish(uint32_t low)
{
    auto it = records_.find(low);
    if (it == records_.end() || it->second.links.empty()) {
        if (EntryOf(low)) TabSet(low, nullptr);
        return;
    }
    const Rec& r = it->second;
    auto* e = new Entry;
    const Link* best = nullptr;
    e->exact = TexLodExact(r);
    if (e->exact) e->mip = r.links.front().mip;
    if (!e->exact) best = ClearBest(r);
    if (best)
        e->highs.push_back(best->high);
    else
        for (const Link& k : r.links) e->highs.push_back(k.high);
    const Entry* was = EntryOf(low);
    if (was && was->exact == e->exact && was->mip == e->mip && was->highs == e->highs) {
        delete e;
        return;
    }
    TabSet(low, e);
}

uint32_t TexLodCache::ExactHigh(const Entry* e)
{
    return e && e->exact ? e->highs.front() : 0;
}

uint32_t TexLodCache::Lookup(uint32_t hash) const
{
    if (g_tex_toggles.lod_alias_off) return 0;
    const Entry* e = EntryOf(hash); // no lock: see tab_
    if (!e || e->exact) return ExactHigh(e);
    return g_settings.texlod_resemblance && e->highs.size() == 1 ? e->highs.front() : 0;
}

uint32_t TexLodCache::Known(uint32_t hash) const
{
    return ExactHigh(EntryOf(hash)); // ungated by the A/B: membership and dedupe, never a swap
}

unsigned TexLodCache::MipOf(uint32_t hash) const
{
    const Entry* e = EntryOf(hash);
    return e && e->exact ? e->mip : 0;
}

bool TexLodCache::Linked(uint32_t hash) const
{
    return EntryOf(hash) != nullptr;
}

bool TexLodCache::Degenerate(uint32_t hash) const
{
    const std::vector<uint32_t>* v = degen_snap_.load(std::memory_order_acquire); // no lock: see degen_snap_
    return v && std::binary_search(v->begin(), v->end(), hash);
}
