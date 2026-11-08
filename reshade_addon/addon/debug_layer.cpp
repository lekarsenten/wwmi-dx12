#include "debug_layer.h"
#include "../core/log.h"
#include "../instrumentation/crash_handler.h"
#include <cstring>
#include <vector>

DebugLayer g_debug_layer;

void __stdcall DebugLayer::OnMessage(
    D3D12_MESSAGE_CATEGORY, D3D12_MESSAGE_SEVERITY sev, D3D12_MESSAGE_ID id, LPCSTR, void* self)
{
    if (sev > D3D12_MESSAGE_SEVERITY_ERROR) return;
    DebugLayer& dl = *static_cast<DebugLayer*>(self);
    unsigned k = 0;
    {
        std::lock_guard<std::mutex> l(dl.traced_mutex_);
        unsigned& seen = dl.traced_[(int)id];
        if (seen >= 3) return;
        k = ++seen;
    }
    char stack[3072];
    CrashHandler::FormatCallers(stack, sizeof(stack), 1);
    const wchar_t* ctx = CrashHandler::Context();
    logger::write("DBGCALLER id=%d #%u thread %lu%s%ls: %s", (int)id, k, GetCurrentThreadId(),
        ctx ? " in section " : "", ctx ? ctx : L"", stack);
}

void DebugLayer::Attach(ID3D12Device* dev)
{
    if (!dev || infoq_) return;
    if (SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&infoq_))) && infoq_) {
        infoq_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, FALSE);
        infoq_->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
        D3D12_MESSAGE_SEVERITY keep[] = { D3D12_MESSAGE_SEVERITY_CORRUPTION, D3D12_MESSAGE_SEVERITY_ERROR };
        D3D12_INFO_QUEUE_FILTER flt{};
        flt.AllowList.NumSeverities = _countof(keep);
        flt.AllowList.pSeverityList = keep;
        infoq_->PushStorageFilter(&flt);
        logger::write("DX12 DEBUG LAYER: InfoQueue attached -- validation errors (not warnings) will be "
                      "logged as DBGLAYER");
        if (SUCCEEDED(infoq_->QueryInterface(IID_PPV_ARGS(&infoq1_))) && infoq1_ &&
            SUCCEEDED(infoq1_->RegisterMessageCallback(OnMessage, D3D12_MESSAGE_CALLBACK_FLAG_NONE, this, &cookie_)))
            logger::write("DX12 DEBUG LAYER: the call stack of each error id is logged as DBGCALLER, "
                          "first 3 per id");
        else
            logger::write("DX12 DEBUG LAYER: no message callback on this layer -- errors carry no call "
                          "stack");
    } else {
        logger::write("DX12 DEBUG LAYER: no ID3D12InfoQueue on this device -- the layer is on but "
                      "nothing can be read back");
    }
}

void DebugLayer::Drain(const char* when)
{
    if (!infoq_) return;
    const UINT64 n = infoq_->GetNumStoredMessages();
    for (UINT64 i = 0; i < n; ++i) {
        SIZE_T len = 0;
        if (FAILED(infoq_->GetMessage(i, nullptr, &len)) || !len) continue;
        std::vector<char> buf(len);
        D3D12_MESSAGE* m = (D3D12_MESSAGE*)buf.data();
        if (FAILED(infoq_->GetMessage(i, m, &len))) continue;
        if (m->Severity > D3D12_MESSAGE_SEVERITY_ERROR) continue;
        const uint64_t k = ++seen_[(int)m->ID];
        const bool full = k <= 3 || strcmp(when, "frame") != 0;
        bool decade = false;
        for (uint64_t p = 10; p <= k; p *= 10)
            if (k == p) {
                decade = true;
                break;
            }
        if (full)
            logger::write("DBGLAYER [%s] sev=%d id=%d #%llu: %.*s", when, (int)m->Severity, (int)m->ID,
                (unsigned long long)k, (int)m->DescriptionByteLength, m->pDescription);
        else if (decade)
            logger::write(
                "DBGLAYER id=%d seen %llu times (only the first 3 are printed)", (int)m->ID, (unsigned long long)k);
    }
    if (n) infoq_->ClearStoredMessages();
}

void DebugLayer::Detach()
{
    if (infoq1_) {
        if (cookie_) infoq1_->UnregisterMessageCallback(cookie_);
        infoq1_->Release();
        infoq1_ = nullptr;
    }
    if (infoq_) {
        infoq_->Release();
        infoq_ = nullptr;
    } // never survive a probe unload
}
