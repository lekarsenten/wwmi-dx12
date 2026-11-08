// The rule set: the texture rules the loaded inis declare. See tex_rules.h.
#include "tex_rules.h"
#include "tex_identity.h"
#include "tex_lod.h"
#include "tex_replace.h"
#include "../migoto/port_api.h"
#include "../mods/mod_set.h"
#include "../core/log.h"
#include <algorithm>
#include <cwchar>
#include <filesystem>
#include <map>
#include <string>

TexRuleSet g_tex_rules;

namespace fs = std::filesystem;

struct TexRuleSet::PerSet : SetState {
    TexRules rules;
    void Build(ModSet&) override
    {
        rules = BuildTexRules();
        g_tex_lod.TargetsReady(
            [](uint32_t h, void* user) { return static_cast<TexRules*>(user)->hash_to_rule.count(h) != 0; }, &rules);
        g_tex_replace.ClearForParse();
        g_tex_rules.ready_.store(true, std::memory_order_release);
    }
};

const TexRules& TexRuleSet::Now() const
{
    return CurrentSet().State<PerSet>().rules;
}

void TexRuleSet::NoteSwap(uint32_t rule)
{
    std::lock_guard l(swap_mutex_);
    ++swap_by_hash_[rule];
}

std::unordered_set<uint32_t> TexRuleSet::RulesOfNs(const wchar_t* ns) const
{
    std::unordered_set<uint32_t> rules;
    for (const auto& kv : Now().hash_to_rule)
        if (PortApi::TextureOverrideInNs(kv.first, ns)) rules.insert(kv.first);
    return rules;
}

void TexRuleSet::TargetAudit()
{
    struct Ent {
        std::wstring leaf;
        std::wstring folder;
    };
    std::unordered_map<uint32_t, Ent> ents;
    PortApi::ForEachTextureOverride(
        [](uint32_t hash, const wchar_t* fq, bool ranged, const wchar_t* folder, bool assigns_this, void* user) {
            auto* m = (std::unordered_map<uint32_t, Ent>*)user;
            if (!hash || !fq || ranged || !assigns_this) return;
            const wchar_t* leaf = wcsrchr(fq, L'\\');
            leaf = leaf ? leaf + 1 : fq;
            (*m)[hash] = { leaf, folder ? folder : L"" };
        },
        &ents);
    std::unordered_map<uint32_t, unsigned> seen;
    g_tex_identity.ForEachHash([&](uint64_t, uint32_t hash) { ++seen[hash]; });
    std::unordered_map<uint32_t, uint32_t> swapped;
    {
        std::lock_guard l(swap_mutex_);
        swapped = swap_by_hash_;
    }
    std::map<std::wstring, std::vector<uint32_t>> by_mod;
    for (auto& kv : ents) by_mod[kv.second.folder].push_back(kv.first);
    logger::write(
        "TEXAUDIT: %zu texture targets over %zu mods; %zu resources carry a hash; %zu rule hashes ever stamped",
        ents.size(), by_mod.size(), seen.size(), swapped.size());
    for (auto& kv : by_mod) {
        std::vector<uint32_t>& hashes = kv.second;
        std::sort(hashes.begin(), hashes.end(), [&](uint32_t a, uint32_t b) { return ents[a].leaf < ents[b].leaf; });
        unsigned n_seen = 0;
        unsigned n_sw = 0;
        std::wstring never;
        std::wstring unswapped;
        wchar_t b[128];
        for (uint32_t h : hashes) {
            const unsigned s = seen.count(h) ? seen[h] : 0;
            const unsigned w = swapped.count(h) ? swapped[h] : 0;
            if (s) ++n_seen;
            if (w) ++n_sw;
            if (!s) {
                swprintf_s(b, L" %ls=%08x", ents[h].leaf.c_str(), h);
                never += b;
            } else if (!w) {
                swprintf_s(b, L" %ls=%08x(x%u)", ents[h].leaf.c_str(), h, s);
                unswapped += b;
            }
        }
        const wchar_t* fl = wcsrchr(kv.first.c_str(), L'\\');
        logger::write("TEXAUDIT %ls: targets=%zu seen=%u stamped=%u | never seen:%ls | seen, never stamped:%ls",
            fl ? fl + 1 : kv.first.c_str(), hashes.size(), n_seen, n_sw, never.empty() ? L" -" : never.c_str(),
            unswapped.empty() ? L" -" : unswapped.c_str());
    }
}

TexRules BuildTexRules()
{
    TexRules rules;
    TexRules* const tr = &rules;
    struct Ctx {
        TexRules* tr;
        int n;
    } ctx{ tr, 0 };
    PortApi::ForEachTextureOverride(
        [](uint32_t hash, const wchar_t* fq, bool ranged, const wchar_t* folder, bool assigns_this, void* user) {
            (void)folder;
            Ctx* c = (Ctx*)user;
            if (!hash || !fq || ranged || !assigns_this) return;
            c->tr->hash_to_rule.emplace(hash, c->n++);
            c->tr->targets.insert(hash);
        },
        &ctx);
    const size_t from_sections = tr->targets.size();
    PortApi::ForEachCustomResource(
        [](const wchar_t*, const wchar_t* filename, int, bool, ID3D12Resource*, void* user) {
            const fs::path f = filename ? filename : L"";
            const std::wstring nm = f.filename().wstring();
            const size_t pos = nm.find(L"t=");
            if (pos == std::wstring::npos || _wcsicmp(f.parent_path().filename().c_str(), L"Textures") != 0) return;
            const uint32_t h = (uint32_t)wcstoul(nm.c_str() + pos + 2, nullptr, 16);
            if (h) static_cast<TexRules*>(user)->targets.insert(h);
        },
        tr);
    std::string s;
    char b[16];
    for (uint32_t h : tr->targets) {
        snprintf(b, sizeof b, "%08x ", h);
        s += b;
    }
    logger::write("TexOverride runtime ready: %zu hashes from engine sections; TEXTARGETS (%zu): %s", from_sections,
        tr->targets.size(), s.c_str());
    return rules;
}
