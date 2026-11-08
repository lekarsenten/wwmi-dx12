#pragma once
// The skeleton capture (dx12_skcap.cpp): arm it, snapshot what a dispatch or a draw read, drain once per present.
#include <d3d12.h>

namespace MigotoPort {

void SkCapArm(int dispatches);
void SkCapArmFrames(int frames);
void SkCapSnapRaw(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const char* name, ID3D12Resource* src,
    unsigned long long off, unsigned long long bytes, unsigned state, const void* cs, unsigned seq, unsigned gx,
    unsigned gy, unsigned gz, const char* prov);
void SkCapSnapDraw(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, const char* name, ID3D12Resource* src,
    unsigned long long off, unsigned long long bytes, unsigned state, unsigned seq, const char* prov);
/* Publish which game dispatch is about to issue the next captured pass -- see SkCapPass::caller_*. */
void SkCapSetCaller(const wchar_t* ns, unsigned long long cs, unsigned tgy, int mod);
bool SkCapArmed();
void SkCapBeforeDispatch(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z);
void SkCapAfterDispatch(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, UINT x, UINT y, UINT z);
void SkCapDrain(); // call once per Present

} // namespace MigotoPort
