// Lineage: a copied texture is the same texture. See tex_lineage.h.
#include "tex_lineage.h"
#include "tex_identity.h"
#include "tex_rules.h"
#include "tex_select.h"
#include "tex_lod.h"
#include "tex_crawl.h"
#include "tex_mip_identity.h"
#include "tex_toggles.h"
#include "../hooks/stamp_ledger.h"

TexLineage g_tex_lineage;

void TexLineage::Propagate(uint64_t srcH, uint64_t dstH, bool same_image)
{
    if (!g_tex_toggles.lineage || srcH == dstH) return;
    const uint32_t shash = g_tex_identity.HashOf(srcH);
    const uint32_t rule = g_tex_select.RuleHashFor(shash);
    if (!shash || (!rule && !same_image))
        return; // a copy holding its source's image (whole, or streamed down) is that texture, target or not
    const bool tagged = g_tex_identity.Inherit(dstH, shash);
    if (tagged) ++g_tex_stats.lineage_count;
    if (tagged && rule) g_stamps.Restamp(dstH);
}

void TexLineage::Alias(uint64_t smallerH, uint64_t biggerH, uint32_t steps)
{
    const uint32_t sId = g_tex_identity.HashOf(smallerH);
    const uint32_t bId = g_tex_identity.HashOf(biggerH);
    if (!sId || sId == bId) return;
    if (!bId) { // the bigger side is not hashed yet: park the edge
        g_tex_mip_identity.ParkFeed(biggerH, sId, steps);
        return;
    }
    const bool known_before = g_tex_lod.Known(sId) != 0;
    g_tex_lod.Feeds(sId, bId, g_tex_rules.Now().hash_to_rule.count(bId) || g_tex_crawl.On() ? bId : 0, steps);
    if (!known_before && g_tex_lod.Known(sId))
        g_stamps.Restamp(smallerH); // recognised now -> re-fire its placement swap
}
