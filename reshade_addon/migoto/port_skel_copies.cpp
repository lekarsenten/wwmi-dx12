// port_skel_copies: one merged skeleton per copy of a character drawn in a frame. See port_api.h.
#include "port_api_internal.h"
#include "globals.h"
#include "IniHandler.h"
#include "CommandList.h"
#include "selection_watch.h"
#include "../core/log.h"

#include <iterator>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace PortApi {
namespace {

struct ResState {
    ID3D11Resource* resource = nullptr;
    ID3D11View* view = nullptr;
    ID3D11Device* device = nullptr;
    bool is_null = true;
    bool substantiated = false;
    ResourcePoolCache pool;
    std::unique_ptr<ResourceHandleInfo> handle_info;
    unsigned frame_no = 0;
    int copies_this_frame = 0;
};

struct Copy {
    std::vector<ResState> res;
    std::vector<float> vars;
    bool drawn = false;
};

struct Skel {
    std::vector<CustomResource*> res;
    std::vector<char> draw_written;
    std::vector<CommandListVariable*> vars;
    std::vector<float> start;
    std::wstring update_fq;
    std::vector<Copy> copies;
};

struct Swapped {
    std::wstring ns;
    unsigned copy = 0;
    std::vector<char> res;
};

std::unordered_map<std::wstring, Skel> g_skels;
unsigned g_skels_gen = ~0u;

std::wstring Lower(const wchar_t* s)
{
    std::wstring out = s ? s : L"";
    for (auto& ch : out) ch = (wchar_t)towlower(ch);
    return out;
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix)
{
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

bool IsGate(const std::wstring& name)
{
    const size_t at = (!name.empty() && name[0] == L'$') ? 1 : 0;
    const std::wstring bare = name.substr(at);
    return StartsWithI(bare, L"merge_status_id") || StartsWithI(bare, L"state_id");
}

void Exchange(CustomResource& r, ResState& s)
{
    std::swap(r.resource, s.resource);
    std::swap(r.view, s.view);
    std::swap(r.device, s.device);
    std::swap(r.is_null, s.is_null);
    std::swap(r.substantiated, s.substantiated);
    r.resource_pool.cache.swap(s.pool);
    r.handle_info.swap(s.handle_info);
    std::swap(r.frame_no, s.frame_no);
    std::swap(r.copies_this_frame, s.copies_this_frame);
}

void ExchangeVars(Skel& s, Copy& c)
{
    bool watched_changed = false;
    for (size_t i = 0; i < s.vars.size(); ++i) {
        float& live = s.vars[i]->fval;
        if (memcmp(&live, &c.vars[i], sizeof live) != 0 && MigotoPort::g_selection_watch.VarWatched(&live))
            watched_changed = true;
        std::swap(live, c.vars[i]);
    }
    if (watched_changed) MigotoPort::g_selection_watch.BumpInputs();
}

void Drop(ResState& s)
{
    if (s.view) s.view->Release();
    if (s.resource) s.resource->Release();
    for (auto& kv : s.pool)
        if (kv.second.first) kv.second.first->Release();
    s = ResState{};
}

void ForgetOnReload()
{
    if (g_skels_gen == g_generation) return;
    g_skels_gen = g_generation;
    for (auto& kv : g_skels)
        for (Copy& c : kv.second.copies)
            for (ResState& r : c.res) Drop(r);
    g_skels.clear();
}

Skel& SkelOf(const wchar_t* ns)
{
    const std::wstring key = Lower(ns);
    auto it = g_skels.find(key);
    if (it != g_skels.end()) return it->second;
    Skel s;
    const std::wstring rp = L"resource\\" + key + L"\\";
    for (auto& kv : customResources) {
        if (!StartsWithI(kv.first, rp)) continue;
        const std::wstring tail = Lower(kv.first.c_str() + rp.size());
        const bool rw = tail == L"mergedskeletonrw" || tail == L"extramergedskeletonrw";
        const bool out = tail == L"mergedskeleton" || tail == L"extramergedskeleton" ||
            StartsWithI(tail, L"remappedskeletoncomponent") || StartsWithI(tail, L"extraremappedskeletoncomponent");
        if (!rw && !out) continue;
        s.res.push_back(&kv.second);
        s.draw_written.push_back(rw ? 1 : 0);
    }
    const std::wstring vp = L"$\\" + key + L"\\";
    for (auto& kv : command_list_globals)
        if (StartsWithI(kv.first, vp) && IsGate(kv.first.substr(vp.size()))) s.vars.push_back(&kv.second);
    const std::wstring tp = L"textureoverride\\" + key + L"\\";
    for (auto& kv : SectionIndex()) {
        if (!StartsWithI(kv.first, tp) || !kv.second) continue;
        for (CommandListVariable& v : kv.second->command_list.static_vars)
            if (IsGate(v.name)) s.vars.push_back(&v);
    }
    wchar_t fq[512] = {};
    if (ResolveSection(ns, nullptr, L"UpdateMergedSkeleton", fq, std::size(fq)) && fq[0]) s.update_fq = fq;
    logger::write("SKCOPIES ns='%ls': %zu skeleton resource(s), %zu merge gate(s), update section %ls -- %s", ns,
        s.res.size(), s.vars.size(), s.update_fq.empty() ? L"(none)" : s.update_fq.c_str(),
        s.res.empty() || s.update_fq.empty() ? "no merged skeleton here: copies share the ini's own"
                                             : "each copy drawn in a frame gets its own");
    return g_skels.emplace(key, std::move(s)).first->second;
}

Copy& CopyOf(Skel& s, unsigned copy)
{
    if (s.copies.size() <= copy) s.copies.resize(copy + 1);
    Copy& c = s.copies[copy];
    if (c.res.size() != s.res.size()) {
        c.res.clear();
        c.res.resize(s.res.size());
        c.vars.resize(s.vars.size());
        for (size_t i = 0; i < s.vars.size(); ++i)
            c.vars[i] = s.start.size() == s.vars.size() ? s.start[i] : s.vars[i]->fval;
    }
    return c;
}

} // namespace

SkelCopyScope::SkelCopyScope(const wchar_t* ns, unsigned copy)
{
    if (!ns || !*ns) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    ForgetOnReload();
    Skel& s = SkelOf(ns);
    if (!copy || s.res.empty() || s.update_fq.empty()) return;
    Copy& c = CopyOf(s, copy);
    Swapped* w = new Swapped{ Lower(ns), copy, std::vector<char>(s.res.size(), 0) };
    for (size_t i = 0; i < s.res.size(); ++i) {
        if (!s.draw_written[i] && !c.res[i].resource) continue;
        Exchange(*s.res[i], c.res[i]);
        w->res[i] = 1;
    }
    ExchangeVars(s, c);
    c.drawn = true;
    swapped_ = w;
}

SkelCopyScope::~SkelCopyScope()
{
    Swapped* w = static_cast<Swapped*>(swapped_);
    if (!w) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    auto it = g_skels.find(w->ns);
    if (it != g_skels.end() && g_skels_gen == g_generation) {
        Skel& s = it->second;
        Copy& c = s.copies[w->copy];
        for (size_t i = 0; i < s.res.size(); ++i)
            if (w->res[i]) Exchange(*s.res[i], c.res[i]);
        ExchangeVars(s, c);
    }
    delete w;
}

void RunSkelCopyUpdates()
{
    ForgetOnReload();
    for (auto& kv : g_skels) {
        Skel& s = kv.second;
        if (s.res.empty() || s.update_fq.empty()) continue;
        s.start.resize(s.vars.size());
        for (size_t i = 0; i < s.vars.size(); ++i) s.start[i] = s.vars[i]->fval;
        for (size_t k = 1; k < s.copies.size(); ++k) {
            Copy& c = s.copies[k];
            if (!c.drawn) continue;
            c.drawn = false;
            bool merged = false;
            for (size_t i = 0; i < s.res.size(); ++i)
                if (s.draw_written[i] && c.res[i].resource) merged = true;
            if (!merged) continue;
            for (size_t i = 0; i < s.res.size(); ++i) Exchange(*s.res[i], c.res[i]);
            ExchangeVars(s, c);
            RunSectionByName(s.update_fq.c_str(), nullptr);
            RunPendingPost();
            for (size_t i = 0; i < s.res.size(); ++i) Exchange(*s.res[i], c.res[i]);
            ExchangeVars(s, c);
        }
    }
}

} // namespace PortApi
