#pragma once
// ===========================================================================
//  dx12_wrap.h -- OURS. PORT-LEAF. D3D11 objects that are really DX12 objects.
//
//  WHY THIS EXISTS, and why a `reinterpret_cast<ID3D11Buffer*>(dx12_resource)` is wrong:
//  their engine does not treat these as opaque handles. It calls D3D11 methods on them --
//  _CreateCompatibleView does `resource->GetType(&dimension)`, FillOutBufferDesc does `buf->GetDesc()`,
//  the copy leaves do `view->GetResource()`. ID3D11Resource::GetType is vtable slot 7, which on
//  ID3D12Resource is GetDevice(REFIID, void**) -- so the cast calls the wrong function with the wrong
//  arguments.
//
//  These wrappers answer in D3D11 terms (GetType/GetDesc/GetResource) while holding the DX12 object as
//  the truth. That is what lets their 22,000 inherited lines run unmodified: the D3D11 vocabulary is
//  real, only the backing object changed.
//
//  Lifetime is COM, theirs to manage: they created it, their Release destroys it. The registry in
//  dx12_wrap.cpp is not a second owner: it holds no reference, and a wrapper unregisters itself as it dies.
// ===========================================================================

#include <d3d11_1.h>
#include <d3d12.h>
#include <vector>
#include <string>
#include <cstdint>
#include "resource_memory.h"

namespace MigotoPort {

_Ret_notnull_ ID3D11Device1* PortDeviceAsD3D11();

ID3D11Texture2D* BackBufferAsD3D11();

void BackBufferPublished(ID3D12Resource* bb, D3D12_RESOURCE_STATES state);

void BackBufferForget();

void BackBufferRestore(ID3D12GraphicsCommandList* cl, D3D12_RESOURCE_STATES to);

template <class I>
class WrapBase : public I {
public:
    ULONG STDMETHODCALLTYPE AddRef() override { return (ULONG)InterlockedIncrement(&rc_); }
    LONG                    RefCount() const { return rc_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        LONG n = InterlockedDecrement(&rc_);
        if (n <= 0) {
            delete this;
            return 0;
        }
        return (ULONG)n;
    }
    void STDMETHODCALLTYPE GetDevice(ID3D11Device** pp) override
    {
        if (!pp) return;
        *pp = PortDeviceAsD3D11();
        (*pp)->AddRef();
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID, UINT* pDataSize, void*) override
    {
        if (pDataSize) *pDataSize = 0;
        return DXGI_ERROR_NOT_FOUND;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID, UINT, const void*) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID, const IUnknown*) override { return S_OK; }

protected:
    virtual ~WrapBase() = default;
    HRESULT QiFrom(REFIID riid, void** ppv, const IID* const* accepted, size_t n)
    {
        if (!ppv) return E_POINTER;
        for (size_t i = 0; i < n; ++i) {
            if (riid == *accepted[i]) {
                *ppv = this;
                AddRef();
                return S_OK;
            }
        }
        if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11DeviceChild)) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    LONG rc_ = 1;
};

void WrapResourceBorn();
void WrapResourceDied();
void RetireDx12Resource(ID3D12Resource* res, ResourceAllocation memory = {});
void RetireReadback(ID3D12Resource* res, ResourceAllocation memory = {});
void DisownBuffer(ID3D12Resource* res);

struct UploadRecord {
    uint64_t                   seq = 0;
    ID3D12GraphicsCommandList* list = nullptr;
    uint64_t                   recording = 0;
};

struct WrapResource {
    WrapResource() { WrapResourceBorn(); }
    ID3D12Resource*                    dx12 = nullptr;
    D3D11_RESOURCE_DIMENSION           dim = D3D11_RESOURCE_DIMENSION_UNKNOWN;
    D3D12_GPU_VIRTUAL_ADDRESS          gpu_va = 0;
    D3D12_RESOURCE_STATES              state = D3D12_RESOURCE_STATE_COMMON;
    bool                               foreign = false;
    D3D12_HEAP_TYPE                    heap_type = (D3D12_HEAP_TYPE)0;
    std::vector<unsigned char>         shadow;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    UINT                               row_pitch = 0; // 0 = not a staging texture
    UINT                               rows = 0;
    std::vector<unsigned char>         uploaded;
    uint64_t                           uploaded_seq = 0;
    uint64_t                           queued_seq = 0;
    UploadRecord                       flushed;
    ResourceAllocation                memory;
    void TrackMemory(ID3D12Device* device)
    {
        const auto desc = dx12->GetDesc();
        memory = ResourceMemory().Created(
            device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes,
            desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER);
    }
    virtual ~WrapResource()
    {
        WrapResourceDied();
        if (dx12 && !foreign) DisownBuffer(dx12);
        if (dx12) ResourceMemory().Retired(memory);
        if (dx12 && heap_type == D3D12_HEAP_TYPE_READBACK)
            RetireReadback(dx12, memory);
        else if (dx12)
            RetireDx12Resource(dx12, memory);
    }
};

void WrapStats(long* created, long* destroyed, long* live);

WrapResource* AsWrapResource(void* p);
struct WrapView;
WrapView* AsWrapView(void* p);
void      RegisterWrap(void* iface, WrapResource* r);
void      RegisterWrapView(void* iface, WrapView* v);
void      UnregisterWrap(void* iface);

class WrapBuffer final : public WrapBase<ID3D11Buffer>, public WrapResource {
public:
    D3D11_BUFFER_DESC desc{};
    WrapBuffer()
    {
        dim = D3D11_RESOURCE_DIMENSION_BUFFER;
        RegisterWrap((ID3D11Buffer*)this, this);
    }
    ~WrapBuffer() override { UnregisterWrap((ID3D11Buffer*)this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        static const IID* const ok[] = { &__uuidof(ID3D11Buffer), &__uuidof(ID3D11Resource) };
        return QiFrom(riid, ppv, ok, 2);
    }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* p) override
    {
        if (p) *p = D3D11_RESOURCE_DIMENSION_BUFFER;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_BUFFER_DESC* p) override
    {
        if (p) *p = desc;
    }
};

class WrapTexture1D final : public WrapBase<ID3D11Texture1D>, public WrapResource {
public:
    D3D11_TEXTURE1D_DESC desc{};
    WrapTexture1D()
    {
        dim = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
        RegisterWrap((ID3D11Texture1D*)this, this);
    }
    ~WrapTexture1D() override { UnregisterWrap((ID3D11Texture1D*)this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        static const IID* const ok[] = { &__uuidof(ID3D11Texture1D), &__uuidof(ID3D11Resource) };
        return QiFrom(riid, ppv, ok, 2);
    }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* p) override
    {
        if (p) *p = D3D11_RESOURCE_DIMENSION_TEXTURE1D;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE1D_DESC* p) override
    {
        if (p) *p = desc;
    }
};

class WrapTexture2D final : public WrapBase<ID3D11Texture2D>, public WrapResource {
public:
    D3D11_TEXTURE2D_DESC desc{};
    WrapTexture2D()
    {
        dim = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
        RegisterWrap((ID3D11Texture2D*)this, this);
    }
    ~WrapTexture2D() override { UnregisterWrap((ID3D11Texture2D*)this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        static const IID* const ok[] = { &__uuidof(ID3D11Texture2D), &__uuidof(ID3D11Resource) };
        return QiFrom(riid, ppv, ok, 2);
    }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* p) override
    {
        if (p) *p = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE2D_DESC* p) override
    {
        if (p) *p = desc;
    }
};

class WrapTexture3D final : public WrapBase<ID3D11Texture3D>, public WrapResource {
public:
    D3D11_TEXTURE3D_DESC desc{};
    WrapTexture3D()
    {
        dim = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
        RegisterWrap((ID3D11Texture3D*)this, this);
    }
    ~WrapTexture3D() override { UnregisterWrap((ID3D11Texture3D*)this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        static const IID* const ok[] = { &__uuidof(ID3D11Texture3D), &__uuidof(ID3D11Resource) };
        return QiFrom(riid, ppv, ok, 2);
    }
    void STDMETHODCALLTYPE GetType(D3D11_RESOURCE_DIMENSION* p) override
    {
        if (p) *p = D3D11_RESOURCE_DIMENSION_TEXTURE3D;
    }
    void STDMETHODCALLTYPE SetEvictionPriority(UINT) override {}
    UINT STDMETHODCALLTYPE GetEvictionPriority() override { return 0; }
    void STDMETHODCALLTYPE GetDesc(D3D11_TEXTURE3D_DESC* p) override
    {
        if (p) *p = desc;
    }
};

struct WrapView {
    ID3D11Resource*             res = nullptr;  // the wrapper it views; we hold a ref
    WrapResource*               wres = nullptr; // same object, our side of it
    char                        kind = 0;       // t=SRV u=UAV r=RTV d=DSV
    UINT                        heap_slot = 0;  // index into our CPU-visible staging heap
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    virtual ~WrapView();
};

template <class I, class D, char KIND>
class WrapViewT final : public WrapBase<I>, public WrapView {
public:
    D desc{};
    WrapViewT()
    {
        this->kind = KIND;
        RegisterWrapView((I*)this, this);
    }
    ~WrapViewT() override { UnregisterWrap((I*)this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        static const IID* const ok[] = { &__uuidof(I), &__uuidof(ID3D11View) };
        return this->QiFrom(riid, ppv, ok, 2);
    }
    void STDMETHODCALLTYPE GetResource(ID3D11Resource** pp) override
    {
        if (pp) {
            *pp = this->res;
            if (this->res) this->res->AddRef();
        }
    }
    void STDMETHODCALLTYPE GetDesc(D* p) override
    {
        if (p) *p = desc;
    }
};

using WrapSRV = WrapViewT<ID3D11ShaderResourceView, D3D11_SHADER_RESOURCE_VIEW_DESC, 't'>;
using WrapUAV = WrapViewT<ID3D11UnorderedAccessView, D3D11_UNORDERED_ACCESS_VIEW_DESC, 'u'>;
using WrapRTV = WrapViewT<ID3D11RenderTargetView, D3D11_RENDER_TARGET_VIEW_DESC, 'r'>;
using WrapDSV = WrapViewT<ID3D11DepthStencilView, D3D11_DEPTH_STENCIL_VIEW_DESC, 'd'>;

struct ShaderReg {
    char          kind; // b t u s
    UINT          reg;
    UINT          space;
    unsigned char sit = 0; // D3D_SHADER_INPUT_TYPE  (0 = not recorded)
    unsigned char dim = 0; // D3D_SRV_DIMENSION      (0 = UNKNOWN)
};

struct ShaderCode {
    char                   stage = 0; // v h d g p c
    std::vector<uint8_t>   code;
    std::vector<ShaderReg> regs;
    uint32_t               cb_size[16] = {};
    bool                   reflected = false;
    uint64_t               content_hash = 0; // of `code`, taken at the first PSO lookup; the PSO cache key
    virtual ~ShaderCode() = default;
};
ShaderCode* AsShaderCode(void* p);
void        RegisterShader(void* iface, ShaderCode* s);

template <class I, char STAGE>
class WrapShader final : public WrapBase<I>, public ShaderCode {
public:
    WrapShader()
    {
        this->stage = STAGE;
        RegisterShader((I*)this, this);
    }
    ~WrapShader() override { UnregisterWrap((I*)this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        static const IID* const ok[] = { &__uuidof(I) };
        return this->QiFrom(riid, ppv, ok, 1);
    }
};

using WrapVS = WrapShader<ID3D11VertexShader, 'v'>;
using WrapHS = WrapShader<ID3D11HullShader, 'h'>;
using WrapDS = WrapShader<ID3D11DomainShader, 'd'>;
using WrapGS = WrapShader<ID3D11GeometryShader, 'g'>;
using WrapPS = WrapShader<ID3D11PixelShader, 'p'>;
using WrapCS = WrapShader<ID3D11ComputeShader, 'c'>;

struct StateDesc {
    char                     kind = 0; // B=blend R=raster D=depth S=sampler
    D3D11_BLEND_DESC         blend{};
    D3D11_RASTERIZER_DESC    raster{};
    D3D11_DEPTH_STENCIL_DESC depth{};
    D3D11_SAMPLER_DESC       sampler{};
    uint64_t                 content_hash = 0; // of the desc `kind` names, taken at the first PSO lookup
    virtual ~StateDesc() = default;
};
StateDesc* AsStateDesc(void* p);
void       RegisterState(void* iface, StateDesc* s);

template <class I, class D, char KIND, D StateDesc::* FIELD>
class WrapState final : public WrapBase<I>, public StateDesc {
public:
    WrapState()
    {
        this->kind = KIND;
        RegisterState((I*)this, this);
    }
    ~WrapState() override { UnregisterWrap((I*)this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        static const IID* const ok[] = { &__uuidof(I) };
        return this->QiFrom(riid, ppv, ok, 1);
    }
    void STDMETHODCALLTYPE GetDesc(D* p) override
    {
        if (p) *p = this->*FIELD;
    }
};

using WrapBlend = WrapState<ID3D11BlendState, D3D11_BLEND_DESC, 'B', &StateDesc::blend>;
using WrapRaster = WrapState<ID3D11RasterizerState, D3D11_RASTERIZER_DESC, 'R', &StateDesc::raster>;
using WrapDepth = WrapState<ID3D11DepthStencilState, D3D11_DEPTH_STENCIL_DESC, 'D', &StateDesc::depth>;
using WrapSampler = WrapState<ID3D11SamplerState, D3D11_SAMPLER_DESC, 'S', &StateDesc::sampler>;

struct InputLayoutDesc {
    std::vector<D3D12_INPUT_ELEMENT_DESC> elems;
    std::vector<std::string>              names;            // owns the strings elems point at
    uint64_t                              content_hash = 0; // of elems and names, taken at the first PSO lookup
    virtual ~InputLayoutDesc() = default;
};
InputLayoutDesc* AsInputLayout(void* p);
void             RegisterInputLayout(void* iface, InputLayoutDesc* l);

class WrapInputLayout final : public WrapBase<ID3D11InputLayout>, public InputLayoutDesc {
public:
    WrapInputLayout() { RegisterInputLayout((ID3D11InputLayout*)this, this); }
    ~WrapInputLayout() override { UnregisterWrap((ID3D11InputLayout*)this); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override
    {
        static const IID* const ok[] = { &__uuidof(ID3D11InputLayout) };
        return QiFrom(riid, ppv, ok, 1);
    }
};

} // namespace MigotoPort
