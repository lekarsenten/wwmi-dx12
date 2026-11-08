#pragma once
#include <cstdint>

/* ---- THE PORT'S ONE TARGET VOCABULARY ------------------------------------------------------------

   Ported from 3Dmigoto, not invented. CommandList.h:707 declares ResourceCopyTargetType and
   CommandList.h:942 keys on it: ResourceCopyTarget : public SyntaxTarget { type; shader_type; slot; }.
   CommandList.cpp:7837 is the grammar that produces it -- `%lcs-t%u`, `%lcs-u%u`, `%lcs-cb%u`, `vb%u`,
   `ib` -- one enum covering every kind of thing an ini line can name.

   Invalid = 0 is theirs (ResourceCopyTargetType::INVALID).

   ONLY WHAT THE PORT ACTUALLY PRODUCES IS LISTED. Their enum also has RENDER_TARGET,
   DEPTH_STENCIL_TARGET, STREAM_OUTPUT, CUSTOM_RESOURCE and the pseudo-resources; nothing in this port
   constructs those yet, and an enumerator no code can produce is the same dead branch as an ini value
   that produces no action. Add one when its parser lands, not before. */
enum class TargetClass : uint8_t {
    Invalid = 0,
    ShaderResource = 1, // t#      -- ResourceCopyTargetType::SHADER_RESOURCE
    UnorderedAccessView = 2, // u#      -- ResourceCopyTargetType::UNORDERED_ACCESS_VIEW
    ConstantBuffer = 3, // b# / cb#-- ResourceCopyTargetType::CONSTANT_BUFFER
    VertexBuffer = 4, // vb#     -- ResourceCopyTargetType::VERTEX_BUFFER
    IndexBuffer = 5, // ib      -- ResourceCopyTargetType::INDEX_BUFFER
};

/* HOW WIDE THE ROOT-SIGNATURE REGISTER FILE IS FOR THIS CLASS, and 0 for a class that is not a
   root-signature register at all.

   NOT the ini grammar's limit. 3Dmigoto validates cs-t%u / cs-u%u / cs-cb%u against 128 / 64 / 14
   (D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, D3D11_1_UAV_SLOT_COUNT,
   D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT) -- that bounds what a MOD'S INI STRING MAY NAME.
   Sizing a runtime array from it produced a compile-clean buffer overflow (the 14 against loops still
   bounded by 16). What sizes these arrays is the port's own register map: LayoutRegMap t_cs[128],
   u_cs[16], b_cs[16] and CsCbvRoots reg[16]. Declared once here; every array and loop bound derives
   from it or from std::size of the array itself.

   VertexBuffer/IndexBuffer return 0 BECAUSE THEY ARE NOT REGISTERS. On D3D12 they are set through
   IASetVertexBuffers / IASetIndexBuffer, never through a root signature, so they have no row in
   LayoutRegMap and no slot in any register-file store. A caller that indexes an array by a class with
   width 0 gets a zero-length array and a compile error, which is the intended outcome. */
constexpr unsigned RegisterFileWidth(TargetClass c)
{
    switch (c) {
    case TargetClass::ShaderResource: return 128; // LayoutRegMap::t_cs
    case TargetClass::UnorderedAccessView: return 16; // LayoutRegMap::u_cs
    case TargetClass::ConstantBuffer: return 16; // LayoutRegMap::b_cs, CsCbvRoots::reg
    default: return 0; // vb / ib / Invalid: not a register
    }
}

/* ---- THE FLAT REGISTER FILE -------------------------------------------------------------------

   RegisterFileBase is the prefix sum of the widths, so (class, slot) maps to ONE index in ONE array.
   This is what lets a per-slot store hold a single `reg[kRegisterFileTotal]` instead of a member per
   class -- and that is the whole point, because a member per class forces a switch to pick between
   them and forces every walk to be written once per member.

   Layout today: t0..t127 at 0..127, u0..u15 at 128..143, b0..b15 at 144..159; vb and ib contribute
   nothing because they are not registers. Total 160. */
constexpr unsigned RegisterFileBase(TargetClass c)
{
    unsigned base = 0;
    for (unsigned i = 0; i < static_cast<unsigned>(c); ++i) base += RegisterFileWidth(static_cast<TargetClass>(i));
    return base;
}

constexpr unsigned kRegisterFileTotal =
    RegisterFileBase(TargetClass::IndexBuffer) + RegisterFileWidth(TargetClass::IndexBuffer);

constexpr unsigned kNoRegister = ~0u;

// (class, slot) -> flat index, or kNoRegister when the class has no register file or the slot is past
// its width. One bounds check, in one place, for every class.
constexpr unsigned RegisterIndex(TargetClass c, unsigned slot)
{
    return slot < RegisterFileWidth(c) ? RegisterFileBase(c) + slot : kNoRegister;
}

/* WHETHER A BINDING OF THIS CLASS IS NAMED BY GPU ADDRESS rather than indexed by the shader.

   A constant buffer is bound with SetComputeRootConstantBufferView, which takes a VA -- so a cb with no
   VA is unusable, while one whose size we failed to read is still perfectly bindable (the DX11 trace
   shows the chain running on exactly such a buffer). A t or u register is INDEXED by the shader, so it
   needs an extent instead. That difference is a property OF THE CLASS, so it is stated here as data
   rather than re-derived by a switch inside every accessor that has to honour it. */
constexpr bool RegisterBoundByAddress(TargetClass c)
{
    return c == TargetClass::ConstantBuffer;
}

// The canonical letter, for logs and for anything that still prints a register name.
constexpr char TargetClassChar(TargetClass c)
{
    switch (c) {
    case TargetClass::ShaderResource: return 't';
    case TargetClass::UnorderedAccessView: return 'u';
    case TargetClass::ConstantBuffer: return 'b';
    case TargetClass::VertexBuffer: return 'v';
    case TargetClass::IndexBuffer: return 'i';
    default: return '?';
    }
}
