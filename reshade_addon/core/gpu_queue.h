#pragma once
#include <d3d12.h>

inline HRESULT CreateGpuQueue(ID3D12Device* dev, D3D12_COMMAND_LIST_TYPE type, const wchar_t* queue_name,
    const wchar_t* list_name, ID3D12CommandQueue** queue, ID3D12CommandAllocator** alloc,
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

inline DWORD WaitForFence(ID3D12Fence* fence, UINT64 value, HANDLE event, DWORD ms)
{
    if (fence->GetCompletedValue() >= value) return WAIT_OBJECT_0;
    if (!event) return WAIT_FAILED;
    fence->SetEventOnCompletion(value, event);
    return WaitForSingleObject(event, ms);
}
