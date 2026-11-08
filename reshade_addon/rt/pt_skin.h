#pragma once
// pt_skin.h -- PTSKIN: under RT, a mod draw through the game's pass-through VS is skinned by our compute pass first.
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include "pt_skin_owned.h"

// One compute pass per injected pass-through draw, root descriptors only, the game's PSO put back after it.
class PtSkinPass {
public:
    // Is `gfx_pso` drawn through the skin-cache pass-through input LAYOUT? Cached per PSO.
    bool IsPassThrough(uint64_t gfx_pso);
    // RTLAYOUT: logs, once per PSO, a mod component drawn under RT through another layout.
    void NoteOtherLayout(uint64_t gfx_pso, uint64_t vs_hash, int mod, int comp);
    // The mod's buffers for what the material VS fetches by vertex id, outside its input layout.
    struct FetchViews {
        ID3D12Resource* tan = nullptr; // skinned tangents: 2 x R16G16B16A16_SNORM per vertex, from element 0
        ID3D12Resource* prev = nullptr; // last frame's skinned positions: 3 x R32_FLOAT per vertex, from element 0
        ID3D12Resource* uv = nullptr; // the mod's vb2: 4 x R16G16_FLOAT per vertex, from element uv_first
        uint64_t uv_first = 0;
        uint32_t uv_elems = 0; // R16G16_FLOAT elements vb2 really holds -- may cover fewer vertices than vb0
        uint32_t vcount = 0;
    };
    bool Run(ID3D12GraphicsCommandList* cl, uint64_t gfx_pso, uint64_t list_gen, uint32_t index_count,
        uint32_t first_index, int mod, int comp, uint64_t game_bones_va, FetchViews* fetch = nullptr,
        bool palette_is_skeleton = false, const wchar_t* pos_fallback_fq = nullptr);
    void NotePalette(int mod, int comp, bool bound, const char* why, bool prev);

private:
    struct ModBufs {
        Owned pos[2];
        Owned tan;
        uint32_t vcount = 0;
        uint32_t written[2] = {}; // g_frame_id each pos buffer was last written in
        uint32_t runs = 0;
    }; // no destructor: releasing into a torn-down port at unload is worse than the leak
    struct PerSet;
    bool Init(ID3D12Device* dev);
    bool Make(ID3D12Device* dev, Owned& o, uint64_t bytes);

    std::mutex mutex_; // Run records from whichever thread owns the list; the buffers and their states are shared
    bool ready_ = false;
    bool failed_ = false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rs_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    std::unordered_map<uint64_t, ModBufs> mods_;
    // A buffer replaced by a bigger one may still be read by a list in flight: released some frames later.
    std::vector<std::pair<uint32_t, Microsoft::WRL::ComPtr<ID3D12Resource>>> grave_;
    std::mutex pso_mutex_;
    std::unordered_map<uint64_t, bool> pso_pass_through_;
    std::mutex other_mutex_;
    std::unordered_map<uint64_t, bool> other_seen_; // RTLAYOUT: PSOs logged
};
extern PtSkinPass g_pt_skin_pass;
