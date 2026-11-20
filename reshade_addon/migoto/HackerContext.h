#pragma once

#include <d3d11_1.h>

DEFINE_GUID(IID_HackerContext,
    0xa3046b1e, 0x336b, 0x4d90, 0x9f, 0xd6, 0x23, 0x4b, 0xc0, 0x9b, 0x86, 0x87);
#include <cstdint>
#include "dx12_context.h" // MigotoPort::Context(): the DX11->DX12 transition layer

class HackerDevice;
class HackerInputLayout;
struct InitialTexturesSnapshot;

class HackerContext {
public:
    HackerContext() = default;
    HackerContext(ID3D11Device1*, ID3D11DeviceContext1*) {}
    ID3D11VertexShader*   mCurrentVertexShaderHandle = nullptr;
    ID3D11HullShader*     mCurrentHullShaderHandle = nullptr;
    ID3D11DomainShader*   mCurrentDomainShaderHandle = nullptr;
    ID3D11GeometryShader* mCurrentGeometryShaderHandle = nullptr;
    ID3D11PixelShader*    mCurrentPixelShaderHandle = nullptr;
    ID3D11ComputeShader*  mCurrentComputeShaderHandle = nullptr;

    UINT64   mCurrentVertexShader = 0;
    UINT64   mCurrentHullShader = 0;
    UINT64   mCurrentDomainShader = 0;
    UINT64   mCurrentGeometryShader = 0;
    UINT64   mCurrentPixelShader = 0;
    UINT64   mCurrentComputeShader = 0;
    uint32_t mCurrentVertexBuffers[D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT] = {};
    uint32_t mCurrentIndexBuffer = 0;

    HackerDevice* mHackerDevice = nullptr;
    HackerDevice* GetHackerDevice() { return mHackerDevice; }

    ID3D11DeviceContext1* GetPassThroughOrigContext1() { return MigotoPort::Context(); }

    void InitIniParams();

    void Bind3DMigotoResources();

    unsigned draw_number = 0;
    unsigned dispatch_number = 0;
    unsigned GetDrawNumber() const { return draw_number; }
    unsigned GetDispatchNumber() const { return dispatch_number; }
    unsigned GetCallNumber() const;
    void     ResetCallCounters()
    {
        draw_number = 0;
        dispatch_number = 0;
    }

    InitialTexturesSnapshot& GetInitialTexturesSnapshot();
    HRESULT ComputeDHash(ID3D11Texture2D* texture, UINT mipLevel, DXGI_FORMAT srvFormat, uint64_t& outHash);

    ID3D11Buffer* GetReadbackBuffer(UINT /*size*/) { return nullptr; }

    unsigned long long mDrawPipeline = 0;
    HackerInputLayout* mOverrideInputLayout = nullptr;

    void DeferInputLayoutOverride(HackerInputLayout* pInputLayout);
    void OverrideInputLayout();
    void RestoreInputLayout();

    void STDMETHODCALLTYPE IAGetInputLayout(ID3D11InputLayout** ppInputLayout);

    HRESULT STDMETHODCALLTYPE Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType,
        UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource)
    {
        return MigotoPort::Context()->Map(pResource, Subresource, MapType, MapFlags, pMappedResource);
    }
    void STDMETHODCALLTYPE Unmap(ID3D11Resource* pResource, UINT Subresource)
    {
        MigotoPort::Context()->Unmap(pResource, Subresource);
    }

    virtual void FrameAnalysisLog(char* /*fmt*/, ...) {}
    virtual void FrameAnalysisTrigger(FrameAnalysisOptions /*new_options*/) {}
    virtual void FrameAnalysisDump(ID3D11Resource* /*resource*/, FrameAnalysisOptions /*options*/,
        const wchar_t* /*target*/, DXGI_FORMAT /*format*/,
        UINT /*stride*/, UINT /*offset*/) {}
};
