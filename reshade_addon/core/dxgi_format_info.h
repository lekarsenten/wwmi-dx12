#pragma once
// One table of what the addon needs to know about a DXGI format.
#include <dxgi.h>
#include <cstdint>

struct DxgiFormatInfo {
    uint8_t bytes; // bytes per element (buffer view) or pixel (plane 0); 0 = not a plain element
};

inline DxgiFormatInfo DxgiInfo(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return { 16 };
    case DXGI_FORMAT_R32G32B32A32_FLOAT: return { 16 };
    case DXGI_FORMAT_R32G32B32A32_UINT:
    case DXGI_FORMAT_R32G32B32A32_SINT: return { 16 };
    case DXGI_FORMAT_R32G32B32_TYPELESS: return { 12 };
    case DXGI_FORMAT_R32G32B32_FLOAT: return { 12 };
    case DXGI_FORMAT_R32G32B32_UINT:
    case DXGI_FORMAT_R32G32B32_SINT: return { 12 };
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return { 8 };
    case DXGI_FORMAT_R16G16B16A16_FLOAT: return { 8 };
    case DXGI_FORMAT_R16G16B16A16_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UINT:
    case DXGI_FORMAT_R16G16B16A16_SNORM:
    case DXGI_FORMAT_R16G16B16A16_SINT: return { 8 };
    case DXGI_FORMAT_R32G32_FLOAT: return { 8 };
    case DXGI_FORMAT_R32G8X24_TYPELESS: return { 4 };
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return { 4 };
    case DXGI_FORMAT_R10G10B10A2_UNORM: return { 4 };
    case DXGI_FORMAT_R11G11B10_FLOAT: return { 4 };
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return { 4 };
    case DXGI_FORMAT_R8G8B8A8_UNORM: return { 4 };
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return { 4 };
    case DXGI_FORMAT_B8G8R8A8_UNORM: return { 4 };
    case DXGI_FORMAT_R16G16_FLOAT: return { 4 };
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: return { 8 };
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: return { 4 };
    case DXGI_FORMAT_D32_FLOAT: return { 4 };
    case DXGI_FORMAT_R32_TYPELESS: return { 4 };
    case DXGI_FORMAT_R32_FLOAT: return { 4 };
    case DXGI_FORMAT_R32_UINT:
    case DXGI_FORMAT_R32_SINT: return { 4 };
    case DXGI_FORMAT_R24G8_TYPELESS: return { 4 };
    case DXGI_FORMAT_R8G8_UNORM: return { 2 };
    case DXGI_FORMAT_R16_TYPELESS: return { 2 };
    case DXGI_FORMAT_R16_FLOAT: return { 2 };
    case DXGI_FORMAT_R16_UNORM: return { 2 };
    case DXGI_FORMAT_R16_UINT: return { 2 };
    case DXGI_FORMAT_R8_UNORM: return { 1 };
    default: return { 0 };
    }
}
