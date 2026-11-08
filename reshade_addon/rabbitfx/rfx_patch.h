#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "pso_clone.h"
#include "rfx_dxil.h"

constexpr unsigned kRfxGlowReg = 50; // ps-t50 / ps-t51, the ini's registers
constexpr unsigned kRfxCutoutReg = 51;
constexpr unsigned kRfxParamsReg = 52; // ours: the ini's t120 is outside the game's t0..t63 table
constexpr unsigned kRfxIniOnParam = 172; // y172: the patched shader reads t50 / t51
constexpr unsigned kRfxIniShiftParam = 217; // x217..w217: hue, saturation, value, brightness
constexpr const wchar_t* kRfxNamespace = L"RabbitFX";
constexpr const wchar_t* kRfxRegexGroup = L"Main"; // [ShaderRegexMain]: the blocks the patch reproduces

struct RfxPatchStats {
    unsigned ps_patched;
    unsigned ps_failed;
    unsigned variants;
}; // per game instance (the disk cache refills them)

struct RfxPatchedPs; // a pixel shader as patched, or a patch that failed: said once, never retried

// The shader-patch variants of the game's PSOs, built off the drawing thread: the RabbitFX patch, the
// decensor-only patch (settles into PipelineRegistry::SetDecensorVariant) and the UID cuts (SetUidVariant).
class RfxPatchWorker {
public:
    ID3D12PipelineState* Variant(
        reshade::api::device* dev, uint64_t src_pso, unsigned* ps_max_srv_reg, bool* has_glow, bool decensor);
    void QueueDecensor(reshade::api::device* dev, uint64_t src_pso);
    // The UID-cut variant of a game PSO, for a back buffer of `target`: its pixel shader with `cuts`.
    void QueueUid(reshade::api::device* dev, uint64_t src_pso, uint32_t target, std::vector<RfxDxilCut> cuts);
    void Stop();
    RfxPatchStats Stats();
    // The source's variants, queued jobs and patched shaders leave the tables; the caller destroys the variants.
    void ForgetSource(uint64_t src_pso, std::vector<ID3D12PipelineState*>& dead);

private:
    struct Made {
        ID3D12PipelineState* pso;
        unsigned max_reg; // the source PS's highest t-register
        bool has_glow;
    };
    struct Job {
        reshade::api::device* dev;
        uint64_t src_pso0;
        uint64_t key;
        bool decensor;
        PsoClone clone;
        bool inject = true;
        std::vector<RfxDxilCut> cuts; // not empty: a UID-cut job (inject and decensor false)
        uint32_t target = 0; // its back buffer size, width << 16 | height
    };
    struct Thread {
        std::thread t;
        ~Thread()
        {
            if (t.joinable()) t.detach();
        }
    };
    void Build(Job& job);
    void Run();
    void Wake();

    std::mutex mutex_; // variants_, miss_logged_, queue_, pending_, uid_pending_, uid_variants_, the counters
    std::unordered_map<uint64_t, Made> variants_; // src -> variant; pso nullptr = a settled no
    std::unordered_set<uint64_t> miss_logged_;
    std::deque<Job> queue_;
    std::unordered_set<uint64_t> pending_; // keys queued or in flight
    // UID-cut jobs: (source PSO, target) queued or in flight, and every variant made, until the source is destroyed.
    std::set<std::pair<uint64_t, uint32_t>> uid_pending_;
    std::unordered_multimap<uint64_t, ID3D12PipelineState*> uid_variants_;
    std::condition_variable cv_;
    std::atomic<bool> stop_{ false };
    Thread worker_; // start (a draw thread) vs stop: under thread_mutex_
    std::mutex thread_mutex_;
    std::unordered_map<uint64_t, std::shared_ptr<RfxPatchedPs>> ps_; // WORKER THREAD ONLY
    unsigned ps_patched_ = 0;
    unsigned ps_failed_ = 0;
    unsigned variants_made_ = 0;
};
extern RfxPatchWorker g_rfx_patch;
