// A23: game textures' dHashes for [Filter] sections. See tex_dhash_table.h.
#include "tex_dhash_table.h"
#include "tex_dhash.h"
#include "tex_toggles.h"
#include "tex_upload_identity.h"
#include "../migoto/port_api.h"
#include <algorithm>
#include <map>

TexDHashTable g_tex_dhash;

namespace {

uint32_t MipSide(uint64_t side, uint32_t mip)
{
    return (std::max)(1u, (uint32_t)(side >> mip));
}

bool HashMip(const texdhash::Mip& m, uint64_t* linear, uint64_t* srgb)
{
    if (!texdhash::Hash(m, false, linear)) return false;
    *srgb = *linear;
    return !texdhash::HasSrgbView(m.fmt) || texdhash::Hash(m, true, srgb);
}

} // namespace

bool TexDHashTable::Wanted(const D3D12_RESOURCE_DESC& d, uint32_t sub)
{
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.DepthOrArraySize != 1 || d.SampleDesc.Count != 1)
        return false;
    if (sub >= d.MipLevels || !texdhash::Decodes((uint32_t)d.Format)) return false;
    uint32_t pick = 0;
    for (uint32_t k = d.MipLevels; k-- > 1;)
        if ((std::min)(MipSide(d.Width, k), MipSide(d.Height, k)) >= 256) {
            pick = k;
            break;
        }
    return sub == pick || (std::min)(MipSide(d.Width, sub), MipSide(d.Height, sub)) < 256;
}

void TexDHashTable::Store(uint64_t res, const Entry& e)
{
    std::lock_guard l(mutex_);
    std::vector<Entry>& v = by_res_[res];
    for (Entry& o : v)
        if (o.w == e.w && o.h == e.h) {
            o = e;
            return;
        }
    v.push_back(e);
}

void TexDHashTable::Drop(uint64_t res, uint32_t w, uint32_t h)
{
    std::lock_guard l(mutex_);
    auto it = by_res_.find(res);
    if (it == by_res_.end()) return;
    std::vector<Entry>& v = it->second;
    v.erase(std::remove_if(v.begin(), v.end(), [&](const Entry& e) { return e.w == w && e.h == h; }), v.end());
    if (v.empty()) by_res_.erase(it);
}

void TexDHashTable::FromUpload(
    ID3D12Resource* src, uint64_t offset, ID3D12Resource* dst, uint32_t sub, const TexAssetLayout& L)
{
    if (!g_tex_toggles.dhash_filters || !src || !dst) return;
    Entry e;
    e.w = L.w;
    e.h = L.h;
    if (PortApi::TextureDHashesWanted() && HashUpload(src, offset, dst->GetDesc(), sub, L, e))
        Store(reinterpret_cast<uint64_t>(dst), e);
    else
        Drop(reinterpret_cast<uint64_t>(dst), L.w, L.h);
}

bool TexDHashTable::HashUpload(ID3D12Resource* src, uint64_t offset, const D3D12_RESOURCE_DESC& d, uint32_t sub,
    const TexAssetLayout& L, Entry& e)
{
    if (!Wanted(d, sub)) return false;
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS hf = {};
    if (FAILED(src->GetHeapProperties(&hp, &hf)) || hp.Type != D3D12_HEAP_TYPE_UPLOAD) return false;
    if (offset + (uint64_t)L.pitch * (L.rows - 1) + L.tight_row > src->GetDesc().Width) return false;
    void* ptr = nullptr;
    if (FAILED(src->Map(0, nullptr, &ptr)) || !ptr) return false;
    const uint8_t* base = (const uint8_t*)ptr + offset;
    TexAssetLayout one = L;
    one.rows = 1;
    std::map<uint32_t, std::vector<uint8_t>> rows;
    texdhash::RowsRead(L.w, L.h, (uint32_t)d.Format, [&](uint32_t r) {
        if (r < L.rows) rows[r] = TightFromUpload(base + (size_t)r * L.pitch, L.pitch, one);
    });
    src->Unmap(0, nullptr);
    texdhash::Mip m;
    m.w = L.w;
    m.h = L.h;
    m.fmt = (uint32_t)d.Format;
    m.row = [&](uint32_t r) -> const uint8_t* {
        auto it = rows.find(r);
        return it == rows.end() ? nullptr : it->second.data();
    };
    return HashMip(m, &e.linear, &e.srgb);
}

void TexDHashTable::FromData(ID3D12Resource* dst, uint32_t sub, const uint8_t* data, size_t row_pitch)
{
    if (!g_tex_toggles.dhash_filters || !dst) return;
    const D3D12_RESOURCE_DESC d = dst->GetDesc();
    TexAssetLayout L;
    Entry e;
    bool hashed = false;
    if (data && PortApi::TextureDHashesWanted() && Wanted(d, sub) && TexAssetLayoutOf(d, sub, L)) {
        if (row_pitch < L.tight_row) row_pitch = L.tight_row;
        texdhash::Mip m;
        m.w = L.w;
        m.h = L.h;
        m.fmt = (uint32_t)d.Format;
        m.row = [&](uint32_t r) -> const uint8_t* { return r < L.rows ? data + (size_t)r * row_pitch : nullptr; };
        e.w = L.w;
        e.h = L.h;
        hashed = HashMip(m, &e.linear, &e.srgb);
    }
    if (hashed)
        Store(reinterpret_cast<uint64_t>(dst), e);
    else
        Unknown(reinterpret_cast<uint64_t>(dst), sub);
}

void TexDHashTable::Copy(uint64_t src, uint32_t src_sub, uint64_t dst, uint32_t dst_sub)
{
    if (!g_tex_toggles.dhash_filters || !src || !dst) return;
    {
        std::lock_guard l(mutex_);
        if (!by_res_.count(src) && !by_res_.count(dst)) return;
    }
    const D3D12_RESOURCE_DESC sd = reinterpret_cast<ID3D12Resource*>(src)->GetDesc();
    const D3D12_RESOURCE_DESC dd = reinterpret_cast<ID3D12Resource*>(dst)->GetDesc();
    if (dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || dst_sub >= dd.MipLevels) return;
    const uint32_t w = MipSide(dd.Width, dst_sub);
    const uint32_t h = MipSide(dd.Height, dst_sub);
    Entry found;
    bool have = false;
    if (sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && src_sub < sd.MipLevels &&
        MipSide(sd.Width, src_sub) == w && MipSide(sd.Height, src_sub) == h) {
        std::lock_guard l(mutex_);
        auto it = by_res_.find(src);
        if (it != by_res_.end()) {
            auto e =
                std::find_if(it->second.begin(), it->second.end(), [&](const Entry& x) { return x.w == w && x.h == h; });
            if (e != it->second.end()) {
                found = *e;
                have = true;
            }
        }
    }
    if (have)
        Store(dst, found);
    else
        Drop(dst, w, h);
}

void TexDHashTable::Unknown(uint64_t res, uint32_t sub)
{
    if (!g_tex_toggles.dhash_filters || !res) return;
    {
        std::lock_guard l(mutex_);
        if (!by_res_.count(res)) return;
    }
    const D3D12_RESOURCE_DESC d = reinterpret_cast<ID3D12Resource*>(res)->GetDesc();
    if (sub < d.MipLevels) Drop(res, MipSide(d.Width, sub), MipSide(d.Height, sub));
}

void TexDHashTable::CopyAll(uint64_t src, uint64_t dst)
{
    if (!g_tex_toggles.dhash_filters || !src || !dst) return;
    std::lock_guard l(mutex_);
    auto it = by_res_.find(src);
    if (it == by_res_.end()) {
        by_res_.erase(dst);
        return;
    }
    by_res_[dst] = it->second;
}

std::vector<TexDHashTable::Kept> TexDHashTable::KeptFor(uint64_t res)
{
    std::lock_guard l(mutex_);
    auto it = by_res_.find(res);
    return it == by_res_.end() ? std::vector<Kept>{} : it->second;
}

uint32_t TexDHashTable::KeptMips(ID3D12Resource* res)
{
    if (!g_tex_toggles.dhash_filters || !res) return 0;
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    std::lock_guard l(mutex_);
    auto it = by_res_.find(reinterpret_cast<uint64_t>(res));
    if (it == by_res_.end()) return 0;
    uint32_t mips = 0;
    for (uint32_t m = 0; m < d.MipLevels && m < 32; ++m) {
        const uint32_t w = MipSide(d.Width, m);
        const uint32_t h = MipSide(d.Height, m);
        for (const Entry& e : it->second)
            if (e.w == w && e.h == h) mips |= 1u << m;
    }
    return mips;
}

void TexDHashTable::Forget(uint64_t res)
{
    std::lock_guard l(mutex_);
    by_res_.erase(res);
}

bool TexDHashTable::Lookup(ID3D12Resource* res, uint32_t mip, uint32_t view_fmt, uint64_t* out)
{
    if (!g_tex_toggles.dhash_filters || !res) return false;
    const D3D12_RESOURCE_DESC d = res->GetDesc();
    if (mip >= d.MipLevels) return false;
    const uint32_t w = MipSide(d.Width, mip);
    const uint32_t h = MipSide(d.Height, mip);
    std::lock_guard l(mutex_);
    auto it = by_res_.find(reinterpret_cast<uint64_t>(res));
    if (it == by_res_.end()) return false;
    for (const Entry& e : it->second)
        if (e.w == w && e.h == h) {
            if (out) *out = texdhash::IsSrgb(view_fmt) ? e.srgb : e.linear;
            return true;
        }
    return false;
}

bool TexDHashLookup(ID3D12Resource* res, unsigned mip, unsigned view_format, uint64_t* out)
{
    return g_tex_dhash.Lookup(res, mip, view_format, out);
}

unsigned TexDHashMips(ID3D12Resource* res)
{
    return g_tex_dhash.KeptMips(res);
}
