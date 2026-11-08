// A texture's identity from its mip0 as it uploads. See tex_upload_identity.h.
#include "tex_upload_identity.h"
#include "tex_identity.h"
#include "tex_rules.h"
#include "tex_select.h"
#include "tex_lod.h"
#include "tex_crawl.h"
#include "tex_mip_identity.h"
#include "tex_readback.h"
#include "tex_look_worker.h"
#include "tex_toggles.h"
#include "../hooks/stamp_ledger.h"
#include "../instrumentation/removal_pin.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include "../core/dxgi_bc.h"
#include "../core/log.h"
#include "../migoto/port_api.h"
#include "../migoto/dx12_bridge.h" // DumpDir: D15 writes under it
#include <algorithm>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_set>
#include <smmintrin.h> // _mm_stream_load_si128 -- reads from the write-combined upload heap

TexUploadIdentity g_tex_upload;

uint32_t TexIdentityHash(uint32_t data_hash, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr, uint32_t dxgi_fmt)
{
    uint32_t desc[11] = { w, h, mips, arr, TexTypelessBC(dxgi_fmt), 1, 0, /*Usage*/ 0, /*Bind*/ 8, /*CPU*/ 0,
        /*Misc*/ 0 };
    return PortApi::Crc32c(data_hash, desc, sizeof(desc));
}

bool TexAssetLayoutOf(const D3D12_RESOURCE_DESC& d, uint32_t mip, TexAssetLayout& o)
{
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || mip >= d.MipLevels) return false;
    if (d.Flags &
        (D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL |
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        return false;
    const uint32_t fmt = (uint32_t)d.Format;
    const uint32_t bb = DxgiBcBlockBytes(fmt);
    const uint32_t bpp = bb ? 0 : DxgiPlainBytesPerPixel(fmt);
    if (!bb && !bpp) return false;
    o.w = std::max<uint32_t>(1u, (uint32_t)d.Width >> mip);
    o.h = std::max<uint32_t>(1u, d.Height >> mip);
    o.rows = bb ? (o.h + 3) / 4 : o.h;
    o.tight_row = bb ? ((o.w + 3) / 4) * bb : o.w * bpp;
    o.pitch = (o.tight_row + 255u) & ~255u;
    o.floor = (uint64_t)o.w * o.h >= kTexHashFloorPx;
    return true;
}

std::vector<uint8_t> TightFromUpload(const uint8_t* src, size_t pitch, const TexAssetLayout& L)
{
    std::vector<uint8_t> tight;
    {
        PROF(PROF_CB_Alloc);
        tight.resize((size_t)L.tight_row * L.rows);
    }
    PROF(PROF_CB_Copy);
    for (uint32_t r = 0; r < L.rows; ++r) {
        const uint8_t* s = src + (size_t)r * pitch;
        uint8_t* d = tight.data() + (size_t)r * L.tight_row;
        size_t n = L.tight_row;
        for (; n && ((uintptr_t)s & 15); --n) *d++ = *s++;
        for (; n >= 64; n -= 64, s += 64, d += 64) {
            const __m128i a = _mm_stream_load_si128((__m128i*)(s));
            const __m128i b = _mm_stream_load_si128((__m128i*)(s + 16));
            const __m128i c = _mm_stream_load_si128((__m128i*)(s + 32));
            const __m128i e = _mm_stream_load_si128((__m128i*)(s + 48));
            _mm_storeu_si128((__m128i*)(d), a);
            _mm_storeu_si128((__m128i*)(d + 16), b);
            _mm_storeu_si128((__m128i*)(d + 32), c);
            _mm_storeu_si128((__m128i*)(d + 48), e);
        }
        for (; n >= 16; n -= 16, s += 16, d += 16) _mm_storeu_si128((__m128i*)d, _mm_stream_load_si128((__m128i*)s));
        for (; n; --n) *d++ = *s++;
    }
    return tight;
}

void DumpIfDegenerate(
    uint32_t hash, const std::vector<uint8_t>& tight, const TexAssetLayout& L, uint32_t fmt, uint32_t sub, uint32_t of)
{
    if (!g_tex_toggles.dump_degenerates || !g_tex_lod.Degenerate(hash)) return;
    static std::mutex s_mtx;
    static std::unordered_set<uint32_t> s_done;
    {
        std::lock_guard l(s_mtx);
        if (!s_done.insert(hash).second) return;
    }
    const std::wstring dir = MigotoPort::DumpDir() + L"\\degenerates";
    CreateDirectoryW(MigotoPort::DumpDir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%08x.dds", dir.c_str(), hash);
    if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) return;
    uint32_t dxgi = fmt;
    switch (dxgi) {
    case 27:
    case 70:
    case 73:
    case 76:
    case 79:
    case 82:
    case 94:
    case 97:
        ++dxgi; // TYPELESS -> UNORM (BC6H: UF16), which a viewer opens
        break;
    case 90: dxgi = 87; break;
    }
    const bool bc = DxgiBcBlockBytes(fmt) != 0;
    uint32_t hdr[37] = {}; // "DDS ", DDS_HEADER, DDS_HEADER_DXT10
    hdr[0] = 0x20534444;
    hdr[1] = 124;
    hdr[2] = 0x1 | 0x2 | 0x4 | 0x1000 | (bc ? 0x80000 : 0x8);
    hdr[3] = L.h;
    hdr[4] = L.w;
    hdr[5] = bc ? (uint32_t)tight.size() : L.tight_row;
    hdr[7] = 1;
    hdr[19] = 32;
    hdr[20] = 0x4;
    hdr[21] = 0x30315844;
    hdr[27] = 0x1000;
    hdr[32] = dxgi;
    hdr[33] = 3;
    hdr[35] = 1;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path, L"wb") != 0 || !f) {
        logger::write("DEGENERATE-DUMP %08x: cannot write %S", hash, path);
        return;
    }
    fwrite(hdr, sizeof hdr, 1, f);
    fwrite(tight.data(), 1, tight.size(), f);
    fclose(f);
    if (sub)
        logger::write("DEGENERATE-DUMP %08x %ux%u fmt=%u: mip %u of %08x -> %S", hash, L.w, L.h, fmt, sub, of, path);
    else
        logger::write("DEGENERATE-DUMP %08x %ux%u fmt=%u: a resident's mip0 -> %S", hash, L.w, L.h, fmt, path);
}

void TexUploadIdentity::HashFromUpload(ID3D12Resource* dest, const uint8_t* src, size_t src_row_pitch)
{
    if (!dest || !src) return;
    auto d = dest->GetDesc();
    TexAssetLayout L;
    if (!TexAssetLayoutOf(d, 0, L) || !L.floor) return;
    const uint32_t w = L.w;
    const uint32_t h = L.h;
    const uint32_t fmt = (uint32_t)d.Format;
    if (src_row_pitch == 0) src_row_pitch = L.pitch; // DX12 copy: 256-aligned rows
    if (src_row_pitch < L.tight_row) src_row_pitch = L.tight_row;
    std::vector<uint8_t> tight = TightFromUpload(src, src_row_pitch, L);
    uint32_t data_hash;
    {
        PROF(PROF_CB_Crc);
        data_hash = PortApi::Crc32c(0, tight.data(), tight.size());
    }
    uint32_t crc = TexIdentityHash(
        data_hash, w, h, d.MipLevels ? d.MipLevels : 1, d.DepthOrArraySize ? d.DepthOrArraySize : 1, fmt);
    const uint32_t prev = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(dest));
    g_tex_stats.hash_all.fetch_add(1, std::memory_order_relaxed);
    g_tex_stats.hash_bytes.fetch_add(tight.size(), std::memory_order_relaxed);
    if (prev) {
        g_tex_stats.hash_rehash.fetch_add(1, std::memory_order_relaxed);
        if (prev == crc) g_tex_stats.hash_same.fetch_add(1, std::memory_order_relaxed);
    }
    bool restamped = false;
    const bool inherited = g_tex_identity.SetHash(reinterpret_cast<uint64_t>(dest), crc);
    if (prev && prev != crc) {
        const uint32_t r_prev = g_tex_select.RuleHashFor(prev);
        const uint32_t r_now = g_tex_select.RuleHashFor(crc);
        if (r_prev && r_now && r_prev != r_now) {
            logger::write(
                "TEXID-REFUTED res=%p %ux%u m%u (%s): believed %08x -> rule %08x, its mip0 hashes to %08x -> rule %08x",
                (void*)dest, w, h, d.MipLevels, inherited ? "inherited" : "hashed", prev, r_prev, crc, r_now);
            if (prev != r_prev) g_tex_lod.Refute(prev, r_prev, "the resource's own mip0 named a different target");
            g_stamps.Restamp(reinterpret_cast<uint64_t>(dest)); // its recorded slot now swaps as what it is
            restamped = true;
        } else if (r_prev && !r_now && inherited) {
            g_tex_identity.KeepHash(reinterpret_cast<uint64_t>(dest), prev);
            if (g_debug && DiagOnce("TEXID-UNCONFIRMED", (int)r_prev))
                logger::write(
                    "TEXID-UNCONFIRMED res=%p %ux%u m%u: believed %08x -> rule %08x, its mip0 hashes to %08x (no target) -- belief kept",
                    (void*)dest, w, h, d.MipLevels, prev, r_prev, crc);
        }
    }
    DumpIfDegenerate(crc, tight, L, fmt, 0, 0);
    const TexRules& tr = g_tex_rules.Now();
    bool tex_is_target = tr.hash_to_rule.count(crc) != 0;
    g_tex_look_worker.Queue(dest, crc, w, h, fmt, tex_is_target, tight);
    if (tex_is_target || tr.targets.count(crc) || g_tex_lod.Known(crc) || g_tex_lod.Lookup(crc))
        g_tex_identity.NoteModTex(reinterpret_cast<uint64_t>(dest));
    if (!restamped && prev != crc && g_tex_select.RuleHashFor(crc)) {
        const bool ok = g_stamps.Restamp(reinterpret_cast<uint64_t>(dest));
        if (g_debug && DiagOnce("TEXRESTAMP", (int)g_tex_select.RuleHashFor(crc)))
            logger::write("TEXRESTAMP res=%p rule=%08x: %s", (void*)dest, g_tex_select.RuleHashFor(crc),
                ok ? "its slot now holds the mod SRV"
                   : "not swapped (no slot recorded, the slot reused, or no mod SRV yet -- then TEXRETRY)");
    }
    if (tex_is_target) { // this texture is one the mod replaces -> remember
        uint64_t prof = ((uint64_t)w << 40) | ((uint64_t)h << 16) | (fmt & 0xffffu); //   its size/format
        g_tex_identity.NoteMatchProfile(prof); //   to target readback
    }
    const std::vector<std::pair<uint32_t, uint32_t>> held =
        g_tex_mip_identity.TakeHeld(reinterpret_cast<uint64_t>(dest));
    g_tex_stats.ts_held_resolved.fetch_add((uint32_t)held.size(), std::memory_order_relaxed);
    // Stream-copy edges parked on this resource before its identity was known (TexLineage::Alias): resolve them now.
    const std::vector<std::pair<uint32_t, uint32_t>> fed = g_tex_mip_identity.TakeFed(reinterpret_cast<uint64_t>(dest));
    for (const auto& e : fed) g_tex_lod.Feeds(e.first, crc, tex_is_target || g_tex_crawl.On() ? crc : 0, e.second);
    if (!held.empty()) {
        const uint32_t rule = g_tex_select.ProvenRuleFor(crc);
        const uint32_t high = rule ? rule : g_tex_crawl.On() ? crc : 0;
        if (high)
            for (const auto& m : held)
                if (m.second != high && m.second != crc) g_tex_mip_identity.Record(dest, m.first, m.second, high);
    }
    if (tex_is_target) { // TEXSTREAM: this resource is a rule target as uploaded -- its mips past the floor
        g_tex_stats.ts_target_mip0.fetch_add(1, std::memory_order_relaxed); // should each leave a record
        if (g_debug) {
            uint32_t past_floor = 0;
            for (uint32_t k = 1; k < d.MipLevels; ++k) {
                TexAssetLayout Lk;
                if (TexAssetLayoutOf(d, k, Lk) && Lk.floor) ++past_floor;
            }
            logger::write("TEXSTREAM-TARGET rule=%08x %ux%u m%u fmt=%u mipsPastFloor=%u heldBeforeMip0=%zu", crc, w, h,
                d.MipLevels, fmt, past_floor, held.size());
        }
    }
}

bool TexUploadIdentity::ReadSlow(const uint8_t* p, uint64_t span)
{
    static const double us_per_tick = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1e6 / (double)f.QuadPart;
    }();
    const size_t n0 = (size_t)std::min<uint64_t>(span, kUpProbeBytes);
    size_t n = n0;
    const uint8_t* s = p;
    __m128i acc = _mm_setzero_si128();
    LARGE_INTEGER a;
    LARGE_INTEGER b;
    {
        PROF(PROF_CB_Copy);
        QueryPerformanceCounter(&a);
        for (; n && ((uintptr_t)s & 15); --n, ++s) acc = _mm_xor_si128(acc, _mm_cvtsi32_si128(*s));
        for (; n >= 16; n -= 16, s += 16) acc = _mm_xor_si128(acc, _mm_stream_load_si128((__m128i*)s));
        QueryPerformanceCounter(&b);
    }
    probe_sink_.fetch_xor((uint32_t)_mm_cvtsi128_si32(acc), std::memory_order_relaxed);
    const size_t read = n0 - n;
    const double us = (double)(b.QuadPart - a.QuadPart) * us_per_tick;
    const double mbps = us > 0 ? (double)read / us : 1e9;
    const bool slow = mbps < kUpSlowMBps;
#ifdef WWMI_DEBUG
    {
        static std::atomic<int> logged{ 0 };
        if (g_debug && (slow || logged.load(std::memory_order_relaxed) < 6) && logged.fetch_add(1) < 24)
            logger::write("TEXUP-PROBE %zu B of the game's upload memory in %.1f us: %.0f MB/s%s", read, us, mbps,
                slow ? " -- slow" : "");
    }
#endif
    if (!slow) {
        slow_run_.store(0, std::memory_order_relaxed);
        return false;
    }
    if (slow_run_.fetch_add(1, std::memory_order_relaxed) + 1 >= 3 && !gpu_.exchange(true))
        logger::write(
            "TEXUP: the processor reads the game's upload memory at %.1f MB/s (Resizable BAR puts it in video memory) -- "
            "texture uploads are hashed from a GPU copy from now on",
            mbps);
    return true;
}

bool TexUploadIdentity::HashOnGpu(reshade::api::command_list* cl, ID3D12Resource* src, uint64_t off, uint64_t span,
    ID3D12Resource* dst, uint32_t sub, const uint8_t* compare_from, const TexAssetLayout& L)
{
    if (sub && (!g_tex_rules.Ready() || !g_settings.texlod_cache)) return true;
    auto* dev = reinterpret_cast<ID3D12Device*>(cl->get_device()->get_native());
    auto* d3dcl = reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native());
    if (!dev || !d3dcl) return false;
    uint32_t cpu_crc = 0;
    if (compare_from) {
        const std::vector<uint8_t> tight = TightFromUpload(compare_from, L.pitch, L);
        cpu_crc = PortApi::Crc32c(0, tight.data(), tight.size());
    }
    if (!g_tex_readback.ScheduleUpload(dev, cl, d3dcl, src, off, span, dst, sub, compare_from ? &cpu_crc : nullptr))
        return false;
    RemovalPin::Ours(d3dcl, "tex-upload-readback");
    g_tex_stats.ts_up_gpu.fetch_add(1, std::memory_order_relaxed);
    return true;
}
