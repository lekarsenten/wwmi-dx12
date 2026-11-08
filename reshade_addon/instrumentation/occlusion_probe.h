#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <atomic>
#include <mutex>
#include <cstdint>
#include "diag_gate.h"

class OcclusionProbe {
public:
    static constexpr UINT kSlots = 64;
    // Claim a slot for one injected draw. Returns -1 when not collecting or out of slots.
    WWMI_DEBUG_FN int Begin(ID3D12GraphicsCommandList* cl, ID3D12Device* dev, int comp, UINT index_count,
        UINT rtv_count, bool has_dsv) WWMI_DEBUG_BODY(return -1;);
    WWMI_DEBUG_FN void End(ID3D12GraphicsCommandList* cl, int slot) WWMI_DEBUG_BODY();
    // Arm on the overlay request, count the window down, map the readback once the GPU has drained.
    WWMI_DEBUG_FN void PresentTick() WWMI_DEBUG_BODY();

private:
    bool Ensure(ID3D12Device* dev);
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb_;
    std::atomic<int> armed_frames_{ 0 }; // >0 = collecting
    std::atomic<UINT> next_slot_{ 0 };
    std::mutex mtx_;
    int slot_comp_[kSlots] = {}; // which component owns each slot
    UINT slot_count_[kSlots] = {}; // its index count
    UINT slot_rtv_[kSlots] = {};
    bool slot_dsv_[kSlots] = {};
    int read_countdown_ = 0; // frames until the readback is safe to map
};
WWMI_DEBUG_VAR OcclusionProbe g_occl;
