// ===========================================================================
//  dx12_wrap.cpp -- OURS. PORT-LEAF. Registry + lifetime for the D3D11-shaped wrappers.
//
//  Their engine hands us back the same pointers it was given, typed as whatever D3D11 interface fits the
//  call. To act on one we must know WHICH of our wrappers it is. Reading a magic field out of the pointer
//  would be a wild read the one time the pointer is not ours, so every wrapper registers itself at
//  construction and is looked up. If a lookup ever misses, that is a real bug and it says so instead of
//  guessing.
// ===========================================================================
#include "dx12_wrap.h"
#include "dx12_pipeline.h"
#include "dx12_dump_readback.h"
#include "dx12_device.h"
#include "dx12_context.h"
#include "../core/log.h"

#include <algorithm>
#include <mutex>
#include <atomic>
#include <unordered_map>

namespace MigotoPort {

_Ret_notnull_ ID3D11Device1* PortDeviceAsD3D11()
{
    return Device();
}

namespace {
std::atomic<long> g_born{ 0 };
std::atomic<long> g_died{ 0 };
}
void WrapResourceBorn()
{
    ++g_born;
}
void WrapResourceDied()
{
    ++g_died;
}

WrapResource::~WrapResource()
{
    WrapResourceDied();
    if (dx12 && !foreign) DisownBuffer(dx12);
    if (dx12 && heap_type == D3D12_HEAP_TYPE_READBACK)
        RetireReadback(dx12);
    else if (dx12)
        RetireDx12Resource(dx12);
}

void WrapStats(long* created, long* destroyed, long* live)
{
    const long b = g_born.load();
    const long d = g_died.load();
    if (created) *created = b;
    if (destroyed) *destroyed = d;
    if (live) *live = b - d;
}

// Cached per DX12 resource. A swapchain cycles through its back buffers, so this settles at 2-3
// entries; rebuilding the wrapper every frame would re-create its descriptors for nothing. Leaked on
// purpose, like Map() below: their statics release wrappers during static teardown.
namespace {
std::mutex& BbMtx()
{
    static std::mutex* m = new std::mutex();
    return *m;
}
std::unordered_map<ID3D12Resource*, WrapTexture2D*>& BbCache()
{
    static auto* m = new std::unordered_map<ID3D12Resource*, WrapTexture2D*>();
    return *m;
}
std::vector<ID3D12Resource*>& BbSeen()
{
    static auto* v = new std::vector<ID3D12Resource*>();
    return *v;
}
}

ID3D11Texture2D* BackBufferAsD3D11()
{
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.back_buffer) return nullptr;

    std::lock_guard<std::mutex> l(BbMtx());
    auto* cache = &BbCache();
    auto it = cache->find(b.back_buffer);
    if (it == cache->end()) {
        const D3D12_RESOURCE_DESC rd = b.back_buffer->GetDesc();
        WrapTexture2D* w = new WrapTexture2D();
        b.back_buffer->AddRef(); // the wrapper holds a reference, as any COM owner would
        w->dx12 = b.back_buffer;
        w->state = b.back_buffer_state;
        w->desc.Width = (UINT)rd.Width;
        w->desc.Height = rd.Height;
        w->desc.MipLevels = rd.MipLevels;
        w->desc.ArraySize = rd.DepthOrArraySize;
        w->desc.Format = rd.Format;
        w->desc.SampleDesc = { rd.SampleDesc.Count, rd.SampleDesc.Quality };
        w->desc.Usage = D3D11_USAGE_DEFAULT;
        w->desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        it = cache->emplace(b.back_buffer, w).first;
        logger::write("PORT: back buffer wrapped -- %ux%u fmt=%d", (unsigned)rd.Width, rd.Height, (int)rd.Format);
    }
    it->second->AddRef();
    return it->second;
}

void BackBufferForget()
{
    std::lock_guard<std::mutex> l(BbMtx());
    auto* cache = &BbCache();
    for (auto& kv : *cache) {
        if (kv.second->dx12) {
            kv.second->dx12->Release();
            kv.second->dx12 = nullptr;
        } // the wrapper stays for the engine's refs; it draws nothing
    }
    if (!cache->empty())
        logger::write("PORT: back buffer wrappers released for the swap chain resize (%zu)", cache->size());
    cache->clear();
    BbSeen().clear();
}

void BackBufferPublished(ID3D12Resource* bb, D3D12_RESOURCE_STATES state)
{
    if (!bb) return;
    std::lock_guard<std::mutex> l(BbMtx());
    std::vector<ID3D12Resource*>& seen = BbSeen();
    if (std::find(seen.begin(), seen.end(), bb) == seen.end()) seen.push_back(bb);
    auto it = BbCache().find(bb);
    if (it != BbCache().end()) it->second->state = state;
}

bool IsBackBuffer(ID3D12Resource* res)
{
    if (!res) return false;
    std::lock_guard<std::mutex> l(BbMtx());
    const std::vector<ID3D12Resource*>& seen = BbSeen();
    return std::find(seen.begin(), seen.end(), res) != seen.end();
}

void BackBufferRestore(ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES to)
{
    const Dx12Bridge& b = GetDx12Bridge();
    if (!cl || !b.back_buffer) return;
    WrapTexture2D* w = nullptr;
    {
        std::lock_guard<std::mutex> l(BbMtx());
        auto it = BbCache().find(b.back_buffer);
        if (it != BbCache().end()) w = it->second;
    }
    if (w && w->state != to) TransitionResource(cl, w, to); // early-outs on state == to
}

namespace {

struct Entry {
    WrapResource* res = nullptr;
    WrapView* view = nullptr;
    ShaderCode* shader = nullptr;
    StateDesc* state = nullptr;
    InputLayoutDesc* layout = nullptr;
};
std::mutex& Mtx()
{
    static std::mutex* m = new std::mutex();
    return *m;
}
std::unordered_map<const void*, Entry>& Map()
{
    static auto* m = new std::unordered_map<const void*, Entry>();
    return *m;
}

template <class T> T* Lookup(void* p, T* Entry::* field)
{
    if (!p) return nullptr;
    std::lock_guard<std::mutex> l(Mtx());
    auto& m = Map();
    auto it = m.find(p);
    return (it == m.end()) ? nullptr : it->second.*field;
}

} // namespace

void RegisterWrap(void* iface, WrapResource* r)
{
    std::lock_guard<std::mutex> l(Mtx());
    Map()[iface].res = r;
}

void RegisterWrapView(void* iface, WrapView* v)
{
    std::lock_guard<std::mutex> l(Mtx());
    Map()[iface].view = v;
}

void RegisterShader(void* iface, ShaderCode* s)
{
    std::lock_guard<std::mutex> l(Mtx());
    Map()[iface].shader = s;
}

void RegisterState(void* iface, StateDesc* s)
{
    std::lock_guard<std::mutex> l(Mtx());
    Map()[iface].state = s;
}

void RegisterInputLayout(void* iface, InputLayoutDesc* l)
{
    std::lock_guard<std::mutex> l2(Mtx());
    Map()[iface].layout = l;
}

void UnregisterWrap(void* iface)
{
    std::lock_guard<std::mutex> l(Mtx());
    Map().erase(iface);
}

WrapResource* AsWrapResource(void* p)
{
    return Lookup(p, &Entry::res);
}
WrapView* AsWrapView(void* p)
{
    return Lookup(p, &Entry::view);
}
ShaderCode* AsShaderCode(void* p)
{
    return Lookup(p, &Entry::shader);
}
StateDesc* AsStateDesc(void* p)
{
    return Lookup(p, &Entry::state);
}
InputLayoutDesc* AsInputLayout(void* p)
{
    return Lookup(p, &Entry::layout);
}

WrapView::~WrapView()
{
    if (kind) ReleaseStagingDescriptor(kind, heap_slot);
    if (res) res->Release();
}

} // namespace MigotoPort
