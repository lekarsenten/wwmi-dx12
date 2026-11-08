// The texture module's ReShade hooks. See tex_hooks.h.
#include "tex_hooks.h"
#include "tex_identity.h"
#include "tex_rules.h"
#include "tex_select.h"
#include "tex_place.h"
#include "tex_toggles.h"
#include "tex_upload_identity.h"
#include "tex_mip_identity.h"
#include "tex_readback.h"
#include "tex_slot_log.h"
#include "tex_lineage.h"
#include "tex_crawl.h"
#include "../mods/ib_hash.h" // ForgetRange: a whole-buffer copy replaces the mesh hashes inside it
#include "../mods/mod_set.h"
#include "../instrumentation/removal_pin.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/addon.h"
#include "../core/dxgi_bc.h"
#include "../core/log.h"
#include <atomic>

using namespace reshade::api;

void TexOnDestroyed(uint64_t res)
{
    g_tex_identity.Forget(res);
    g_tex_mip_identity.Forget(res);
    g_tex_place.ForgetMissed(res);
    g_tex_readback.Forget(reinterpret_cast<ID3D12Resource*>(res));
    if (g_debug) g_tex_slot_log.Forget(res);
    g_tex_identity.ForgetModSrv(res);
}

bool on_update_texture_region(
    device*, const subresource_data& data, resource dest, uint32_t dest_subresource, const subresource_box*)
{
    if (g_addon.disabled) return false;
    ModSetScope set_scope(nullptr);
    if (dest_subresource == 0 && dest.handle != 0) // mip0 only
        g_tex_upload.HashFromUpload(
            reinterpret_cast<ID3D12Resource*>(dest.handle), (const uint8_t*)data.data, data.row_pitch);
    return false;
}

static void DiagTexPath(const char* path, ID3D12Resource* dest, ID3D12Resource* src, uint32_t dest_sub)
{
    if (!g_debug || !dest || dest_sub != 0) return;
    auto d = dest->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    if (!DxgiIsBc((uint32_t)d.Format)) return; // BC char-textures only
    if ((uint64_t)d.Width * d.Height < kTexHashFloorPx) return;
    if (g_tex_identity.HashKnown(reinterpret_cast<uint64_t>(dest))) return;
    static std::atomic<int> n{ 0 };
    if (n.fetch_add(1) > 60) return;
    int srcHeap = -1;
    if (src) {
        D3D12_HEAP_PROPERTIES hp = {};
        D3D12_HEAP_FLAGS hf = {};
        src->GetHeapProperties(&hp, &hf);
        srcHeap = (int)hp.Type;
    }
    logger::write("TEXPATH %s dest=%p w=%u h=%u fmt=%u srcHeap=%d (1=DEFAULT 2=UPLOAD 3=READBACK)", path, (void*)dest,
        (uint32_t)d.Width, d.Height, (uint32_t)d.Format, srcHeap);
}

bool on_copy_texture_region(command_list* cl, resource source, uint32_t src_sub, const subresource_box* src_box,
    resource dest, uint32_t dest_subresource, const subresource_box* dst_box, reshade::api::filter_mode)
{
    PROF(PROF_CopyTexRegion);
    if (g_addon.disabled) return false;
    if (g_tex_toggles.disable || !g_tex_rules.Ready()) return false;
    ModSetScope set_scope(cl);
    if (!source.handle || !dest.handle || !cl) return false;
    auto* dst = reinterpret_cast<ID3D12Resource*>(dest.handle);
    auto* src = reinterpret_cast<ID3D12Resource*>(source.handle);
    D3D12_RESOURCE_DESC dd = dst->GetDesc();
    D3D12_RESOURCE_DESC sd = src->GetDesc();
    uint32_t fmt = (uint32_t)dd.Format;
    uint32_t w = (uint32_t)dd.Width;
    uint32_t h = dd.Height;
    {
        TexAssetLayout L;
        if (!TexAssetLayoutOf(dd, 0, L) || !L.floor) return false;
    } // the same assets the hash covers
    g_tex_stats.ts_tex2tex.fetch_add(1, std::memory_order_relaxed);
    if (sd.Format == dd.Format && sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D && !src_box && !dst_box) {
        uint32_t sMipW = (uint32_t)(sd.Width >> src_sub);
        uint32_t sMipH = (uint32_t)(sd.Height >> src_sub);
        uint32_t dMipW = (uint32_t)(dd.Width >> dest_subresource);
        uint32_t dMipH = (uint32_t)(dd.Height >> dest_subresource);
        bool same_mip = sMipW && sMipH && sMipW == dMipW && sMipH == dMipH;
        bool diff_res = (sd.Width != dd.Width || sd.Height != dd.Height);
        if (same_mip && diff_res) {
            bool dst_smaller = (dd.Width < sd.Width); // stream-out: dst is the lower-LOD variant
            const D3D12_RESOURCE_DESC& big = dst_smaller ? sd : dd;
            const D3D12_RESOURCE_DESC& sm = dst_smaller ? dd : sd;
            uint32_t steps = 0;
            while ((big.Width >> steps) > sm.Width) ++steps;
            if ((big.Width >> steps) != sm.Width || (big.Height >> steps) != sm.Height) steps = 0;
            g_tex_stats.ts_stream.fetch_add(1, std::memory_order_relaxed);
            if (!g_tex_identity.HashOf(dst_smaller ? source.handle : dest.handle))
                g_tex_stats.ts_stream_nobig.fetch_add(1, std::memory_order_relaxed);
            g_tex_lineage.Propagate(source.handle, dest.handle, dst_smaller); // tag the fresh variant (the chain)
            if (dst_smaller)
                g_tex_lineage.Alias(dest.handle, source.handle, steps);
            else
                g_tex_lineage.Alias(source.handle, dest.handle, steps);
        }
    }

    // -----------------------------------------------------

    bool already = g_tex_identity.HashKnown(dest.handle);
    if (already && g_tex_identity.Inherited(dest.handle)) already = false; // a belief gets measured
    bool profMatch;
    {
        uint64_t prof = ((uint64_t)w << 40) | ((uint64_t)h << 16) | (fmt & 0xffffu);
        profMatch = g_tex_identity.MatchProfile(prof);
    }
    bool dimsEq = (sd.Width == dd.Width && sd.Height == dd.Height && sd.Format == dd.Format);
    if (g_debug && (fmt >= 94 && fmt <= 99) && sd.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
        (uint64_t)sd.Width * sd.Height >= 256ull * 256ull) {
        static ProbeBudget n;
        if (n.take(601)) {
            const uint32_t sh = g_tex_identity.HashOf(source.handle);
            const uint32_t dh = g_tex_identity.HashOf(dest.handle);
            logger::trace(
                "STREAMCOPY src=%llx[%ux%u m%u s%u h=%08x] -> dst=%llx[%ux%u m%u s%u h=%08x] dimsEq=%d box=%d",
                (unsigned long long)source.handle, (uint32_t)sd.Width, (uint32_t)sd.Height, sd.MipLevels, src_sub, sh,
                (unsigned long long)dest.handle, w, h, dd.MipLevels, dest_subresource, dh, (int)dimsEq,
                (int)(src_box != nullptr || dst_box != nullptr));
        }
    }
    if (src_sub != 0 || dest_subresource != 0 || src_box || dst_box) return false; // full mip0->mip0 only
    if (!dimsEq) return false; // identical mip0
    g_tex_lineage.Propagate(source.handle, dest.handle, true);
    if (already || !profMatch) return false;

    auto* dev = reinterpret_cast<ID3D12Device*>(cl->get_device()->get_native());
    auto* d3dcl = reinterpret_cast<ID3D12GraphicsCommandList*>(cl->get_native());
    if (!dev || !d3dcl) return false;
    if (!g_tex_readback.Schedule(dev, cl, d3dcl, src, dst, sd)) return false;
    RemovalPin::Ours(d3dcl, "tex-readback");
    if (g_debug) {
        static ProbeBudget n;
        if (n.take(61)) logger::write("TEXRB schedule dst=%p w=%u h=%u fmt=%u", (void*)dst, w, h, fmt);
    }
    return false;
}

bool on_copy_resource(command_list* cl, resource source, resource dest)
{
    if (g_addon.disabled) return false;
    if (dest.handle) {
        auto* d = reinterpret_cast<ID3D12Resource*>(dest.handle);
        if (const uint64_t va = d->GetGPUVirtualAddress()) {
            IbHash::ForgetRange(va, d->GetDesc().Width);
            return false;
        }
    }
    if (g_tex_rules.Ready() && source.handle && dest.handle) { // lineage for whole-resource copies too
        ModSetScope set_scope(cl);
        auto* d = reinterpret_cast<ID3D12Resource*>(dest.handle);
        auto* s = reinterpret_cast<ID3D12Resource*>(source.handle);
        D3D12_RESOURCE_DESC dd = d->GetDesc();
        TexAssetLayout L; // the same assets the hash covers (on_copy_texture_region's test), not BC only
        if (TexAssetLayoutOf(dd, 0, L) && L.floor && s->GetDesc().Format == dd.Format)
            g_tex_lineage.Propagate(source.handle, dest.handle, true);
    }
    DiagTexPath("copy_resource", reinterpret_cast<ID3D12Resource*>(dest.handle),
        reinterpret_cast<ID3D12Resource*>(source.handle), 0);
    return false;
}

bool on_copy_buffer_to_texture(command_list* cl, resource source, uint64_t source_offset, uint32_t /*row_length*/,
    uint32_t /*slice_height*/, resource dest, uint32_t dest_subresource, const subresource_box* dst_box)
{
    PROF(PROF_CopyBufToTex);
    if (g_addon.disabled) return false;
    if (!dest.handle || !cl) return false;
    ModSetScope set_scope(cl);

    auto* dst = reinterpret_cast<ID3D12Resource*>(dest.handle);
    D3D12_RESOURCE_DESC dd = dst->GetDesc();
    TexAssetLayout L;
    if (!TexAssetLayoutOf(dd, dest_subresource, L) || !L.floor) return false;
    // a partial upload (an atlas region) is not the texture, and its source holds only the region
    if (dst_box && (dst_box->left || dst_box->top || dst_box->right != L.w || dst_box->bottom != L.h)) return false;

    if (source.handle == 0) return false;
    if (dest_subresource != 0 && !g_tex_toggles.mip_exact) return false;
    auto* srcRes = reinterpret_cast<ID3D12Resource*>(source.handle);
    D3D12_HEAP_PROPERTIES hp = {};
    D3D12_HEAP_FLAGS hf = {};
    srcRes->GetHeapProperties(&hp, &hf);
    if (hp.Type != D3D12_HEAP_TYPE_UPLOAD) {
        DiagTexPath("buf2tex(non-upload)", dst, srcRes, dest_subresource);
        return false;
    }
    // every row the hasher reads must lie inside the upload buffer (the placed footprint's pitch is assumed)
    if (source_offset + (uint64_t)L.pitch * (L.rows - 1) + L.tight_row > srcRes->GetDesc().Width) return false;
    if (dest_subresource != 0) {
        // Only a target's mips are worth a map: a known mip0 that is no rule hash and no alias is nothing.
        const uint32_t h0 = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(dst));
        if (h0 && !g_tex_select.RuleHashFor(h0) && !g_tex_crawl.On()) return false;
    }
    const uint64_t span = (uint64_t)L.pitch * (L.rows - 1) + L.tight_row;
    std::atomic<uint32_t>& arrivals = dest_subresource == 0 ? g_tex_stats.ts_buf_mip0 : g_tex_stats.ts_buf_mipk;
    if (!g_tex_toggles.upload_gpu && g_tex_upload.GpuPreferred() &&
        g_tex_upload.HashOnGpu(cl, srcRes, source_offset, span, dst, dest_subresource, nullptr, L)) {
        arrivals.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    void* ptr = nullptr;
    HRESULT map_hr;
    {
        PROF(PROF_CB_Map);
        map_hr = srcRes->Map(0, nullptr, &ptr);
    }
    if (SUCCEEDED(map_hr) && ptr) {
        arrivals.fetch_add(1, std::memory_order_relaxed);
        const uint8_t* up = (const uint8_t*)ptr + source_offset;
        const bool gpu = g_tex_toggles.upload_gpu || g_tex_upload.ReadSlow(up, span);
        if (!gpu ||
            !g_tex_upload.HashOnGpu(
                cl, srcRes, source_offset, span, dst, dest_subresource, g_tex_toggles.upload_gpu ? up : nullptr, L)) {
            if (dest_subresource == 0)
                g_tex_upload.HashFromUpload(dst, up, 0 /*256-aligned*/);
            else
                g_tex_mip_identity.FromUpload(dst, dest_subresource, up);
        }
        PROF(PROF_CB_Map);
        srcRes->Unmap(0, nullptr);
    }
    return false;
}
