// ===========================================================================================
//  dx12_dump_readback.h -- OURS. PORT-LEAF: the synchronous staging readback their frame-analysis
//  engine assumes it has, because DX11's immediate context provided it for free.
//
//  Their engine does copy-then-Map(D3D11_MAP_READ) with nothing in between. DX12 has neither an
//  immediate context nor a blocking Map, so DumpReadbackList and DumpReadbackFlush supply the missing half:
//  a private DIRECT queue to record staging copies on, and a flush that submits and waits.
//
//  See dx12_dump_readback.cpp for why the blocking wait is correct here (their own engine pumps
//  input events mid-dump because it expects to stall for seconds) and why buffers are safe on a
//  private queue while textures are not (implicit COMMON promotion applies to buffers only).
// ===========================================================================================
#pragma once

#include <d3d12.h>

namespace MigotoPort {

ID3D12GraphicsCommandList* DumpReadbackList();

bool DumpReadbackFlush();
// A staging buffer's release: held until the open dump batch has flushed, retired like any resource if that
// batch went stale, released at once otherwise.
void RetireReadback(ID3D12Resource* res);

// Begin a new pre-drain batch (drops whatever the previous draw cached).
void PreDrainBegin();
void PreDrainAdd(ID3D12Resource* res);
// Execute the batch: ONE command list, ONE fence, then map once and keep the bytes.
void PreDrainFlush();
const void* PreDrainLookup(ID3D12Resource* res, unsigned* out_bytes, unsigned* out_row_pitch);

} // namespace MigotoPort
