// ===========================================================================
//  wwmi_runtime.cpp -- the two file/buffer helpers the addon's loaders share (WwmiReadFile,
//  WwmiMakeBuffer). See wwmi_runtime.h.
// ===========================================================================
#include "wwmi_runtime.h"
#include "../core/d3d12_buffer.h"
#include <d3d12.h>
#include <wrl.h>
#include <vector>
#include <cstdio>

using Microsoft::WRL::ComPtr;

std::vector<uint8_t> WwmiReadFile(const wchar_t* path)
{
    std::vector<uint8_t> b;
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"rb");
    if (!f) return b;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n > 0) {
        b.resize(n);
        fread(b.data(), 1, n, f);
    }
    fclose(f);
    return b;
}

ComPtr<ID3D12Resource> WwmiMakeBuffer(
    ID3D12Device* dev, UINT64 bytes, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_FLAGS flags, D3D12_RESOURCE_STATES state)
{
    ComPtr<ID3D12Resource> r;
    CreateD3D12Buffer(dev, heap, bytes, state, &r, flags);
    return r;
}
