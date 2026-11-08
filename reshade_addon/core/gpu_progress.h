#pragma once
#include <d3d12.h>
#include <cstdint>

void GpuProgressTick(ID3D12Device* dev, ID3D12CommandQueue* const* queues, unsigned n);
uint64_t GpuStamp();
bool GpuPassed(uint64_t stamp);
