// occlusion_probe.cpp -- the G12 occlusion probe (see occlusion_probe.h).
#include "../addon_common.h"
#include "occlusion_probe.h"
#include "removal_pin.h"
#include "../core/d3d12_buffer.h"
#include "diag_arms.h"

OcclusionProbe g_occl;

bool OcclusionProbe::Ensure(ID3D12Device* dev)
{
    if (heap_ && rb_) return true;
    if (!dev) return false;
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
    qd.Count = kSlots;
    if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&heap_)))) return false;
    if (FAILED(CreateD3D12Buffer(
            dev, D3D12_HEAP_TYPE_READBACK, kSlots * sizeof(UINT64), D3D12_RESOURCE_STATE_COPY_DEST, &rb_))) {
        heap_.Reset();
        return false;
    }
    return true;
}

// Claim a slot for one injected draw. Returns -1 when not collecting or out of slots.
int OcclusionProbe::Begin(
    ID3D12GraphicsCommandList* cl, ID3D12Device* dev, int comp, UINT index_count, UINT rtv_count, bool has_dsv)
{
    if (armed_frames_.load() <= 0 || !cl || !Ensure(dev)) return -1;
    const UINT s = next_slot_.fetch_add(1);
    if (s >= kSlots) return -1;
    {
        std::lock_guard<std::mutex> l(mtx_);
        slot_comp_[s] = comp;
        slot_count_[s] = index_count;
        slot_rtv_[s] = rtv_count;
        slot_dsv_[s] = has_dsv;
    }
    cl->BeginQuery(heap_.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, s);
    RemovalPin::Ours(cl, "occl-begin"); // begin and end tallied apart: a query left open fails Close
    return (int)s;
}

void OcclusionProbe::End(ID3D12GraphicsCommandList* cl, int slot)
{
    if (slot < 0 || !cl || !heap_) return;
    cl->EndQuery(heap_.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, (UINT)slot);
    RemovalPin::Ours(cl, "occl-end");
    cl->ResolveQueryData(
        heap_.Get(), D3D12_QUERY_TYPE_BINARY_OCCLUSION, (UINT)slot, 1, rb_.Get(), (UINT64)slot * sizeof(UINT64));
}

void OcclusionProbe::PresentTick()
{
    if (const int arm = g_diag_arms.occl_arm.exchange(0)) { // overlay button pressed
        next_slot_.store(0);
        armed_frames_.store(arm);
    }
    int a = armed_frames_.load();
    if (a > 0) {
        armed_frames_.store(a - 1);
        if (a == 1) read_countdown_ = 4; // window closed: let the GPU drain
    } else if (read_countdown_ > 0 && --read_countdown_ == 0 && rb_) {
        const UINT used = std::min<UINT>(next_slot_.load(), kSlots);
        void* p = nullptr;
        D3D12_RANGE rr{ 0, used * sizeof(UINT64) };
        if (used && SUCCEEDED(rb_->Map(0, &rr, &p)) && p) {
            const UINT64* v = (const UINT64*)p;
            UINT drew = 0;
            for (UINT i = 0; i < used; ++i) {
                int comp;
                UINT cnt;
                UINT nrtv;
                bool dsv;
                {
                    std::lock_guard<std::mutex> l(mtx_);
                    comp = slot_comp_[i];
                    cnt = slot_count_[i];
                    nrtv = slot_rtv_[i];
                    dsv = slot_dsv_[i];
                }
                if (v[i]) ++drew;
                logger::write("OCCL comp=%d indices=%u rtv=%u dsv=%d pass=%-6s -> %s", comp, cnt, nrtv, (int)dsv,
                    (nrtv == 0 && dsv) ? "DEPTH" : (nrtv ? "COLOR" : "?"),
                    v[i] ? "RASTERISED (fragments passed)" : "NO FRAGMENTS");
            }
            D3D12_RANGE none{ 0, 0 };
            rb_->Unmap(0, &none);
            logger::write("OCCL SUMMARY: %u/%u injected draws produced fragments -- %s", drew, used,
                drew ? "geometry rasterises; invisibility is MATERIAL/TEXTURE"
                     : "geometry never rasterises; the fault is in the DRAW, not textures");
        } else {
            logger::write("OCCL: nothing collected (used=%u) -- were any injected draws issued?", used);
        }
        next_slot_.store(0);
    }
}
