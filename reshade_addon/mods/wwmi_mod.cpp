// ===========================================================================
//  wwmi_mod.cpp -- adopting a mod's buffers from their engine, plus two accessors.
//
//  This file reads no .ini files. The mod's component layout, its resources and its object
//  constants all come from THEIR parsed engine; wwmi_mod_engine.cpp is the whole reader.
//
//  It creates no resources either: LoadResources ADOPTS the DX12 buffer their
//  CustomResource::Substantiate already made from the same file, so there is one loader for a mod's
//  bytes, not two.
// ===========================================================================
#include "wwmi_mod.h"
#include "../migoto/port_api.h" // GetBufferResource -- their object, not a second copy
#include "../core/log.h"
#include <cwctype>
#include <cstring>
#include <string>

namespace WwmiMod {

static std::wstring lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

int LoadResources(Mod& m, std::vector<PortApi::HeldBuffer>& held)
{
    int n = 0;
    int missing = 0;
    for (auto& r : m.resources) {
        r.res = nullptr;
        r.gpu_va = 0;
        r.size = 0;
        if (r.fq.empty()) continue;
        PortApi::BufferResourceInfo bi;
        if (!PortApi::GetBufferResource(r.fq.c_str(), &bi) || !bi.dx12) {
            const size_t dot = r.filename.find_last_of(L'.');
            if (dot != std::wstring::npos && _wcsicmp(r.filename.c_str() + dot, L".buf") == 0) ++missing;
            continue;
        }
        void* wrapper = PortApi::AcquireBufferObject(r.fq.c_str());
        if (wrapper) held.push_back({ wrapper, r.fq });
        r.res = bi.dx12;
        r.gpu_va = bi.gpu_va;
        r.size = bi.byte_width;
        r.stride = bi.stride;
        ++n;
    }
    logger::write("WwmiMod: adopted %d buffer(s) from their engine%s", n,
        missing ? " (some file resources had no DX12 buffer -- see below)" : "");
    if (missing)
        for (const auto& r : m.resources)
            if (!r.res && r.filename.size() >= 4 && _wcsicmp(r.filename.c_str() + r.filename.size() - 4, L".buf") == 0)
                logger::write("  no DX12 buffer for %ls ('%ls')", r.name.c_str(), r.filename.c_str());
    return n;
}

const Resource* Find(const Mod& m, const wchar_t* name)
{
    std::wstring want = lower(name);
    for (auto& r : m.resources)
        if (lower(r.name) == want) return &r;
    return nullptr;
}

void LogSelfCheck(const Mod& m)
{
    logger::write("WwmiMod self-check: %zu components, %zu resources, fwdmap='%ls'", m.comps.size(), m.resources.size(),
        m.forward_map_file.c_str());
    for (const auto& c : m.comps) {
        int ndraws = (int)c.draws.size();
        if (c.remapped)
            logger::write("  comp%d first=%u count=%u vg_off=%u vg_cnt=%u REMAPPED id=%d remap_vg=%u draws=%d", c.index,
                c.match_first_index, c.match_index_count, c.vg_offset, c.vg_count, c.remap_id, c.remap_vg, ndraws);
        else
            logger::write("  comp%d first=%u count=%u vg_off=%u vg_cnt=%u direct draws=%d", c.index,
                c.match_first_index, c.match_index_count, c.vg_offset, c.vg_count, ndraws);
        for (const auto& d : c.draws)
            logger::write(
                "      draw count=%u start=%u%s%ls", d.count, d.start, d.cond.empty() ? "" : "  if ", d.cond.c_str());
    }
    int nfile = 0;
    for (const auto& r : m.resources)
        if (!r.filename.empty()) ++nfile;
    logger::write("  (%d resources have filenames)", nfile);
    const wchar_t* key[] = { L"ResourcePositionBuffer", L"ResourceBlendBuffer", L"ResourceVectorBuffer",
        L"ResourceTexcoordBuffer", L"ResourceColorBuffer", L"ResourceIndexBuffer" };
    for (const wchar_t* nm : key) {
        const Resource* r = Find(m, nm);
        if (r)
            logger::write("  %ls: file='%ls' size=%u stride=%u va=%llx%s", nm, r->filename.c_str(), r->size, r->stride,
                r->gpu_va, r->res ? "" : " (NOT LOADED)");
    }
}

} // namespace WwmiMod
