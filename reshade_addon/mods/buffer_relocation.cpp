// allow_buffer_resize: widening a game buffer the mod's ini declares too small. See buffer_relocation.h.
#include "../addon_common.h"
#include "buffer_relocation.h"
#include "../instrumentation/removal_pin.h"
#include "../instrumentation/sk_ring.h"
#include "../instrumentation/morph_dump.h"
#include "../instrumentation/profiler.h"
#include "../migoto/dx12_pipeline.h"
#include "../core/gpu_progress.h"
#include "../core/d3d12_name.h"
#include "../hooks/resource_tracker.h"

BufferRelocator g_buffer_reloc;

bool BufferRelocator::Resolve(ID3D12Resource* orig, RelocBuf* out)
{
    if (!orig) return false;
    std::lock_guard<std::mutex> l(mtx_);
    auto it = reloc_.find(reinterpret_cast<uint64_t>(orig));
    if (it == reloc_.end() || !it->second.res || !it->second.va || !it->second.bytes) return false;
    if (out) *out = it->second;
    return true;
}

void BufferRelocator::RememberOriginal(ID3D12Resource* relocated, unsigned original_bytes)
{
    if (!relocated || !original_bytes) return;
    std::lock_guard<std::mutex> l(mtx_);
    orig_bytes_[reinterpret_cast<uint64_t>(relocated)] = original_bytes;
}

void BufferRelocator::ForgetResource(uint64_t handle)
{
    std::lock_guard<std::mutex> l(mtx_);
    orig_bytes_.erase(handle);
    auto it = reloc_.find(handle);
    if (it == reloc_.end()) return;
    RetireLocked(it->second);
    reloc_.erase(it);
}

void BufferRelocator::RetireLocked(const RelocBuf& b)
{
    if (b.res) grave_.emplace_back(b, GpuStamp());
}

void BufferRelocator::Sweep(reshade::api::device* dev)
{
    std::vector<RelocBuf> gone;
    {
        std::lock_guard<std::mutex> l(mtx_);
        for (size_t k = 0; k < grave_.size();) {
            if (GpuPassed(grave_[k].second)) {
                gone.push_back(grave_[k].first);
                grave_[k] = grave_.back();
                grave_.pop_back();
            } else
                ++k;
        }
    }
    for (const RelocBuf& b : gone) {
        g_resources.ForgetAt(b.va, reinterpret_cast<uint64_t>(b.res));
        {
            std::lock_guard<std::mutex> l(mtx_);
            orig_bytes_.erase(reinterpret_cast<uint64_t>(b.res));
        }
        MigotoPort::DisownBuffer(b.res);
        if (dev)
            dev->destroy_resource(resource{ reinterpret_cast<uint64_t>(b.res) });
        else
            b.res->Release();
    }
}

/* Did WE change this buffer's size, and what was it born as? */
bool BufferRelocator::BirthBytes(uint64_t handle, unsigned* out)
{
    std::lock_guard<std::mutex> l(mtx_);
    auto it = orig_bytes_.find(handle);
    if (it == orig_bytes_.end() || !it->second) return false;
    if (out) *out = it->second;
    return true;
}

unsigned BufferRelocator::IdentityBytes(ID3D12Resource* res, unsigned current_bytes)
{
    if (!res) return current_bytes;
    std::lock_guard<std::mutex> l(mtx_);
    auto it = orig_bytes_.find(reinterpret_cast<uint64_t>(res));
    return (it != orig_bytes_.end()) ? it->second : current_bytes;
}

bool BufferRelocator::Ensure(command_list* cmd_list, ID3D12Resource* orig, unsigned orig_bytes, unsigned hash,
    const BufResize& want, ID3D12Resource** out_res, uint64_t* out_va)
{
    if (!cmd_list || !orig || !want.stride || !want.vertex_count) return false;
    // Their sizing rule: a whole number of records, never a partial one (CommandList.cpp:6314).
    const uint64_t need = (want.bytes() / want.stride) * want.stride;
    if (!need || need > (1ull << 31)) return false; // sanity: never allocate 2 GB+

    const uint64_t key = reinterpret_cast<uint64_t>(orig);
    {
        std::lock_guard<std::mutex> l(mtx_);
        auto it = reloc_.find(key);
        if (it != reloc_.end() && it->second.res && it->second.bytes >= need) {
            *out_res = it->second.res;
            *out_va = it->second.va;
            return true;
        }
    }

    reshade::api::device* dev = cmd_list->get_device();
    if (!dev) return false;

    resource big_h{};
    if (!dev->create_resource(
            resource_desc(need, memory_heap::default_,
                resource_usage::unordered_access | resource_usage::copy_dest | resource_usage::copy_source),
            nullptr, resource_usage::general, &big_h) ||
        !big_h.handle) {
        logger::write("BUFRESIZE: create_resource FAILED for %08x (%llu B) -- keeping the game's buffer", hash,
            (unsigned long long)need);
        return false;
    }
    ID3D12Resource* big = reinterpret_cast<ID3D12Resource*>(big_h.handle);
    DevName(big, L"wwmi relocated buffer");
    MigotoPort::OwnBuffer(big);

    // Seed it with the game's data. The tail past that is where the mod's extra vertices live, zeroed by D3D12.
    const uint64_t copy = (orig_bytes < need) ? orig_bytes : need;
    if (copy) {
        const resource src_h{ reinterpret_cast<uint64_t>(orig) };
        cmd_list->barrier(src_h, resource_usage::unordered_access, resource_usage::copy_source);
        MigotoPort::OwnedTransition(
            reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native()), big, D3D12_RESOURCE_STATE_COPY_DEST);
        cmd_list->copy_buffer_region(src_h, 0, big_h, 0, copy);
        ProfGpuWork(PGW_SkCopy, copy);
        cmd_list->barrier(src_h, resource_usage::copy_source, resource_usage::unordered_access);
        RemovalPin::Ours(reinterpret_cast<const void*>(cmd_list->get_native()), "reloc-seed");
        RemovalPin::Refs(reinterpret_cast<const void*>(cmd_list->get_native()), orig, "reloc-seed source");
    }

    const uint64_t va = big->GetGPUVirtualAddress();
    {
        std::lock_guard<std::mutex> l(mtx_);
        RelocBuf& slot = reloc_[key];
        if (slot.res && slot.bytes >= need) { // another thread relocated it meanwhile: theirs wins
            RetireLocked(RelocBuf{ big, va, need }); //   ours is retired, its seeding copy is already recorded
            *out_res = slot.res;
            *out_va = slot.va;
            return true;
        }
        if (slot.res) RetireLocked(slot); // too small: replaced
        slot = RelocBuf{ big, va, need };
    }
    unsigned nth = 0;
    {
        std::lock_guard<std::mutex> l(count_mtx_);
        nth = ++reloc_count_[hash];
    }
    int list_type = -1;
    if (auto* nat = reinterpret_cast<ID3D12CommandList*>(cmd_list->get_native())) list_type = (int)nat->GetType();
    logger::write("BUFRESIZE: RELOCATED %08x  %u B -> %llu B (%u x %u), seeded %llu B from the game's "
                  "buffer %p (list type %d). The vanilla ceiling was %u vertices. Relocation #%u of this hash",
        hash, orig_bytes, (unsigned long long)need, want.vertex_count, want.stride, (unsigned long long)copy,
        (void*)orig, list_type, want.stride ? orig_bytes / want.stride : 0u, nth);
    SkRing::Note(SkRing::Reloc, hash, (uint64_t)orig, need, nth);
    MorphDump::NoteNewInstance(hash);
    if (nth == 8)
        logger::write("BUFRESIZE: RELOC-STORM %08x -- eight relocations of one hash this session: the game recreates "
                      "this buffer, and each recreation costs a %llu B buffer and a seeding copy",
            hash, (unsigned long long)need);
    {
        ResInfo ri{};
        ri.res = big;
        ri.size = (unsigned)need;
        g_resources.Add(va, ri, reinterpret_cast<uint64_t>(big));
    }
    RememberOriginal(big, orig_bytes); // identity stays the size the GAME created it with

    *out_res = big;
    *out_va = va;
    return true;
}

void BufferRelocator::ClearDeclared()
{
    std::lock_guard<std::mutex> l(declared_mtx_);
    declared_.clear();
}

void BufferRelocator::Declare(uint32_t hash, const BufResize& want)
{
    std::lock_guard<std::mutex> l(declared_mtx_);
    declared_[hash] = want;
}

BufResize BufferRelocator::Declared(uint32_t hash)
{
    std::lock_guard<std::mutex> l(declared_mtx_);
    auto it = declared_.find(hash);
    return it == declared_.end() ? BufResize{} : it->second;
}

size_t BufferRelocator::DeclaredCount()
{
    std::lock_guard<std::mutex> l(declared_mtx_);
    return declared_.size();
}
