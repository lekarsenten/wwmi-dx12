// Mip-exact identities. See tex_mip_identity.h.
#include "tex_mip_identity.h"
#include "tex_identity.h"
#include "tex_rules.h"
#include "tex_select.h"
#include "tex_lod.h"
#include "tex_crawl.h"
#include "tex_toggles.h"
#include "tex_upload_identity.h"
#include "../hooks/stamp_ledger.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include "../core/log.h"
#include "../migoto/port_api.h"
#include <algorithm>

TexMipIdentity g_tex_mip_identity;

void TexMipIdentity::Record(ID3D12Resource* dest, uint32_t sub, uint32_t id, uint32_t rule)
{
    bool degenerate = false;
    const uint32_t prev = g_tex_lod.AddMipExact(id, rule, sub, &degenerate);
    if (g_debug && DiagOnce("MIPEXACT", (int)(id ^ (sub << 28))))
        logger::write("MIPEXACT dest=%p mip=%u identity=%08x -> rule=%08x (%s)", (void*)dest, sub, id, rule,
            !prev              ? "new record"
                : prev == rule ? "the content match had agreed"
                : degenerate   ? "DEGENERATE: an exact record for another rule stood"
                               : "REPLACES a content match that pointed elsewhere");
    if (!degenerate) return;
    std::vector<uint64_t> carriers;
    g_tex_identity.ForEachHash([&](uint64_t res, uint32_t hash) {
        if (hash == id) carriers.push_back(res);
    });
    for (uint64_t r : carriers) g_stamps.Unstamp(r);
}

void TexMipIdentity::Forget(uint64_t res)
{
    {
        std::lock_guard l(mip_mutex_);
        mip_pending_.erase(res);
    }
    {
        std::lock_guard l(feed_mutex_);
        feed_pending_.erase(res);
    }
}

std::vector<std::pair<uint32_t, uint32_t>> TexMipIdentity::TakeHeld(uint64_t dest)
{
    std::vector<std::pair<uint32_t, uint32_t>> held;
    std::lock_guard l(mip_mutex_);
    auto it = mip_pending_.find(dest);
    if (it != mip_pending_.end()) {
        held.swap(it->second);
        mip_pending_.erase(it);
    }
    return held;
}

void TexMipIdentity::ParkFeed(uint64_t bigger, uint32_t small_id, uint32_t steps)
{
    std::lock_guard l(feed_mutex_);
    auto& v = feed_pending_[bigger];
    if (std::none_of(v.begin(), v.end(), [&](const auto& e) { return e.first == small_id; }))
        v.emplace_back(small_id, steps);
}

std::vector<std::pair<uint32_t, uint32_t>> TexMipIdentity::TakeFed(uint64_t dest)
{
    std::vector<std::pair<uint32_t, uint32_t>> fed;
    std::lock_guard l(feed_mutex_);
    auto it = feed_pending_.find(dest);
    if (it != feed_pending_.end()) {
        fed.swap(it->second);
        feed_pending_.erase(it);
    }
    return fed;
}

void TexMipIdentity::FromUpload(ID3D12Resource* dest, uint32_t sub, const uint8_t* src)
{
    if (!dest || !src || sub == 0 || !g_tex_rules.Ready() || !g_settings.texlod_cache) return;
    const D3D12_RESOURCE_DESC d = dest->GetDesc();
    TexAssetLayout L;
    if (!TexAssetLayoutOf(d, sub, L) || !L.floor) return;
    const uint32_t fmt = (uint32_t)d.Format;
    const uint32_t w = L.w;
    const uint32_t h = L.h;
    const std::vector<uint8_t> tight = TightFromUpload(src, L.pitch, L);
    uint32_t data_hash;
    {
        PROF(PROF_CB_Crc);
        data_hash = PortApi::Crc32c(0, tight.data(), tight.size());
    }
    const uint32_t id =
        TexIdentityHash(data_hash, w, h, d.MipLevels - sub, d.DepthOrArraySize ? d.DepthOrArraySize : 1, fmt);
    const uint32_t h0 = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(dest));
    if (!h0) { // mip0 not seen yet: hold it
        DumpIfDegenerate(id, tight, L, fmt, sub, 0);
        std::lock_guard l(mip_mutex_);
        mip_pending_[reinterpret_cast<uint64_t>(dest)].emplace_back(sub, id);
        g_tex_stats.ts_mipk_held.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    const uint32_t rule = g_tex_select.ProvenRuleFor(h0);
    if (rule) g_tex_stats.ts_mipk_target.fetch_add(1, std::memory_order_relaxed);
    const uint32_t high = rule ? rule : g_tex_crawl.On() ? h0 : 0;
    if (high && id != high && id != h0) Record(dest, sub, id, high);
    DumpIfDegenerate(id, tight, L, fmt, sub, rule ? rule : h0);
}
