#pragma once
#include <cstdint>
#include <mutex>
#include <d3d12.h>
#include <wrl/client.h>
#include <reshade.hpp>
#include "../mods/wwmi_mod.h" // WwmiMod::Component
#include "../core/cb_ring.h" // RTFETCH: our LocalVF copies
#include "pt_skin.h" // PtSkinPass::FetchViews

struct VfRestore {
    uint8_t param = 0xFF;
    uint64_t va = 0;
};
struct RtPalResult {
    bool tried = false;
    bool published = false;
    bool no_merge = false;
};

// The RT module's part of a claimed draw: PTSKIN, RTFETCH, RTPAL.
class RtDrawPass {
public:
    // PTSKIN + RTFETCH + H27 on a pass-through draw of component `wc`; any other draw is left alone.
    VfRestore SkinDraw(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* d3d12_cl, int dm,
        const WwmiMod::Component* wc, uint32_t c, uint32_t s2, uint32_t first_index, uint32_t index_count,
        int32_t vertex_offset, uint64_t vs_hash, uint64_t b4_va, int b4_root, bool rtpal_no_merge);
    RtPalResult PalPublish(uint64_t draw_pso, int dm, const WwmiMod::Component* wc, uint64_t b4_va);

private:
    VfRestore StampVertexFetch(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
        const PtSkinPass::FetchViews& fv, int dm, int comp);
    static constexpr uint32_t kFetchRing = 3 * 1024; // CPU-only views: a shadow bind later in the draw copies from them
    CbRing lvf_ring_;
    std::mutex fetch_mutex_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> fetch_heap_;
    uint32_t fetch_next_ = 0;
};
extern RtDrawPass g_rt_draw;
