// The stamp ledger: taking mod textures out of the game's descriptor slots again. See stamp_ledger.h.
#include "../addon_common.h"
#include "stamp_ledger.h"
#include "../addon/addon.h"
#include "../core/log.h"
#include "../textures/mod_texture_store.h"
#include "../instrumentation/diag_gate.h"
#include "../textures/tex_place.h"
#include "../textures/tex_slot_log.h"
#include "../textures/tex_identity.h"
#include "../textures/tex_toggles.h"
#include <cstring>

StampLedger g_stamps;

void StampLedger::NotePlacement(uint64_t res, const HeapKey& slot)
{
    slot_of_.put(res, slot);
}

void StampLedger::ForgetResource(uint64_t res)
{
    slot_of_.erase(res);
}

void StampLedger::Clear()
{
    slot_of_.erase_if([](const HeapKey&) { return true; });
}

uint32_t StampLedger::DescInc()
{
    static const uint32_t inc =
        g_addon.d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return inc;
}

D3D12_CPU_DESCRIPTOR_HANDLE StampLedger::BackupSlot(uint32_t idx)
{
    return { backup_heaps_[idx / kBackupBlock]->GetCPUDescriptorHandleForHeapStart().ptr +
        (SIZE_T)(idx % kBackupBlock) * DescInc() };
}

void StampLedger::BackupsOfRetiredHeap(uint64_t heap)
{
    StampLedger& self = g_stamps;
    std::lock_guard l(self.backup_mutex_);
    for (auto it = self.backup_of_.begin(); it != self.backup_of_.end();)
        if (it->first.heap == heap) {
            self.backup_free_.push_back(it->second.idx);
            it = self.backup_of_.erase(it);
        } else
            ++it;
}

bool StampLedger::RestoreFromBackup(const HeapKey& key, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    std::lock_guard l(backup_mutex_);
    auto it = backup_of_.find(key);
    if (it == backup_of_.end()) return false;
    const Backup bk = it->second;
    backup_of_.erase(it);
    backup_free_.push_back(bk.idx);
    if (memcmp(reinterpret_cast<const void*>(dst.ptr), reinterpret_cast<const void*>(bk.mod_srv), DescInc()) != 0) {
        if (g_debug && DiagOnce("UNSTAMP-NOTOURS", (int)(key.offset ^ (key.heap >> 4))))
            logger::write(
                "UNSTAMP-NOTOURS heap %llx slot %u: recorded as stamped, but it no longer holds the mod SRV's "
                "bytes -- the game wrote it after the stamp; the slot is left alone",
                (unsigned long long)key.heap, key.offset);
        return false;
    }
    g_addon.d3d12_device->CopyDescriptorsSimple(1, dst, BackupSlot(bk.idx), D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    return true;
}

void StampLedger::BackupBeforeStamp(uint64_t heap_h, uint32_t offset, uint64_t mod_srv)
{
    if (!g_addon.d3d12_device || !heap_h || !mod_srv || g_descriptors.ShaderVisible(heap_h)) return;
    const HeapKey key{ heap_h, offset };
    ResInfo ri;
    if (g_descriptors.Find(key, ri) && ri.swapped) return;
    const D3D12_CPU_DESCRIPTOR_HANDLE slot{
        reinterpret_cast<ID3D12DescriptorHeap*>(heap_h)->GetCPUDescriptorHandleForHeapStart().ptr +
        (SIZE_T)offset * DescInc()
    };
    std::lock_guard l(backup_mutex_);
    g_descriptors.SetRetireHook(BackupsOfRetiredHeap);
    auto it = backup_of_.find(key);
    uint32_t idx;
    if (it != backup_of_.end())
        idx = it->second.idx;
    else if (!backup_free_.empty()) {
        idx = backup_free_.back();
        backup_free_.pop_back();
    } else {
        if (backup_next_ == backup_heaps_.size() * kBackupBlock) {
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> h;
            D3D12_DESCRIPTOR_HEAP_DESC hd{ D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kBackupBlock,
                D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0 };
            if (FAILED(g_addon.d3d12_device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&h)))) return;
            backup_heaps_.push_back(h);
        }
        idx = backup_next_++;
    }
    g_addon.d3d12_device->CopyDescriptorsSimple(1, BackupSlot(idx), slot, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    backup_of_[key] = { idx, mod_srv };
}

bool StampLedger::Unstamp(uint64_t resH, bool log)
{
    if (!g_addon.d3d12_device || !resH) return false;
    HeapKey key{ 0, 0 };
    if (!slot_of_.find(resH, key)) return false;
    if (!key.heap) return false;
    ResInfo ri;
    if (!g_descriptors.Find(key, ri)) return false;
    if (ri.res != reinterpret_cast<ID3D12Resource*>(resH)) return false; // slot reused
    return UnstampSlot(key.heap, key.offset, log);
}

bool StampLedger::UnstampSlot(uint64_t heap_h, uint32_t offset, bool log)
{
    if (!g_addon.d3d12_device || !heap_h) return false;
    const HeapKey key{ heap_h, offset };
    ResInfo ri;
    if (!g_descriptors.Find(key, ri)) return false;
    if (!ri.res || !ri.swapped) return false; // never stamped
    const uint64_t resH = reinterpret_cast<uint64_t>(ri.res);
#ifdef WWMI_DEBUG
    const bool traced = g_tex_trace.Armed() && g_tex_trace.On(g_tex_identity.HashOf(resH));
    const auto trace = [&](const char* what) {
        if (traced)
            g_tex_trace.Line("unstamp res=%p heap=%llx slot=%u: %s", (void*)resH, (unsigned long long)key.heap,
                key.offset, what);
    };
#endif
    auto* heap = reinterpret_cast<ID3D12DescriptorHeap*>(key.heap);
    const uint32_t inc = g_addon.d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const uint64_t lo = heap->GetCPUDescriptorHandleForHeapStart().ptr;
    const D3D12_CPU_DESCRIPTOR_HANDLE dst{ (SIZE_T)(lo + (uint64_t)key.offset * inc) };
    HeapKey src{};
    const bool src_tracked = ri.view_handle && g_descriptors.SlotOfCpu(ri.view_handle, &src);
    static thread_local int t_depth = 0;
    if (src_tracked && !(src == key) && t_depth < 2) {
        ResInfo sri;
        if (g_descriptors.Find(src, sri) && sri.swapped) {
            ++t_depth;
            UnstampSlot(src.heap, src.offset, false);
            --t_depth;
        }
    }
    ResInfo sri{};
    const bool src_ok = src_tracked
        ? !g_descriptors.ShaderVisible(src.heap) && g_descriptors.Find(src, sri) && sri.res == ri.res && !sri.swapped
        : g_descriptors.FindView(ri.view_handle, sri) && sri.res == ri.res;
    if (!ri.view_handle || ri.view_handle == dst.ptr || !src_ok) {
        if (!g_descriptors.ShaderVisible(key.heap) && RestoreFromBackup(key, dst)) {
            g_descriptors.SetSwapped(key, nullptr);
#ifdef WWMI_DEBUG
            trace("the game's descriptor restored from the copy kept before the stamp");
#endif
            if (log)
                logger::write(
                    "UNSTAMP res=%p slot %u: the game's descriptor restored from the copy kept before the stamp",
                    (void*)resH, key.offset);
            return true;
        }
        if (log)
            logger::write(
                "UNSTAMP res=%p slot %u: no unstamped CPU-only source for the game's descriptor -- the mod SRV stays",
                (void*)resH, key.offset);
#ifdef WWMI_DEBUG
        trace("no unstamped source for the game's descriptor; the stamp stays");
#endif
        return false;
    }
    g_addon.d3d12_device->CopyDescriptorsSimple(
        1, dst, { (SIZE_T)ri.view_handle }, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_descriptors.SetSwapped(key, nullptr);
#ifdef WWMI_DEBUG
    trace("the game's descriptor restored");
#endif
    if (log)
        logger::write(
            "UNSTAMP res=%p slot %u: the game's descriptor restored (degenerate identity)", (void*)resH, key.offset);
    return true;
}

std::vector<std::pair<HeapKey, uint64_t>> StampLedger::StampedCpuOnlySlots()
{
    std::vector<std::pair<HeapKey, uint64_t>> out;
    g_descriptors.ForEachStamped([&](uint64_t h, uint64_t, bool vis, uint32_t o, FlatHeapMap::Slot*) {
        ResInfo ri;
        if (!vis && g_descriptors.Find({ h, o }, ri))
            out.push_back({ HeapKey{ h, o }, reinterpret_cast<uint64_t>(ri.res) });
    });
    return out;
}

UnstampTally StampLedger::UnstampAll(void (*hold)(uint64_t res))
{
    UnstampTally t;
    std::vector<uint64_t> recorded;
    slot_of_.for_each([&](const uint64_t& r, const HeapKey&) { recorded.push_back(r); });
    for (uint64_t r : recorded) {
        if (!Unstamp(r, /*log*/ false)) continue;
        ++t.recorded;
        if (hold) hold(r);
    }
    const std::vector<std::pair<HeapKey, uint64_t>> sources = StampedCpuOnlySlots();
    t.cpu_only_stamped = sources.size();
    for (const auto& [key, res] : sources) {
        if (!UnstampSlot(key.heap, key.offset)) continue;
        ++t.cpu_only;
        if (hold) hold(res);
    }
    return t;
}

void SetAddonDisabled(bool dis)
{
    if (dis == g_addon.disabled.load()) return;
    if (dis) {
        const UnstampTally t = g_stamps.UnstampAll(nullptr);
        g_addon.disabled.store(true);
        logger::write("A1: addon DISABLED -- %u stamped slot(s) given the game's texture back, and %u of %zu still "
                      "stamped in CPU-only heaps",
            t.recorded, t.cpu_only, t.cpu_only_stamped);
    } else {
        // BEFORE the hooks resume: nothing may read a stale slot
        g_descriptors.Clear();
        g_stamps.Clear();
        logger::write(
            "A1: addon re-enabled -- descriptor slot records cleared (stale while bypassed); they refill as the game writes descriptors");
        g_addon.disabled.store(false);
    }
}

bool StampLedger::Restamp(uint64_t resH)
{
    if (!g_addon.d3d12_device || !resH || !g_tex_toggles.lineage) return false;
    HeapKey key{ 0, 0 };
    if (!slot_of_.find(resH, key) || !key.heap) {
        if (g_debug && DiagOnce("RESTAMP-NOSLOT", (int)(resH >> 4)))
            logger::write("RESTAMP-FAIL res=%p: no slot recorded for it", (void*)resH);
        return false;
    }
    ID3D12Resource* res = nullptr;
    {
        ResInfo ri;
        if (g_descriptors.Find(key, ri)) res = ri.res;
    }
    if (res != reinterpret_cast<ID3D12Resource*>(resH)) {
        if (g_debug && DiagOnce("RESTAMP-REUSED", (int)(resH >> 4)))
            logger::write("RESTAMP-FAIL res=%p: its slot heap=%llx off=%u now holds %p", (void*)resH,
                (unsigned long long)key.heap, (unsigned)key.offset, (void*)res);
        return false;
    }
    const char* const site_was = t_tex_swap_site;
    if (!site_was) t_tex_swap_site = "restamp";
    const bool ok = g_tex_place.Swap(g_addon.d3d12_device, key.heap, key.offset, res);
    t_tex_swap_site = site_was;
    if (!ok && g_debug && DiagOnce("RESTAMP-REFUSED", (int)(resH >> 4)))
        logger::write("RESTAMP-FAIL res=%p: slot heap=%llx off=%u is its own, the swap itself declined (%s)",
            (void*)resH, (unsigned long long)key.heap, (unsigned)key.offset,
            g_tex_place.Decline()[0] ? g_tex_place.Decline() : "miss: no mod SRV");
    return ok;
}

StampTally StampLedger::Move(const std::unordered_map<ID3D12Resource*, StampMove>& moves)
{
    StampTally t;
    if (!g_addon.d3d12_device || moves.empty()) return t;
    const uint32_t inc = DescInc();
    std::atomic_thread_fence(std::memory_order_seq_cst);
    g_descriptors.ForEachStamped([&](uint64_t, uint64_t lo, bool vis, uint32_t o, FlatHeapMap::Slot* slot) {
        ID3D12Resource* was = slot->swapped.load(std::memory_order_acquire);
        const auto it = was && lo ? moves.find(was) : moves.end();
        if (it == moves.end()) return;
        if (!slot->swapped.compare_exchange_strong(was, it->second.to, std::memory_order_acq_rel)) {
            ++t.raced;
            return;
        }
        g_addon.d3d12_device->CopyDescriptorsSimple(
            1, { (SIZE_T)(lo + (uint64_t)o * inc) }, it->second.to_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (vis)
            ++t.moved_visible;
        else
            ++t.moved_cpu_only;
    });
    std::unordered_map<uint64_t, uint64_t> srv_to;
    for (const auto& [from, m] : moves)
        if (m.from_srv.ptr) srv_to[m.from_srv.ptr] = m.to_srv.ptr;
    std::lock_guard l(backup_mutex_);
    for (auto& [key, bk] : backup_of_) {
        const auto it = srv_to.find(bk.mod_srv);
        if (it == srv_to.end()) continue;
        bk.mod_srv = it->second;
        ++t.backups;
    }
    return t;
}

D3D12_CPU_DESCRIPTOR_HANDLE StampLedger::NullSrv(ID3D12Device* dev, uint8_t kind)
{
    // DXIL kind -> our slot -> D3D12 view dimension
    static const D3D12_SRV_DIMENSION dims[8] = { D3D12_SRV_DIMENSION_TEXTURE2D, D3D12_SRV_DIMENSION_TEXTURE1D,
        D3D12_SRV_DIMENSION_TEXTURE2DARRAY, D3D12_SRV_DIMENSION_TEXTURE3D, D3D12_SRV_DIMENSION_TEXTURECUBE,
        D3D12_SRV_DIMENSION_TEXTURE1DARRAY, D3D12_SRV_DIMENSION_TEXTURECUBEARRAY, D3D12_SRV_DIMENSION_TEXTURE2DMS };
    unsigned slot = 0;
    switch (kind) {
    case 1: slot = 1; break;
    case 3: slot = 2; break;
    case 4: slot = 3; break;
    case 5: slot = 4; break;
    case 6: slot = 5; break;
    case 7: slot = 6; break;
    case 8: slot = 7; break;
    default: slot = 0; break;
    }
    std::lock_guard<std::mutex> l(null_mutex_);
    if (!dev) return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
    if (!null_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC d{};
        d.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        d.NumDescriptors = 8;
        if (FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&null_heap_)))) return D3D12_CPU_DESCRIPTOR_HANDLE{ 0 };
        null_inc_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (!null_srv_[slot].ptr) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
        sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sd.ViewDimension = dims[slot];
        sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        switch (dims[slot]) {
        case D3D12_SRV_DIMENSION_TEXTURE1D: sd.Texture1D.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURE1DARRAY:
            sd.Texture1DArray.MipLevels = 1;
            sd.Texture1DArray.ArraySize = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DARRAY:
            sd.Texture2DArray.MipLevels = 1;
            sd.Texture2DArray.ArraySize = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE3D: sd.Texture3D.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBE: sd.TextureCube.MipLevels = 1; break;
        case D3D12_SRV_DIMENSION_TEXTURECUBEARRAY:
            sd.TextureCubeArray.MipLevels = 1;
            sd.TextureCubeArray.NumCubes = 1;
            break;
        case D3D12_SRV_DIMENSION_TEXTURE2DMS: break;
        default: sd.Texture2D.MipLevels = 1; break;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE h{ null_heap_->GetCPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)slot * null_inc_ };
        dev->CreateShaderResourceView(nullptr, &sd, h);
        null_srv_[slot] = h;
    }
    return null_srv_[slot];
}

// A slot stamped with a texture that is dying given back: the game's descriptor if one can be had, else a null view.
// False when the game wrote the slot meanwhile.
bool StampLedger::GiveBack(const HeapKey& key, uint64_t lo, FlatHeapMap::Slot* slot, ID3D12Resource* res, StampTally& t)
{
    ResInfo ri{};
    const bool game_res = g_descriptors.Find(key, ri) && ri.res;
    if (UnstampSlot(key.heap, key.offset)) {
        ++t.restored;
        if (game_res) g_tex_place.HoldForRetry(reinterpret_cast<uint64_t>(ri.res));
        return true;
    }
    const D3D12_CPU_DESCRIPTOR_HANDLE null_srv = NullSrv(g_addon.d3d12_device, 0);
    ID3D12Resource* was = res;
    if (!null_srv.ptr || !slot->swapped.compare_exchange_strong(was, nullptr, std::memory_order_acq_rel)) {
        ++t.raced;
        return false;
    }
    g_addon.d3d12_device->CopyDescriptorsSimple(
        1, { (SIZE_T)(lo + (uint64_t)key.offset * DescInc()) }, null_srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    ++t.nulled;
    if (game_res) g_tex_place.HoldForRetry(reinterpret_cast<uint64_t>(ri.res));
    return true;
}

StampTally StampLedger::UnstampDying(const std::unordered_set<ID3D12Resource*>& dying)
{
    StampTally t;
    if (!g_addon.d3d12_device || dying.empty()) return t;
    struct Found {
        HeapKey key;
        uint64_t lo;
        FlatHeapMap::Slot* slot;
        ID3D12Resource* res;
    };
    std::vector<Found> found;
    std::atomic_thread_fence(std::memory_order_seq_cst);
    g_descriptors.ForEachStamped([&](uint64_t h, uint64_t lo, bool, uint32_t o, FlatHeapMap::Slot* slot) {
        ID3D12Resource* was = slot->swapped.load(std::memory_order_acquire);
        if (lo && was && dying.count(was)) found.push_back({ { h, o }, lo, slot, was });
    });
    for (Found& f : found) GiveBack(f.key, f.lo, f.slot, f.res, t);
    return t;
}

void StampLedger::StampedStoreTexture(uint64_t heap, uint32_t offset, ID3D12Resource* res)
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    if (!res || !g_addon.d3d12_device || g_mod_textures.IndexOf(res) >= 0) return;
    FlatHeapMap::Entry* e = g_descriptors.EntryOf(heap);
    FlatHeapMap::Slot* slot = FlatHeapMap::at(e, offset);
    const uint64_t lo = e ? e->lo.load(std::memory_order_relaxed) : 0;
    if (!slot || !lo || slot->swapped.load(std::memory_order_acquire) != res) return;
    StampTally t;
    GiveBack({ heap, offset }, lo, slot, res, t);
}
