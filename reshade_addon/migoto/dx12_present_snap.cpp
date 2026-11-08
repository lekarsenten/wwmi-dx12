// See dx12_present_snap.h.
#include "dx12_present_snap.h"
#include "dx12_pipeline.h"
#include "dx12_wrap.h"
#include "../core/log.h"
#include <windows.h>
#include <algorithm>
#include <cstring>

namespace MigotoPort {

PresentSnapshots& g_present_snapshots = *new PresentSnapshots();

namespace {

ID3D11Resource* ResourceOf(void* view)
{
    WrapView* v = view ? AsWrapView(view) : nullptr;
    return v ? v->res : nullptr;
}

bool SameBuffer(const D3D11_BUFFER_DESC& a, const D3D11_BUFFER_DESC& b)
{
    return a.ByteWidth == b.ByteWidth && a.BindFlags == b.BindFlags && a.MiscFlags == b.MiscFlags &&
        a.StructureByteStride == b.StructureByteStride;
}

bool SameTexture(const D3D11_TEXTURE2D_DESC& a, const D3D11_TEXTURE2D_DESC& b)
{
    return a.Width == b.Width && a.Height == b.Height && a.MipLevels == b.MipLevels && a.ArraySize == b.ArraySize &&
        a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count && a.BindFlags == b.BindFlags &&
        a.MiscFlags == b.MiscFlags;
}

void Drop(ID3D11Resource*& res)
{
    if (res) res->Release();
    res = nullptr;
}

} // namespace

bool PresentSnapshots::Mine() const
{
    return mode_.load(std::memory_order_relaxed) != Mode::Off &&
        thread_.load(std::memory_order_relaxed) == GetCurrentThreadId();
}

void PresentSnapshots::Forget()
{
    for (auto& kv : copies_) {
        for (Copy& c : kv.second) {
            for (View& v : c.views)
                if (v.view) v.view->Release();
            Drop(c.res);
        }
    }
    copies_.clear();
    captured_.clear();
    rewritten_.clear();
}

void PresentSnapshots::Begin(Mode mode, unsigned generation)
{
    if (generation != generation_) {
        Forget();
        generation_ = generation;
    }
    thread_.store(GetCurrentThreadId(), std::memory_order_relaxed);
    mode_.store(mode, std::memory_order_relaxed);
    reads_.clear();
    swaps_.clear();
    if (mode == Mode::Replay) return;
    step_ = mode;
    read_.clear();
    captured_.clear();
}

void PresentSnapshots::Resume()
{
    thread_.store(GetCurrentThreadId(), std::memory_order_relaxed);
    mode_.store(step_, std::memory_order_relaxed);
}

void PresentSnapshots::End()
{
    mode_.store(Mode::Off, std::memory_order_relaxed);
}

void PresentSnapshots::NoteWrite(ID3D11Resource* res)
{
    if (!res || mode_.load(std::memory_order_relaxed) != Mode::Capture || !Mine()) return;
    if (read_.erase(res)) rewritten_.insert(res);
}

PresentSnapshots::Copy* PresentSnapshots::CopyFor(ID3D11Resource* src, unsigned nth)
{
    WrapResource* w = AsWrapResource(src);
    if (!w || (w->dim != D3D11_RESOURCE_DIMENSION_BUFFER && w->dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D))
        return nullptr;
    std::vector<Copy>& list = copies_[src];
    if (list.size() < nth) list.resize(nth);
    Copy& c = list[nth - 1];
    if (w->dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        D3D11_BUFFER_DESC d{};
        static_cast<WrapBuffer*>(w)->GetDesc(&d);
        if (c.res && SameBuffer(c.buffer, d)) return &c;
        for (View& v : c.views)
            if (v.view) v.view->Release();
        c.views.clear();
        Drop(c.res);
        c.buffer = d;
        d.Usage = D3D11_USAGE_DEFAULT;
        d.CPUAccessFlags = 0;
        ID3D11Buffer* b = nullptr;
        if (FAILED(PortDeviceAsD3D11()->CreateBuffer(&d, nullptr, &b)) || !b) return nullptr;
        c.res = b;
        return &c;
    }
    D3D11_TEXTURE2D_DESC d{};
    static_cast<WrapTexture2D*>(w)->GetDesc(&d);
    if (c.res && SameTexture(c.texture, d)) return &c;
    for (View& v : c.views)
        if (v.view) v.view->Release();
    c.views.clear();
    Drop(c.res);
    c.texture = d;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.CPUAccessFlags = 0;
    ID3D11Texture2D* t = nullptr;
    if (FAILED(PortDeviceAsD3D11()->CreateTexture2D(&d, nullptr, &t)) || !t) return nullptr;
    c.res = t;
    return &c;
}

ID3D11ShaderResourceView* PresentSnapshots::ViewOf(Copy& c, ID3D11ShaderResourceView* like)
{
    D3D11_SHADER_RESOURCE_VIEW_DESC d{};
    like->GetDesc(&d);
    for (View& v : c.views)
        if (!std::memcmp(&v.desc, &d, sizeof d)) return v.view;
    ID3D11ShaderResourceView* view = nullptr;
    if (FAILED(PortDeviceAsD3D11()->CreateShaderResourceView(c.res, &d, &view)) || !view) return nullptr;
    c.views.push_back({ d, view });
    return view;
}

void PresentSnapshots::BeforeDraw(ID3D11DeviceContext* ctx, PipeState& st)
{
    if (!Mine()) return;
    const Mode mode = mode_.load(std::memory_order_relaxed);
    const unsigned cs = (unsigned)StageIndex('c');
    std::vector<ID3D11Resource*> bound;
    for (unsigned s = 0; s < kStages; ++s) {
        if (s == cs) continue;
        for (unsigned r = 0; r < kMaxSRV; ++r) {
            ID3D11Resource* res = ResourceOf(st.srv[s][r]);
            if (res && std::find(bound.begin(), bound.end(), res) == bound.end()) bound.push_back(res);
        }
    }
    for (ID3D11Resource* res : bound) {
        if (mode == Mode::Capture) read_.insert(res);
        if (!rewritten_.count(res)) continue;
        const unsigned nth = ++reads_[res];
        if (mode == Mode::Capture) {
            Copy* c = CopyFor(res, nth);
            if (!c) continue;
            ctx->CopyResource(c->res, res);
            captured_[res] = nth;
#ifdef WWMI_DEBUG
            static bool s_told = false;
            if (!s_told) {
                s_told = true;
                logger::write("PRESENT-SNAP: a [Present] draw reads a resource the pass writes again after it -- "
                              "copied at each read, so the frame-generation redraw draws what this step drew");
            }
#endif
            continue;
        }
        const auto cap = captured_.find(res);
        if (cap == captured_.end() || nth > cap->second) continue;
        Copy& c = copies_[res][nth - 1];
        if (!c.res) continue;
        for (unsigned s = 0; s < kStages; ++s) {
            if (s == cs) continue;
            for (unsigned r = 0; r < kMaxSRV; ++r) {
                if (ResourceOf(st.srv[s][r]) != res) continue;
                ID3D11ShaderResourceView* v = ViewOf(c, st.srv[s][r]);
                if (!v) continue;
                swaps_.push_back({ s, r, st.srv[s][r] });
                st.srv[s][r] = v;
            }
        }
    }
}

void PresentSnapshots::AfterDraw(PipeState& st)
{
    if (swaps_.empty()) return;
    for (auto it = swaps_.rbegin(); it != swaps_.rend(); ++it) st.srv[it->stage][it->slot] = it->was;
    swaps_.clear();
}

} // namespace MigotoPort
