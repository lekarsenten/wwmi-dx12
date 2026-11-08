#pragma once
// ib_hash.h -- IB-hash character recognition: a GPU crc32c of the vertex data the index buffer reaches,
// chained on the CPU with the synthesized D3D11_BUFFER_DESC to reproduce 3dmigoto's ini `hash=`. One
// dispatch per present, recorded into the present pass's list (its submit and fence are the frame's), the
// result cached per buffer until the buffer dies or its region is written over.
#include <d3d12.h>
#include <cstdint>

namespace IbHash {
bool Init(ID3D12Device* dev); // lazy; safe to call repeatedly
uint32_t Lookup(ID3D12Resource* ib_res, uint32_t ib_off, uint32_t ib_bytes, uint32_t idx_size, ID3D12Resource* vb_res,
    uint32_t vb_off, uint32_t vb_stride, bool grow = false);
// Lookup for a small mesh: queued behind every Lookup job. A vertex buffer outside a default heap is never hashed
// (the CPU rewrites it under the same address) and reads 0, as a pending one does.
uint32_t LookupSmall(ID3D12Resource* ib_res, uint32_t ib_off, uint32_t ib_bytes, uint32_t idx_size,
    ID3D12Resource* vb_res, uint32_t vb_off, uint32_t vb_stride);
void ForgetRange(uint64_t va, uint64_t bytes);
void DrainQueue(); // collects the last dispatch's result once its fence passed
bool Record(ID3D12GraphicsCommandList* cl); // records the next queued hash into cl; true = something recorded
void Submitted(ID3D12Fence* fence, uint64_t value); // the fence that completes the list Record wrote into
void LogInflight(const char* why);

}
