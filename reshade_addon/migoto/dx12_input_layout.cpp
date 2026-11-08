#include "dx12_input_layout.h"
#include "dx12_wrap.h"
#include "HackerInputLayout.h"
#include <wrl/client.h> // Microsoft::WRL::ComPtr, for the layout blob

EXTERN_C const GUID InputLayoutDescGuid;
#include <d3dcompiler.h>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <string>
#include <vector>
#include <cstdio> // _snprintf_s for DescribeInputLayout (X20)
#include <cstddef>

namespace MigotoPort {

namespace {

class LayoutStandIn final : public ::ID3D11InputLayout {
public:
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    std::vector<D3D11_INPUT_ELEMENT_DESC> elems;

    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&rc_); }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&rc_);
        if (n <= 0) {
            delete this;
            return 0;
        }
        return (ULONG)n;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        if (!ppv) return E_POINTER;
        if (riid == __uuidof(::ID3D11InputLayout) || riid == __uuidof(ID3D11DeviceChild) ||
            riid == __uuidof(IUnknown)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** pp) override
    {
        if (!pp) return;
        *pp = PortDeviceAsD3D11();
        (*pp)->AddRef();
    }

    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID guid, UINT* pDataSize, void* pData) override
    {
        if (!pDataSize) return E_INVALIDARG;
        if (guid == InputLayoutDescGuid && blob) {
            if (!pData || *pDataSize < sizeof(ID3DBlob*)) {
                *pDataSize = sizeof(ID3DBlob*);
                return S_OK;
            }
            ID3DBlob* b = blob.Get();
            b->AddRef();
            *(ID3DBlob**)pData = b;
            *pDataSize = sizeof(ID3DBlob*);
            return S_OK;
        }
        *pDataSize = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

    void Seal()
    {
        const SIZE_T bytes = elems.size() * sizeof(D3D11_INPUT_ELEMENT_DESC);
        if (SUCCEEDED(D3DCreateBlob(bytes, &blob)) && blob) memcpy(blob->GetBufferPointer(), elems.data(), bytes);
    }

private:
    LONG rc_ = 1;
};

struct PipelineLayout {
    LayoutStandIn* layout = nullptr;
    HackerInputLayout* hacker = nullptr;

    void Release()
    {
        if (hacker) hacker->Release();
        if (layout) layout->Release();
    }
};

static_assert(
    sizeof(D3D11_INPUT_ELEMENT_DESC) == offsetof(D3D11_INPUT_ELEMENT_DESC, InstanceDataStepRate) + sizeof(UINT),
    "the custom layout key copies the fields after SemanticName as bytes");

std::unordered_map<unsigned long long, PipelineLayout> g_layouts;
std::unordered_map<std::string, HackerInputLayout*> g_custom_layouts;
std::unordered_set<std::string> g_semantics;
std::mutex g_layout_mtx;
LayoutStandIn* g_current = nullptr;

} // namespace

void RecordInputLayout(unsigned long long pso, const InputElement* in, unsigned count)
{
    if (!pso || !in || !count) return;

    auto* L = new LayoutStandIn();
    L->elems.reserve(count);
    std::lock_guard<std::mutex> l(g_layout_mtx);
    for (unsigned i = 0; i < count; ++i) {
        D3D11_INPUT_ELEMENT_DESC d{};
        d.SemanticName = g_semantics.emplace(in[i].semantic ? in[i].semantic : "").first->c_str();
        d.SemanticIndex = in[i].semantic_index;
        d.Format = (DXGI_FORMAT)in[i].format;
        d.InputSlot = in[i].buffer_binding;
        d.AlignedByteOffset = in[i].offset;
        d.InputSlotClass = in[i].instance_step_rate ? D3D11_INPUT_PER_INSTANCE_DATA : D3D11_INPUT_PER_VERTEX_DATA;
        d.InstanceDataStepRate = in[i].instance_step_rate;
        L->elems.push_back(d);
    }
    L->Seal();

    auto it = g_layouts.find(pso);
    if (it != g_layouts.end()) { // PSO handles are reused after destruction
        if (g_current == it->second.layout) g_current = nullptr;
        it->second.Release();
    }
    g_layouts[pso] = { L, nullptr };
}

void SetCurrentInputLayout(unsigned long long pso)
{
    std::lock_guard<std::mutex> l(g_layout_mtx);
    auto it = g_layouts.find(pso);
    g_current = (it != g_layouts.end()) ? it->second.layout : nullptr;
}

::ID3D11InputLayout* CurrentInputLayout()
{
    std::lock_guard<std::mutex> l(g_layout_mtx);
    if (!g_current) return nullptr;
    g_current->AddRef(); // IAGetInputLayout AddRefs; their DumpVBs Releases it at the end
    return g_current;
}

unsigned DescribeInputLayout(unsigned long long pso, char* buf, unsigned n)
{
    if (!buf || !n) return 0;
    buf[0] = 0;
    std::lock_guard<std::mutex> l(g_layout_mtx);
    auto it = g_layouts.find(pso);
    if (it == g_layouts.end() || !it->second.layout) return 0;
    const auto& el = it->second.layout->elems;
    unsigned off = 0;
    for (size_t i = 0; i < el.size() && off + 64 < n; ++i) {
        const int w = _snprintf_s(buf + off, n - off, _TRUNCATE, "%s%s%u@s%u+%u:f%u%s", i ? " " : "",
            el[i].SemanticName ? el[i].SemanticName : "?", el[i].SemanticIndex, el[i].InputSlot,
            el[i].AlignedByteOffset, (unsigned)el[i].Format,
            el[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA ? "/inst" : "");
        if (w < 0) break;
        off += (unsigned)w;
    }
    return (unsigned)el.size();
}

HackerInputLayout* HackerInputLayoutOf(unsigned long long pso)
{
    std::lock_guard<std::mutex> l(g_layout_mtx);
    auto it = g_layouts.find(pso);
    if (!pso || it == g_layouts.end() || !it->second.layout || it->second.layout->elems.empty()) return nullptr;
    PipelineLayout& P = it->second;
    if (!P.hacker) {
        P.layout->AddRef();
        P.hacker = new HackerInputLayout(P.layout, P.layout->elems.data(), (UINT)P.layout->elems.size(), nullptr, 0);
    }
    P.hacker->AddRef();
    return P.hacker;
}

HackerInputLayout* NewHackerInputLayout(const D3D11_INPUT_ELEMENT_DESC* elements, unsigned count)
{
    if (!elements || !count) return nullptr;
    thread_local std::string key;
    key.clear();
    for (unsigned i = 0; i < count; ++i) {
        const D3D11_INPUT_ELEMENT_DESC& d = elements[i];
        key += d.SemanticName ? d.SemanticName : "";
        key += '\0';
        key.append(reinterpret_cast<const char*>(&d.SemanticIndex),
            sizeof(d) - offsetof(D3D11_INPUT_ELEMENT_DESC, SemanticIndex));
    }
    std::lock_guard<std::mutex> l(g_layout_mtx);
    HackerInputLayout*& H = g_custom_layouts[key];
    if (!H) {
        auto* L = new LayoutStandIn();
        L->elems.assign(elements, elements + count);
        for (D3D11_INPUT_ELEMENT_DESC& d : L->elems)
            d.SemanticName = g_semantics.emplace(d.SemanticName ? d.SemanticName : "").first->c_str();
        L->Seal();
        H = new HackerInputLayout(L, elements, count, nullptr, 0);
    }
    H->AddRef();
    return H;
}

} // namespace MigotoPort
