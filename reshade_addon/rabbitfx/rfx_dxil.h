// rfx_dxil.h -- OURS. The DX12 form of RabbitFX's [ShaderRegexMain] patch.
//
// On DX11 RabbitFX regex-patches the game's pixel shader ASSEMBLY. The DX12 game ships DXIL, and the
// same edit is possible one level up: dxcompiler.dll disassembles a DXIL container to LLVM IR text and
// ASSEMBLES such text back (IDxcAssembler), and dxil.dll validates and signs the result (IDxcValidator)
// -- the same signature every .cso we build with dxc carries. DXIL's discard, texture load / sample and
// output writes are plain calls, so the ini's straight-line injection needs no control flow: it goes in
// at the function's entry.
//
// Self-contained on purpose (windows + dxcapi + std only): tools\rfx_dxil_test.cpp compiles this file
// into an offline harness, so a patch can be proved against a dumped shader without the game.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

// A box of the render target in pixels, and the vertex colour range a pixel inside it is discarded for.
struct RfxDxilCut {
    float x0; // SV_Position.xy in [x0, x1) x [y0, y1)
    float y0;
    float x1;
    float y1;
    float lo[3]; // COLOR0.rgb in [lo, hi]
    float hi[3];
};

struct RfxDxilSpec {
    unsigned glow_reg = 50; // Texture2D: the GlowMap
    unsigned fx_reg = 51; // Texture2D: the FX map
    unsigned params_reg = 52; // Buffer<float4>: [0] = hue turns, sat bias, val bias, brightness (x217..w217)
    bool decensor = false;
    bool inject = true; // false: the decensor constants only, no SRVs added (the game's own root signature)
    std::vector<RfxDxilCut> cuts; // not empty: these cuts alone -- no RabbitFX blocks, no decensor
};

struct RfxDxilResult {
    std::vector<uint8_t> code; // the patched, validated, signed container
    std::string why; // on failure: the step and the reason; on success: what was resolved
    bool settled = false; // the outcome follows from the shader and the spec alone, so it can be kept on disk
    std::string ll_before; // the disassembly and the patched text, for the corpus dump
    std::string ll_after;
    unsigned srv_max_reg = 0;
    bool has_glow = false;
    std::string glow_why;
    unsigned decensored = 0; // COLOR0.w - 1/3 constants moved to -0.15
    unsigned degated = 0; // dither subtractions neutralised
    unsigned unmasked = 0; // FMin - 1/3 discard thresholds moved to +1
};

bool RfxDxilInit(const wchar_t* dll_dir, std::string& why);

bool RfxDxilPatch(const void* ps, size_t ps_size, const RfxDxilSpec& spec, RfxDxilResult& out);
