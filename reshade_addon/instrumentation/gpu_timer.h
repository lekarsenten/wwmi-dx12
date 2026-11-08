#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <atomic>
#include <cstdint>
#include "diag_gate.h"
#include "hot_path.h"

enum GpuTimerBucket {
    GT_Present = 0, // the whole [Present] list: their chains, uploads, back-buffer restore, mesh hash
    GT_PresentDraw, //   - a draw their engine issues on it (dx12_pipeline.cpp IssueDraw)
    GT_PresentDispatch, //   - a dispatch their engine issues on it (IssueDispatch): skeleton chains
    GT_IbHash, //   - the mesh-hash dispatch
    GT_ClaimedDraw, // the claimed-draw path on the game's list: VB/IB swap, barriers, sections, restore
    GT_SectionDraw, //   - one injected section draw (draw_section_draw.cpp)
    GT_IssueDraw, //   - a draw their engine issues on the game's list
    GT_IssueDispatch, //   - a dispatch their engine issues on the game's list (shapekeys, chains)
    GT_ShapeKeys, //   - RunShapeKeyPasses: the mod's shapekey chain, copies and dispatches with barriers
    GT_RelocRestore, //   - the relocated buffers' transitions back to UAV (prep's mirror; one each way)
    GT_DispatchHook, // the dispatch hook on the game's list: what the dispatch overrides record
    GT_UploadFlush, // FlushPendingUploads, either list: IniParams and texture uploads with their barriers
    GT_ShimCopyXit, // the shim's CopyResource: its two transitions (the first stamp waits for prior work)
    GT_ShimCopy, // the shim's CopyResource / buffer region copy itself, either list
    GT_ShimCopyCs, // the same copy as the BufCopy compute dispatch (transitions, dispatch, restore)
    GT_COUNT
};
inline const char* const kGtNames[GT_COUNT] = {
    "Mod scripts once a frame",
    "  their draws",
    "  their compute",
    "  recognising meshes",
    "Modded character draws",
    "  mod parts drawn",
    "  draws by mod scripts",
    "  compute by mod scripts",
    "  shape keys",
    "  moved buffers put back",
    "Shape keys at the game's own compute",
    "Uploads",
    "Copies by mod scripts: state changes",
    "Copies by mod scripts",
    "Copies by mod scripts, as compute",
};

class GpuTimer {
public:
    static constexpr UINT kRing = 8; // frame regions kept before one is reused
    static constexpr UINT kPairs = 2048; // timestamp pairs per region
    // Stamp the start of one bracket. Returns the pair, or -1 when off, out of pairs, or not built.
    int Begin(ID3D12GraphicsCommandList* cl, int bucket);
    // Stamp its end and resolve the pair into this frame's region.
    void End(ID3D12GraphicsCommandList* cl, int pair);
    // After the present-pass submit: the region is stamped with that fence value and the next one opens.
    void FrameSubmitted(ID3D12Fence* fence, uint64_t value, ID3D12CommandQueue* queue);
    // Reads every region whose fence has passed; the sums land in g_gpu_timer_stats for the performance monitor.
    void PresentTick(ID3D12Device* dev);

private:
    bool Ensure(ID3D12Device* dev);
    void ReadRegion(UINT r);
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> heap_;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb_; // the readback: 2 stamps per pair, mapped per read
    std::atomic<bool> ready_{ false }; // set once heap_ and rb_ both exist; Begin reads it from the draw threads
    double ms_per_tick_ = 0.0; // from the queue's timestamp frequency
    std::atomic<UINT> region_{ 0 }; // where Begin lands now
    std::atomic<UINT> next_pair_[kRing] = {};
    uint8_t pair_bucket_[kRing][kPairs] = {};
    uint64_t region_fence_[kRing] = {}; // present-pass fence value stamped at the region's close
    bool region_open_[kRing] = {}; // stamped and not yet read
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_; // the present pass's, held for GetCompletedValue
};
extern GpuTimer g_gpu_timer;

// One bracket over a scope: the end stamp lands after the last command the scope recorded.
struct GpuBracket {
    ID3D12GraphicsCommandList* cl;
    int pair;
    void* hp;
    GpuBracket(ID3D12GraphicsCommandList* c, int bucket)
        : cl(c),
          pair(g_gpu_timer.Begin(c, bucket)),
          hp(HotPath::GpuBracketBegin(c, bucket))
    {}
    ~GpuBracket()
    {
        HotPath::GpuBracketEnd(cl, hp);
        g_gpu_timer.End(cl, pair);
    }
    GpuBracket(const GpuBracket&) = delete;
    GpuBracket& operator=(const GpuBracket&) = delete;
};

// This frame's readback, taken by the performance monitor (ns summed over the pairs read, pairs counted).
struct GpuTimerStats {
    std::atomic<uint64_t> ns[GT_COUNT] = {};
    std::atomic<uint32_t> n[GT_COUNT] = {};
    std::atomic<uint32_t> frames_read{ 0 }; // regions read this frame (0, 1 or, after a stall, more)
    std::atomic<uint32_t> dropped{ 0 }; // regions reused before their fence passed (never read)
    std::atomic<uint32_t> untimed{ 0 }; // brackets refused: their region's pairs were all taken
};
extern GpuTimerStats g_gpu_timer_stats;
