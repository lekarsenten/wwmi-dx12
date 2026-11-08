// ===========================================================================
//  wwmi_mod_engine.cpp -- the mod's layout, READ FROM THE PORTED ENGINE.
//
//  This file is the WHOLE reader: no second parser re-reads the .ini files their IniHandler has
//  already read.
//
//  Every value below is READ, never derived:
//     index / hash / match_first_index / match_index_count   [TextureOverrideComponentN] keys, and those of
//                                                            any other section matching an IB's index range
//     vg_offset / vg_count                                   $\WWMIv1\vg_offset, $\WWMIv1\vg_count
//     drawindexed sub-draws                                  the section's lines, IN FILE ORDER,
//                                                            including those reached by `run =`
//     remapped / remap_id / remap_vg                         [CommandListRemapMergedSkeleton]
//     resources + their filenames                            their [Resource*] store, ABSOLUTE paths
//     object_guid / mesh_vertex_count                        [Constants] globals, via their var store
//     the mod folder                                         their ini_path bookkeeping, not the ns
//
//  NAME SHAPES ARE NOT GUESSED. They were dumped from the live parse with `parse_test.exe -dumpns`
//  (writes port_sections.txt):
//     TextureOverride\SomeMod\Component0              <- author case, ResolveSection finds it
//     resource\somemod\blendbuffer  <abs filename>    <- lowercased
//     customshader\mods\somemod\mod.ini\transparency_1
//  ResolveSection indexes TextureOverride* and CommandList* only, so a [CustomShader*] is reached by
//  CONSTRUCTING `CustomShader\<ns>\<tail>`. That is proven, not assumed: the same harness reports
//  "customshader run-sites: 3 referenced, 3 reachable by constructed name, 3 drawindexed inside them".
// ===========================================================================
#include "wwmi_mod.h"
#include "../migoto/port_api.h"
#include "../core/log.h"

#include <algorithm>
#include <cstdio> // swscanf_s -- drawindexed parsing
#include <cwctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace WwmiMod {

namespace {

std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool ParseDrawIndexed(const std::wstring& val, SubDraw& out)
{
    if (Lower(val).rfind(L"auto", 0) == 0) return false; // handled by the draw hook, not a fixed range
    unsigned c = 0;
    unsigned s = 0;
    if (swscanf_s(val.c_str(), L" %u , %u", &c, &s) < 2) return false;
    out.count = c;
    out.start = s;
    return c != 0;
}

std::wstring NsOfFq(const std::wstring& fq)
{
    const size_t first = fq.find(L'\\');
    const size_t last = fq.find_last_of(L'\\');
    if (first == std::wstring::npos || last == std::wstring::npos || first >= last) return std::wstring();
    return fq.substr(first + 1, last - first - 1);
}

std::wstring TailOfFq(const std::wstring& fq)
{
    const size_t last = fq.find_last_of(L'\\');
    return last == std::wstring::npos ? fq : fq.substr(last + 1);
}

const wchar_t* AfterPsSlot(const wchar_t* p)
{
    if (_wcsnicmp(p, L"ps-t", 4) != 0) return nullptr;
    const wchar_t* q = p + 4;
    if (*q == L'[') {
        while (*q && *q != L']') ++q;
        return *q ? q + 1 : nullptr;
    }
    if (!iswdigit(*q)) return nullptr;
    while (iswdigit(*q)) ++q;
    return q;
}

bool NamesPsSlot(const wchar_t* s, bool metadata_only)
{
    if (!s) return false;
    for (const wchar_t* p = s; *p; ++p) {
        const wchar_t* q = AfterPsSlot(p);
        if (!q) continue;
        if (!metadata_only) return true;
        while (*q == L' ') ++q;
        if (q[0] == L'-' && q[1] == L'>') return true;
    }
    return false;
}

bool IsIniParam(const wchar_t* k)
{
    if (!k || !*k || !wcschr(L"xyzwXYZW", *k)) return false;
    for (const wchar_t* p = k + 1; *p; ++p)
        if (!iswdigit(*p)) return false;
    return true;
}

bool QueriesPsSlot(const wchar_t* line)
{
    if (!line) return false;
    while (*line == L' ' || *line == L'\t') ++line;
    if (!_wcsnicmp(line, L"if ", 3) || !_wcsnicmp(line, L"elif ", 5) || !_wcsnicmp(line, L"else if ", 8))
        return NamesPsSlot(line, false);
    const wchar_t* eq = wcschr(line, L'=');
    if (!eq) return NamesPsSlot(line, true);
    const wchar_t* k = line;
    for (;;) {
        if (!_wcsnicmp(k, L"pre ", 4))
            k += 4;
        else if (!_wcsnicmp(k, L"post ", 5))
            k += 5;
        else if (!_wcsnicmp(k, L"local ", 6))
            k += 6;
        else
            break;
        while (*k == L' ') ++k;
    }
    std::wstring target(k, eq);
    while (!target.empty() && (target.back() == L' ' || target.back() == L'\t')) target.pop_back();
    if ((!target.empty() && target[0] == L'$') || IsIniParam(target.c_str())) return NamesPsSlot(eq + 1, false);
    return NamesPsSlot(line, true);
}

struct KeyCtx {
    Component* comp;
    std::vector<SubDraw>* draws;
    std::wstring ns; // author-case namespace, for constructing CustomShader names
    std::wstring pending_cond; // `if <expr>` currently open; recorded for the log only
    int depth; // run-expansion depth guard
};

void OnKey(const wchar_t* key, const wchar_t* val, void* user);

void ExpandRun(KeyCtx* k, const std::wstring& target)
{
    if (k->depth >= 4) return; // pathological self-reference guard
    if (_wcsnicmp(target.c_str(), L"CustomShader", 12) != 0) return;
    const std::wstring tail = target.substr(12);
    if (tail.empty() || tail[0] == L'\\') return; // namespaced framework shader, not a part

    const std::wstring fq = L"CustomShader\\" + k->ns + L"\\" + tail;
    KeyCtx sub{ k->comp, k->draws, k->ns, k->pending_cond, k->depth + 1 };
    PortApi::ForEachSectionKey(fq.c_str(), &OnKey, &sub);
}

void OnKey(const wchar_t* key, const wchar_t* val, void* user)
{
    KeyCtx* k = (KeyCtx*)user;
    const std::wstring lk = Lower(key ? key : L"");
    const std::wstring v = val ? val : L"";

    if (lk == L"hash") {
        k->comp->ib_hash = (uint32_t)wcstoul(v.c_str(), nullptr, 16);
        return;
    }
    if (lk == L"match_first_index") {
        k->comp->match_first_index = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }
    if (lk == L"match_index_count") {
        k->comp->match_index_count = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }
    if (lk == L"match_priority") {
        k->comp->match_priority = (int)wcstol(v.c_str(), nullptr, 10);
        return;
    }

    // WWMI passes the merge window as namespaced variables rather than section keys.
    if (lk.find(L"vg_offset") != std::wstring::npos) {
        k->comp->vg_offset = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }
    if (lk.find(L"vg_count") != std::wstring::npos) {
        k->comp->vg_count = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }

    if (lk == L"if") {
        k->pending_cond = v;
        return;
    }
    if (lk == L"endif") {
        k->pending_cond.clear();
        return;
    }

    if (lk == L"run") {
        ExpandRun(k, v);
        return;
    }

    if (lk == L"drawindexed") {
        SubDraw d{};
        if (ParseDrawIndexed(v, d)) {
            d.cond = k->pending_cond;
            k->draws->push_back(d);
        }
        return;
    }
}

struct RemapInfo {
    int comp_idx;
    int id;
    uint32_t vg;
};

struct RemapCtx {
    std::vector<RemapInfo>* out;
    int rm_id = -1;
    uint32_t rm_vg = 0;
};

void OnRemapKey(const wchar_t* key, const wchar_t* val, void* user)
{
    RemapCtx* r = (RemapCtx*)user;
    const std::wstring lk = Lower(key ? key : L"");
    const std::wstring v = val ? val : L"";
    static const std::wstring kPrimary = L"resourceremappedskeletoncomponent";

    if (lk == L"$\\wwmiv1\\blend_remap_id") {
        r->rm_id = _wtoi(v.c_str());
        return;
    }
    if (lk == L"$\\wwmiv1\\vg_count") {
        r->rm_vg = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }

    if (lk.rfind(kPrimary, 0) == 0) {
        // "ResourceRemappedSkeletonComponentN = copy ResourceRemappedSkeletonRW" -- PRIMARY remap output
        if (Lower(v).rfind(L"copy", 0) == 0)
            r->out->push_back({ _wtoi(lk.c_str() + kPrimary.size()), r->rm_id, r->rm_vg });
        return;
    }
}

struct ResCtx {
    const std::wstring* ns_lower;
    std::vector<Resource>* out;
};

bool MakeResource(const std::wstring& fq, const wchar_t* filename, Resource& r)
{
    const size_t last = fq.find_last_of(L'\\');
    if (last == std::wstring::npos) return false;
    r.name = L"Resource" + fq.substr(last + 1);
    r.fq = fq; // their key -- how the buffer is asked for later
    r.filename = filename ? filename : L"";
    return true;
}

void OnResource(const wchar_t* fq_name, const wchar_t* filename, int /*type*/, bool /*substantiated*/,
    ID3D12Resource* /*dx12*/, void* user)
{
    ResCtx* c = (ResCtx*)user;
    if (!fq_name) return;
    const std::wstring fq = fq_name;
    if (Lower(NsOfFq(fq)) != *c->ns_lower) return;
    Resource r;
    if (MakeResource(fq, filename, r)) c->out->push_back(std::move(r));
}

void OnResourceGrouped(const wchar_t* fq_name, const wchar_t* filename, int /*type*/, bool /*substantiated*/,
    ID3D12Resource* /*dx12*/, void* user)
{
    if (!fq_name) return;
    const std::wstring fq = fq_name;
    Resource r;
    if (MakeResource(fq, filename, r))
        (*(std::unordered_map<std::wstring, NsEntries>*)user)[Lower(NsOfFq(fq))].resources.push_back(std::move(r));
}

NsEntries EntriesOfNs(const std::wstring& ns_lower)
{
    NsEntries e;
    struct RangedCtx {
        const std::wstring* ns_lower;
        std::vector<std::wstring>* fqs;
    } rc{ &ns_lower, &e.ranged_overrides };
    PortApi::ForEachTextureOverride(
        [](uint32_t hash, const wchar_t* fq, bool ranged, const wchar_t*, bool, void* user) {
            auto* c = (RangedCtx*)user;
            if (hash && ranged && fq && Lower(NsOfFq(fq)) == *c->ns_lower) c->fqs->push_back(fq);
        },
        &rc);
    std::sort(e.ranged_overrides.begin(), e.ranged_overrides.end());
    ResCtx res{ &ns_lower, &e.resources };
    PortApi::ForEachCustomResource(&OnResource, &res);
    return e;
}

} // namespace

std::unordered_map<std::wstring, NsEntries> GroupByNamespace()
{
    std::unordered_map<std::wstring, NsEntries> groups;
    PortApi::ForEachTextureOverride(
        [](uint32_t hash, const wchar_t* fq, bool ranged, const wchar_t*, bool, void* user) {
            if (hash && ranged && fq)
                (*(std::unordered_map<std::wstring, NsEntries>*)user)[Lower(NsOfFq(fq))].ranged_overrides.push_back(
                    fq);
        },
        &groups);
    for (auto& kv : groups) std::sort(kv.second.ranged_overrides.begin(), kv.second.ranged_overrides.end());
    PortApi::ForEachCustomResource(&OnResourceGrouped, &groups);
    return groups;
}

// Read one component section into `out`. `fq_section` is their registered name.
static bool ReadComponent(const std::wstring& fq_section, int index, Component& out)
{
    out = Component{};
    out.index = index;
    out.tail = TailOfFq(fq_section);
    KeyCtx ctx{ &out, &out.draws, NsOfFq(fq_section), std::wstring(), 0 };
    PortApi::ForEachSectionKey(fq_section.c_str(), &OnKey, &ctx);
    return true;
}

// The whole mod layout for a namespace, straight from the engine. Returns the number of components read.
static int ReadModFrom(const std::wstring& ns, const std::unordered_map<std::wstring, NsEntries>* groups, Mod& out)
{
    out = Mod{};

    std::wstring ns_fq; // the namespace exactly as their parser spells it, taken from a real section
    {
        wchar_t probe[512] = {};
        if (PortApi::ResolveSection(ns.c_str(), nullptr, L"Component0", probe, std::size(probe)) && probe[0]) {
            ns_fq = NsOfFq(probe);
            wchar_t folder[512] = {};
            if (PortApi::SectionFolder(probe, folder, std::size(folder))) out.dir = folder;
        }
    }
    if (ns_fq.empty()) ns_fq = ns;
    const std::wstring ns_lower = Lower(ns_fq);
    NsEntries own;
    const NsEntries* entries = &own;
    if (!groups)
        own = EntriesOfNs(ns_lower);
    else if (const auto it = groups->find(ns_lower); it != groups->end())
        entries = &it->second;

    // Components are numbered from 0 and dense in every shipped WWMI mod: the first gap ends the scan.
    for (int i = 0;; ++i) {
        wchar_t fq[512] = {};
        const std::wstring sec = L"Component" + std::to_wstring(i);
        if (!PortApi::ResolveSection(ns.c_str(), nullptr, sec.c_str(), fq, std::size(fq)) || !fq[0]) break;
        Component c;
        ReadComponent(fq, i, c);
        out.comps.push_back(std::move(c));
    }

    {
        int next = 0;
        for (const Component& c : out.comps) next = (std::max)(next, c.index + 1);
        for (const std::wstring& fq : entries->ranged_overrides) {
            const std::wstring tail = TailOfFq(fq);
            if (std::any_of(out.comps.begin(), out.comps.end(),
                    [&](const Component& c) { return _wcsicmp(c.tail.c_str(), tail.c_str()) == 0; }))
                continue;
            Component c;
            ReadComponent(fq, next, c);
            if (!c.ib_hash || !c.match_index_count) continue;
            if (out.dir.empty()) {
                wchar_t folder[512] = {};
                if (PortApi::SectionFolder(fq.c_str(), folder, std::size(folder))) out.dir = folder;
            }
            out.comps.push_back(std::move(c));
            ++next;
        }
    }

    out.comps.erase(std::remove_if(out.comps.begin(), out.comps.end(),
                        [](const Component& c) { return c.match_index_count == 0 && c.draws.empty(); }),
        out.comps.end());

    {
        std::vector<RemapInfo> remaps;
        wchar_t fq[512] = {};
        if (PortApi::ResolveSection(ns.c_str(), nullptr, L"RemapMergedSkeleton", fq, std::size(fq)) && fq[0]) {
            RemapCtx rc;
            rc.out = &remaps;
            PortApi::ForEachSectionKey(fq, &OnRemapKey, &rc);
        }
        for (const auto& ri : remaps)
            for (auto& c : out.comps)
                if (c.index == ri.comp_idx) {
                    c.remapped = true;
                    c.remap_id = ri.id;
                    c.remap_vg = ri.vg;
                }
    }

    // Resources, from their [Resource*] store, filtered to this namespace.
    {
        out.resources = entries->resources;
        std::set<std::wstring> seen;
        std::vector<Resource> uniq;
        uniq.reserve(out.resources.size());
        for (auto& r : out.resources)
            if (seen.insert(Lower(r.name)).second) uniq.push_back(std::move(r));
        out.resources.swap(uniq);
        for (const auto& r : out.resources)
            if (Lower(r.name) == L"resourceblendremapforwardbuffer") {
                out.forward_map_file = r.filename;
                break;
            }
    }

    PortApi::ForEachSectionInNs(
        ns_fq.c_str(),
        [](const wchar_t* fq, void* user) {
            bool* hit = (bool*)user;
            if (*hit) return;
            PortApi::ForEachSectionLine(
                fq,
                [](const wchar_t* line, void* u) {
                    bool* h = (bool*)u;
                    if (!*h) *h = QueriesPsSlot(line);
                },
                user);
        },
        &out.queries_ps_slots);

    // [Constants] globals, from their variable store rather than a re-read of the file.
    out.object_guid = (int)PortApi::GetVarNs(ns.c_str(), L"object_guid", 0.f);
    out.mesh_vertex_count = (uint32_t)PortApi::GetVarNs(ns.c_str(), L"mesh_vertex_count", 0.f);

    logger::write("WwmiMod: ns='%ls' -> %zu comp(s), %zu resource(s), guid=%d verts=%u dir='%ls'%s%s", ns.c_str(),
        out.comps.size(), out.resources.size(), out.object_guid, out.mesh_vertex_count, out.dir.c_str(),
        out.forward_map_file.empty() ? "" : " (has fwdmap)",
        out.queries_ps_slots ? " (asks what the game bound in ps-tN: every read slot published)" : "");
    return (int)out.comps.size();
}

int ReadMod(const std::wstring& ns, const std::unordered_map<std::wstring, NsEntries>& groups, Mod& out)
{
    return ReadModFrom(ns, &groups, out);
}

int ReadComponentsFromEngine(const std::wstring& ns, Mod& out)
{
    return ReadModFrom(ns, nullptr, out);
}

} // namespace WwmiMod
