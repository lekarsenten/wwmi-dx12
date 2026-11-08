#pragma once
#include <d3d12.h>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
#include "../instrumentation/diag_gate.h" // WWMI_DEBUG_FN

// The H4 / H5 skeleton readbacks: the whole chain's inputs and outputs (H4), the merged skeleton alone (H5); each
// recorded on the [Present] list and read four frames later.
class SkeletonDump {
public:
    // What the last component draw published as vs-cb4 / vs-cb3, kept for the chain dump.
    WWMI_DEBUG_FN void NoteDrawCbs(ID3D12Resource* cb4_res, uint64_t cb4_va, int cb4_root, ID3D12Resource* cb3_res,
        uint64_t cb3_va, int cb3_root) WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN void ChainTick(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* q)
        WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN void MergedTick(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* q)
        WWMI_DEBUG_BODY();

private:
    struct ChainSrc {
        std::wstring label;
        ID3D12Resource* res;
        UINT64 off;
    };
    static constexpr UINT64 kBytes = 64 * 16; // 64 float4 per source = ~21 bones

    ID3D12Resource* cb4_res_ = nullptr;
    uint64_t cb4_va_ = 0;
    ID3D12Resource* cb3_res_ = nullptr;
    uint64_t cb3_va_ = 0;
    int cb4_root_ = -1;
    int cb3_root_ = -1;
    std::vector<std::pair<std::wstring, ID3D12Resource*>> chain_readbacks_; // label -> readback
    uint32_t chain_ready_frame_ = 0;
    ID3D12Resource* merged_readback_ = nullptr;
    std::wstring merged_fq_;
    uint32_t merged_ready_frame_ = 0;
};
WWMI_DEBUG_VAR SkeletonDump g_skeleton_dump;
