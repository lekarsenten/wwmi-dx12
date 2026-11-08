#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <wrl.h>
#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <unordered_set>
#include <vector>

// A texture whose bytes never passed an upload hook: its mip0 copied to a readback buffer, hashed frames later.
class TexReadback {
public:
    bool Schedule(ID3D12Device* dev, reshade::api::command_list* list, ID3D12GraphicsCommandList* cl,
        ID3D12Resource* src, ID3D12Resource* dst, const D3D12_RESOURCE_DESC& sd);
    bool ScheduleUpload(ID3D12Device* dev, reshade::api::command_list* list, ID3D12GraphicsCommandList* cl,
        ID3D12Resource* src, uint64_t src_off, uint64_t bytes, ID3D12Resource* dst, uint32_t sub,
        const uint32_t* cpu_crc);
    // A game list submitted: the readbacks recorded on it are timed by a fence signalled behind it.
    void OnExecute(reshade::api::command_queue* queue, reshade::api::command_list* list);
    void Drain(); // once per present: what the graphics card has copied, hashed
    void Forget(ID3D12Resource* dst) // destroyed: its address is free again
    {
        std::lock_guard l(mtx_);
        tried_.erase(dst);
    }

private:
    struct QueueFence {
        ID3D12CommandQueue* queue = nullptr;
        Microsoft::WRL::ComPtr<ID3D12Fence> fence;
        uint64_t next = 0;
    };
    struct Pending {
        ID3D12Resource* dst = nullptr;
        uint32_t off = 0;
        uint32_t rowPitch = 0;
        uint32_t sub = 0; // an upload's mip
        bool compare = false; // D16: cpu_crc is the processor's read of the same upload
        uint32_t cpu_crc = 0;
        Microsoft::WRL::ComPtr<ID3D12Resource> own; // not in the shared buffer
        const void* list = nullptr; // the game's list the copy is recorded on
        uint64_t gen = 0; // ...in this recording of it
        QueueFence* qf = nullptr; // the queue it was submitted to
        uint32_t tid = 0; // ...by this thread
        uint64_t resets = 0; // ...at this t_list_resets
        ULONGLONG exec_ms = 0;
        uint64_t value = 0; // signalled on qf behind the submission; the copy has run once the fence reaches it
    };
    struct Spare {
        Microsoft::WRL::ComPtr<ID3D12Resource> buf;
        uint64_t bytes = 0;
    };
    bool Reserve(ID3D12Device* dev, uint64_t total, bool once, Pending& p, ID3D12Resource** buf);
    void KeepSpare(Microsoft::WRL::ComPtr<ID3D12Resource> b);
    static void Arrived(const Pending& p, const uint8_t* data);
    QueueFence* FenceOf(ID3D12CommandQueue* q);
    template <class Behind> unsigned SignalBehind(QueueFence* qf, Behind behind);
    static constexpr uint32_t kBytes = 48u * 1024 * 1024;
    ID3D12Device* dev_ = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> rb_;
    uint32_t cursor_ = 0; // the shared buffer is free from here; back to 0 once no copy into it is outstanding
    std::vector<Pending> pending_;
    std::vector<Spare> spare_; // own buffers whose readback was hashed or dropped, for the next that does not fit
    ULONGLONG own_used_ms_ = 0; // when an own buffer was last taken; the spares go after 10 s without one
    std::deque<QueueFence> fences_; // one per queue: queues finish out of order
    std::atomic<uint32_t> unsignalled_{ 0 }; // pending_ entries with no signal behind them yet
    std::unordered_set<ID3D12Resource*> tried_;
    std::mutex mtx_;
};
extern TexReadback g_tex_readback;
