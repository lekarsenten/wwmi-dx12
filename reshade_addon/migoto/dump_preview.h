#pragma once
// ===========================================================================
//  dump_preview.h -- OURS. PORT-LEAF. A .jpg beside every .dds their writer could only save as data.
//
//  Their Dump2DResource tries WIC (.jpg) and falls back to .dds when WIC has no encoding for the format:
//  every BC texture, the HDR scene and lighting buffers (R11G11B10_FLOAT, R16G16B16A16_FLOAT), the
//  16-bit targets. A capture is read as pictures. So the GPU samples such a texture into R8G8B8A8 --
//  the hardware decodes BC1-7, BC6H and the packed floats; float formats are tone-mapped x/(1+x) and
//  gamma 2.2 -- and their own WIC writer encodes that through a staging wrap. The .dds stays the data.
//  Recorded on the dump's readback queue, so the ordering after the game's work and the fence wait are
//  the ones every other dump copy already uses.
// ===========================================================================
#include <d3d11_1.h>
#include <cstddef>

namespace MigotoPort {

// DumpPreview.cso, handed over by the addon (which owns the cso directory) at init.
void SetDumpPreviewShader(const void* cso, size_t bytes);

bool DumpPreviewJpeg(ID3D11Device* dev11, ID3D11DeviceContext* ctx, ID3D11Texture2D* source, DXGI_FORMAT view_format,
    const wchar_t* path);

}
