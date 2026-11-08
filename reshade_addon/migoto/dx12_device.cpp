// GENERATED skeleton -- real transitions live in dx12_device_impl.cpp. See dx12_device.h.
#include "dx12_device.h"
#include "dx12_context.h"
#include "../core/log.h"

namespace MigotoPort {

Dx12Device* Device()
{
    static Dx12Device d;
    return &d;
}

HRESULT STDMETHODCALLTYPE Dx12Device::QueryInterface(REFIID riid, void** ppv)
{
    if (!ppv) return E_POINTER;
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID3D11Device) || riid == __uuidof(ID3D11Device1)) {
        *ppv = this;
        AddRef();
        return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE Dx12Device::AddRef()
{
    return (ULONG)InterlockedIncrement(&m_ref);
}
ULONG STDMETHODCALLTYPE Dx12Device::Release()
{
    LONG r = InterlockedDecrement(&m_ref);
    return (ULONG)(r < 0 ? 0 : r);
}

HRESULT STDMETHODCALLTYPE Dx12Device::CreateDeferredContext1(UINT, _COM_Outptr_opt_ ID3D11DeviceContext1** ppDeferredContext)
{
    NotImplOnce("Device::CreateDeferredContext1");
    if (ppDeferredContext) *ppDeferredContext = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreateBlendState1(const D3D11_BLEND_DESC1*, _COM_Outptr_opt_ ID3D11BlendState1** ppBlendState)
{
    NotImplOnce("Device::CreateBlendState1");
    if (ppBlendState) *ppBlendState = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreateRasterizerState1(const D3D11_RASTERIZER_DESC1*, _COM_Outptr_opt_ ID3D11RasterizerState1** ppRasterizerState)
{
    NotImplOnce("Device::CreateRasterizerState1");
    if (ppRasterizerState) *ppRasterizerState = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreateDeviceContextState(UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, REFIID, D3D_FEATURE_LEVEL*, ID3DDeviceContextState**)
{
    NotImplOnce("Device::CreateDeviceContextState");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::OpenSharedResource1(HANDLE, REFIID, _COM_Outptr_ void** ppResource)
{
    NotImplOnce("Device::OpenSharedResource1");
    if (ppResource) *ppResource = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::OpenSharedResourceByName(LPCWSTR, DWORD, REFIID, _COM_Outptr_ void** ppResource)
{
    NotImplOnce("Device::OpenSharedResourceByName");
    if (ppResource) *ppResource = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreateGeometryShaderWithStreamOutput(const void*, SIZE_T, const D3D11_SO_DECLARATION_ENTRY*, UINT, const UINT*, UINT, UINT, ID3D11ClassLinkage*, _COM_Outptr_opt_ ID3D11GeometryShader** ppGeometryShader)
{
    NotImplOnce("Device::CreateGeometryShaderWithStreamOutput");
    if (ppGeometryShader) *ppGeometryShader = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreateClassLinkage(_COM_Outptr_ ID3D11ClassLinkage** ppLinkage)
{
    NotImplOnce("Device::CreateClassLinkage");
    if (ppLinkage) *ppLinkage = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreateQuery(const D3D11_QUERY_DESC*, _COM_Outptr_opt_ ID3D11Query** ppQuery)
{
    NotImplOnce("Device::CreateQuery");
    if (ppQuery) *ppQuery = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreatePredicate(const D3D11_QUERY_DESC*, _COM_Outptr_opt_ ID3D11Predicate** ppPredicate)
{
    NotImplOnce("Device::CreatePredicate");
    if (ppPredicate) *ppPredicate = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreateCounter(const D3D11_COUNTER_DESC*, _COM_Outptr_opt_ ID3D11Counter** ppCounter)
{
    NotImplOnce("Device::CreateCounter");
    if (ppCounter) *ppCounter = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CreateDeferredContext(UINT, _COM_Outptr_opt_ ID3D11DeviceContext** ppDeferredContext)
{
    NotImplOnce("Device::CreateDeferredContext");
    if (ppDeferredContext) *ppDeferredContext = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::OpenSharedResource(HANDLE, REFIID, _COM_Outptr_opt_ void** ppResource)
{
    NotImplOnce("Device::OpenSharedResource");
    if (ppResource) *ppResource = nullptr;
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CheckMultisampleQualityLevels(DXGI_FORMAT, UINT, UINT*)
{
    NotImplOnce("Device::CheckMultisampleQualityLevels");
    return E_NOTIMPL;
}
void STDMETHODCALLTYPE Dx12Device::CheckCounterInfo(D3D11_COUNTER_INFO* pCounterInfo)
{
    NotImplOnce("Device::CheckCounterInfo");
    if (pCounterInfo) *pCounterInfo = {};
}
HRESULT STDMETHODCALLTYPE Dx12Device::CheckCounter(const D3D11_COUNTER_DESC*, D3D11_COUNTER_TYPE*, UINT*, LPSTR, UINT*, LPSTR, UINT*, LPSTR, UINT*)
{
    NotImplOnce("Device::CheckCounter");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::CheckFeatureSupport(D3D11_FEATURE, void*, UINT)
{
    NotImplOnce("Device::CheckFeatureSupport");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::GetPrivateData(REFGUID, UINT*, void*)
{
    NotImplOnce("Device::GetPrivateData");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::SetPrivateData(REFGUID, UINT, const void*)
{
    NotImplOnce("Device::SetPrivateData");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::SetPrivateDataInterface(REFGUID, const IUnknown*)
{
    NotImplOnce("Device::SetPrivateDataInterface");
    return E_NOTIMPL;
}
UINT STDMETHODCALLTYPE Dx12Device::GetCreationFlags(void)
{
    NotImplOnce("Device::GetCreationFlags");
    return (UINT)0;
}
HRESULT STDMETHODCALLTYPE Dx12Device::GetDeviceRemovedReason(void)
{
    NotImplOnce("Device::GetDeviceRemovedReason");
    return E_NOTIMPL;
}
HRESULT STDMETHODCALLTYPE Dx12Device::SetExceptionMode(UINT)
{
    NotImplOnce("Device::SetExceptionMode");
    return E_NOTIMPL;
}
UINT STDMETHODCALLTYPE Dx12Device::GetExceptionMode(void)
{
    NotImplOnce("Device::GetExceptionMode");
    return (UINT)0;
}

} // namespace MigotoPort
