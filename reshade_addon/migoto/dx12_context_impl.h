#pragma once
// The non-member API of dx12_context_impl.cpp (dx12_context.h is generated and holds the class): the bound DX11
// state as the add-on's own compute passes read it.
#include <d3d12.h>
#include <cstdint>

namespace MigotoPort {

// The SRV write counter (see PipeState::srv_gen): monotonic, compare by signed difference.
uint32_t SrvWriteGen();
// The same for constant buffers (PipeState::cb_gen).
uint32_t CbWriteGen();

struct ShimBufView {
    ID3D12Resource* res = nullptr; // valid within this frame only; never stored
    unsigned long long va = 0;
    unsigned size = 0;
    unsigned stride = 0;
    unsigned state = 0;
};
bool ShimBufferForCompute(ID3D12GraphicsCommandList* cl, char kind, unsigned slot, ShimBufView* out, const char** why,
    void* object = nullptr);
void* AcquireShimVsCb(unsigned slot);
void ReleaseShimObject(void* object);
// True for a buffer the GAME owns (a wrapper PushGameConstantBuffer and friends made) -- never a mod skeleton.
bool ShimObjectForeign(void* object);

} // namespace MigotoPort
