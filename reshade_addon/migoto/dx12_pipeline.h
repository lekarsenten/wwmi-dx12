#pragma once
// ===========================================================================
//  dx12_pipeline.h -- OURS. PORT-LEAF. PUBLIC to the addon.
//  The DX11 pipeline state -> DX12 PSO transition.
//
//  This is the single biggest DX11->DX12 gap in the port, and it is a translation, not an emulation:
//
//    DX11: you SET things on the context (shaders, CBs, SRVs, blend, raster, viewport, VB/IB) and then
//          Draw. The driver assembles the pipeline behind your back.
//    DX12: there is no such state. A draw reads a PIPELINE STATE OBJECT plus a ROOT SIGNATURE plus
//          descriptor tables, all of which must exist before you record the draw.
//
//  So every `*Set*` from their engine RECORDS into the state block below, and Draw/Dispatch is where the
//  transition actually happens: build (or reuse) the PSO and root signature, write the descriptors, and
//  record the draw. That ordering is forced by DX12 -- it is not a shortcut.
//
//  Two hazards this file exists to respect, both learned the hard way in this project:
//
//   1. SetDescriptorHeaps INVALIDATES EVERY ROOT TABLE on the command list. Our work needs our own heap, so after issuing we MUST let the addon put
//      the game's heaps and every one of its root tables back -- hence Dx12Bridge::restore_game_state.
//   2. DX12 has no default scissor rect. A PSO and viewport that look perfect rasterise NOTHING if the
//      scissor is left empty, so IssueDraw always sets one.
// ===========================================================================

#include <d3d11_1.h>
#include <d3d12.h>
#include <cstdint>

namespace MigotoPort {

extern bool (*g_query_reloc)(unsigned long long res, unsigned long long* new_va, unsigned long long* new_bytes);

// D3D11 limits, so the state block holds exactly what a DX11 context could.
enum : unsigned {
    kStages = 6, // v h d g p c  (index order matches StageIndex below)
    kMaxCB = D3D11_COMMONSHADER_CONSTANT_BUFFER_API_SLOT_COUNT, // 14
    kMaxSRV = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT, // 128
    kMaxUAV = D3D11_PS_CS_UAV_REGISTER_COUNT, // 8
    kMaxSamp = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT, // 16
    kMaxRTV = D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT, // 8
    kMaxVB = D3D11_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT, // 32
};

inline int StageIndex(char stage)
{
    switch (stage) {
    case 'v': return 0;
    case 'h': return 1;
    case 'd': return 2;
    case 'g': return 3;
    case 'p': return 4;
    case 'c': return 5;
    }
    return -1;
}

struct ShimVbRecord {
    ID3D12Resource* res = nullptr; // identity + copy source; used only while this frame runs
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    UINT size = 0;
    UINT stride = 0;
};

struct LayoutOverride {
    unsigned long long pso = 0;
    const D3D11_INPUT_ELEMENT_DESC* elements = nullptr;
    UINT count = 0;
};

struct PipeState {
    ID3D11DeviceChild* shader[kStages] = {};
    ID3D11Buffer* cb[kStages][kMaxCB] = {};
    uint32_t cb_gen[kStages][kMaxCB] = {};
    ID3D11ShaderResourceView* srv[kStages][kMaxSRV] = {};
    uint32_t srv_gen[kStages][kMaxSRV] = {};
    ID3D11SamplerState* samp[kStages][kMaxSamp] = {};
    ID3D11UnorderedAccessView* uav[kMaxUAV] = {}; // CS uses u0..u7; OM UAVs land here too
    ID3D11RenderTargetView* rtv[kMaxRTV] = {};
    ID3D11DepthStencilView* dsv = nullptr;
    UINT num_rtv = 0;

    ID3D11Buffer* vb[kMaxVB] = {};
    UINT vb_stride[kMaxVB] = {};
    UINT vb_offset[kMaxVB] = {};
    UINT num_vb = 0;
    ShimVbRecord shim_vb[kMaxVB] = {}; // what we wrote to the real list; see above
    ID3D11Buffer* ib = nullptr;
    DXGI_FORMAT ib_format = DXGI_FORMAT_UNKNOWN;
    UINT ib_offset = 0;
    ID3D11InputLayout* layout = nullptr;
    LayoutOverride layout_override;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;

    ID3D11BlendState* blend = nullptr;
    FLOAT blend_factor[4] = { 1, 1, 1, 1 };
    UINT sample_mask = 0xffffffff;
    ID3D11DepthStencilState* depth = nullptr;
    UINT stencil_ref = 0;
    ID3D11RasterizerState* raster = nullptr;
    D3D11_VIEWPORT viewport = {};
    bool has_viewport = false;
    D3D11_RECT scissor = {};
    bool has_scissor = false;
};

PipeState& State();

typedef void (*ModDrawFn)(unsigned count, unsigned start, int base_vertex, unsigned instances, unsigned start_instance,
    bool indexed, void* user);
void SetModDrawCallback(ModDrawFn fn, void* user);
ModDrawFn ModDrawCallback(void** out_user);

bool IssueDraw(bool indexed, UINT count, UINT start, INT base_vertex, UINT instances, UINT start_instance);
bool IssueDispatch(UINT x, UINT y, UINT z);

void ArmPresentDrawProbe(int frames);
bool PresentDrawProbeArmed();
void PresentDrawProbeFrameDone();
unsigned long long OwnListDrawsIssued();

unsigned long long DispatchesIssued();
// Times IssueDispatch was ENTERED. calls - issued = dispatches abandoned at a gate; see DISPATCH-BAIL.
unsigned long long DispatchCalls();

void UploadFrameTick();
// Released by UploadFrameTick once the GPU has passed its last use.
void RetireDx12Resource(ID3D12Resource* res);
void SetRetireObserver(void (*fn)(ID3D12Resource* res)); // told of each, on the thread whose release it was
void SetTextureUseObserver(void (*fn)(ID3D12Resource* res)); // a 2D texture bound for one of our draws or dispatches
void NoteTextureUse(ID3D11Resource* res); // ... or a texture a mod's list assigns to a slot

bool ReflectShader(struct ShaderCode& sc);

void TransitionResource(ID3D12GraphicsCommandList* cl, ID3D11Resource* res, D3D12_RESOURCE_STATES to);

// Buffers OURS (mod geometry, relocations): their state per list, and the UAV barriers owed on them.
void OwnBuffer(ID3D12Resource* res);
void DisownBuffer(ID3D12Resource* res);
bool OwnedBuffer(ID3D12Resource* res);
D3D12_RESOURCE_STATES OwnedTransition(ID3D12GraphicsCommandList* cl, ID3D12Resource* res, D3D12_RESOURCE_STATES to);
void ListBuffersToCommon(ID3D12GraphicsCommandList* cl);
void FlushUavBarriers(ID3D12GraphicsCommandList* cl);
void ListBuffersForget(ID3D12GraphicsCommandList* cl);

D3D12_RESOURCE_STATES ListStateOf(ID3D12GraphicsCommandList* cl, struct WrapResource* r);
D3D12_GPU_VIRTUAL_ADDRESS StaticCbSnapshot(
    ID3D12GraphicsCommandList* cl, ID3D12Resource* src, UINT64 src_off, UINT bytes);

void SetBufCopyShader(const void* dxil, size_t bytes);
bool CopyBufferCs(ID3D12GraphicsCommandList* cl, struct WrapResource* dst, UINT64 dst_off, struct WrapResource* src,
    UINT64 src_off, UINT64 bytes);

void QueueUpload(ID3D11Resource* dst, const void* data, size_t bytes, uint64_t seq = 0);
void QueueTextureUpload(ID3D11Resource* dst, const D3D11_SUBRESOURCE_DATA* data, UINT subresources, uint64_t seq = 0);
void FlushPendingUploads(ID3D12GraphicsCommandList* cl);
bool UploadPendingFor(ID3D11Resource* res); // its copy is not recorded on a command list yet
uint64_t NextUploadSeq();
bool ListHoldsUpload(ID3D12GraphicsCommandList* cl, const struct WrapResource* wr);

// Descriptor heaps OURS alone. Never the game's shared bindless heap.
bool EnsureHeaps();
bool ShaderVisibleCopy(D3D12_CPU_DESCRIPTOR_HANDLE src, D3D12_GPU_DESCRIPTOR_HANDLE* out_gpu,
    D3D12_CPU_DESCRIPTOR_HANDLE* out_cpu, ID3D12GraphicsCommandList* cl);
D3D12_CPU_DESCRIPTOR_HANDLE AllocStagingDescriptor(char kind, UINT* out_slot);
void ReleaseStagingDescriptor(char kind, UINT slot);
// True once per SetDescriptorHeaps of ours: the game's heaps and root tables need putting back.
bool TakeHeapsChanged();


// GAME PIPELINE STATE INHERITANCE -- attempted, REMOVED, do not rebuild it this way.
// The idea: an injected draw should inherit the GAME's raster/depth (on DX11 it does, implicitly, by
// running in the live context), instead of the DX11 API defaults FillRaster/FillDepth fall back to.
// It cannot be sourced from ReShade: `rasterizer_state` / `depth_stencil_state` subobjects are NOT
// surfaced for the game's D3D12 pipelines (measured: raster=0 depth=0 on every graphics PSO in
// on_init_pipeline), and D3D12 offers no way to read state back off an ID3D12PipelineState. The only
// remaining route is intercepting ID3D12Device::CreateGraphicsPipelineState ourselves.

} // namespace MigotoPort
