#pragma once
#include <cstdint>
#include <d3d12.h>

inline HRESULT CreateGpuQueue(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type,
    const wchar_t* queue_name, const wchar_t* list_name,
    ID3D12CommandQueue** queue, ID3D12CommandAllocator** alloc,
    ID3D12GraphicsCommandList** list)
{
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = type;
    HRESULT hr = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(queue));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandAllocator(type, IID_PPV_ARGS(alloc));
    if (SUCCEEDED(hr)) hr = dev->CreateCommandList(0, type, *alloc, nullptr, IID_PPV_ARGS(list));
    if (FAILED(hr)) return hr;
    (*list)->Close();
    if (queue_name) (*queue)->SetName(queue_name);
    if (list_name) (*list)->SetName(list_name);
    return S_OK;
}

// The shared event must be auto-reset, as in the queue and present-pass callers.
inline DWORD WaitForFence(ID3D12Fence* fence, UINT64 value, HANDLE event, DWORD ms)
{
    if (!fence) return WAIT_FAILED;
    UINT64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return WAIT_FAILED; // device removed, not successful completion
    if (completed >= value) return WAIT_OBJECT_0;
    if (!event) return WAIT_FAILED;
    const ULONGLONG started = GetTickCount64();
    if (FAILED(fence->SetEventOnCompletion(value, event))) return WAIT_FAILED;
    for (;;) {
        const ULONGLONG elapsed = GetTickCount64() - started;
        const DWORD remaining = ms == INFINITE ? INFINITE :
            (elapsed >= ms ? 0 : ms - static_cast<DWORD>(elapsed));
        const DWORD result = WaitForSingleObject(event, remaining);
        if (result != WAIT_OBJECT_0) return result;
        completed = fence->GetCompletedValue();
        if (completed == UINT64_MAX) return WAIT_FAILED;
        if (completed >= value) return WAIT_OBJECT_0;
        // A previous timed-out registration may have signalled this shared event.
        // Only the requested fence value proves the allocator/readback is ready.
        if (ms != INFINITE && GetTickCount64() - started >= ms) return WAIT_TIMEOUT;
    }
}
