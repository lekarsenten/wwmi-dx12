// dump_preview.cpp -- OURS. PORT-LEAF. See dump_preview.h.
#include "dump_preview.h"
#include "dx12_bridge.h" // GetDx12Bridge, ObservedResourceState
#include "dx12_wrap.h" // AsWrapResource: the D3D11 texture their engine holds -> its DX12 resource
#include "dx12_dump_readback.h" // DumpReadbackList: the dump's own queue, flushed by their Map(READ)
#include "dump_watchdog.h" // DumpStage: the preview names its position for a failure report
#include "../core/log.h"
#include "../core/d3d12_barrier.h"
#include "../core/d3d12_name.h"
#include <ScreenGrab.h>
#include <wincodec.h>
#include <d3d12.h>
#include <vector>

namespace MigotoPort {

namespace {

std::vector<unsigned char> g_cso;

struct Pass {
    bool tried = false;
    bool ok = false;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;
    ID3D12DescriptorHeap* heap = nullptr; // [0] = source SRV, [1] = destination UAV
    UINT inc = 0;
};
Pass g_pass;

bool Init(ID3D12Device* dev)
{
    if (g_pass.tried) return g_pass.ok;
    g_pass.tried = true;
    if (!dev || g_cso.empty()) {
        logger::write("DUMP PREVIEW: no DumpPreview.cso -- .dds files get no .jpg");
        return false;
    }

    // Root constants (w, h, tonemap, pad) at b0; one table [t0 SRV, u0 UAV]; a point clamp sampler at s0.
    D3D12_DESCRIPTOR_RANGE r[2] = {};
    r[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    r[0].NumDescriptors = 1;
    r[0].OffsetInDescriptorsFromTableStart = 0;
    r[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    r[1].NumDescriptors = 1;
    r[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER rp[2] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 4;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable = { 2, r };
    D3D12_STATIC_SAMPLER_DESC samp = {};
    samp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    samp.AddressU = samp.AddressV = samp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    samp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = rp;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &samp;
    ID3DBlob* bl = nullptr;
    ID3DBlob* er = nullptr;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &bl, &er)) ||
        FAILED(dev->CreateRootSignature(0, bl->GetBufferPointer(), bl->GetBufferSize(), IID_PPV_ARGS(&g_pass.rs)))) {
        logger::write("DUMP PREVIEW: root signature failed");
        if (er) er->Release();
        if (bl) bl->Release();
        return false;
    }
    bl->Release();
    if (er) er->Release();
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
    pd.pRootSignature = g_pass.rs;
    pd.CS = { g_cso.data(), g_cso.size() };
    if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&g_pass.pso)))) {
        logger::write("DUMP PREVIEW: PSO failed");
        return false;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 2,
        D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0 };
    if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&g_pass.heap)))) {
        logger::write("DUMP PREVIEW: heap failed");
        return false;
    }
    g_pass.inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    g_pass.ok = true;
    return true;
}

bool IsFloatFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_BC6H_UF16:
    case DXGI_FORMAT_BC6H_SF16:
    case DXGI_FORMAT_D32_FLOAT:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return true;
    default: return false;
    }
}

bool IsIntegerFormat(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT:
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SINT:
    case DXGI_FORMAT_R32G32_UINT:
    case DXGI_FORMAT_R32G32_SINT:
    case DXGI_FORMAT_R10G10B10A2_UINT:
    case DXGI_FORMAT_R8G8B8A8_UINT:
    case DXGI_FORMAT_R8G8B8A8_SINT:
    case DXGI_FORMAT_R16G16_UINT:
    case DXGI_FORMAT_R16G16_SINT:
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT:
    case DXGI_FORMAT_R8G8_UINT:
    case DXGI_FORMAT_R8G8_SINT:
    case DXGI_FORMAT_R16_UINT:
    case DXGI_FORMAT_R16_SINT:
    case DXGI_FORMAT_R8_UINT:
    case DXGI_FORMAT_R8_SINT:
    case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
    case DXGI_FORMAT_X24_TYPELESS_G8_UINT: return true;
    default: return false;
    }
}

DXGI_FORMAT NoSrgb(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_BC1_UNORM_SRGB: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_UNORM_SRGB: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_UNORM_SRGB: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC7_UNORM_SRGB: return DXGI_FORMAT_BC7_UNORM;
    default: return f;
    }
}

void Once(const char* why)
{
    static const char* seen[16] = {};
    static int n = 0; // once per reason: eight cube-map lines hid the rest
    for (int i = 0; i < n; ++i)
        if (seen[i] == why) return;
    if (n < 16) seen[n++] = why;
    logger::write("DUMP PREVIEW: no .jpg -- %s", why);
}

} // namespace

void SetDumpPreviewShader(const void* cso, size_t bytes)
{
    g_cso.assign((const unsigned char*)cso, (const unsigned char*)cso + bytes);
}

bool DumpPreviewJpeg(ID3D11Device* dev11, ID3D11DeviceContext* ctx, ID3D11Texture2D* source, DXGI_FORMAT view_format,
    const wchar_t* path)
{
    const Dx12Bridge& b = GetDx12Bridge();
    if (!dev11 || !ctx || !source || !path || !b.device) return false;
    if (!Init(b.device)) return false;
    WrapResource* wr = AsWrapResource(source);
    if (!wr || !wr->dx12) {
        Once("the source is not a DX12 resource");
        return false;
    }
    ID3D12Resource* src = wr->dx12;
    const D3D12_RESOURCE_DESC sd = src->GetDesc();
    if (sd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || sd.DepthOrArraySize != 1 || sd.SampleDesc.Count != 1) {
        Once("not a single-slice, single-sample 2D texture");
        return false;
    }
    const DXGI_FORMAT srv_fmt = NoSrgb(view_format);
    if (IsIntegerFormat(srv_fmt)) {
        Once("integer format");
        return false;
    }
    const uint32_t st = ObservedResourceState(src);
    if (st == kResStateUnknown) {
        Once("source state unknown");
        return false;
    }
    const UINT w = (UINT)sd.Width;
    const UINT h = sd.Height;

    ID3D12Resource* dst = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp = {};
        hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC td = {};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = w;
        td.Height = h;
        td.DepthOrArraySize = 1;
        td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.SampleDesc = { 1, 0 };
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(b.device->CreateCommittedResource(
                &hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&dst)))) {
            Once("destination texture failed");
            return false;
        }
        DevName(dst, L"wwmi dump preview");
    }
    ID3D11Texture2D* staging = nullptr;
    {
        D3D11_TEXTURE2D_DESC sdesc = {};
        sdesc.Width = w;
        sdesc.Height = h;
        sdesc.MipLevels = 1;
        sdesc.ArraySize = 1;
        sdesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        sdesc.SampleDesc = { 1, 0 };
        sdesc.Usage = D3D11_USAGE_STAGING;
        sdesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        if (FAILED(dev11->CreateTexture2D(&sdesc, nullptr, &staging)) || !staging) {
            dst->Release();
            Once("staging failed");
            return false;
        }
    }
    WrapResource* sw = AsWrapResource(staging);
    ID3D12GraphicsCommandList* cl = DumpReadbackList();
    if (!sw || !sw->dx12 || !cl) {
        staging->Release();
        dst->Release();
        Once("no dump queue");
        return false;
    }

    DumpStage("Preview: record", (unsigned long long)src);
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = g_pass.heap->GetCPUDescriptorHandleForHeapStart();
    {
        D3D12_SHADER_RESOURCE_VIEW_DESC v = {};
        v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        v.Format = srv_fmt;
        v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        v.Texture2D.MipLevels = 1;
        b.device->CreateShaderResourceView(src, &v, cpu);
        D3D12_CPU_DESCRIPTOR_HANDLE u = cpu;
        u.ptr += g_pass.inc;
        D3D12_UNORDERED_ACCESS_VIEW_DESC ud = {};
        ud.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        ud.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        b.device->CreateUnorderedAccessView(dst, nullptr, &ud, u);
    }
    const D3D12_RESOURCE_STATES before = (D3D12_RESOURCE_STATES)st;
    const bool need = (before & D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE) == 0;
    if (need) TransitionBarrier(cl, src, before, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    ID3D12DescriptorHeap* heaps[] = { g_pass.heap };
    cl->SetDescriptorHeaps(1, heaps);
    cl->SetPipelineState(g_pass.pso);
    cl->SetComputeRootSignature(g_pass.rs);
    const UINT consts[4] = { w, h, IsFloatFormat(srv_fmt) ? 1u : 0u, 0u };
    cl->SetComputeRoot32BitConstants(0, 4, consts, 0);
    cl->SetComputeRootDescriptorTable(1, g_pass.heap->GetGPUDescriptorHandleForHeapStart());
    cl->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    if (need) TransitionBarrier(cl, src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, before);
    TransitionBarrier(cl, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION dl = {};
    dl.pResource = sw->dx12;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dl.PlacedFootprint = sw->footprint;
    D3D12_TEXTURE_COPY_LOCATION sl = {};
    sl.pResource = dst;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);

    // THEIR writer, on the readback: its Map(READ) submits this list, orders it after the game's work and waits.
    DumpStage("Preview: write", (unsigned long long)src);
    const HRESULT hr = DirectX::SaveWICTextureToFile(ctx, staging, GUID_ContainerFormatJpeg, path);
    staging->Release();
    dst->Release();
    if (FAILED(hr)) {
        Once("WIC could not encode the R8G8B8A8 preview");
        return false;
    }
    return true;
}

} // namespace MigotoPort
