// The selector: which mod texture a game texture gets. See tex_select.h.
#include "tex_select.h"
#include "tex_identity.h"
#include "tex_rules.h"
#include "tex_lod.h"
#include "tex_pipe_alias.h"
#include "mod_texture_store.h"
#include "../hooks/descriptor_shadow.h"
#include "../mods/layout_registry.h"
#include "../addon/settings.h"
#include "../migoto/port_api.h"
#include "../instrumentation/diag_gate.h"
#include "../core/log.h"
#include "../core/ns_hash.h"
#include <unordered_map>

TexSelector g_tex_select;

int TexSelector::WclSelect(uint32_t hash)
{
    return g_mod_textures.IndexOf(PortApi::SelectTextureOverride(hash));
}

int TexSelector::Touched(int sel, unsigned mip)
{
    g_mod_textures.TouchAt(sel, mip);
    return sel;
}

unsigned TexSelector::DrawnMip(uint32_t hash, uint32_t target)
{
    if (!g_settings.tex_distance || !hash) return ModTextureStore::kMipUnknown;
    if (hash == target) return 0;
    const unsigned k = g_tex_lod.MipOf(hash);
    return k ? k : ModTextureStore::kMipUnknown;
}

D3D12_CPU_DESCRIPTOR_HANDLE TexSelector::ModSrvByResourceFq(const std::wstring& res_fq)
{
    ModTextureStore::Entry e;
    return g_mod_textures.EntryAt(Touched(g_mod_textures.IndexOf(PortApi::FindCustomResource(res_fq.c_str())),
                                      ModTextureStore::kMipUnknown),
               &e)
        ? e.srv
        : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
}

D3D12_CPU_DESCRIPTOR_HANDLE TexSelector::RfxMapSrv(const std::wstring& res_fq, const wchar_t* ns)
{
    if (res_fq.empty()) return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    const uint32_t sw = PortApi::SwitchOfResource(res_fq.c_str());
    const int sel = sw ? SelectIdxNs(sw, ns, NsHash(ns), ModTextureStore::kMipUnknown) : -1;
    ModTextureStore::Entry e;
    if (sel < 0 || !g_mod_textures.EntryAt(sel, &e)) return ModSrvByResourceFq(res_fq);
    if (g_debug && DiagChanged("RFX-SWITCH", (int)(sw & 0x7fffffff), (unsigned long long)(unsigned)sel)) {
        const bool named = sel == g_mod_textures.IndexOf(PortApi::FindCustomResource(res_fq.c_str()));
        logger::write("RFX-SWITCH %ls: [TextureOverride] hash=%08x switches its variants; bound its selection %d (%s)",
            res_fq.c_str(), sw, sel, named ? "the named one" : "NOT the named one: the switch wins");
    }
    return e.srv;
}

namespace {
struct FlickerSlot {
    uint32_t hash;
    int sel;
    int logged;
};
thread_local FlickerSlot t_flick[512] = {};
}

void TexSelector::FlickerWatch(uint32_t hash, int sel)
{
    if (flicker_watch_.load(std::memory_order_relaxed) <= 0) return;
    FlickerSlot& s = t_flick[hash & 511];
    if (s.hash != hash) {
        s.hash = hash;
        s.sel = sel;
        s.logged = 0;
        return;
    } // new occupant of this slot
    if (s.sel == sel) return;
    const int was = s.sel;
    s.sel = sel;
    if (s.logged >= 4) return; // a flipping texture proves itself in a few lines
    ++s.logged;
    // Their section name, so a flipping texture names the mod responsible rather than just a hash.
    wchar_t owner[256] = {};
    PortApi::TextureOverrideSection(hash, owner, std::size(owner));
    logger::write(
        "TEXFLICKER hash=%08lx  %d -> %d   [%ls]", (unsigned long)hash, was, sel, owner[0] ? owner : L"(no section)");
}

void TexSelector::TickFlickerWatch()
{
    int f = flicker_watch_.load(std::memory_order_relaxed);
    if (f > 0) flicker_watch_.store(f - 1, std::memory_order_relaxed);
}

int TexSelector::SelectIdx(uint32_t hash, unsigned mip)
{
    int wsel = WclSelect(hash);
    ModTextureStore::Entry e;
    const int sel = (wsel >= 0 && g_mod_textures.EntryAt(wsel, &e)) ? wsel : -1; // else: no section, or vanilla
    FlickerWatch(hash, sel);
    return Touched(sel, mip);
}

int TexSelector::SelectIdxNs(uint32_t hash, const wchar_t* ns, uint64_t ns_hash, unsigned mip)
{
    struct Sel {
        uint32_t hash;
        int sel;
        unsigned load_gen;
        unsigned inputs_gen;
        unsigned tex_gen;
        long churn;
    };
    if (!PortApi::SelectionCacheOk()) {
        const int wsel = g_mod_textures.IndexOf(PortApi::SelectTextureOverrideNs(hash, ns));
        ModTextureStore::Entry e;
        return Touched((wsel >= 0 && g_mod_textures.EntryAt(wsel, &e)) ? wsel : -1, mip);
    }
    thread_local std::unordered_map<uint64_t, Sel> t_sel;
    const unsigned load_gen = PortApi::LoadGeneration();
    const uint64_t key = (ns_hash ^ hash) * 0x9E3779B97F4A7C15ull;
    const unsigned inputs_gen = PortApi::SelectionInputsGen();
    const unsigned tex_gen = g_mod_textures.Gen();
    const long churn = PortApi::WrapChurn();
    auto it = t_sel.find(key);
    if (it != t_sel.end() && it->second.hash == hash && it->second.load_gen == load_gen &&
        it->second.inputs_gen == inputs_gen && it->second.tex_gen == tex_gen && it->second.churn == churn)
        return Touched(it->second.sel, mip);
    bool cacheable = false;
    const int wsel = g_mod_textures.IndexOf(PortApi::SelectTextureOverrideNs(hash, ns, &cacheable));
    ModTextureStore::Entry e;
    const int sel = (wsel >= 0 && g_mod_textures.EntryAt(wsel, &e)) ? wsel : -1;
    if (cacheable)
        t_sel[key] = Sel{ hash, sel, load_gen, inputs_gen, tex_gen, churn };
    else if (it != t_sel.end())
        t_sel.erase(it);
    return Touched(sel, mip);
}

uint32_t TexSelector::RuleHashFor(uint32_t hash)
{
    if (const uint32_t a = g_tex_pipe_alias.Of(hash); a && g_tex_rules.Now().hash_to_rule.count(a)) return a;
    return ProvenRuleFor(hash);
}

uint32_t TexSelector::ProvenRuleFor(uint32_t hash)
{
    if (!hash) return 0;
    const TexRules& tr = g_tex_rules.Now();
    if (tr.hash_to_rule.find(hash) != tr.hash_to_rule.end()) return hash;
    const uint32_t tgt = g_tex_lod.Lookup(hash);
    return (tgt && tr.hash_to_rule.find(tgt) != tr.hash_to_rule.end()) ? tgt : 0;
}

uint32_t TexSelector::LodTargetOf(uint32_t hash)
{
    if (const uint32_t a = hash ? g_tex_pipe_alias.Of(hash) : 0) return a;
    return hash ? g_tex_lod.Lookup(hash) : 0;
}

uint32_t TexSelector::ResolveBound(uint64_t heap, uint32_t base, uint32_t num, ID3D12Resource** res, uint32_t* hash,
    uint32_t cap, const std::bitset<512>* want)
{
    static_assert(kTexScanMax == 512, "the read-slot mask is sized by kTexScanMax");
    FlatHeapMap::Entry* const e = g_descriptors.EntryOf(heap);
    uint32_t n = 0;
    for (uint32_t s = 0; s < cap; ++s) {
        const uint32_t slot = base + s;
        if (slot >= num) break;
        ResInfo ri;
        const bool read = !want || (s < kTexScanMax && want->test(s));
        res[s] = (read && e && g_descriptors.FindIn(e, slot, ri)) ? ri.res : nullptr;
        hash[s] = 0;
        n = s + 1;
    }
    const uint32_t gen = g_tex_identity.Gen();
    uint32_t miss[kTexScanMax];
    uint32_t nm = 0;
    for (uint32_t s = 0; s < n; ++s) {
        if (!res[s]) continue;
        const uint64_t r = reinterpret_cast<uint64_t>(res[s]);
        const TexIdentityTable::Memo& m = TexIdentityTable::MemoAt(r);
        if (m.res == r && m.gen == gen)
            hash[s] = m.hash;
        else if (nm < kTexScanMax)
            miss[nm++] = s;
    }
    if (nm) g_tex_identity.HashesOf(res, miss, nm, hash, gen);
    return n;
}

D3D12_CPU_DESCRIPTOR_HANDLE TexSelector::Replacement(uint32_t hash)
{
    ModTextureStore::Entry e;
    return g_mod_textures.EntryAt(SelectIdx(hash, ModTextureStore::kMipUnknown), &e) ? e.srv
                                                                                     : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
}
