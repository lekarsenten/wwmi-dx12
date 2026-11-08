#pragma once
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <mutex>
#include <unordered_map>

// The D3D12 debug layer's message queue (ini dx12_debug_layer=1): validation errors drained into our log as
// DBGLAYER, with the caller's stack for the first three of each id (DBGCALLER) where the layer offers a callback.
class DebugLayer {
public:
    void Attach(ID3D12Device* dev);
    // Drain the queue into our log, tagged with where the drain happened.
    void Drain(const char* when);
    void Detach();

private:
    static void __stdcall OnMessage(
        D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR, void* self);
    ID3D12InfoQueue* infoq_ = nullptr;
    ID3D12InfoQueue1* infoq1_ = nullptr;
    DWORD cookie_ = 0;
    std::mutex traced_mutex_;
    std::unordered_map<int, unsigned> traced_; // DBGCALLER lines written per message id
    std::unordered_map<int, uint64_t> seen_; // DBGLAYER messages drained per id (the drain runs on one thread)
};
extern DebugLayer g_debug_layer;
