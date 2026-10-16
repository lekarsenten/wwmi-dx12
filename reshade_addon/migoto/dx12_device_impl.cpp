// ===========================================================================
//  dx12_device_impl -- OURS. PORT-LEAF. Device-side DX11->DX12 transitions, by hand.
//
//  dx12_device.cpp is generated; this file holds the Create* calls that actually do something, so
//  regenerating never overwrites real work. See docs/PORT-DX11-TO-DX12-TRANSITIONS.md.
//
//  Everything here returns a WRAPPER (dx12_wrap.h), never a cast DX12 pointer. Their engine calls D3D11
//  methods on what it is given -- GetType, GetDesc, GetResource -- and a cast lands on whatever DX12 put
//  at that vtable slot instead.
// ===========================================================================
#include "dx12_device.h"
#include "../instrumentation/tex_timing.h"
#include "dx12_context.h"
#include "dx12_wrap.h"
#include "dx12_pipeline.h"
#include "port_toggles.h" // g_buffers_in_vram
#include "prof_core.h" // ProfGpuWork -- views created, for the performance report's work rows
#include "../../src/log.h"
#include "../../src/d3d12_buffer.h"
#include "../../src/dxgi_bc.h"
#include "../../src/buffer_view_fit.h"

namespace MigotoPort {

// ---------------------------------------------------------------------------
//  Resources
// ---------------------------------------------------------------------------
namespace {

// PORT_NO_TEX=1 fails texture creation while buffers still succeed.
static bool NoTextures()
{
    static const bool v = [] {
        char   buf[8] = {};
        size_t n = 0;
        return getenv_s(&n, buf, sizeof(buf), "PORT_NO_TEX") == 0 && n && buf[0] == '1';
    }();
    return v;
}

ID3D12Resource* CreateBufferDx12(UINT64 bytes, bool allow_uav, D3D12_RESOURCE_STATES& state,
    bool upload)
{
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device || !bytes) return nullptr;

    const D3D12_HEAP_TYPE      heap = upload ? D3D12_HEAP_TYPE_UPLOAD : D3D12_HEAP_TYPE_DEFAULT;
    const UINT64               width = (bytes + 255) & ~(UINT64)255; // 256-align, DX12's buffer placement rule
    const D3D12_RESOURCE_FLAGS flags = (allow_uav && !upload) ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS
                                                              : D3D12_RESOURCE_FLAG_NONE;

    state = upload ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;

    ID3D12Resource* res = nullptr;
    if (FAILED(CreateD3D12Buffer(b.device, heap, width, state, &res, flags)))
        return nullptr;
    DevName(res, L"wwmi mod buffer");
    if (!upload) OwnBuffer(res);
    return res;
}

static inline void PortNormalizeBufferDesc(D3D11_BUFFER_DESC& d)
{
    if (!(d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED))
        d.StructureByteStride = 0;
}

} // namespace

HRESULT STDMETHODCALLTYPE Dx12Device::CreateBuffer(const D3D11_BUFFER_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA*                                           pInitialData,
    _COM_Outptr_opt_ ID3D11Buffer**                                         ppBuffer)
{
    if (ppBuffer) *ppBuffer = nullptr;
    if (!pDesc) return E_INVALIDARG;
    const bool uav = (pDesc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) != 0;

    if (pDesc->Usage == D3D11_USAGE_STAGING && (pDesc->CPUAccessFlags & D3D11_CPU_ACCESS_READ)) {
        const Dx12Bridge& b = GetDx12Bridge();
        if (!b.device) {
            NotImplOnce("Device::CreateBuffer STAGING (no DX12 device bound)");
            return E_FAIL;
        }
        ID3D12Resource* sres = nullptr;
        if (FAILED(CreateD3D12Buffer(b.device, D3D12_HEAP_TYPE_READBACK, (pDesc->ByteWidth + 255) & ~255u,
                D3D12_RESOURCE_STATE_COPY_DEST, &sres))) {
            NotImplOnce("Device::CreateBuffer STAGING (READBACK heap creation failed)");
            return E_FAIL;
        }
        WrapBuffer* sw = new WrapBuffer();
        sw->dx12 = sres;
        sw->TrackMemory(GetDx12Bridge().device);
        sw->desc = *pDesc;
        PortNormalizeBufferDesc(sw->desc);          // match D3D11 GetDesc (see PortNormalizeBufferDesc note)
        sw->state = D3D12_RESOURCE_STATE_COPY_DEST; // READBACK is pinned here for its lifetime
        sw->heap_type = D3D12_HEAP_TYPE_READBACK;   // set now, so CopyResource/Map can just read
                                                    // it instead of probing GetHeapProperties
        if (ppBuffer)
            *ppBuffer = sw;
        else
            sw->Release();
        return S_OK;
    }

    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    const bool            host = !g_buffers_in_vram && pInitialData != nullptr && !uav;
    ID3D12Resource*       res = CreateBufferDx12(pDesc->ByteWidth, true, state, host);
    if (!res) {
        NotImplOnce("Device::CreateBuffer (no DX12 device bound, or creation failed)");
        return E_FAIL;
    }
    if (host && pInitialData->pSysMem) {
        void*       dst = nullptr;
        D3D12_RANGE none{ 0, 0 };
        if (SUCCEEDED(res->Map(0, &none, &dst)) && dst) {
            memcpy(dst, pInitialData->pSysMem, pDesc->ByteWidth);
            res->Unmap(0, nullptr);
        } else {
            logger::write("PORT: CreateBuffer initial data -- Map FAILED (%u bytes)", pDesc->ByteWidth);
        }
    }

    WrapBuffer* w = new WrapBuffer();
    w->dx12 = res;
    w->TrackMemory(GetDx12Bridge().device);
    w->state = state;
    w->desc = *pDesc;
    PortNormalizeBufferDesc(w->desc); // match D3D11 GetDesc (see PortNormalizeBufferDesc note)
    if (!host && pInitialData && pInitialData->pSysMem)
        QueueUpload(w, pInitialData->pSysMem, pDesc->ByteWidth);
    if (ppBuffer)
        *ppBuffer = w;
    else
        w->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateTexture1D(const D3D11_TEXTURE1D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA*                                                 pInitialData,
    _COM_Outptr_opt_ ID3D11Texture1D**                                            pp)
{
    if (pp) *pp = nullptr;
    if (!pDesc) return E_INVALIDARG;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device) {
        NotImplOnce("Device::CreateTexture1D (no DX12 device bound)");
        return E_FAIL;
    }
    if (NoTextures()) return E_FAIL; // PORT_NO_TEX bisect

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE1D;
    d.Width = pDesc->Width;
    d.Height = 1;
    d.DepthOrArraySize = (UINT16)(pDesc->ArraySize ? pDesc->ArraySize : 1);
    d.MipLevels = (UINT16)pDesc->MipLevels;
    d.Format = pDesc->Format;
    d.SampleDesc = { 1, 0 };
    if (pDesc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* res = nullptr;
    if (FAILED(b.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&res)))) {
        NotImplOnce("Device::CreateTexture1D (CreateCommittedResource failed)");
        return E_FAIL;
    }
    DevName(res, L"wwmi mod texture");

    WrapTexture1D* w = new WrapTexture1D();
    w->dx12 = res;
    w->TrackMemory(GetDx12Bridge().device);
    w->desc = *pDesc;
    if (pInitialData)
        QueueTextureUpload(w, pInitialData,
            (pDesc->MipLevels ? pDesc->MipLevels : 1) *
                (pDesc->ArraySize ? pDesc->ArraySize : 1));
    if (pp)
        *pp = w;
    else
        w->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateTexture2D(const D3D11_TEXTURE2D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA*                                                 pInitialData,
    _COM_Outptr_opt_ ID3D11Texture2D**                                            ppTexture2D)
{
    if (ppTexture2D) *ppTexture2D = nullptr;
    if (!pDesc) return E_INVALIDARG;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device) {
        NotImplOnce("Device::CreateTexture2D (no DX12 device bound)");
        return E_FAIL;
    }
    if (NoTextures()) return E_FAIL; // PORT_NO_TEX bisect

    if (pDesc->Usage == D3D11_USAGE_STAGING && (pDesc->CPUAccessFlags & D3D11_CPU_ACCESS_READ)) {
        DXGI_FORMAT stage_fmt = pDesc->Format;
        switch (pDesc->Format) {
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32G8X24_TYPELESS:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
        case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
            stage_fmt = DXGI_FORMAT_R32_FLOAT; // plane 0 of the 32+8+24 group
            break;
        case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24G8_TYPELESS:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
        case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
            stage_fmt = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; // plane 0 of the 24+8 group
            break;
        default: break;
        }
        if (stage_fmt != pDesc->Format)
            NotImplOnce("CreateTexture2D STAGING: planar depth -- staging the DEPTH PLANE only");

        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = pDesc->Width;
        td.Height = pDesc->Height;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = stage_fmt;
        td.SampleDesc = { 1, 0 };
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        UINT                               rows = 0;
        UINT64                             row_bytes = 0;
        UINT64                             total = 0;
        b.device->GetCopyableFootprints(&td, 0, 1, 0, &fp, &rows, &row_bytes, &total);
        if (!total) {
            NotImplOnce("Device::CreateTexture2D STAGING (zero footprint)");
            return E_FAIL;
        }

        ID3D12Resource* rb = nullptr;
        if (FAILED(CreateD3D12Buffer(b.device, D3D12_HEAP_TYPE_READBACK, total, D3D12_RESOURCE_STATE_COPY_DEST, &rb))) {
            NotImplOnce("Device::CreateTexture2D STAGING (READBACK heap creation failed)");
            return E_FAIL;
        }
        WrapTexture2D* sw = new WrapTexture2D();
        sw->dx12 = rb;
        sw->TrackMemory(GetDx12Bridge().device);
        sw->desc = *pDesc;
        sw->desc.Format = stage_fmt;
        sw->state = D3D12_RESOURCE_STATE_COPY_DEST;
        sw->heap_type = D3D12_HEAP_TYPE_READBACK;
        sw->footprint = fp;
        sw->row_pitch = fp.Footprint.RowPitch; // non-zero marks it as a staging texture
        sw->rows = rows;
        if (ppTexture2D)
            *ppTexture2D = sw;
        else
            sw->Release();
        return S_OK;
    }

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = pDesc->Width;
    d.Height = pDesc->Height;
    d.DepthOrArraySize = (UINT16)(pDesc->ArraySize ? pDesc->ArraySize : 1);
    d.MipLevels = (UINT16)pDesc->MipLevels;
    d.Format = pDesc->Format;
    d.SampleDesc = { pDesc->SampleDesc.Count ? pDesc->SampleDesc.Count : 1,
        pDesc->SampleDesc.Quality };
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    if (pDesc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if (pDesc->BindFlags & D3D11_BIND_DEPTH_STENCIL) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;

    const bool    timed = TexTiming::Armed() && pInitialData;
    LARGE_INTEGER tq{};
    LARGE_INTEGER t0{};
    LARGE_INTEGER t1{};
    LARGE_INTEGER t2{};
    if (timed) {
        QueryPerformanceFrequency(&tq);
        QueryPerformanceCounter(&t0);
    }
    ID3D12Resource* res = nullptr;
    if (FAILED(b.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&res)))) {
        NotImplOnce("Device::CreateTexture2D (CreateCommittedResource failed)");
        return E_FAIL;
    }
    DevName(res, L"wwmi mod texture");
    if (timed) QueryPerformanceCounter(&t1);

    {
        static int n = 0;
        if (n < 64) {
            ++n;
            logger::write("PORT-TEX %ux%u mips=%u array=%u fmt=%d bind=%x -> dx12 res=%p mips=%u array=%u",
                pDesc->Width, pDesc->Height, pDesc->MipLevels, pDesc->ArraySize,
                (int)pDesc->Format, pDesc->BindFlags, (void*)res,
                (unsigned)res->GetDesc().MipLevels, (unsigned)res->GetDesc().DepthOrArraySize);
        }
    }

    WrapTexture2D* w = new WrapTexture2D();
    w->dx12 = res;
    w->TrackMemory(GetDx12Bridge().device);
    w->desc = *pDesc;
    if (pInitialData)
        QueueTextureUpload(w, pInitialData,
            (pDesc->MipLevels ? pDesc->MipLevels : 1) *
                (pDesc->ArraySize ? pDesc->ArraySize : 1));
    if (timed) {
        QueryPerformanceCounter(&t2);
        const double ms = 1000.0 / (double)tq.QuadPart;
        TexTiming::NoteCreate(res, pDesc->Width, pDesc->Height, pDesc->MipLevels, (uint32_t)pDesc->Format,
            (double)(t1.QuadPart - t0.QuadPart) * ms, (double)(t2.QuadPart - t1.QuadPart) * ms);
    }
    if (ppTexture2D)
        *ppTexture2D = w;
    else
        w->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateTexture3D(const D3D11_TEXTURE3D_DESC* pDesc,
    const D3D11_SUBRESOURCE_DATA*                                                 pInitialData,
    _COM_Outptr_opt_ ID3D11Texture3D**                                            pp)
{
    if (pp) *pp = nullptr;
    if (!pDesc) return E_INVALIDARG;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device) {
        NotImplOnce("Device::CreateTexture3D (no DX12 device bound)");
        return E_FAIL;
    }
    if (NoTextures()) return E_FAIL; // PORT_NO_TEX bisect

    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = pDesc->Width;
    d.Height = pDesc->Height;
    d.DepthOrArraySize = (UINT16)pDesc->Depth;
    d.MipLevels = (UINT16)pDesc->MipLevels;
    d.Format = pDesc->Format;
    d.SampleDesc = { 1, 0 };
    if (pDesc->BindFlags & D3D11_BIND_UNORDERED_ACCESS) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (pDesc->BindFlags & D3D11_BIND_RENDER_TARGET) d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* res = nullptr;
    if (FAILED(b.device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_COMMON, nullptr,
            IID_PPV_ARGS(&res)))) {
        NotImplOnce("Device::CreateTexture3D (CreateCommittedResource failed)");
        return E_FAIL;
    }
    DevName(res, L"wwmi mod texture");

    WrapTexture3D* w = new WrapTexture3D();
    w->dx12 = res;
    w->TrackMemory(GetDx12Bridge().device);
    w->desc = *pDesc;
    if (pInitialData)
        QueueTextureUpload(w, pInitialData, pDesc->MipLevels ? pDesc->MipLevels : 1);
    if (pp)
        *pp = w;
    else
        w->Release();
    return S_OK;
}

// ---------------------------------------------------------------------------
//  Views -- on DX12 these are descriptors, written into a heap WE own
// ---------------------------------------------------------------------------
namespace {

static bool NoViews()
{
    static const bool v = [] {
        char   buf[8] = {};
        size_t n = 0;
        return getenv_s(&n, buf, sizeof(buf), "PORT_NO_VIEWS") == 0 && n && buf[0] == '1';
    }();
    return v;
}

template <class W>
W* NewView(ID3D11Resource* res, char kind)
{
    if (NoViews()) return nullptr;
    WrapResource* wr = AsWrapResource(res);
    if (!wr || !wr->dx12) {
        NotImplOnce("Device::Create*View on a resource that is not one of our wrappers");
        return nullptr;
    }
    UINT                        slot = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = AllocStagingDescriptor(kind, &slot);
    if (!cpu.ptr) return nullptr;
    ProfGpuWork(PGW_ViewCreate, 0); // every SRV/UAV/RTV/DSV the shim makes comes through here
    W* v = new W();
    v->res = res;
    res->AddRef();
    v->wres = wr;
    v->heap_slot = slot;
    v->cpu = cpu;
    return v;
}

} // namespace

HRESULT STDMETHODCALLTYPE Dx12Device::CreateShaderResourceView(ID3D11Resource* pResource,
    const D3D11_SHADER_RESOURCE_VIEW_DESC* pDesc, _COM_Outptr_opt_ ID3D11ShaderResourceView** ppSRView)
{
    if (ppSRView) *ppSRView = nullptr;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!pResource || !b.device) return E_INVALIDARG;
    WrapSRV* v = NewView<WrapSRV>(pResource, 't');
    if (!v) return E_FAIL;

    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    if (pDesc) {
        v->desc = *pDesc;
        d.Format = pDesc->Format;
        switch (pDesc->ViewDimension) {
        case D3D11_SRV_DIMENSION_BUFFER:
            d.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
            d.Buffer.FirstElement = pDesc->Buffer.FirstElement;
            d.Buffer.NumElements = pDesc->Buffer.NumElements;
            if (WrapBuffer* wb = (WrapBuffer*)AsWrapResource(pResource)) {
                d.Buffer.StructureByteStride =
                    (pDesc->Format == DXGI_FORMAT_UNKNOWN) ? wb->desc.StructureByteStride : 0;
            }
            break;
        case D3D11_SRV_DIMENSION_TEXTURE1D:
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            d.Texture1D.MostDetailedMip = pDesc->Texture1D.MostDetailedMip;
            d.Texture1D.MipLevels = pDesc->Texture1D.MipLevels;
            break;
        case D3D11_SRV_DIMENSION_TEXTURE3D:
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
            d.Texture3D.MostDetailedMip = pDesc->Texture3D.MostDetailedMip;
            d.Texture3D.MipLevels = pDesc->Texture3D.MipLevels;
            break;
        case D3D11_SRV_DIMENSION_TEXTURE2DARRAY:
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.MostDetailedMip = pDesc->Texture2DArray.MostDetailedMip;
            d.Texture2DArray.MipLevels = pDesc->Texture2DArray.MipLevels;
            d.Texture2DArray.FirstArraySlice = pDesc->Texture2DArray.FirstArraySlice;
            d.Texture2DArray.ArraySize = pDesc->Texture2DArray.ArraySize;
            break;
        case D3D11_SRV_DIMENSION_TEXTURECUBE:
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
            d.TextureCube.MostDetailedMip = pDesc->TextureCube.MostDetailedMip;
            d.TextureCube.MipLevels = pDesc->TextureCube.MipLevels;
            break;
        default:
            d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            d.Texture2D.MostDetailedMip = pDesc->Texture2D.MostDetailedMip;
            d.Texture2D.MipLevels = pDesc->Texture2D.MipLevels;
            break;
        }
        if (d.ViewDimension == D3D12_SRV_DIMENSION_BUFFER &&
            BufferViewFit(v->wres->dx12, d.Buffer.FirstElement, d.Buffer.NumElements,
                d.Buffer.StructureByteStride ? d.Buffer.StructureByteStride
                                             : DxgiPlainBytesPerPixel((uint32_t)d.Format),
                "mod-srv") < d.Buffer.NumElements) {
            v->Release();
            return E_INVALIDARG;
        }
        b.device->CreateShaderResourceView(v->wres->dx12, &d, v->cpu);
    } else {
        // Default view of the whole resource: DX12 accepts a null desc for textures, but NOT for buffers.
        if (v->wres->dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            v->Release();
            NotImplOnce("Device::CreateShaderResourceView with a NULL desc on a buffer "
                        "(neither API has a default buffer view)");
            return E_INVALIDARG;
        }
        b.device->CreateShaderResourceView(v->wres->dx12, nullptr, v->cpu);
    }
    if (ppSRView)
        *ppSRView = v;
    else
        v->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateUnorderedAccessView(ID3D11Resource* pResource,
    const D3D11_UNORDERED_ACCESS_VIEW_DESC* pDesc, _COM_Outptr_opt_ ID3D11UnorderedAccessView** ppUAView)
{
    if (ppUAView) *ppUAView = nullptr;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!pResource || !b.device) return E_INVALIDARG;
    WrapUAV* v = NewView<WrapUAV>(pResource, 'u');
    if (!v) return E_FAIL;

    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    if (pDesc) {
        v->desc = *pDesc;
        d.Format = pDesc->Format;
        switch (pDesc->ViewDimension) {
        case D3D11_UAV_DIMENSION_BUFFER:
            d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            d.Buffer.FirstElement = pDesc->Buffer.FirstElement;
            d.Buffer.NumElements = pDesc->Buffer.NumElements;
            if (WrapBuffer* wb = (WrapBuffer*)AsWrapResource(pResource)) {
                d.Buffer.StructureByteStride =
                    (pDesc->Format == DXGI_FORMAT_UNKNOWN) ? wb->desc.StructureByteStride : 0;
            }
            if (pDesc->Buffer.Flags & D3D11_BUFFER_UAV_FLAG_RAW)
                d.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
            if (pDesc->Buffer.Flags & (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))
                NotImplOnce("Device::CreateUnorderedAccessView with APPEND/COUNTER "
                            "(DX12 needs a separate counter resource)");
            break;
        case D3D11_UAV_DIMENSION_TEXTURE1D:
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            d.Texture1D.MipSlice = pDesc->Texture1D.MipSlice;
            break;
        case D3D11_UAV_DIMENSION_TEXTURE2DARRAY:
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            d.Texture2DArray.MipSlice = pDesc->Texture2DArray.MipSlice;
            d.Texture2DArray.FirstArraySlice = pDesc->Texture2DArray.FirstArraySlice;
            d.Texture2DArray.ArraySize = pDesc->Texture2DArray.ArraySize;
            break;
        case D3D11_UAV_DIMENSION_TEXTURE3D:
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
            d.Texture3D.MipSlice = pDesc->Texture3D.MipSlice;
            d.Texture3D.FirstWSlice = pDesc->Texture3D.FirstWSlice;
            d.Texture3D.WSize = pDesc->Texture3D.WSize;
            break;
        default:
            d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            d.Texture2D.MipSlice = pDesc->Texture2D.MipSlice;
            break;
        }
        if (d.ViewDimension == D3D12_UAV_DIMENSION_BUFFER &&
            BufferViewFit(v->wres->dx12, d.Buffer.FirstElement, d.Buffer.NumElements,
                d.Buffer.StructureByteStride                       ? d.Buffer.StructureByteStride
                    : (d.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW) ? 4u
                                                                   : DxgiPlainBytesPerPixel((uint32_t)d.Format),
                "mod-uav") < d.Buffer.NumElements) {
            v->Release();
            return E_INVALIDARG;
        }
        b.device->CreateUnorderedAccessView(v->wres->dx12, nullptr, &d, v->cpu);
    } else {
        if (v->wres->dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
            v->Release();
            NotImplOnce("Device::CreateUnorderedAccessView with a NULL desc on a buffer");
            return E_INVALIDARG;
        }
        b.device->CreateUnorderedAccessView(v->wres->dx12, nullptr, nullptr, v->cpu);
    }
    if (ppUAView)
        *ppUAView = v;
    else
        v->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateRenderTargetView(ID3D11Resource* pResource,
    const D3D11_RENDER_TARGET_VIEW_DESC* pDesc, _COM_Outptr_opt_ ID3D11RenderTargetView** ppRTView)
{
    if (ppRTView) *ppRTView = nullptr;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!pResource || !b.device) return E_INVALIDARG;
    WrapRTV* v = NewView<WrapRTV>(pResource, 'r');
    if (!v) return E_FAIL;
    if (pDesc) {
        v->desc = *pDesc;
        D3D12_RENDER_TARGET_VIEW_DESC d{};
        d.Format = pDesc->Format;
        d.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = (pDesc->ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D)
            ? pDesc->Texture2D.MipSlice
            : 0;
        b.device->CreateRenderTargetView(v->wres->dx12, &d, v->cpu);
    } else {
        b.device->CreateRenderTargetView(v->wres->dx12, nullptr, v->cpu);
    }
    if (ppRTView)
        *ppRTView = v;
    else
        v->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateDepthStencilView(ID3D11Resource* pResource,
    const D3D11_DEPTH_STENCIL_VIEW_DESC* pDesc, _COM_Outptr_opt_ ID3D11DepthStencilView** ppDepthStencilView)
{
    if (ppDepthStencilView) *ppDepthStencilView = nullptr;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!pResource || !b.device) return E_INVALIDARG;
    WrapDSV* v = NewView<WrapDSV>(pResource, 'd');
    if (!v) return E_FAIL;
    if (pDesc) {
        v->desc = *pDesc;
        D3D12_DEPTH_STENCIL_VIEW_DESC d{};
        d.Format = pDesc->Format;
        d.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
        d.Texture2D.MipSlice = (pDesc->ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D)
            ? pDesc->Texture2D.MipSlice
            : 0;
        if (pDesc->Flags & D3D11_DSV_READ_ONLY_DEPTH) d.Flags |= D3D12_DSV_FLAG_READ_ONLY_DEPTH;
        if (pDesc->Flags & D3D11_DSV_READ_ONLY_STENCIL) d.Flags |= D3D12_DSV_FLAG_READ_ONLY_STENCIL;
        b.device->CreateDepthStencilView(v->wres->dx12, &d, v->cpu);
    } else {
        b.device->CreateDepthStencilView(v->wres->dx12, nullptr, v->cpu);
    }
    if (ppDepthStencilView)
        *ppDepthStencilView = v;
    else
        v->Release();
    return S_OK;
}

// ---------------------------------------------------------------------------
//  Shaders -- the object is the bytecode a PSO gets built from
// ---------------------------------------------------------------------------
// Their CustomShader::compile does the D3DCompile itself (with the profile the ini names and their
// 3DMIGOTO_* defines) and hands the bytecode here. DX12 accepts DXBC directly, so nothing is recompiled:
// the same bytes go into the PSO, and D3DReflect on the same bytes derives the root signature.
namespace {

template <class W, class I>
HRESULT MakeShader(const void* code, SIZE_T len, _COM_Outptr_opt_ I** out, const char* what)
{
    if (out) *out = nullptr;
    if (!code || !len) return E_INVALIDARG;
    W* s = new W();
    s->code.assign((const uint8_t*)code, (const uint8_t*)code + len);
    if (out)
        *out = s;
    else
        s->Release();
    logger::write("PORT: %s accepted, %zu bytes", what, (size_t)len);
    return S_OK;
}

} // namespace

HRESULT STDMETHODCALLTYPE Dx12Device::CreateVertexShader(const void* code, SIZE_T len,
    ID3D11ClassLinkage*, _COM_Outptr_opt_ ID3D11VertexShader**                    out)
{
    return MakeShader<WrapVS>(code, len, out, "vertex shader");
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateHullShader(const void* code, SIZE_T len,
    ID3D11ClassLinkage*, _COM_Outptr_opt_ ID3D11HullShader**                    out)
{
    return MakeShader<WrapHS>(code, len, out, "hull shader");
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateDomainShader(const void* code, SIZE_T len,
    ID3D11ClassLinkage*, _COM_Outptr_opt_ ID3D11DomainShader**                    out)
{
    return MakeShader<WrapDS>(code, len, out, "domain shader");
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateGeometryShader(const void* code, SIZE_T len,
    ID3D11ClassLinkage*, _COM_Outptr_opt_ ID3D11GeometryShader**                    out)
{
    return MakeShader<WrapGS>(code, len, out, "geometry shader");
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreatePixelShader(const void* code, SIZE_T len,
    ID3D11ClassLinkage*, _COM_Outptr_opt_ ID3D11PixelShader**                    out)
{
    return MakeShader<WrapPS>(code, len, out, "pixel shader");
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateComputeShader(const void* code, SIZE_T len,
    ID3D11ClassLinkage*, _COM_Outptr_opt_ ID3D11ComputeShader**                    out)
{
    return MakeShader<WrapCS>(code, len, out, "compute shader");
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateBlendState(const D3D11_BLEND_DESC* pDesc,
    _COM_Outptr_opt_ ID3D11BlendState**                                        pp)
{
    if (pp) *pp = nullptr;
    if (!pDesc) return E_INVALIDARG;
    WrapBlend* s = new WrapBlend();
    s->blend = *pDesc;
    if (pp)
        *pp = s;
    else
        s->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateRasterizerState(const D3D11_RASTERIZER_DESC* pDesc,
    _COM_Outptr_opt_ ID3D11RasterizerState**                                             pp)
{
    if (pp) *pp = nullptr;
    if (!pDesc) return E_INVALIDARG;
    WrapRaster* s = new WrapRaster();
    s->raster = *pDesc;
    if (pp)
        *pp = s;
    else
        s->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateDepthStencilState(const D3D11_DEPTH_STENCIL_DESC* pDesc,
    _COM_Outptr_opt_ ID3D11DepthStencilState**                                                pp)
{
    if (pp) *pp = nullptr;
    if (!pDesc) return E_INVALIDARG;
    WrapDepth* s = new WrapDepth();
    s->depth = *pDesc;
    if (pp)
        *pp = s;
    else
        s->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateSamplerState(const D3D11_SAMPLER_DESC* pDesc,
    _COM_Outptr_opt_ ID3D11SamplerState**                                          pp)
{
    if (pp) *pp = nullptr;
    if (!pDesc) return E_INVALIDARG;
    WrapSampler* s = new WrapSampler();
    s->sampler = *pDesc;
    if (pp)
        *pp = s;
    else
        s->Release();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateInputLayout(const D3D11_INPUT_ELEMENT_DESC* pElems,
    UINT NumElements, const void*, SIZE_T, _COM_Outptr_opt_ ID3D11InputLayout** pp)
{
    if (pp) *pp = nullptr;
    if (!pElems && NumElements) return E_INVALIDARG;
    WrapInputLayout* l = new WrapInputLayout();
    l->names.reserve(NumElements);
    l->elems.reserve(NumElements);
    for (UINT i = 0; i < NumElements; ++i) {
        l->names.push_back(pElems[i].SemanticName ? pElems[i].SemanticName : "");
        D3D12_INPUT_ELEMENT_DESC e{};
        e.SemanticIndex = pElems[i].SemanticIndex;
        e.Format = pElems[i].Format;
        e.InputSlot = pElems[i].InputSlot;
        e.AlignedByteOffset = pElems[i].AlignedByteOffset;
        e.InputSlotClass = (pElems[i].InputSlotClass == D3D11_INPUT_PER_INSTANCE_DATA)
            ? D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA
            : D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
        e.InstanceDataStepRate = pElems[i].InstanceDataStepRate;
        l->elems.push_back(e);
    }
    for (UINT i = 0; i < NumElements; ++i) l->elems[i].SemanticName = l->names[i].c_str();
    if (pp)
        *pp = l;
    else
        l->Release();
    return S_OK;
}

// Their engine asks this before choosing a copy or resolve strategy (ResolveMSAA, the staging paths).
HRESULT STDMETHODCALLTYPE Dx12Device::CheckFormatSupport(DXGI_FORMAT Format, UINT* pFormatSupport)
{
    if (!pFormatSupport) return E_INVALIDARG;
    *pFormatSupport = 0;
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.device) return E_FAIL;

    D3D12_FEATURE_DATA_FORMAT_SUPPORT fs{};
    fs.Format = Format;
    if (FAILED(b.device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &fs, sizeof(fs))))
        return E_FAIL;
    UINT out = 0;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_TEXTURE2D) out |= D3D11_FORMAT_SUPPORT_TEXTURE2D;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_RENDER_TARGET) out |= D3D11_FORMAT_SUPPORT_RENDER_TARGET;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_DEPTH_STENCIL) out |= D3D11_FORMAT_SUPPORT_DEPTH_STENCIL;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) out |= D3D11_FORMAT_SUPPORT_SHADER_LOAD;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_SAMPLE) out |= D3D11_FORMAT_SUPPORT_SHADER_SAMPLE;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_MULTISAMPLE_RESOLVE) out |= D3D11_FORMAT_SUPPORT_MULTISAMPLE_RESOLVE;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_BLENDABLE) out |= D3D11_FORMAT_SUPPORT_BLENDABLE;
    if (fs.Support1 & D3D12_FORMAT_SUPPORT1_TYPED_UNORDERED_ACCESS_VIEW)
        out |= D3D11_FORMAT_SUPPORT_TYPED_UNORDERED_ACCESS_VIEW;
    *pFormatSupport = out;
    return S_OK;
}

// Their engine branches on feature level in a few places. DX12 on this hardware is at least 11_1.
D3D_FEATURE_LEVEL STDMETHODCALLTYPE Dx12Device::GetFeatureLevel()
{
    return D3D_FEATURE_LEVEL_11_1;
}

void STDMETHODCALLTYPE Dx12Device::GetImmediateContext(ID3D11DeviceContext** pp)
{
    if (pp) *pp = Context();
}

void STDMETHODCALLTYPE Dx12Device::GetImmediateContext1(ID3D11DeviceContext1** pp)
{
    if (pp) *pp = Context();
}

} // namespace MigotoPort
