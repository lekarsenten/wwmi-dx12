#pragma once

#include <d3d11_1.h> // ID3D11InputLayout -- the real one; see CurrentInputLayout below
// ---------------------------------------------------------------------------------------------
//  INPUT LAYOUT -- the second DX11-without-DX12 leaf their dump engine needs.
//
//  DumpVBs (frame_analysis_engine.cpp) does:
//      GetPassThroughOrigContext1()->IAGetInputLayout(&layout);
//      layout->GetPrivateData(InputLayoutDescGuid, &size, &layout_desc);
//  i.e. it expects the D3D11_INPUT_ELEMENT_DESC[] to be hanging off the ID3D11InputLayout object,
//  where HackerDevice::CreateInputLayout put it. DX12 has NO input-layout object at all: the layout
//  is baked into the PSO and there is no API to read it back. Creation time is the only chance.
//
//  So the substitution is: capture the layout when the PSO is created, and hand their engine an
//  ID3D11InputLayout stand-in whose GetPrivateData returns the blob they expect. Their code is
//  unchanged -- it cannot tell the difference.
// ---------------------------------------------------------------------------------------------
class HackerInputLayout;

namespace MigotoPort {

struct InputElement {
    const char* semantic = nullptr;
    unsigned semantic_index = 0;
    unsigned format = 0; // DXGI_FORMAT value
    unsigned buffer_binding = 0; // input slot
    unsigned offset = 0; // aligned byte offset
    unsigned instance_step_rate = 0; // 0 = per vertex
};
void RecordInputLayout(unsigned long long pipeline_handle, const InputElement* elements, unsigned count);

void SetCurrentInputLayout(unsigned long long pipeline_handle);

::ID3D11InputLayout* CurrentInputLayout();

unsigned DescribeInputLayout(unsigned long long pipeline_handle, char* buf, unsigned n);

HackerInputLayout* HackerInputLayoutOf(unsigned long long pipeline_handle);

HackerInputLayout* NewHackerInputLayout(const D3D11_INPUT_ELEMENT_DESC* elements, unsigned count);

} // namespace MigotoPort
