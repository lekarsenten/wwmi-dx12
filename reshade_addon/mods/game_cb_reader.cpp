// The game's constant buffers, read back from their UPLOAD-heap mappings. See game_cb_reader.h.
#include "game_cb_reader.h"
#include "../core/log.h"
#include "../hooks/command_list_registry.h"
#include "../instrumentation/diag_gate.h"
#include <cmath>
#include <cstring>

using namespace reshade::api;

GameCbReader g_game_cbs;

bool LooksLikeBone(const float* m)
{
    float r0[3] = { m[0], m[1], m[2] }; // rotation row 0 (skip translation m[3])
    float r1[3] = { m[4], m[5], m[6] }; // rotation row 1
    float n0 = sqrtf(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
    float n1 = sqrtf(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
    if (n0 < 0.5f || n0 > 2.0f || n1 < 0.5f || n1 > 2.0f) return false;
    float dot = (r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2]) / (n0 * n1);
    return (dot > -0.25f && dot < 0.25f); // rows ~orthogonal -> a rotation
}

uint8_t* GameCbReader::At(ID3D12Resource* res, uint64_t va, bool map_on_demand, uint64_t* bytes_left)
{
    if (!res || !va) return nullptr;
    Mapping e;
    bool have = false;
    {
        std::shared_lock l(mutex_);
        auto it = maps_.find(res);
        if (it != maps_.end()) {
            e = it->second;
            have = true;
        }
    }
    if (!have) {
        if (!map_on_demand) return nullptr;
        D3D12_HEAP_PROPERTIES hp = {};
        D3D12_HEAP_FLAGS hf = {};
        res->GetHeapProperties(&hp, &hf);
        if (hp.Type == D3D12_HEAP_TYPE_UPLOAD) {
            D3D12_RANGE rr = { 0, 0 };
            void* m = nullptr;
            if (SUCCEEDED(res->Map(0, &rr, &m)) && m) {
                e.mapped = (uint8_t*)m;
                e.base = res->GetGPUVirtualAddress();
                e.width = res->GetDesc().Width;
            }
        }
        std::unique_lock l(mutex_);
        auto ins = maps_.try_emplace(res, e);
        if (!ins.second) {
            if (e.mapped) {
                D3D12_RANGE none = { 0, 0 };
                res->Unmap(0, &none);
            }
            e = ins.first->second;
        }
    }
    if (!e.mapped) return nullptr;
    if (va < e.base) return nullptr;
    const uint64_t off = va - e.base;
    if (off >= e.width) return nullptr;
    *bytes_left = e.width - off;
    return e.mapped + off;
}

void GameCbReader::Forget(ID3D12Resource* res)
{
    if (!res) return;
    size_t erased;
    {
        std::unique_lock l(mutex_);
        erased = maps_.erase(res);
    }
    if (erased && g_debug) {
        const uint64_t n = forgotten_.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n % 256) == 0)
            logger::write("CBMAP-FORGET #%llu: destroyed resource %p had a cached CB mapping -- erased, so a new "
                          "resource at this address maps afresh instead of reading the dead one",
                (unsigned long long)n, (void*)res);
    }
}

uint32_t GameCbReader::Snapshot(ID3D12Resource* res, uint64_t va, uint8_t* dst, uint32_t max_bytes, bool map_on_demand)
{
    if (!dst || !max_bytes) return 0;
    uint64_t left = 0;
    const uint8_t* at = At(res, va, map_on_demand, &left);
    if (!at) return 0;
    const uint32_t n = (left < max_bytes) ? (uint32_t)left : max_bytes;
    memcpy(dst, at, n);
    return n;
}

bool GameCbReader::ReadRootCbv(command_list* cmd_list, uint32_t param, void* dst, uint32_t n)
{
    const RootCbvBind& cb = g_command_lists.Cbv(cmd_list).binds[param];
    uint8_t raw[64];
    if (n > sizeof raw || !cb.res || Snapshot(cb.res, cb.va, raw, n, true) != n) return false;
    memcpy(dst, raw, n);
    return true;
}

uint32_t GameCbReader::BoneRootMask(command_list* cmd_list)
{
    RootCbvBind local[16] = {};
    {
        const CmdCbvBinds& cb = g_command_lists.Cbv(cmd_list);
        for (int i = 0; i < 16; ++i) local[i] = cb.binds[i];
    }
    uint32_t mask = 0;
    for (int rp = 0; rp < 16; ++rp) {
        float m[12]; // one matrix, copied out of the mapping (Snapshot)
        if (Snapshot(local[rp].res, local[rp].va, (uint8_t*)m, sizeof(m)) != sizeof(m)) continue;
        if (LooksLikeBone(m)) mask |= (1u << rp);
    }
    return mask;
}
