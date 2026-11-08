// Resource tracking: the shadow of the game's own bookkeeping, and the resource hooks that write it.
// See resource_tracker.h.
#include "../mods/ib_hash.h"
#include "../addon_common.h"
#include "resource_tracker.h"
#include "command_list_registry.h"
#include "stamp_ledger.h"
#include "../textures/tex_identity.h"
#include "../textures/tex_upload_identity.h"
#include "../textures/tex_hooks.h"
#include "../instrumentation/removal_pin.h" // DEAD-REF: a game buffer we named, destroyed before our list executed
#include "../instrumentation/hot_path.h"
#include "../instrumentation/morph_dump.h"
#include "../instrumentation/profiler.h"
#include "../mods/character_recognition.h" // ForgetCharMeshVb0 / IbAnchorForget -- what is learned inside a buffer
#include "../mods/component_table.h"
#include "../mods/game_cb_reader.h"
#include "../mods/mod_set.h"
#include "../rt/rt_rings.h" // RtSkinRings::ForgetRange
#include "../textures/mod_texture_store.h" // TexTouchRes
#include "../core/dxgi_bc.h"
#include "../core/frame_clock.h"
#include "../migoto/dx12_bridge.h"
#include "../migoto/port_api.h"
#include <unordered_set>
#include <cstring>
#include "hooks_toggles.h"
#include "../present/present_log.h"
#include "../addon/addon.h"
#include "../present/frame_sync.h"
#include "../shapekeys/shapekey_chain.h"
#include "../textures/tex_toggles.h"
#include "../mods/buffer_relocation.h"

HooksToggles g_hooks_toggles;
HooksStats g_hooks_stats;

ResourceTracker g_resources;

void ResourceTracker::Add(uint64_t va, const ResInfo& info, uint64_t res)
{
    std::lock_guard lock(mutex_);
    at_va_[va] = info;
    va_of_[res] = va;
}

void ResourceTracker::ForgetAt(uint64_t va, uint64_t res)
{
    std::lock_guard lock(mutex_);
    at_va_.erase(va);
    va_of_.erase(res);
}

bool ResourceTracker::Forget(uint64_t res, uint64_t* va, uint32_t* size)
{
    std::lock_guard lock(mutex_);
    auto it = va_of_.find(res);
    if (it == va_of_.end()) return false;
    if (va) *va = it->second;
    auto vi = at_va_.find(it->second);
    if (size) *size = vi != at_va_.end() ? vi->second.size : 0;
    at_va_.erase(it->second);
    va_of_.erase(it);
    return true;
}

bool ResourceTracker::Live(uint64_t res) const
{
    std::lock_guard lock(mutex_);
    return va_of_.find(res) != va_of_.end();
}

bool ResourceTracker::VaOf(uint64_t res, uint64_t* va) const
{
    std::lock_guard lock(mutex_);
    auto vi = va_of_.find(res);
    if (vi == va_of_.end()) return false;
    *va = vi->second;
    return true;
}

bool ResourceTracker::InfoOf(uint64_t res, ResInfo* out) const
{
    std::lock_guard lock(mutex_);
    auto vi = va_of_.find(res);
    if (vi == va_of_.end()) return false;
    auto ri = at_va_.find(vi->second);
    if (ri == at_va_.end()) return false;
    *out = ri->second;
    return true;
}

bool ResourceTracker::AtVa(uint64_t va, ResInfo* out) const
{
    std::lock_guard lock(mutex_);
    auto si = at_va_.find(va);
    if (si == at_va_.end()) return false;
    *out = si->second;
    return true;
}

uint32_t ResourceTracker::SizeAtVa(uint64_t va) const
{
    std::lock_guard lock(mutex_);
    auto it = at_va_.find(va);
    return it != at_va_.end() ? it->second.size : 0;
}

size_t ResourceTracker::BufferCount() const
{
    std::lock_guard lock(mutex_);
    return va_of_.size();
}

uint32_t ResourceTracker::StateOf(void* res) const
{
    if (g_mod_textures.IndexOf(reinterpret_cast<ID3D12Resource*>(res)) >= 0)
        return (uint32_t)D3D12_RESOURCE_STATE_COMMON;
    uint32_t state = 0;
    if (state_.find(reinterpret_cast<uint64_t>(res), state)) return state;
    return MigotoPort::kResStateUnknown;
}

void ResourceTracker::NoteState(uint64_t res, uint32_t state)
{
    state_.put(res, state);
}

void ResourceTracker::ForgetState(uint64_t res)
{
    state_.erase(res);
}

static bool IsBackBufferSized(command_list* cmd_list, resource res)
{
    const resource_desc d = cmd_list->get_device()->get_resource_desc(res);
    return d.type == resource_type::texture_2d &&
        d.texture.width == g_frames.bb_width.load(std::memory_order_relaxed) &&
        d.texture.height == g_frames.bb_height.load(std::memory_order_relaxed);
}

void on_barrier(command_list* cmd_list, uint32_t count, const resource* res,
    [[maybe_unused]] const resource_usage* old_s, const resource_usage* new_s)
{
    PROF(PROF_Barrier);
    HotPath::GpuNoteBarriers(count);
    CmdBlock* const blk = g_command_lists.Block(cmd_list);
    for (uint32_t i = 0; i < count; ++i)
        if (res[i].handle) {
            if (blk && new_s[i] == resource_usage::general &&
                g_frames.frame_end_armed.load(std::memory_order_relaxed) && IsBackBufferSized(cmd_list, res[i]))
                blk->frame_end_gen = blk->gen;
#ifdef WWMI_DEBUG
            if (blk && (new_s[i] == resource_usage::general || old_s[i] == resource_usage::general) &&
                g_present_log.left.load(std::memory_order_relaxed) > 0 && IsBackBufferSized(cmd_list, res[i])) {
                if (blk->bb_notes_gen != blk->gen) {
                    blk->bb_notes.clear();
                    blk->bb_notes_gen = blk->gen;
                }
                blk->bb_notes.push_back({ res[i].handle, (uint32_t)old_s[i], (uint32_t)new_s[i] });
            }
#endif
            g_resources.NoteState(res[i].handle, (uint32_t)new_s[i]);
            if (blk && ((uint32_t)new_s[i] & (uint32_t)resource_usage::unordered_access)) {
                auto& v = blk->uav_recent;
                size_t old = 0;
                while (old < v.size() && v[old].frame + 1 < g_frame_id) ++old;
                if (old) v.erase(v.begin(), v.begin() + old);
                v.push_back(UavNote{ res[i].handle, g_frame_id });
            }
        }
}

bool on_create_resource(device*, resource_desc& desc, subresource_data*, resource_usage)
{
    if (g_addon.is_dx12 && !g_addon.disabled && desc.type == resource_type::buffer &&
        (desc.usage & resource_usage::unordered_access) != resource_usage::unordered_access) {
        const uint64_t sz = desc.buffer.size;
        if (sz && sz <= 0xFFFFFFFFull) {
            const unsigned h = PortApi::GameComputeUavBufferHash((unsigned)sz);
            if (h && PortApi::ShapeKeyCallbackDeclaresHash(h)) {
                desc.usage |= resource_usage::unordered_access;
                if (ProbeArmed() && DiagOnce("UAVFLAG", (int)h))
                    logger::write("UAVFLAG: buffer %llu B (desc %08x) created WITHOUT "
                                  "ALLOW_UNORDERED_ACCESS -- added it, so WWMI's atomic_iadd into u0 has "
                                  "a valid view. Size and placement unchanged.",
                        (unsigned long long)sz, h);
                return true; // desc modified
            }
        }
    }

    return false;
}

void on_init_resource(
    device*, const resource_desc& desc, const subresource_data* init_data, resource_usage, resource res)
{
    if (g_addon.disabled) return;
    g_hooks_stats.init_res_all.fetch_add(1, std::memory_order_relaxed);
    if (desc.type == resource_type::buffer) {
        ID3D12Resource* d3d12_res = reinterpret_cast<ID3D12Resource*>(res.handle);
        if (d3d12_res) {
            uint64_t va = d3d12_res->GetGPUVirtualAddress();
            if (va != 0) {
                g_hooks_stats.init_res_buf.fetch_add(1, std::memory_order_relaxed);
                g_resources.Add(va, { (uint32_t)desc.buffer.size, d3d12_res, va }, res.handle);
            }
        }
    } else if (desc.type == resource_type::texture_2d) {
        ModSetScope set_scope(nullptr);
        g_tex_identity.Insert(res.handle,
            { desc.texture.width, desc.texture.height, (uint32_t)desc.texture.format,
                desc.texture.levels ? desc.texture.levels : 1u,
                desc.texture.depth_or_layers ? desc.texture.depth_or_layers : 1u, // mips/arr for RfxTexClass
                (uint32_t)(desc.usage &
                    (resource_usage::render_target | resource_usage::depth_stencil |
                        resource_usage::unordered_access)) == 0 &&
                    (uint64_t)desc.texture.width * desc.texture.height >= kTexHashFloorPx });
        if (g_debug) {
            uint32_t tf = (uint32_t)desc.texture.format;
            uint32_t tw = desc.texture.width;
            uint32_t th = desc.texture.height;
            if (DxgiIsBc(tf) && (uint64_t)tw * th >= 256ull * 256ull) {
                static ProbeBudget sn;
                if (sn.take(601))
                    logger::trace("STREAMNEW res=%llx %ux%u mips=%u fmt=%u initData=%d", (unsigned long long)res.handle,
                        tw, th, desc.texture.levels, tf, (int)(init_data != nullptr));
            }
        }
        if (init_data && init_data[0].data && res.handle)
            g_tex_upload.HashFromUpload(reinterpret_cast<ID3D12Resource*>(res.handle),
                (const uint8_t*)init_data[0].data, init_data[0].row_pitch);
    }
}

bool on_copy_buffer_region(command_list*, resource, uint64_t, resource dest, uint64_t dest_offset, uint64_t size)
{
    PROF(PROF_CopyBufRegion);
    if (!dest.handle || !size) return false;
    if (const uint64_t va = reinterpret_cast<ID3D12Resource*>(dest.handle)->GetGPUVirtualAddress())
        IbHash::ForgetRange(
            va + dest_offset, size); // a mesh streamed over this region replaces the hashes taken inside it
    return false;
}

void on_destroy_resource(device*, resource res)
{
    RemovalPin::OnDestroyResource(reinterpret_cast<const void*>(res.handle));
    uint64_t gone_va = 0;
    uint32_t gone_bytes = 0;
    // Recorded BEFORE it is forgotten: this is the only moment the handle is known to have been ours.
    if (g_resources.Forget(res.handle, &gone_va, &gone_bytes)) MorphDump::NoteResourceErased(res.handle);
    g_recognition.ForgetCharMeshVb0(gone_va, gone_bytes); // a learned character VB0 dies with its buffer
    IbHash::ForgetRange(gone_va, gone_bytes); // and so do the mesh hashes taken inside it
    g_recognition.IbAnchorForget(gone_va, gone_bytes); // and the index-buffer anchor learned inside it
    g_rt_rings.ForgetRange(gone_va, gone_bytes); // and the RT skin rings inside it
    if (gone_va && gone_bytes) g_recognition.ForgetSkinOutRange(gone_va, gone_bytes); // and the skin-cache outputs
    TexOnDestroyed(res.handle);
    if (g_hooks_toggles.reloc_forget_on_destroy) g_buffer_reloc.ForgetResource((uint64_t)res.handle);
    g_stamps.ForgetResource(res.handle); // re-stamp must not target a reused ptr's old slot
    g_resources.ForgetState(res.handle);
    SkOnDestroyed(reinterpret_cast<ID3D12Resource*>(res.handle));
    g_game_cbs.Forget(reinterpret_cast<ID3D12Resource*>(res.handle));
}
