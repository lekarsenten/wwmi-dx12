// The component table. See component_table.h.
#include "component_table.h"
#include "game_cb_reader.h"
#include "mod_registry.h"
#include "../addon/addon.h"
#include "../core/d3d12_buffer.h"
#include "../core/log.h"
#include <wrl.h>
#include <cstring>
#include "mod_set.h"


int ComponentTable::SlotOf(uint32_t first_index, uint32_t index_count) const
{
    auto it = by_range_.find(FirstCountKey(first_index, index_count));
    return it == by_range_.end() ? -1 : it->second;
}

const std::vector<int>* ComponentTable::SharedSlots(uint32_t first_index, uint32_t index_count) const
{
    auto it = shared_by_range_.find(FirstCountKey(first_index, index_count));
    return it == shared_by_range_.end() ? nullptr : &it->second;
}

int ComponentTable::SlotOfIbHash(uint32_t ib_hash) const
{
    auto it = by_ib_hash_.find(ib_hash);
    return it == by_ib_hash_.end() ? -1 : it->second;
}

void ComponentTable::Index()
{
    by_range_.clear();
    shared_by_range_.clear();
    for (size_t i = 0; i < comps_.size(); ++i) {
        const uint64_t key = FirstCountKey(comps_[i].first_index, comps_[i].match_count);
        auto prev = by_range_.find(key);
        if (prev != by_range_.end()) {
            auto& shared = shared_by_range_[key];
            if (shared.empty()) shared.push_back(prev->second);
            shared.push_back((int)i);
        }
        by_range_[key] = (int)i;
    }
    by_ib_hash_.clear();
    for (size_t i = 0; i < comps_.size(); ++i) {
        const WwmiMod::Component* wc = Mods().Comp(comps_[i].mod_id, comps_[i].comp_idx);
        if (wc && wc->ib_hash) by_ib_hash_.emplace(wc->ib_hash, (int)i);
    }
}

static uint64_t SharedIdentityCbVa(bool* failed)
{
    static uint64_t s_va = 0;
    if (s_va) return s_va;
    Microsoft::WRL::ComPtr<ID3D12Resource> cb;
    if (FAILED(CreateD3D12Buffer(g_addon.d3d12_device, D3D12_HEAP_TYPE_UPLOAD, kBoneCBBytes,
            D3D12_RESOURCE_STATE_GENERIC_READ, &cb)))
        return 0;
    D3D12_RANGE nr = { 0, 0 };
    void* m = nullptr;
    if (FAILED(cb->Map(0, &nr, &m)) || !m) {
        *failed = true;
        return 0;
    }
    memset(m, 0, kBoneCBBytes);
    float* f = (float*)m;
    for (uint32_t b = 0; b < kMaxBones; ++b) {
        f[b * 12 + 0] = 1.f;
        f[b * 12 + 5] = 1.f;
        f[b * 12 + 10] = 1.f;
    }
    s_va = cb->GetGPUVirtualAddress();
    cb.Detach();
    return s_va;
}

void ComponentTable::Finalize()
{
    if (!g_addon.d3d12_device) return;
    bool failed = false;
    identity_cb_va_ = SharedIdentityCbVa(&failed);
    if (failed) return;
    loaded_ = true;
    logger::write("MergedRemap: finalized %zu total components across all mods", comps_.size());
}
