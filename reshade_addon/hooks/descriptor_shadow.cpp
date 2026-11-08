// The descriptor shadow and the descriptor hooks that write it. See descriptor_shadow.h.
#include "../addon_common.h"
#include "descriptor_shadow.h"
#include "../addon/addon.h"
#include "../core/dxgi_bc.h"
#include "../core/frame_clock.h"
#include "../textures/mod_texture_store.h"
#include "../instrumentation/diag_gate.h"
#include "../instrumentation/diag_arms.h"
#include "../instrumentation/profiler.h"
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "../mods/mod_set.h"
#include "../textures/tex_identity.h"
#include "../textures/tex_select.h"
#include "../textures/tex_place.h"
#include <unordered_set>
#include "hooks_toggles.h"

DescriptorShadow g_descriptors;

void DescriptorShadow::TrackView(device* device, resource_view view)
{
    if (view.handle == 0) return;
    views_.init(reinterpret_cast<ID3D12Device*>(device->get_native())); // once: the page granularity
    resource res = device->get_resource_from_view(view);
    if (res.handle == 0) {
        views_.forget(view.handle);
        return;
    }
    auto res_desc = device->get_resource_desc(res);
    ID3D12Resource* d3d12_res = reinterpret_cast<ID3D12Resource*>(res.handle);
    if (res_desc.type == resource_type::buffer) {
        ResInfo info = { (uint32_t)res_desc.buffer.size, d3d12_res, d3d12_res->GetGPUVirtualAddress(), false, 0,
            view.handle };
        info.is_buffer = true; // decided HERE, where res_desc.type is authoritative
        views_.put(view.handle, info);
    } else if (res_desc.type == resource_type::texture_2d) {
        // Any texture an upload can name (TexInfo::hashable's test), not BC only: a mod may replace an R8 mask.
        const bool cand =
            (uint32_t)(res_desc.usage &
                (resource_usage::render_target | resource_usage::depth_stencil | resource_usage::unordered_access)) ==
                0 &&
            (uint64_t)res_desc.texture.width * res_desc.texture.height >= kTexHashFloorPx; // tex_identity.h
        views_.put(view.handle, { 0, d3d12_res, 0, cand, 0, view.handle });
    } else
        views_.forget(view.handle);
}

void DescriptorShadow::RecordViewInTrackedHeap(resource res, resource_view view)
{
    HeapKey key{};
    if (!SlotOfCpu(view.handle, &key)) return;
    ResInfo vinfo{};
    const bool have = res.handle && views_.find(view.handle, vinfo);
    FlatHeapMap::Slot* const sp = heaps_.slot(key.heap, key.offset, false);
    if (g_debug) {
        static std::atomic<unsigned> s_n{ 0 };
        static std::atomic<unsigned> s_stale{ 0 };
        ResInfo old{};
        if (Find(key, old) && old.res != reinterpret_cast<ID3D12Resource*>(res.handle))
            s_stale.fetch_add(1, std::memory_order_relaxed);
        const unsigned k = s_n.fetch_add(1, std::memory_order_relaxed) + 1;
        if ((k & 0xFFFF) == 0)
            logger::write("VIEW-IN-HEAP-N: %u views created straight into tracked heaps, %u over a record naming "
                          "another resource -- the record now follows the view",
                k, s_stale.load(std::memory_order_relaxed));
    }
    if (!have) {
        FlatHeapMap::erase_at(sp);
        return;
    }
    FlatHeapMap::set_at(sp, view.handle, views_.current_seq(), g_frame_id, 3, nullptr);
}

void DescriptorShadow::ViewCreated(device* device, resource res, resource_view view)
{
    if (res.handle)
        TrackView(device, view);
    else
        views_.forget(view.handle);
    RecordViewInTrackedHeap(res, view);
}

static void TexNoSwapNote(uint64_t heap, uint32_t off, ID3D12Resource* res, const char* how)
{
    TexInfo di{};
    if (!res || !g_tex_identity.DimsOf(reinterpret_cast<uint64_t>(res), &di) || !di.hashable) return;
    const uint32_t h = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(res));
    const uint32_t rule = g_tex_select.RuleHashFor(h);
    if (h && !rule) return;
    const bool shaped = g_tex_identity.MatchProfileWithin(di.width, di.height, di.format, 4);
    if (!shaped) return;
    {
        static std::mutex m;
        static std::unordered_set<uint64_t> seen;
        static unsigned gen = ~0u;
        std::lock_guard l(m);
        const unsigned g = g_diag_arms.texnoswap_gen.load(std::memory_order_relaxed);
        if (gen != g) {
            gen = g;
            seen.clear();
        }
        if (!seen.insert(reinterpret_cast<uint64_t>(res)).second) return;
    }
    if (g_diag_arms.texnoswap_n.fetch_add(1, std::memory_order_relaxed) >= 400) return;
    logger::write("TEXNOSWAP f=%u heap=%llx slot=%u by %s: res=%p %ux%u m%u fmt=%u hash=%08x rule=%08x -- %s",
        g_frame_id, (unsigned long long)heap, off, how, (void*)res, di.width, di.height, di.mips, di.format, h, rule,
        !h ? "no identity yet" : "a target, no mod SRV selected");
}

void DescriptorShadow::DescriptorsWritten(device* device, uint32_t count, const descriptor_table_update* updates)
{
    for (uint32_t u = 0; u < count; ++u) {
        const auto& update = updates[u];
        if (update.type == descriptor_type::constant_buffer && update.descriptors) {
            descriptor_heap cheap = { 0 };
            uint32_t cbase = 0;
            device->get_descriptor_heap_offset(update.table, update.binding, 0, &cheap, &cbase);
            if (cheap.handle) {
                auto* brs = static_cast<const buffer_range*>(update.descriptors);
                for (uint32_t i = 0; i < update.count; ++i) {
                    if (!brs[i].buffer.handle) continue;
                    auto* br = reinterpret_cast<ID3D12Resource*>(brs[i].buffer.handle);
                    // size == UINT64_MAX means "rest of the buffer" -- resolve it so the byte-size probe is real.
                    uint64_t sz = brs[i].size;
                    if (sz == UINT64_MAX) {
                        D3D12_RESOURCE_DESC rd = br->GetDesc();
                        sz = (rd.Width > brs[i].offset) ? (rd.Width - brs[i].offset) : 0;
                    }
                    cbvs_.put(
                        { cheap.handle, cbase + i }, { br, br->GetGPUVirtualAddress() + brs[i].offset, (uint32_t)sz });
                }
            }
            continue;
        }
        if ((update.type != descriptor_type::shader_resource_view &&
                update.type != descriptor_type::unordered_access_view &&
                update.type != descriptor_type::buffer_shader_resource_view &&
                update.type != descriptor_type::buffer_unordered_access_view) ||
            !update.descriptors)
            continue;
        descriptor_heap heap = { 0 };
        uint32_t base_offset = 0;
        device->get_descriptor_heap_offset(update.table, update.binding, 0, &heap, &base_offset);
        if (heap.handle == 0) continue;
        auto* rvs = static_cast<const resource_view*>(update.descriptors);
        auto* dev_u = reinterpret_cast<ID3D12Device*>(device->get_native());
        FlatHeapMap::Entry* const de = heaps_.enter(heap.handle); // once per update, not per descriptor
        for (uint32_t i = 0; i < update.count; ++i) {
            ResInfo vinfo;
            FlatHeapMap::Slot* const dp = FlatHeapMap::at(de, base_offset + i);
            if (!views_.find(rvs[i].handle, vinfo)) {
                FlatHeapMap::erase_at(dp);
            } else {
                FlatHeapMap::set_at(dp, rvs[i].handle, views_.current_seq(), g_frame_id, 1, nullptr);
                if (update.type == descriptor_type::shader_resource_view && vinfo.tex_cand && // hashable textures only
                    !g_tex_place.Swap(
                        dev_u, heap.handle, base_offset + i, vinfo.res) && //   -> skip swap lock otherwise
                    g_diag_arms.texnoswap_arm.load(std::memory_order_relaxed) > 0)
                    TexNoSwapNote(heap.handle, base_offset + i, vinfo.res, "update");
            }
        }
    }
}

static inline D3D12_CPU_DESCRIPTOR_HANDLE HeapCpuStart(uint64_t heap_handle)
{
    if (!heap_handle) return { 0 };
    return reinterpret_cast<ID3D12DescriptorHeap*>(heap_handle)->GetCPUDescriptorHandleForHeapStart();
}

void DescriptorShadow::DescriptorsCopied(device* device, uint32_t count, const descriptor_table_copy* copies)
{
    auto* d3d12_dev = reinterpret_cast<ID3D12Device*>(device->get_native());
    // Also a device constant, not a per-call query.
    static uint32_t s_inc = 0;
    if (!s_inc && d3d12_dev)
        s_inc = d3d12_dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t inc = s_inc ? s_inc : 32u;
    uint64_t prev_s = 0;
    uint64_t prev_d = 0;
    FlatHeapMap::Entry* prev_se = nullptr;
    FlatHeapMap::Entry* prev_de = nullptr;
    uint64_t prev_s_lo = 0;
    for (uint32_t u = 0; u < count; ++u) {
        const auto& copy = copies[u];
        descriptor_heap s_heap = { 0 };
        uint32_t s_off = 0;
        descriptor_heap d_heap = { 0 };
        uint32_t d_off = 0;
        device->get_descriptor_heap_offset(copy.source_table, copy.source_binding, 0, &s_heap, &s_off);
        device->get_descriptor_heap_offset(copy.dest_table, copy.dest_binding, 0, &d_heap, &d_off);
        if (s_heap.handle == 0 || d_heap.handle == 0) continue;
        if (s_heap.handle != prev_s) {
            prev_se = heaps_.entry_of(s_heap.handle); // once per heap per call, not per descriptor
            prev_s_lo = prev_se ? prev_se->lo.load(std::memory_order_relaxed) : HeapCpuStart(s_heap.handle).ptr;
            prev_s = s_heap.handle;
        }
        if (d_heap.handle != prev_d) {
            prev_de = heaps_.enter(d_heap.handle);
            prev_d = d_heap.handle;
        }
        FlatHeapMap::Entry* const se = prev_se;
        FlatHeapMap::Entry* const de = prev_de;
        if (!de) continue;
        const uint64_t s_lo = prev_s_lo;
        if (!s_lo) continue;
        const uint32_t now = views_.current_seq();
        for (uint32_t i = 0; i < copy.count; ++i) {
            FlatHeapMap::Slot* const dp = FlatHeapMap::at(de, d_off + i);
            if (!dp) break;
            uint64_t src = s_lo + (uint64_t)(s_off + i) * inc;
            uint32_t seq = now;
            ID3D12Resource* swapped = nullptr;
            if (const FlatHeapMap::Slot* const sp = FlatHeapMap::at(se, s_off + i)) {
                const uint32_t sq = sp->seq.load(std::memory_order_acquire);
                if (sq) {
                    src = sp->src.load(std::memory_order_relaxed);
                    seq = sq;
                }
                swapped = sp->swapped.load(std::memory_order_relaxed);
            }
            FlatHeapMap::set_at(dp, src, seq, g_frame_id, 2, swapped);
            if (swapped) {
                FlatHeapMap::mark(de, d_off + i);
                g_mod_textures.TouchRes(swapped);
            }
        }
    }
}

bool DescriptorShadow::Find(const HeapKey& key, ResInfo& out)
{
    return heaps_.find(views_, key, out);
}

bool DescriptorShadow::FindIn(FlatHeapMap::Entry* e, uint32_t offset, ResInfo& out)
{
    return heaps_.find_in(views_, e, offset, out);
}

bool DescriptorShadow::FindView(uint64_t view, ResInfo& out)
{
    return views_.find(view, out);
}

FlatHeapMap::Entry* DescriptorShadow::EntryOf(uint64_t heap)
{
    return heaps_.entry_of(heap);
}

bool DescriptorShadow::FindCbv(const HeapKey& key, HeapCbv& out)
{
    return cbvs_.find(key, out);
}

bool DescriptorShadow::SlotOfCpu(uint64_t cpu, HeapKey* out)
{
    static uint32_t s_inc = 0;
    if (!cpu || !g_addon.d3d12_device) return false;
    if (!s_inc) s_inc = g_addon.d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint32_t inc = s_inc;
    bool hit = false;
    heaps_.for_each_heap([&](uint64_t heap, uint64_t lo, uint32_t n, bool, FlatHeapMap::Slot*) {
        if (!lo || cpu < lo || cpu >= lo + (uint64_t)n * inc) return false;
        *out = HeapKey{ heap, (uint32_t)((cpu - lo) / inc) };
        hit = true;
        return true;
    });
    return hit;
}

bool DescriptorShadow::ShaderVisible(uint64_t heap)
{
    return heaps_.shader_visible(heap);
}

void DescriptorShadow::SetSwapped(const HeapKey& key, ID3D12Resource* res)
{
    heaps_.set_swapped(key, res);
}

void DescriptorShadow::SetMiss(const HeapKey& key, uint32_t frame)
{
    heaps_.set_miss(key, frame);
}

void DescriptorShadow::SetRetireHook(void (*hook)(uint64_t heap))
{
    heaps_.retire_hook.store(hook, std::memory_order_release);
}

void DescriptorShadow::Clear()
{
    heaps_.clear_all();
    cbvs_.erase_if([](const HeapCbv&) { return true; });
}

void DescriptorShadow::UnwatchAll()
{
    heaps_.unwatch_all();
}

void on_init_resource_view(
    device* device, resource res, resource_usage, const resource_view_desc& desc, resource_view view)
{
    PROF(PROF_InitView);
    g_hooks_stats.init_view_all.fetch_add(1, std::memory_order_relaxed);
    if (!g_addon.is_dx12 || view.handle == 0) return;
    g_descriptors.ViewCreated(device, res, view);
    if (res.handle == 0) return;

    if (g_debug && desc.type == resource_view_type::texture_2d) {
        uint32_t tw = 0;
        uint32_t th = 0;
        uint32_t tf = 0;
        {
            TexInfo di{};
            if (g_tex_identity.DimsOf(res.handle, &di)) {
                tw = di.width;
                th = di.height;
                tf = di.format;
            }
        }
        if (DxgiIsBc(tf) && (uint64_t)tw * th >= 256ull * 256ull) {
            static ProbeBudget sv;
            if (sv.take(601))
                logger::trace("STREAMSRV res=%llx %ux%u fmt=%u firstMip=%u mipCount=%u", (unsigned long long)res.handle,
                    tw, th, tf, desc.texture.first_level, desc.texture.levels);
        }
    }
}

bool on_update_descriptor_tables(device* device, uint32_t count, const descriptor_table_update* updates)
{
    PROF(PROF_DescUpdate);
    if (g_addon.disabled) return false;
    ModSetScope set_scope(nullptr);
    g_descriptors.DescriptorsWritten(device, count, updates);
    return false;
}

bool on_copy_descriptor_tables(device* device, uint32_t count, const descriptor_table_copy* copies)
{
    PROF(PROF_DescCopy);
    if (g_addon.disabled) return false;
    ModSetScope set_scope(nullptr);
    g_descriptors.DescriptorsCopied(device, count, copies);
    return false;
}
