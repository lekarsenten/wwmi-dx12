#pragma once
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdint>

// Shared small helpers.
std::vector<uint8_t> WwmiReadFile(const wchar_t* path);
Microsoft::WRL::ComPtr<ID3D12Resource> WwmiMakeBuffer(
    ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state);
