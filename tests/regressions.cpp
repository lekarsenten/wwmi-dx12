#include "../src/gpu_queue.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <cstdio>
#include <stdexcept>
#include <thread>

using Microsoft::WRL::ComPtr;

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

struct Event {
    HANDLE handle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    Event() { Check(handle != nullptr, "CreateEventW"); }
    ~Event() { CloseHandle(handle); }
    Event(const Event&) = delete;
    Event& operator=(const Event&) = delete;
};

// Fault injection for outcomes a healthy WARP device cannot produce on demand.
struct Fence : ID3D12Fence {
    UINT64 completed = 0;
    HRESULT registration = S_OK;
    bool remove_on_registration = false;
    unsigned registrations = 0;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void**) override { return E_NOINTERFACE; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT*, void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID, void**) override { return E_NOTIMPL; }
    UINT64 STDMETHODCALLTYPE GetCompletedValue() override { return completed; }
    HRESULT STDMETHODCALLTYPE SetEventOnCompletion(UINT64, HANDLE event) override
    {
        ++registrations;
        if (remove_on_registration) {
            completed = UINT64_MAX;
            SetEvent(event);
        }
        return registration;
    }
    HRESULT STDMETHODCALLTYPE Signal(UINT64 value) override { completed = value; return S_OK; }
};

static void FenceFailures()
{
    Event event;
    Fence fence;
    Check(WaitForFence(nullptr, 1, event.handle, 0) == WAIT_FAILED, "null fence");
    Check(WaitForFence(&fence, 1, nullptr, 0) == WAIT_FAILED, "missing event");
    fence.completed = 12;
    Check(WaitForFence(&fence, 12, nullptr, 0) == WAIT_OBJECT_0, "completed fence fast path");
    Check(fence.registrations == 0, "completed fence must not register an event");
    fence.completed = UINT64_MAX;
    Check(WaitForFence(&fence, 12, event.handle, 0) == WAIT_FAILED, "removed device before wait");
    fence.completed = 11;
    fence.registration = E_OUTOFMEMORY;
    SetEvent(event.handle);
    Check(WaitForFence(&fence, 12, event.handle, 0) == WAIT_FAILED, "registration failure with signalled event");
    ResetEvent(event.handle);
    fence.registration = S_OK;
    Check(WaitForFence(&fence, 12, event.handle, 1) == WAIT_TIMEOUT, "unfinished fence timeout");
    SetEvent(event.handle);
    Check(WaitForFence(&fence, 12, event.handle, 1) == WAIT_TIMEOUT, "stale event is not fence completion");
    fence.remove_on_registration = true;
    Check(WaitForFence(&fence, 12, event.handle, 1000) == WAIT_FAILED, "removed device after wake");
}

static void WarpFence()
{
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
    Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
    Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(SUCCEEDED(CreateGpuQueue(device.Get(), D3D12_COMMAND_LIST_TYPE_DIRECT,
        L"WWMI regression queue", L"WWMI regression list", &queue, &allocator, &list)), "GPU queue");
    ComPtr<ID3D12Fence> fence;
    Check(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))), "GPU fence");
    Event event;
    for (UINT64 frame = 1; frame <= 6; ++frame) {
        Check(WaitForFence(fence.Get(), frame - 1, event.handle, 5000) == WAIT_OBJECT_0, "allocator completion");
        Check(SUCCEEDED(allocator->Reset()), "allocator reset after completion");
        Check(SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "command list reset");
        Check(SUCCEEDED(list->Close()), "command list close");
        ID3D12CommandList* lists[] = { list.Get() };
        queue->ExecuteCommandLists(1, lists);
        Check(SUCCEEDED(queue->Signal(fence.Get(), frame)), "queue signal");
    }
    Check(WaitForFence(fence.Get(), 6, event.handle, 5000) == WAIT_OBJECT_0, "final submission completion");
    Check(WaitForFence(fence.Get(), 7, event.handle, 1) == WAIT_TIMEOUT, "real fence timeout");
    Check(SUCCEEDED(fence->Signal(7)), "late signal of timed-out registration");
    Check(WaitForFence(fence.Get(), 8, event.handle, 1) == WAIT_TIMEOUT, "real stale registration");
    std::atomic<HRESULT> signal_result{ E_FAIL };
    std::thread completion([&] {
        Sleep(20);
        signal_result.store(fence->Signal(8));
    });
    const DWORD result = WaitForFence(fence.Get(), 8, event.handle, 5000);
    completion.join();
    Check(SUCCEEDED(signal_result.load()) && result == WAIT_OBJECT_0, "asynchronous real fence completion");
    Check(WaitForFence(fence.Get(), 8, event.handle, INFINITE) == WAIT_OBJECT_0, "infinite completed wait");
}

int main()
{
    try {
        FenceFailures();
        WarpFence();
        std::puts("PASS: fence failures, D3D12 WARP submission and event reuse");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
