// The LOD cache's learning: what the uploads and copies teach. See tex_lod.h.
#include "tex_lod_internal.h"

void TexLodCache::MarkDirty()
{ // caller holds mutex_
    dirty_ = true;
    dirty_at_ = GetTickCount64();
}

// `low` was just made an exact low, or its level filled in. Caller holds mutex_.
void TexLodCache::Proven(uint32_t low)
{
    MarkDirty();
    Publish(low);
}

bool TexLodCache::LearnExact(uint32_t low, uint32_t high, uint32_t mip, bool chain, bool identity, const char* how)
{
    if (!low || !high || low == high || degenerate_.count(low)) return false;
    {
        auto rf = refuted_.find(low);
        if (rf != refuted_.end() && rf->second == high) {
            if (DiagOnce("TEXLOD-REFUSED", (int)low))
                logger::write(
                    "TexLod: REFUSED %08x -> %08x (%s): this pair was refuted by the texture's own mip0 this session",
                    low, high, how);
            return false;
        }
    }
    Rec& r = records_[low];
    if (!r.links.empty() && !TexLodExact(r)) {
        if ((r.links.size() > 1 || !TexLodLinkTo(r, high)) && DiagOnce("TEXLOD-PROVEN", (int)low))
            logger::write(
                "TexLod: %08x proven -> %08x (%s); %zu look-alike target(s) dropped", low, high, how, r.links.size());
        r.links.clear();
    }
    if (!r.links.empty()) {
        Link& e = r.links.front();
        const uint32_t one = OneTexture(e.high, high);
        if (e.high == high || one == e.high) {
            if (e.high != high || !mip || e.mip) return true; // on file already
            e.mip = mip;
            Proven(low);
            return true;
        }
        if (one == high) {
            logger::write(
                "TexLod: %08x -> %08x (%s): its target %08x is a lower mip of that one", low, high, how, e.high);
            e.high = high;
            e.mip = mip;
            e.chain = chain;
            Proven(low);
            return true;
        }
        if (!identity) return false;
        logger::write(
            "TexLod: DEGENERATE %08x -- it was the exact mip of %08x, now also mip %u of %08x: identical data, "
            "never swapped from here on (persisted)",
            low, e.high, mip, high);
        records_.erase(low);
        degenerate_.insert(low);
        PublishDegenerate();
        MarkDirty();
        Publish(low);
        return true;
    }
    Link k;
    k.high = high;
    k.mip = mip;
    k.exact = true;
    k.chain = chain;
    r.links.push_back(k);
    Proven(low);
    return true;
}

bool TexLodCache::LearnLookalike(uint32_t low, uint32_t high, uint32_t mip, float ssim, bool chain, bool place)
{
    if (!low || !high || low == high || degenerate_.count(low)) return false;
    auto rf = refuted_.find(low);
    if (rf != refuted_.end() && rf->second == high) return false;
    if (ExactRecord(low)) return false;
    Rec& r = records_[low];
    if (TexLodLinkTo(r, high)) return false;
    Link k;
    k.high = high;
    k.mip = mip;
    k.chain = chain;
    k.place = place;
    k.ssim = ssim;
    r.links.push_back(k);
    MarkDirty();
    Publish(low);
    return true;
}

void TexLodCache::ChainAlias(uint32_t id, int depth, uint32_t mip, ChainWalk& walk)
{
    const uint32_t rule = walk.rule;
    const bool look = walk.look;
    if (!id || !rule || id == rule || depth > 16) return;
    const auto walked = walk.depth_of.find(id);
    if (walked != walk.depth_of.end() && walked->second <= depth) {
        ++walk.rewalks_skipped;
        return;
    }
    walk.depth_of[id] = depth;
    const bool degen = degenerate_.count(id) != 0;
    auto rec = records_.find(id);
    if (degen || (rec != records_.end() && TexLodExact(rec->second))) {
        if (degen || !TexLodLinkTo(rec->second, rule)) {
            if (g_debug && DiagOnce("LINEAGE-STOP", (int)id)) {
                auto fe = feeders_.find(id);
                logger::write(
                    "LINEAGE-STOP %08x -> %08x (depth %d): its exact record points to %08x%s; %zu feeder(s) below not walked",
                    id, rule, depth, SingleHigh(id), degen ? " (degenerate)" : "",
                    fe == feeders_.end() ? (size_t)0 : fe->second.size());
            }
            return;
        }
    } else {
        const bool added = look ? LearnLookalike(id, rule, mip, walk.ssim, true, false)
                                : LearnExact(id, rule, mip, true, false, "lineage-chain");
        if (g_debug && added)
            logger::write("LINEAGE-CHAIN %08x -> %08x mip %u (%s: fed a mip of a texture that is that target)", id,
                rule, mip, look ? "content-chain" : "lineage-chain");
    }
    auto it = feeders_.find(id);
    if (it == feeders_.end()) return;
    for (const Feeder& f : it->second) ChainAlias(f.low, depth + 1, TexLodBelow(mip, f.steps), walk);
}

void TexLodCache::Feeds(uint32_t lower, uint32_t big, uint32_t big_rule, uint32_t steps)
{
    if (!lower || !big || lower == big) return;
    std::lock_guard l(mutex_);
    if (!g_settings.texlod_cache) return;
    auto& v = feeders_[big];
    if (std::none_of(v.begin(), v.end(), [&](const Feeder& f) { return f.low == lower; }))
        v.push_back({ lower, steps });
    uint32_t rule = big_rule;
    if (!rule) rule = SingleHigh(big);
    if (!rule) return;
    const bool look = !big_rule && !ExactRecord(big);
    uint32_t mip = steps;
    float ssim = 0;
    if (!big_rule) {
        const Link& k = records_.find(big)->second.links.front();
        mip = TexLodBelow(k.mip, steps);
        ssim = k.ssim;
    }
    if (g_debug && DiagOnce("LINEAGE-WALK", (int)lower))
        logger::write("LINEAGE-WALK %08x fed %08x (%s) -> rule %08x", lower, big,
            big_rule   ? "a target"
                : look ? "look-alike record"
                       : "exact record",
            rule);
    ChainWalk walk;
    walk.rule = rule;
    walk.look = look;
    walk.ssim = ssim;
    ChainAlias(lower, 0, mip, walk);
#ifdef WWMI_DEBUG
    if (g_debug && walk.rewalks_skipped && DiagChanged(&feeders_, (int)lower, walk.depth_of.size()))
        logger::write("LINEAGE-WEB %08x fed %08x: %zu texture(s) walked, %u route(s) to one already walked not taken",
            lower, big, walk.depth_of.size(), walk.rewalks_skipped);
#endif
}

void TexLodCache::TargetsReady(bool (*is_target)(uint32_t hash, void* user), void* user)
{
    std::lock_guard l(mutex_);
    if (!g_settings.texlod_cache) return;
    unsigned folded = 0;
    unsigned narrowed = 0;
    std::vector<uint32_t> dropped;
    for (auto& kv : records_) {
        if (!is_target || !TexLodExact(kv.second)) continue;
        Link& k = kv.second.links.front();
        if (is_target(k.high, user)) continue;
        auto up = records_.find(k.high);
        if (up == records_.end() || !TexLodExact(up->second)) continue;
        const Link& via = up->second.links.front();
        if (via.high == kv.first) continue;
        k.high = via.high;
        k.mip = k.mip && via.mip ? k.mip + via.mip : 0;
        ++folded;
    }
    for (auto it = records_.begin(); it != records_.end();) {
        auto& links = it->second.links;
        if (!is_target || TexLodExact(it->second)) {
            ++it;
            continue;
        }
        const size_t was = links.size();
        links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& k) { return !is_target(k.high, user); }),
            links.end());
        if (links.empty()) {
            dropped.push_back(it->first);
            it = records_.erase(it);
            continue;
        }
        if (links.size() != was) ++narrowed;
        ++it;
    }
    for (auto& kv : records_) Publish(kv.first);
    for (uint32_t low : dropped) Publish(low);
    feeders_.clear();
    RewriteCache();
    logger::write(
        "TexLod: sweep -- %zu lows kept; %u folded onto the target their undeclared one is a lower mip of; "
        "look-alike matches: %u lost a target no ini declares now, %zu dropped: none of their targets is declared",
        records_.size(), folded, narrowed, dropped.size());
}

void TexLodCache::SetResemblance(bool on)
{
    std::lock_guard l(mutex_);
    if (g_settings.texlod_resemblance == on) return;
    g_settings.texlod_resemblance = on;
    size_t lookalike = 0;
    for (auto& kv : records_) lookalike += !TexLodExact(kv.second);
    logger::write("TexLod: look-alike matches %s -- %zu look-alike low(s) %s", on ? "ON" : "OFF", lookalike,
        on ? "now swapped like exact ones" : "learned, not used");
}

void TexLodCache::Refute(uint32_t low, uint32_t high, const char* why)
{
    if (!low || !high) return;
    std::lock_guard<std::mutex> l(mutex_);
    if (!g_settings.texlod_cache) return;
    refuted_[low] = high;
    auto it = records_.find(low);
    if (it == records_.end()) return;
    auto& links = it->second.links;
    auto k = std::find_if(links.begin(), links.end(), [&](const Link& x) { return x.high == high; });
    if (k == links.end()) return;
    const bool was_exact = k->exact;
    links.erase(k);
    if (links.empty()) records_.erase(it);
    Publish(low);
    RewriteCache();
    logger::write(
        "TexLod: REFUTED %08x -> %08x (%s; was %s) -- dropped from memory and from the cache, refused if re-learned",
        low, high, why ? why : "", was_exact ? "exact" : "a look-alike");
}

uint32_t TexLodCache::AddMipExact(uint32_t low, uint32_t high, unsigned mip, bool* now_degenerate)
{
    if (now_degenerate) *now_degenerate = false;
    if (!low || !high || low == high) return 0;
    std::lock_guard l(mutex_);
    if (!g_settings.texlod_cache || degenerate_.count(low)) return 0;
    const uint32_t prev = SingleHigh(low);
    LearnExact(low, high, mip, false, true, "mip-exact"); // replaces a look-alike; an exact record stands
    if (degenerate_.count(low)) { // the exact mip of TWO rules: of neither
        if (now_degenerate) *now_degenerate = true;
        return prev;
    }
    auto it = feeders_.find(low); // what fed `low` is `high` at a lower resolution too
    if (it != feeders_.end()) {
        ChainWalk walk;
        walk.rule = high;
        for (const Feeder& f : it->second) ChainAlias(f.low, 0, TexLodBelow(mip, f.steps), walk);
    }
    return prev;
}

bool TexLodCache::LearnVisual(uint32_t low, uint32_t high, float ssim)
{
    std::lock_guard l(mutex_);
    return g_settings.texlod_cache && LearnLookalike(low, high, 0, ssim, false, false);
}

bool TexLodCache::LearnPlace(uint32_t low, uint32_t high)
{
    std::lock_guard l(mutex_);
    return g_settings.texlod_cache && LearnLookalike(low, high, 0, 1.f, false, true);
}
