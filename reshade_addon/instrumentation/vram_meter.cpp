#include "vram_meter.h"
#include "../addon/addon.h" // g_addon.d3d12_device
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>

VramMeter g_vram;

void VramMeter::Open()
{
    if (!g_addon.d3d12_device || for_ == g_addon.d3d12_device) return;
    if (adapter_) {
        if (event_) adapter_->UnregisterVideoMemoryBudgetChangeNotification(cookie_);
        adapter_->Release();
        adapter_ = nullptr;
    }
    for_ = g_addon.d3d12_device;
    IDXGIFactory4* fac = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) && fac) {
        IDXGIAdapter1* a1 = nullptr;
        if (SUCCEEDED(fac->EnumAdapterByLuid(g_addon.d3d12_device->GetAdapterLuid(), IID_PPV_ARGS(&a1))) && a1) {
            a1->QueryInterface(IID_PPV_ARGS(&adapter_));
            a1->Release();
        }
        fac->Release();
    }
    if (!adapter_) return;
    if (!event_) event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ && FAILED(adapter_->RegisterVideoMemoryBudgetChangeNotificationEvent(event_, &cookie_))) {
        CloseHandle(event_);
        event_ = nullptr;
    }
}

bool VramMeter::Read(VramReading* out)
{
    Open();
    if (!adapter_ || !out) return false;
    DXGI_QUERY_VIDEO_MEMORY_INFO vi{};
    if (FAILED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi))) return false;
    out->budget = vi.Budget;
    out->used = vi.CurrentUsage;
    if (SUCCEEDED(adapter_->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_NON_LOCAL, &vi))) {
        out->shared_budget = vi.Budget;
        out->shared_used = vi.CurrentUsage;
    }
    return true;
}

bool VramMeter::BudgetChanged()
{
    return event_ && WaitForSingleObject(event_, 0) == WAIT_OBJECT_0;
}
