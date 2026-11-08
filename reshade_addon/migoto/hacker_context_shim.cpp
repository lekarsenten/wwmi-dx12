#include "globals.h"
#include "HackerContext.h"
#include "HackerDevice.h"
#include "HackerInputLayout.h"
#include "dx12_input_layout.h"
#include "dx12_pipeline.h"
#include "../core/log.h"

void HackerContext::Bind3DMigotoResources()
{
    if (!mHackerDevice || !mHackerDevice->mIniResourceView || G->IniParamsReg < 0) return;
    ID3D11DeviceContext1* ctx = GetPassThroughOrigContext1();
    if (!ctx) return;
    ID3D11ShaderResourceView* srv = mHackerDevice->mIniResourceView;
    const UINT slot = (UINT)G->IniParamsReg;
    ctx->VSSetShaderResources(slot, 1, &srv);
    ctx->HSSetShaderResources(slot, 1, &srv);
    ctx->DSSetShaderResources(slot, 1, &srv);
    ctx->GSSetShaderResources(slot, 1, &srv);
    ctx->PSSetShaderResources(slot, 1, &srv);
    ctx->CSSetShaderResources(slot, 1, &srv);
    logger::write("PORT: IniParams bound at t%u on all six stages (view %p)", slot, (void*)srv);
}

void HackerContext::InitIniParams()
{
    if (!G->iniParams.empty()) memset(G->iniParams.data(), 0, sizeof(DirectX::XMFLOAT4) * G->iniParams.size());
    Bind3DMigotoResources();
}

void HackerContext::DeferInputLayoutOverride(HackerInputLayout* pInputLayout)
{
    if (mOverrideInputLayout) mOverrideInputLayout->Release();
    mOverrideInputLayout = pInputLayout;
}

void HackerContext::OverrideInputLayout()
{
    if (!mOverrideInputLayout) return;
    MigotoPort::State().layout_override = { mDrawPipeline, mOverrideInputLayout->GetElements(),
        mOverrideInputLayout->GetElementCount() };
}

void HackerContext::RestoreInputLayout()
{
    if (mOverrideInputLayout) mOverrideInputLayout->Release();
    mOverrideInputLayout = nullptr;
    MigotoPort::State().layout_override = {};
}

void STDMETHODCALLTYPE HackerContext::IAGetInputLayout(ID3D11InputLayout** ppInputLayout)
{
    if (!ppInputLayout) return;
    if (mOverrideInputLayout) {
        mOverrideInputLayout->AddRef();
        *ppInputLayout = mOverrideInputLayout;
        return;
    }
    *ppInputLayout = MigotoPort::HackerInputLayoutOf(mDrawPipeline);
}

HRESULT STDMETHODCALLTYPE HackerDevice::CreateCustomInputLayout(const D3D11_INPUT_ELEMENT_DESC* pInputElementDescs,
    UINT NumElements, const void* /*pShaderBytecodeWithInputSignature*/, SIZE_T /*BytecodeLength*/,
    ID3D11InputLayout** ppInputLayout)
{
    if (!ppInputLayout) return E_INVALIDARG;
    *ppInputLayout = MigotoPort::NewHackerInputLayout(pInputElementDescs, NumElements);
    return *ppInputLayout ? S_OK : E_FAIL;
}
