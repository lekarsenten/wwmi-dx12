// ===========================================================================================
//  frame_analysis_format.h -- THEIRS. The frame-dump FORMAT layer, ported from
//  3Dmigoto/DirectX11/FrameAnalysis.cpp lines 732-1548.
//
//  These write the .txt sidecars that wwmi-tools imports alongside the raw .buf files: the
//  buffer/VB/IB dumps and the dedupe filename rules. They take a D3D11_MAPPED_SUBRESOURCE --
//  which is only {pData, RowPitch, DepthPitch} -- so they are pure CPU code with no D3D11 API
//  calls and no member state, and port unchanged.
//
//  OUR half is everything they do NOT cover: getting the bytes off a DX12 DEFAULT-heap resource
//  (staging copy + fence + Map of a READBACK buffer) and then handing the mapped pointer here.
//  That split is the whole point -- the format is theirs, the transfer is the leaf.
//
//  Reference for the DX11 side of the transfer, when we build ours:
//    CreateStagingResource   FrameAnalysis.cpp:509
//    StageResource                            :623
//    DeferDumpBuffer / dump_deferred_resources:1549 / :1574   (stage now, write later --
//                                                              the same shape as our fence)
//    FrameAnalysisAfterDraw                   :2835           (dumps EVERY draw, no
//                                                              serialisation -- unlike ours)
// ===========================================================================================
#pragma once

#include <d3d11_1.h>
#include <d3dcommon.h>
#include <stdio.h>
#include <stdint.h>
#include <Strsafe.h>          // StringCchPrintfW / StringCchPrintfExW, as theirs uses

#include "DrawCallInfo.h"
#include "../core/log.h"

errno_t wfopen_ensuring_access(FILE** pFile, const wchar_t *filename, const wchar_t *mode);

// Theirs routes these through FrameAnalysisContext::FrameAnalysisLog, a MEMBER writing to the
// per-frame analysis log (DirectX9/FrameAnalysisFunctions.h:118-125). We ported the format layer as
// free functions, so there is no member to call -- they go to our addon log instead. Defined HERE
// rather than in the generated .cpp so re-running the extractor cannot drop them.
#define FALogInfo(fmt, ...) logger::write("FA: " fmt, __VA_ARGS__)
#define FALogErr(fmt, ...)  logger::write("FA ERROR: " fmt, __VA_ARGS__)

namespace FrameAnalysisFormat {

void dedupe_buf_filename_txt(const wchar_t *bin_filename,
		wchar_t *txt_filename, size_t size, char type, int idx,
		UINT stride, UINT offset);

void DumpBufferTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, char type, int idx, UINT stride, UINT offset);

void dedupe_buf_filename_vb_txt(const wchar_t *bin_filename,
		wchar_t *txt_filename, size_t size, int idx, UINT stride,
		UINT offset, UINT first, UINT count, ID3DBlob *layout,
		D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info);

void DumpVBTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, int slot, UINT stride, UINT offset, UINT first, UINT count, ID3DBlob *layout,
		D3D11_PRIMITIVE_TOPOLOGY topology, DrawCallInfo *call_info);

void dedupe_buf_filename_ib_txt(const wchar_t *bin_filename,
		wchar_t *txt_filename, size_t size, DXGI_FORMAT ib_fmt,
		UINT offset, UINT first, UINT count, D3D11_PRIMITIVE_TOPOLOGY topology);

void DumpIBTxt(wchar_t *filename, D3D11_MAPPED_SUBRESOURCE *map,
		UINT size, DXGI_FORMAT format, UINT offset, UINT first, UINT count,
		D3D11_PRIMITIVE_TOPOLOGY topology);

// Template in theirs (any D3D11_*_DESC); kept as one so the reference body compiles unchanged.
template <typename DescType>
void DumpDesc(DescType *desc, const wchar_t *filename);

} // namespace FrameAnalysisFormat
