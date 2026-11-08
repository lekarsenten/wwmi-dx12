// ===========================================================================
//  dxil_cb.h -- what a DXIL shader DECLARES at each constant-buffer register.
//
//  A character's bow: the translucent pass's VS declares cb3 = Primitive (720 bytes) where the
//  GBuffer VS declares cb3 = PreviousBones (12288); the port published every b3 as a 16 KB skeleton and
//  WWMI's `if vs-cb3 == 3381.7777` bound bones over Primitive -> LocalToWorld = bone matrices -> the bow
//  at the spawn position. Resource identity cannot tell the two apart: the game copies bones
//  into per-frame rings on non-GBuffer passes. The shader's own declaration can, and it lives only in
//  the DXIL part's LLVM bitcode metadata (`!dx.resources`, the CBV list: id, type, name, space, register,
//  range, SIZE IN BYTES) -- the container's PSV0 / RDAT parts carry no cbuffer sizes.
//
//  A self-contained reader of that one thing: the container -> DXIL part -> bitcode -> type table (for
//  the cbuffer's struct name, e.g. "hostlayout.Bones"), module constants (the i32 fields), metadata
//  (the resource records). No LLVM, no dxcompiler.dll: it runs on a customer's machine at pipeline
//  creation. Verified offline against dxc -dumpbin over captured shaders (tools/dxil_cb_test).
// ===========================================================================
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

struct DxilCbuf {
    uint32_t space = 0; // register space
    uint32_t reg = 0; // first register
    uint32_t count = 0; // range
    uint32_t bytes = 0; // declared size
    std::string name; // the cbuffer's variable name ("" for the DXBC-style layouts)
    std::string type; // its struct type name, e.g. "hostlayout.Bones"
};

bool DxilCbuffers(const void* bytes, size_t len, std::vector<DxilCbuf>& out, std::string* why = nullptr);
