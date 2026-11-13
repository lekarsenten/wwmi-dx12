#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <vector>
#include <reshade.hpp>

// Their engine's frame: the load probe on a new parse, and [Present] run with a command list of ours that is
// submitted on the game's queue.
class EngineFrame {
public:
    void ProbeTick(reshade::api::command_queue* queue, reshade::api::swapchain* sc);
    // The per-frame engine step; with no back buffer bound, [Present]'s draws to it are left to Redraw.
    void Tick(reshade::api::command_queue* queue, reshade::api::swapchain* sc, bool bind_back_buffer);
    void Redraw(reshade::api::command_queue* queue, reshade::api::swapchain* sc);
    // Held through a step and a redraw: they share the port's frame context, and with frame generation run on two
    // threads.
    std::mutex& Mutex() { return mutex_; }

private:
    struct Pass {
        static const UINT kRing = 3;
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc[kRing];
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        HANDLE ev = nullptr;
        uint64_t counter = 0;
        uint64_t slot_fence[kRing] = {};
        UINT slot = 0;
        uint32_t skipped = 0;
        bool ready = false;
        bool Init(ID3D12Device* dev);
        ID3D12GraphicsCommandList* Begin();
        HRESULT Submit(ID3D12CommandQueue* game_queue);
    };
    struct ChainProf {
        std::wstring ns;
        float od_pre;
        float sid_pre;
        float od_mid = 0.f;
        float sid_mid = 0.f;
    };
    struct Frame { // one Tick's values
        reshade::api::command_queue* queue;
        reshade::api::swapchain* sc;
        float rw = 0;
        float rh = 0;
        bool focused = false;
        ID3D12Resource* bb12 = nullptr;
        Pass* pass = nullptr;
        ID3D12GraphicsCommandList* cl = nullptr;
        int gt = -1;
        void* hp = nullptr;
        unsigned long long own_draws_before = 0;
    };
    // The game window's client size (left as passed until it is known), and whether it has the focus.
    void GameClient(float& rw, float& rh, bool& focused);
    void InstallKeyGate();
    void BeginList(Frame& f);
    void DebugHotkeys(bool focused);
    void PresentPasses(Frame& f, std::vector<ChainProf>& cp);
    void SubmitList(Frame& f);

    Pass present_pass_;
    Pass frame_end_pass_; // the step's list when it runs at the game's frame end, on the game's thread
    std::mutex mutex_;
    HWND game_hwnd_ = nullptr;
    bool port_tried_ = false;
    unsigned port_built_for_ = ~0u;
    bool gate_installed_ = false;
    uint32_t held_retry_frame_ = 0;
    int x18_last_ = -1;
    int x34_last_ = -1;
};
extern EngineFrame g_engine_frame;
