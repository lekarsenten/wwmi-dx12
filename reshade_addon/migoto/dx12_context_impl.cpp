// ===========================================================================
//  dx12_context_impl -- OURS. PORT-LEAF. The DX11->DX12 transitions, written by hand.
//
//  dx12_context.cpp is GENERATED (signatures come from the SDK header, defaults are loud
//  E_NOTIMPL/no-ops). This file holds the methods that actually do something, so regenerating the
//  skeleton never overwrites real work. See docs/PORT-DX11-TO-DX12-TRANSITIONS.md for the full map.
//
//  The shape of the whole file follows from one fact: DX11 pipeline state is SET on the context and DX12
//  has none. So every `*Set*` here RECORDS into PipeState (dx12_pipeline.h) and Draw/Dispatch is where
//  the state becomes a PSO, a root signature and descriptors. That ordering is DX12's requirement, not a
//  shortcut -- a PSO cannot be built until every input to it is known.
// ===========================================================================
#include "dx12_context.h"
#include "dx12_context_impl.h"
#include "dx12_pipeline.h"
#include "dx12_present_snap.h"
#include "dx12_wrap.h"
#include "command_observer.h"
#include "shim_unmet.h"
#include "globals.h" // G->track_texture_updates -- their gate on hash propagation
#include "ResourceHash.h" // PropagateResourceHash / UpdateResourceHashFromCPU (HackerContext.cpp: CopyResource / UpdateSubresource)
#include "dx12_dump_readback.h" // the synchronous staging readback their dump engine assumes
#include "dump_watchdog.h" // DumpStage: the Map(READ) / CopyResource leaves report their position
#include "dx12_input_layout.h" // CurrentInputLayout, for IAGetInputLayout
#include "prof_core.h" // ProfGpuWork -- the performance report's work rows
#include "../instrumentation/gpu_timer.h" // GT_ShimCopy -- the performance monitor's graphics card rows
#include "../instrumentation/diag_gate.h" // DiagOnce -- COPY-WHAT, one line per destination
#include "../core/log.h"
#include "../core/d3d12_barrier.h"
#include "../instrumentation/removal_pin.h" // REMOVAL-PIN tally: the copies this file records on the game's list

#include <atomic> // COPY-REFUSED/COPY-OK counters in CopyResource
#include <mutex>
#include <vector>

namespace MigotoPort {

// ---- binding a slot holds a REFERENCE ---------------------------------------------------------
// A real D3D11 context AddRefs everything bound to it and Releases it when the slot is overwritten or
// unbound, and their engine is written against exactly that contract:
//
//     ResourceCopyTarget::SetResource, RENDER_TARGET (CommandList.cpp)
//         OMGetRenderTargets(...)          <- takes a reference on each bound view
//         render_view[slot] = view
//         OMSetRenderTargets(...)
//         for (...) render_view[i]->Release()
//
// The views their engine creates are owned by nothing else, so a slot that held no reference would point
// at freed memory after their first Release -- and the next `o0 = null`
// (BuiltInCommandListUnbindAllRenderTargets) would AddRef it.
namespace {

// Their command lists run under the engine lock (PortApi::EngineLock), so plain counters are enough here.
static uint32_t g_srv_write_gen = 0;
static uint32_t g_cb_write_gen = 0;

void SetSrvs(char stage, UINT start, UINT num, ID3D11ShaderResourceView* const* views)
{
    PipeState& st = State();
    const int si = StageIndex(stage);
    for (UINT i = 0; i < num; ++i) {
        ID3D11ShaderResourceView* v = views ? views[i] : nullptr;
        const UINT slot = start + i;
        if (si >= 0 && slot < kMaxSRV) {
            BindSlot(st.srv[si][slot], v);
            st.srv_gen[si][slot] = ++g_srv_write_gen;
        }
    }
}

// A texture slot of the draw's game stages that their list reads, not written since the draw scope opened, holds the
// game's texture there (DX11: their lists run in the game's state); the addon publishes it at the first read.
void InheritSrvsForRead(char stage, UINT start, UINT num)
{
    static thread_local bool t_publishing = false; // PushGameTexture reads the slot back
    const Dx12Bridge* br = BridgeScope();
    if (!br || !br->game.publish_srv || t_publishing || (stage != 'v' && stage != 'p')) return;
    PipeState& st = State();
    const int si = StageIndex(stage);
    t_publishing = true;
    for (UINT slot = start; slot < kMaxSRV && slot - start < num; ++slot)
        if ((int32_t)(st.srv_gen[si][slot] - br->game.srv_gen) <= 0) br->game.publish_srv(br->game.user, stage, slot);
    t_publishing = false;
}

static bool VaAndSize(ID3D11Buffer* b, UINT offset, D3D12_GPU_VIRTUAL_ADDRESS* va, UINT* size)
{
    WrapResource* wr = AsWrapResource(b);
    if (!wr || !wr->dx12) return false;
    unsigned long long rva = 0;
    unsigned long long rbytes = 0;
    if (!wr->gpu_va && g_query_reloc && g_query_reloc((unsigned long long)wr->dx12, &rva, &rbytes)) {
        if (rbytes > offset) {
            *va = (D3D12_GPU_VIRTUAL_ADDRESS)(rva + offset);
            *size = (UINT)(rbytes - offset);
            return true;
        }
        return false; // the view starts past the replacement: refuse, never guess
    }
    const D3D12_RESOURCE_DESC rd = wr->dx12->GetDesc();
    if (rd.Width <= offset) return false;
    const D3D12_GPU_VIRTUAL_ADDRESS base = wr->gpu_va ? wr->gpu_va : wr->dx12->GetGPUVirtualAddress();
    *va = base + offset;
    *size = (UINT)(rd.Width - offset);
    return true;
}

void SetCbs(char stage, UINT start, UINT num, ID3D11Buffer* const* bufs)
{
    PipeState& st = State();
    const int si = StageIndex(stage);
    if (si < 0) return;
    for (UINT i = 0; i < num; ++i) {
        const UINT slot = start + i;
        if (slot < kMaxCB) {
            BindSlot(st.cb[si][slot], bufs ? bufs[i] : nullptr);
            st.cb_gen[si][slot] = ++g_cb_write_gen;
        }
    }
}

void SetSamplers(char stage, UINT start, UINT num, ID3D11SamplerState* const* ss)
{
    PipeState& st = State();
    const int si = StageIndex(stage);
    if (si < 0) return;
    for (UINT i = 0; i < num; ++i) {
        const UINT slot = start + i;
        if (slot < kMaxSamp) BindSlot(st.samp[si][slot], ss ? ss[i] : nullptr);
    }
}

} // namespace

uint32_t SrvWriteGen()
{
    return g_srv_write_gen;
}

uint32_t CbWriteGen()
{
    return g_cb_write_gen;
}

bool ShimBufferForCompute(
    ID3D12GraphicsCommandList* cl, char kind, unsigned slot, ShimBufView* out, const char** why, void* object)
{
    const char* dummy = nullptr;
    if (!why) why = &dummy;
    if (!cl || !out) {
        *why = "no list";
        return false;
    }
    const PipeState& st = State();
    ID3D11Buffer* b = nullptr;
    UINT off = 0;
    UINT stride = 0;
    D3D12_RESOURCE_STATES to = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
    if (kind == 'v' && slot < kMaxVB) {
        b = st.vb[slot];
        off = st.vb_offset[slot];
        stride = st.vb_stride[slot];
        to |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    } else if (kind == 'c' && slot < kMaxCB) {
        b = st.cb[0][slot];
        to |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    } else if (kind == 'i') {
        b = st.ib;
        off = st.ib_offset;
        stride = (st.ib_format == DXGI_FORMAT_R16_UINT) ? 2u : 4u;
        to |= D3D12_RESOURCE_STATE_INDEX_BUFFER;
    } else if (kind == 'o') {
        b = static_cast<ID3D11Buffer*>(object);
        to |= D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER;
    } else {
        *why = "bad kind/slot";
        return false;
    }
    if (!b) {
        *why = "nothing bound";
        return false;
    }
    WrapResource* wr = AsWrapResource(b);
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    UINT sz = 0;
    if (!wr || !wr->dx12 || !VaAndSize(b, off, &va, &sz)) {
        *why = "no VA";
        return false;
    }
    const D3D12_GPU_VIRTUAL_ADDRESS base = wr->dx12->GetGPUVirtualAddress();
    const UINT64 width = wr->dx12->GetDesc().Width;
    if (va < base || va >= base + width) {
        *why = "relocated (replacement not tracked)";
        return false;
    }
    if (va + sz > base + width) sz = (UINT)(base + width - va);
    D3D12_HEAP_PROPERTIES hp{};
    D3D12_HEAP_FLAGS hf{};
    const bool upload = SUCCEEDED(wr->dx12->GetHeapProperties(&hp, &hf)) && hp.Type == D3D12_HEAP_TYPE_UPLOAD;
    if (!upload) TransitionResource(cl, b, to);
    out->res = wr->dx12;
    out->va = va;
    out->size = sz;
    out->stride = stride;
    out->state = upload ? (unsigned)D3D12_RESOURCE_STATE_GENERIC_READ : (unsigned)wr->state;
    return true;
}

void* AcquireShimVsCb(unsigned slot)
{
    if (slot >= kMaxCB) return nullptr;
    ID3D11Buffer* b = State().cb[0][slot];
    if (b) b->AddRef();
    return b;
}

void ReleaseShimObject(void* object)
{
    if (object) static_cast<ID3D11Buffer*>(object)->Release();
}

bool ShimObjectForeign(void* object)
{
    WrapResource* wr = object ? AsWrapResource(static_cast<ID3D11Buffer*>(object)) : nullptr;
    return !wr || wr->foreign;
}

static void ReadyForGameDraw(ID3D12GraphicsCommandList* cl, ID3D11Resource* res, D3D12_RESOURCE_STATES to)
{
    WrapResource* w = AsWrapResource(res);
    if (w && w->dx12 && OwnedBuffer(w->dx12)) OwnedTransition(cl, w->dx12, to);
}

void STDMETHODCALLTYPE Dx12Context::VSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{
    SetSrvs('v', s, n, v);
}
void STDMETHODCALLTYPE Dx12Context::HSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{
    SetSrvs('h', s, n, v);
}
void STDMETHODCALLTYPE Dx12Context::DSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{
    SetSrvs('d', s, n, v);
}
void STDMETHODCALLTYPE Dx12Context::GSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{
    SetSrvs('g', s, n, v);
}
void STDMETHODCALLTYPE Dx12Context::PSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{
    SetSrvs('p', s, n, v);
}
void STDMETHODCALLTYPE Dx12Context::CSSetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView* const* v)
{
    SetSrvs('c', s, n, v);
}

void STDMETHODCALLTYPE Dx12Context::VSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b)
{
    SetCbs('v', s, n, b);
    const Dx12Bridge& br = GetDx12Bridge();
    if (!br.cl || !br.vs_cb_root) return;
    PipeState& st = State();
    for (UINT i = 0; i < n; ++i) {
        const UINT slot = s + i;
        if (slot >= kMaxCB || !st.cb[0][slot]) continue;
        const int root = br.vs_cb_root(br.vs_cb_root_user, slot);
        if (root < 0 || root >= 64) continue;
        D3D12_GPU_VIRTUAL_ADDRESS va = 0;
        UINT sz = 0;
        if (VaAndSize(st.cb[0][slot], 0, &va, &sz)) {
            WrapResource* w = AsWrapResource(st.cb[0][slot]);
            const D3D12_GPU_VIRTUAL_ADDRESS snap = (w && w->dx12 && OwnedBuffer(w->dx12))
                ? StaticCbSnapshot(br.cl, w->dx12, va - w->dx12->GetGPUVirtualAddress(), sz)
                : 0;
            if (!snap) ReadyForGameDraw(br.cl, st.cb[0][slot], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            br.cl->SetGraphicsRootConstantBufferView((UINT)root, snap ? snap : va);
        }
    }
}
void STDMETHODCALLTYPE Dx12Context::HSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b)
{
    SetCbs('h', s, n, b);
}
void STDMETHODCALLTYPE Dx12Context::DSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b)
{
    SetCbs('d', s, n, b);
}
void STDMETHODCALLTYPE Dx12Context::GSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b)
{
    SetCbs('g', s, n, b);
}
void STDMETHODCALLTYPE Dx12Context::PSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b)
{
    SetCbs('p', s, n, b);
}
void STDMETHODCALLTYPE Dx12Context::CSSetConstantBuffers(UINT s, UINT n, ID3D11Buffer* const* b)
{
    SetCbs('c', s, n, b);
}

void STDMETHODCALLTYPE Dx12Context::VSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* ss)
{
    SetSamplers('v', s, n, ss);
}
void STDMETHODCALLTYPE Dx12Context::HSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* ss)
{
    SetSamplers('h', s, n, ss);
}
void STDMETHODCALLTYPE Dx12Context::DSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* ss)
{
    SetSamplers('d', s, n, ss);
}
void STDMETHODCALLTYPE Dx12Context::GSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* ss)
{
    SetSamplers('g', s, n, ss);
}
void STDMETHODCALLTYPE Dx12Context::PSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* ss)
{
    SetSamplers('p', s, n, ss);
}
void STDMETHODCALLTYPE Dx12Context::CSSetSamplers(UINT s, UINT n, ID3D11SamplerState* const* ss)
{
    SetSamplers('c', s, n, ss);
}

// ---- shaders ----------------------------------------------------------------------------------
// ClassInstances are D3D11 shader-interface plumbing with no DX12 counterpart and no user in the mod
// library; a mod that used them would need the whole interface mechanism, not a shim, so it is named.
namespace {
void SetShader(char stage, ID3D11DeviceChild* sh, UINT num_class_instances, const char* what)
{
    const int si = StageIndex(stage);
    if (si >= 0) BindSlot(State().shader[si], sh);
    if (num_class_instances) NotImplOnce(what);
}
} // namespace

void STDMETHODCALLTYPE Dx12Context::VSSetShader(ID3D11VertexShader* s, ID3D11ClassInstance* const*, UINT n)
{
    SetShader('v', s, n, "VSSetShader with class instances (D3D11 shader interfaces have no DX12 analogue)");
}
void STDMETHODCALLTYPE Dx12Context::HSSetShader(ID3D11HullShader* s, ID3D11ClassInstance* const*, UINT n)
{
    SetShader('h', s, n, "HSSetShader with class instances");
}
void STDMETHODCALLTYPE Dx12Context::DSSetShader(ID3D11DomainShader* s, ID3D11ClassInstance* const*, UINT n)
{
    SetShader('d', s, n, "DSSetShader with class instances");
}
void STDMETHODCALLTYPE Dx12Context::GSSetShader(ID3D11GeometryShader* s, ID3D11ClassInstance* const*, UINT n)
{
    SetShader('g', s, n, "GSSetShader with class instances");
}
void STDMETHODCALLTYPE Dx12Context::PSSetShader(ID3D11PixelShader* s, ID3D11ClassInstance* const*, UINT n)
{
    SetShader('p', s, n, "PSSetShader with class instances");
}
void STDMETHODCALLTYPE Dx12Context::CSSetShader(ID3D11ComputeShader* s, ID3D11ClassInstance* const*, UINT n)
{
    SetShader('c', s, n, "CSSetShader with class instances");
}

void STDMETHODCALLTYPE Dx12Context::CSSetUnorderedAccessViews(
    UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView* const* ppUAVs, const UINT* pUAVInitialCounts)
{
    PipeState& st = State();
    for (UINT i = 0; i < NumUAVs; ++i) {
        const UINT slot = StartSlot + i;
        if (slot < kMaxUAV) BindSlot(st.uav[slot], ppUAVs ? ppUAVs[i] : nullptr);
    }
    // Initial counts belong to append/consume buffers, which need a counter resource we do not create.
    if (pUAVInitialCounts) {
        for (UINT i = 0; i < NumUAVs; ++i) {
            if (pUAVInitialCounts[i] != (UINT)-1) {
                NotImplOnce("CSSetUnorderedAccessViews with an initial count "
                            "(DX12 append/consume needs a separate counter resource)");
                break;
            }
        }
    }
}

void STDMETHODCALLTYPE Dx12Context::CSGetUnorderedAccessViews(
    UINT StartSlot, UINT NumUAVs, ID3D11UnorderedAccessView** ppUAVs)
{
    PipeState& st = State();
    if (!ppUAVs) return;
    for (UINT i = 0; i < NumUAVs; ++i) {
        const UINT slot = StartSlot + i;
        ID3D11UnorderedAccessView* v = (slot < kMaxUAV) ? st.uav[slot] : nullptr;
        ppUAVs[i] = v;
        if (v) v->AddRef(); // Get* returns a reference; their code Releases it
    }
}

// ---- input assembler --------------------------------------------------------------------------
void STDMETHODCALLTYPE Dx12Context::IASetInputLayout(ID3D11InputLayout* l)
{
    BindSlot(State().layout, l);
}

void STDMETHODCALLTYPE Dx12Context::IASetVertexBuffers(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer* const* ppVB, const UINT* pStrides, const UINT* pOffsets)
{
    PipeState& st = State();
    for (UINT i = 0; i < NumBuffers; ++i) {
        const UINT slot = StartSlot + i;
        if (slot >= kMaxVB) break;
        BindSlot(st.vb[slot], ppVB ? ppVB[i] : nullptr);
        // S17 tap: which PipeState the bind landed on, and whether it bound anything at all.
        NoteVbBind((const void*)&st, st.vb[slot] ? 1 : 0);
        st.vb_stride[slot] = pStrides ? pStrides[i] : 0;
        st.vb_offset[slot] = pOffsets ? pOffsets[i] : 0;
        if (st.vb[slot] && slot + 1 > st.num_vb) st.num_vb = slot + 1;

        // ...and through to the real pipeline, exactly as theirs does.
        const Dx12Bridge& b = GetDx12Bridge();
        D3D12_GPU_VIRTUAL_ADDRESS va = 0;
        UINT sz = 0;
        const bool have_va = st.vb[slot] && VaAndSize(st.vb[slot], st.vb_offset[slot], &va, &sz);
        bool explicit_null = false; // the K8 line below must say what actually reached the list
        if (b.cl && have_va) {
            D3D12_VERTEX_BUFFER_VIEW v{ va, sz, st.vb_stride[slot] };
            ReadyForGameDraw(b.cl, st.vb[slot], D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
            b.cl->IASetVertexBuffers(slot, 1, &v);
            WrapResource* wrv = AsWrapResource(st.vb[slot]);
            st.shim_vb[slot] = { wrv ? wrv->dx12 : nullptr, va, sz, st.vb_stride[slot] };
        } else {
            st.shim_vb[slot] = {}; // nothing written through: the game's own VB is what draws
            if (b.cl && !st.vb[slot]) {
                const D3D12_VERTEX_BUFFER_VIEW nullv{ 0, 0, 0 };
                b.cl->IASetVertexBuffers(slot, 1, &nullv);
                NoteVbNullUnbind();
                explicit_null = true;
            }
        }
        if (st.vb[slot] && !(b.cl && have_va) && logger::enabled(1)) { // ShimUnmet is log-only
            wchar_t d[64];
            _snwprintf_s(d, _TRUNCATE, L"vb%u", slot);
            g_shim_unmet.Note(!b.cl ? L"no DX12 command list published" : L"no VA for the wrapped buffer", d);
        }
        if (g_command_observer.Active()) {
            wchar_t line[256];
            if (b.cl && have_va)
                _snwprintf_s(line, _TRUNCATE, L"  LEAF   vb%u -> game list: va %016llX size %u stride %u", slot,
                    (unsigned long long)va, sz, st.vb_stride[slot]);
            else if (explicit_null)
                _snwprintf_s(line, _TRUNCATE,
                    L"  LEAF   vb%u = null -> game list: EXPLICIT UNBIND (zeroed vertex-buffer view)", slot);
            else
                _snwprintf_s(line, _TRUNCATE, L"  LEAF   vb%u NOT WRITTEN THROUGH (%s) -- game's own VB stays bound",
                    slot,
                    !b.cl ? L"no DX12 command list published"
                          : (st.vb[slot] ? L"no VA for the wrapped buffer" : L"no VB bound"));
            g_command_observer.Observe(line);
        }
    }
}

void STDMETHODCALLTYPE Dx12Context::IASetIndexBuffer(ID3D11Buffer* pIB, DXGI_FORMAT Format, UINT Offset)
{
    PipeState& st = State();
    BindSlot(st.ib, pIB);
    st.ib_format = Format;
    st.ib_offset = Offset;

    const Dx12Bridge& b = GetDx12Bridge();
    D3D12_GPU_VIRTUAL_ADDRESS va = 0;
    UINT sz = 0;
    const bool have_va = st.ib && VaAndSize(st.ib, st.ib_offset, &va, &sz);
    if (b.cl && have_va) {
        D3D12_INDEX_BUFFER_VIEW v{};
        v.BufferLocation = va;
        v.SizeInBytes = sz;
        v.Format = (Format == DXGI_FORMAT_UNKNOWN) ? DXGI_FORMAT_R32_UINT : Format;
        ReadyForGameDraw(b.cl, st.ib, D3D12_RESOURCE_STATE_INDEX_BUFFER);
        b.cl->IASetIndexBuffer(&v);
    }
    if (st.ib && !(b.cl && have_va))
        g_shim_unmet.Note(!b.cl ? L"no DX12 command list published" : L"no VA for the wrapped buffer", L"ib");
    if (g_command_observer.Active()) {
        wchar_t line[256];
        if (b.cl && have_va)
            _snwprintf_s(line, _TRUNCATE, L"  LEAF   ib -> game list: va %016llX size %u fmt %u",
                (unsigned long long)va, sz, (unsigned)Format);
        else
            _snwprintf_s(line, _TRUNCATE, L"  LEAF   ib NOT WRITTEN THROUGH (%s) -- game's own IB stays bound",
                !b.cl ? L"no DX12 command list published" : (st.ib ? L"no VA for the wrapped buffer" : L"no IB bound"));
        g_command_observer.Observe(line);
    }
}

void STDMETHODCALLTYPE Dx12Context::IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY t)
{
    State().topology = t;
}

// ---- output merger ---------------------------------------------------------------------------
void STDMETHODCALLTYPE Dx12Context::OMSetRenderTargets(
    UINT NumViews, ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV)
{
    PipeState& st = State();
    st.num_rtv = 0;
    for (UINT i = 0; i < kMaxRTV; ++i) {
        BindSlot(st.rtv[i], (i < NumViews && ppRTVs) ? ppRTVs[i] : nullptr);
        if (st.rtv[i]) st.num_rtv = i + 1;
    }
    BindSlot(st.dsv, pDSV);
}

void STDMETHODCALLTYPE Dx12Context::OMSetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs,
    ID3D11RenderTargetView* const* ppRTVs, ID3D11DepthStencilView* pDSV, UINT UAVStartSlot, UINT NumUAVs,
    ID3D11UnorderedAccessView* const* ppUAVs, const UINT* pUAVInitialCounts)
{
    // D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL means "leave the RTs alone, only change UAVs".
    if (NumRTVs != D3D11_KEEP_RENDER_TARGETS_AND_DEPTH_STENCIL) OMSetRenderTargets(NumRTVs, ppRTVs, pDSV);
    if (NumUAVs != D3D11_KEEP_UNORDERED_ACCESS_VIEWS) {
        PipeState& st = State();
        for (UINT i = 0; i < NumUAVs; ++i) {
            const UINT slot = UAVStartSlot + i;
            if (slot < kMaxUAV) BindSlot(st.uav[slot], ppUAVs ? ppUAVs[i] : nullptr);
        }
        (void)pUAVInitialCounts;
    }
}

void STDMETHODCALLTYPE Dx12Context::OMSetBlendState(
    ID3D11BlendState* pBlendState, const FLOAT BlendFactor[4], UINT SampleMask)
{
    PipeState& st = State();
    BindSlot(st.blend, pBlendState);
    if (BlendFactor) {
        for (int i = 0; i < 4; ++i) st.blend_factor[i] = BlendFactor[i];
    } else {
        for (int i = 0; i < 4; ++i) st.blend_factor[i] = 1.0f;
    } // D3D11's default
    st.sample_mask = SampleMask;
}

void STDMETHODCALLTYPE Dx12Context::OMSetDepthStencilState(ID3D11DepthStencilState* pDSS, UINT StencilRef)
{
    PipeState& st = State();
    BindSlot(st.depth, pDSS);
    st.stencil_ref = StencilRef;
}

// ---- rasteriser -----------------------------------------------------------------------------
void STDMETHODCALLTYPE Dx12Context::RSSetState(ID3D11RasterizerState* rs)
{
    BindSlot(State().raster, rs);
}

void STDMETHODCALLTYPE Dx12Context::RSSetViewports(UINT NumViewports, const D3D11_VIEWPORT* pViewports)
{
    PipeState& st = State();
    if (!NumViewports || !pViewports) {
        st.has_viewport = false;
        return;
    }
    st.viewport = pViewports[0];
    st.has_viewport = true;
    if (NumViewports > 1) NotImplOnce("RSSetViewports with more than one viewport");
}

void STDMETHODCALLTYPE Dx12Context::RSSetScissorRects(UINT NumRects, const D3D11_RECT* pRects)
{
    PipeState& st = State();
    if (!NumRects || !pRects) {
        st.has_scissor = false;
        return;
    }
    st.scissor = pRects[0];
    st.has_scissor = true;
}

void STDMETHODCALLTYPE Dx12Context::RSGetScissorRects(UINT* pNumRects, D3D11_RECT* pRects)
{
    if (!pNumRects) return;
    PipeState& st = State();
    if (!pRects) {
        *pNumRects = st.has_scissor ? 1u : 0u;
        return;
    }
    if (*pNumRects == 0) return;
    if (st.has_scissor) {
        pRects[0] = st.scissor;
        *pNumRects = 1;
    } else {
        *pNumRects = 0;
    }
}

static bool DispatchToAddon(bool indexed, UINT count, UINT start, INT base, UINT instances, UINT start_instance)
{
    void* user = nullptr;
    ModDrawFn fn = ModDrawCallback(&user);
    if (!fn || AsShaderCode(State().shader[0])) return false;
    PayGameState();
    fn(count, start, base, instances, start_instance, indexed, user);
    return true;
}

static void IssueDrawSnapped(
    ID3D11DeviceContext* ctx, bool indexed, UINT count, UINT start, INT base, UINT instances, UINT start_instance)
{
    g_present_snapshots.BeforeDraw(ctx, State());
    IssueDraw(indexed, count, start, base, instances, start_instance);
    g_present_snapshots.AfterDraw(State());
}

void STDMETHODCALLTYPE Dx12Context::DrawIndexed(UINT IndexCount, UINT StartIndexLocation, INT BaseVertexLocation)
{
    if (DispatchToAddon(true, IndexCount, StartIndexLocation, BaseVertexLocation, 1, 0)) return;
    IssueDrawSnapped(this, true, IndexCount, StartIndexLocation, BaseVertexLocation, 1, 0);
}

void STDMETHODCALLTYPE Dx12Context::Draw(UINT VertexCount, UINT StartVertexLocation)
{
    if (DispatchToAddon(false, VertexCount, StartVertexLocation, 0, 1, 0)) return;
    IssueDrawSnapped(this, false, VertexCount, StartVertexLocation, 0, 1, 0);
}

void STDMETHODCALLTYPE Dx12Context::DrawIndexedInstanced(UINT IndexCountPerInstance, UINT InstanceCount,
    UINT StartIndexLocation, INT BaseVertexLocation, UINT StartInstanceLocation)
{
    if (DispatchToAddon(
            true, IndexCountPerInstance, StartIndexLocation, BaseVertexLocation, InstanceCount, StartInstanceLocation))
        return;
    IssueDrawSnapped(this, true, IndexCountPerInstance, StartIndexLocation, BaseVertexLocation, InstanceCount,
        StartInstanceLocation);
}

void STDMETHODCALLTYPE Dx12Context::DrawInstanced(
    UINT VertexCountPerInstance, UINT InstanceCount, UINT StartVertexLocation, UINT StartInstanceLocation)
{
    if (DispatchToAddon(false, VertexCountPerInstance, StartVertexLocation, 0, InstanceCount, StartInstanceLocation))
        return;
    IssueDrawSnapped(this, false, VertexCountPerInstance, StartVertexLocation, 0, InstanceCount, StartInstanceLocation);
}

void STDMETHODCALLTYPE Dx12Context::Dispatch(UINT ThreadGroupCountX, UINT ThreadGroupCountY, UINT ThreadGroupCountZ)
{
    if (g_present_redraw) return;
    for (ID3D11UnorderedAccessView* u : State().uav)
        if (WrapView* v = u ? AsWrapView(u) : nullptr) g_present_snapshots.NoteWrite(v->res);
    NoteEmittedDispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
    IssueDispatch(ThreadGroupCountX, ThreadGroupCountY, ThreadGroupCountZ);
}

HRESULT STDMETHODCALLTYPE Dx12Context::Map(ID3D11Resource* pResource, UINT Subresource, D3D11_MAP MapType,
    UINT MapFlags, D3D11_MAPPED_SUBRESOURCE* pMappedResource)
{
    (void)Subresource;
    (void)MapFlags;
    if (pMappedResource) *pMappedResource = D3D11_MAPPED_SUBRESOURCE{}; // pData = nullptr, pitches = 0

    if (pMappedResource && (MapType == D3D11_MAP_WRITE_DISCARD || MapType == D3D11_MAP_WRITE)) {
        WrapResource* wr = AsWrapResource(pResource);
        if (wr && wr->dx12) {
            const D3D12_RESOURCE_DESC rd = wr->dx12->GetDesc();
            UINT64 bytes = 0;
            UINT64 row = 0;
            if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
                bytes = rd.Width;
                row = rd.Width;
            } else {
                UINT bpp = 0;
                switch (rd.Format) {
                case DXGI_FORMAT_R32G32B32A32_FLOAT:
                case DXGI_FORMAT_R32G32B32A32_UINT:
                case DXGI_FORMAT_R32G32B32A32_SINT: bpp = 16; break;
                case DXGI_FORMAT_R32G32_FLOAT:
                case DXGI_FORMAT_R16G16B16A16_FLOAT: bpp = 8; break;
                case DXGI_FORMAT_R32_FLOAT:
                case DXGI_FORMAT_R32_UINT:
                case DXGI_FORMAT_R8G8B8A8_UNORM: bpp = 4; break;
                default: bpp = 0; break;
                }
                if (bpp) {
                    row = rd.Width * bpp;
                    bytes = row * (rd.Height ? rd.Height : 1);
                }
            }
            if (bytes) {
                if (wr->shadow.size() < (size_t)bytes) wr->shadow.resize((size_t)bytes);
                pMappedResource->pData = wr->shadow.data();
                pMappedResource->RowPitch = (UINT)row;
                pMappedResource->DepthPitch = (UINT)bytes;
                return S_OK;
            }
        }
    }

    if (pMappedResource && MapType == D3D11_MAP_READ) {
        WrapResource* wr = AsWrapResource(pResource);
        if (wr && wr->dx12 && wr->heap_type == D3D12_HEAP_TYPE_READBACK) {
            DumpStage("Map(READ): flush", (unsigned long long)wr->dx12);
            if (!DumpReadbackFlush()) {
                NotImplOnce("Map(READ): dump-queue flush failed");
                return E_FAIL;
            }
            const D3D12_RESOURCE_DESC rd = wr->dx12->GetDesc();
            void* p = nullptr;
            D3D12_RANGE all{ 0, (SIZE_T)rd.Width };
            DumpStage("Map(READ): map", (unsigned long long)wr->dx12);
            if (SUCCEEDED(wr->dx12->Map(0, &all, &p)) && p) {
                pMappedResource->pData = p;
                if (wr->row_pitch) {
                    pMappedResource->RowPitch = wr->row_pitch;
                    pMappedResource->DepthPitch = wr->row_pitch * (wr->rows ? wr->rows : 1);
                } else {
                    pMappedResource->RowPitch = (UINT)rd.Width;
                    pMappedResource->DepthPitch = (UINT)rd.Width;
                }
                return S_OK;
            }
            NotImplOnce("Map(READ): READBACK resource Map failed");
            return E_FAIL;
        }
    }

    NotImplOnce("Map for reading a resource that is not a READBACK staging buffer");
    return E_NOTIMPL;
}

void STDMETHODCALLTYPE Dx12Context::Unmap(ID3D11Resource* pResource, UINT)
{
    WrapResource* wr = AsWrapResource(pResource);
    if (wr && wr->dx12 && !wr->shadow.empty()) {
        if (wr->uploaded.size() == wr->shadow.size() &&
            memcmp(wr->uploaded.data(), wr->shadow.data(), wr->shadow.size()) == 0 &&
            ListHoldsUpload(GetDx12Bridge().cl, wr))
            return;
        wr->uploaded = wr->shadow;
        wr->uploaded_seq = NextUploadSeq();

        const D3D12_RESOURCE_DESC rd = wr->dx12->GetDesc();
        if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
            QueueUpload(pResource, wr->shadow.data(), wr->shadow.size(), wr->uploaded_seq);
        } else {
            D3D11_SUBRESOURCE_DATA sd{};
            sd.pSysMem = wr->shadow.data();
            sd.SysMemPitch = (UINT)(wr->shadow.size() / (rd.Height ? rd.Height : 1)); // Map laid rows out at Width*bpp
            QueueTextureUpload(pResource, &sd, 1, wr->uploaded_seq);
        }
        return;
    }
}

void STDMETHODCALLTYPE Dx12Context::CopyResource(ID3D11Resource* pDst, ID3D11Resource* pSrc)
{
    if (g_present_redraw) return;
    g_present_snapshots.NoteWrite(pDst);
    WrapResource* d = AsWrapResource(pDst);
    WrapResource* s = AsWrapResource(pSrc);
    if (!d || !s || !d->dx12 || !s->dx12) {
        NotImplOnce("CopyResource with a resource that is not one of our wrappers");
        return;
    }

    if (d->heap_type == D3D12_HEAP_TYPE_READBACK) DumpStage("CopyResource: record", (unsigned long long)s->dx12);
    if (d->heap_type == D3D12_HEAP_TYPE_READBACK && d->row_pitch && s->dim == D3D11_RESOURCE_DIMENSION_TEXTURE2D) {
        if (ID3D12GraphicsCommandList* dl = DumpReadbackList()) {
            D3D12_TEXTURE_COPY_LOCATION dstL{};
            dstL.pResource = d->dx12;
            dstL.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dstL.PlacedFootprint = d->footprint;

            D3D12_TEXTURE_COPY_LOCATION srcL{};
            srcL.pResource = s->dx12;
            srcL.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            srcL.SubresourceIndex = 0;

            unsigned pre_bytes = 0;
            unsigned pre_pitch = 0;
            if (const void* pre = PreDrainLookup(s->dx12, &pre_bytes, &pre_pitch)) {
                void* dstp = nullptr;
                D3D12_RANGE none{ 0, 0 };
                if (SUCCEEDED(d->dx12->Map(0, &none, &dstp)) && dstp) {
                    const UINT64 cap = d->dx12->GetDesc().Width;
                    SIZE_T end = 0;
                    if (!pre_pitch || pre_pitch == d->row_pitch) {
                        end = (SIZE_T)(pre_bytes < cap ? pre_bytes : cap);
                        memcpy(dstp, pre, end);
                    } else {
                        static std::atomic<unsigned> s_pitch_logs{ 0 };
                        if (s_pitch_logs.fetch_add(1, std::memory_order_relaxed) < 4)
                            logger::write(
                                "DUMPRB: pre-drained %p (fmt %d) has row pitch %u, its staging (fmt %d) %u x %u rows: copied row by row",
                                (void*)s->dx12, (int)s->dx12->GetDesc().Format, pre_pitch,
                                (int)d->footprint.Footprint.Format, d->row_pitch, d->rows);
                        const UINT64 row = d->row_pitch < pre_pitch ? d->row_pitch : pre_pitch;
                        for (UINT r = 0; r < d->rows; ++r) {
                            const UINT64 so = (UINT64)r * pre_pitch;
                            const UINT64 dof = (UINT64)r * d->row_pitch;
                            if (so >= pre_bytes || dof >= cap) break;
                            UINT64 len = row;
                            if (len > pre_bytes - so) len = pre_bytes - so;
                            if (len > cap - dof) len = cap - dof;
                            memcpy((uint8_t*)dstp + dof, (const uint8_t*)pre + so, (size_t)len);
                            end = (SIZE_T)(dof + len);
                        }
                    }
                    D3D12_RANGE wrote{ 0, end };
                    d->dx12->Unmap(0, &wrote);
                    return;
                }
            }

            const uint32_t st_before = ObservedResourceState(s->dx12);
            if (st_before == kResStateUnknown) {
                NotImplOnce("CopyResource: game texture with no tracked state -- skipped, not guessed");
                return;
            }
            CopyAsSource(dl, s->dx12, (D3D12_RESOURCE_STATES)st_before, // and back, as the game left it
                [&] { dl->CopyTextureRegion(&dstL, 0, 0, 0, &srcL, nullptr); });
            return;
        }
        NotImplOnce("CopyResource to a staging texture with no dump queue");
        return;
    }

    if (d->heap_type == D3D12_HEAP_TYPE_READBACK && d->dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        if (ID3D12GraphicsCommandList* dl = DumpReadbackList()) {
            const UINT64 dw = d->dx12->GetDesc().Width;
            const UINT64 sw = s->dx12->GetDesc().Width;
            UINT64 src_off = 0;
            if (s->gpu_va) {
                const D3D12_GPU_VIRTUAL_ADDRESS sb = s->dx12->GetGPUVirtualAddress();
                if (s->gpu_va > sb) src_off = s->gpu_va - sb;
            }
            if (src_off || dw != sw) {
                const UINT64 avail = (src_off < sw) ? (sw - src_off) : 0;
                const UINT64 n = (dw < avail) ? dw : avail;
                if (n) dl->CopyBufferRegion(d->dx12, 0, s->dx12, src_off, n);
                return;
            }
            dl->CopyResource(d->dx12, s->dx12);
            return;
        }
        NotImplOnce("CopyResource to a staging buffer with no dump queue -- frame analysis readback "
                    "unavailable");
        return;
    }

    static std::atomic<unsigned long long> s_copy_ok{ 0 };
    static std::atomic<unsigned long long> s_copy_nocl{ 0 };

    const Dx12Bridge& b = GetDx12Bridge();
    const bool log_on = logger::enabled(1);
    if (!b.cl) {
        const unsigned long long n = log_on ? s_copy_nocl.fetch_add(1, std::memory_order_relaxed) + 1 : 0ull;
        if (log_on && (n <= 4 || (n % 512) == 0))
            logger::write("COPY-REFUSED (no DX12 command list): dst=%llu B src=%llu B  -- refused %llu, "
                          "succeeded %llu. A refused copy leaves the destination holding whatever it "
                          "held before; if that destination is bound as vb0, the draw reads a buffer "
                          "nothing wrote.",
                (unsigned long long)d->dx12->GetDesc().Width, (unsigned long long)s->dx12->GetDesc().Width, n,
                s_copy_ok.load(std::memory_order_relaxed));
        NotImplOnce("CopyResource (no DX12 command list bound)");
        return;
    }
    if (log_on) {
        const unsigned long long n = s_copy_ok.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 4 || (n % 2048) == 0)
            logger::write("COPY-OK: dst=%llu B src=%llu B  -- succeeded %llu, refused %llu",
                (unsigned long long)d->dx12->GetDesc().Width, (unsigned long long)s->dx12->GetDesc().Width, n,
                s_copy_nocl.load(std::memory_order_relaxed));
    }
    FlushPendingUploads(b.cl);
    bool cs = false;
    if (d->dim == D3D11_RESOURCE_DIMENSION_BUFFER && s->dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        GpuBracket _gt(b.cl, GT_ShimCopyCs);
        const UINT64 dw = d->dx12->GetDesc().Width;
        const UINT64 sw = s->dx12->GetDesc().Width;
        cs = CopyBufferCs(b.cl, d, 0, s, 0, dw < sw ? dw : sw);
    }
    if (!cs) {
        {
            GpuBracket _gt(b.cl, GT_ShimCopyXit);
            TransitionResource(b.cl, pSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
            TransitionResource(b.cl, pDst, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        {
            GpuBracket _gt(b.cl, GT_ShimCopy);
            b.cl->CopyResource(d->dx12, s->dx12);
        }
    }
    if (DiagOnce("COPY-WHAT", (int)(reinterpret_cast<uintptr_t>(d->dx12) >> 4))) {
        const D3D12_RESOURCE_DESC dd = d->dx12->GetDesc();
        const D3D12_RESOURCE_DESC sd = s->dx12->GetDesc();
        const wchar_t* line = g_command_observer.CurrentIniLine();
        logger::write(
            "COPY-WHAT dst=%p dim=%d %llux%u fmt=%d mips=%u flags=%x <- src=%p dim=%d %llux%u fmt=%d  (own_list=%d) line='%ls'",
            (void*)d->dx12, (int)dd.Dimension, (unsigned long long)dd.Width, dd.Height, (int)dd.Format,
            (unsigned)dd.MipLevels, (unsigned)dd.Flags, (void*)s->dx12, (int)sd.Dimension, (unsigned long long)sd.Width,
            sd.Height, (int)sd.Format, (int)b.own_list, line ? line : L"(none)");
    }
    if (g_prof_enabled) { // the GetDesc is a COM call; only pay it while measuring
        if (s->dim == D3D11_RESOURCE_DIMENSION_BUFFER)
            ProfGpuWork(PGW_ShimCopy, s->dx12->GetDesc().Width);
        else
            ProfGpuWork(PGW_ShimCopyTex, 0);
    }
    if (G->track_texture_updates == 1) PropagateResourceHash(pDst, pSrc);
}

static bool PortExpandRegionCopy(ID3D11Resource* pDstResource, ID3D11Resource* pSrcResource, const D3D11_BOX* pSrcBox,
    UINT* replaceDstX, D3D11_BOX* replaceBox)
{
    static int s_any_expand = -1;
    if (s_any_expand < 0) {
        s_any_expand = 0;
        for (auto& e : G->mTextureOverrideMap)
            for (TextureOverride& to : e.second)
                if (to.expand_region_copy) {
                    s_any_expand = 1;
                    break;
                }
    }
    if (!s_any_expand) return false;

    if (!pSrcResource || !pDstResource || !pSrcBox) return false;
    WrapResource* s = AsWrapResource(pSrcResource);
    WrapResource* d = AsWrapResource(pDstResource);
    if (!s || !d) return false;
    if (s->dim != d->dim || s->dim != D3D11_RESOURCE_DIMENSION_TEXTURE2D) return false;

    const uint32_t dstHash = GetResourceHash(pDstResource);
    TextureOverrideMap::iterator i = lookup_textureoverride(dstHash);
    if (i == G->mTextureOverrideMap.end()) return false;
    if (!i->second.begin()->expand_region_copy) return false;

    memcpy(replaceBox, pSrcBox, sizeof(D3D11_BOX));
    *replaceDstX = 0;
    replaceBox->left = 0;
    replaceBox->right = static_cast<WrapTexture2D*>(d)->desc.Width;
    return true;
}

void STDMETHODCALLTYPE Dx12Context::CopySubresourceRegion(ID3D11Resource* pDst, UINT DstSubresource, UINT DstX,
    UINT DstY, UINT DstZ, ID3D11Resource* pSrc, UINT SrcSubresource, const D3D11_BOX* pSrcBox)
{
    if (g_present_redraw) return;
    g_present_snapshots.NoteWrite(pDst);
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.cl) {
        NotImplOnce("CopySubresourceRegion (no DX12 command list bound)");
        return;
    }
    WrapResource* d = AsWrapResource(pDst);
    WrapResource* s = AsWrapResource(pSrc);
    if (!d || !s || !d->dx12 || !s->dx12) {
        NotImplOnce("CopySubresourceRegion with a resource that is not one of our wrappers");
        return;
    }
    FlushPendingUploads(b.cl);
    TransitionResource(b.cl, pSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
    TransitionResource(b.cl, pDst, D3D12_RESOURCE_STATE_COPY_DEST);

    // HackerContext::CopySubresourceRegion substitutes BOTH the box and DstX before the copy.
    D3D11_BOX replaceSrcBox;
    UINT replaceDstX = DstX;
    if (PortExpandRegionCopy(pDst, pSrc, pSrcBox, &replaceDstX, &replaceSrcBox)) {
        pSrcBox = &replaceSrcBox;
        DstX = replaceDstX;
    }

    if (d->dim == D3D11_RESOURCE_DIMENSION_BUFFER && s->dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        const UINT64 src_off = pSrcBox ? pSrcBox->left : 0;
        const UINT64 bytes = pSrcBox ? (UINT64)(pSrcBox->right - pSrcBox->left) : s->dx12->GetDesc().Width;
        bool cs = false;
        {
            GpuBracket _gt(b.cl, GT_ShimCopyCs);
            cs = CopyBufferCs(b.cl, d, DstX, s, src_off, bytes);
        }
        if (!cs) {
            GpuBracket _gt(b.cl, GT_ShimCopy);
            b.cl->CopyBufferRegion(d->dx12, DstX, s->dx12, src_off, bytes);
        }
        ProfGpuWork(PGW_ShimCopy, bytes);
        RemovalPin::Ours(b.cl, "port-copy");
        return;
    }

    D3D12_TEXTURE_COPY_LOCATION dl{};
    dl.pResource = d->dx12;
    dl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    dl.SubresourceIndex = DstSubresource;
    D3D12_TEXTURE_COPY_LOCATION sl{};
    sl.pResource = s->dx12;
    sl.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    sl.SubresourceIndex = SrcSubresource;
    D3D12_BOX box{};
    if (pSrcBox) {
        box = { pSrcBox->left, pSrcBox->top, pSrcBox->front, pSrcBox->right, pSrcBox->bottom, pSrcBox->back };
    }
    b.cl->CopyTextureRegion(&dl, DstX, DstY, DstZ, &sl, pSrcBox ? &box : nullptr);
    ProfGpuWork(PGW_ShimCopyTex, 0);
    if (G->track_texture_updates == 1 && DstSubresource == 0 && DstX == 0 && DstY == 0 && DstZ == 0 &&
        pSrcBox == nullptr)
        PropagateResourceHash(pDst, pSrc);
}

void STDMETHODCALLTYPE Dx12Context::UpdateSubresource(ID3D11Resource* pDst, UINT DstSubresource,
    const D3D11_BOX* pDstBox, const void* pSrcData, UINT SrcRowPitch, UINT SrcDepthPitch)
{
    if (g_present_redraw) return;
    g_present_snapshots.NoteWrite(pDst);
    WrapResource* d = AsWrapResource(pDst);
    if (!d || !d->dx12 || !pSrcData) {
        NotImplOnce("UpdateSubresource with a resource that is not one of our wrappers");
        return;
    }
    if (d->dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        const UINT64 bytes = pDstBox ? (UINT64)(pDstBox->right - pDstBox->left) : d->dx12->GetDesc().Width;
        if (pDstBox && pDstBox->left != 0) {
            NotImplOnce("UpdateSubresource into a buffer at a non-zero offset");
            return;
        }
        QueueUpload(pDst, pSrcData, (size_t)bytes);
        return;
    }
    if (DstSubresource != 0 || pDstBox) {
        NotImplOnce("UpdateSubresource into a texture subresource or sub-box");
        return;
    }
    if (G->track_texture_updates == 1) UpdateResourceHashFromCPU(pDst, pSrcData, SrcRowPitch, SrcDepthPitch);
    D3D11_SUBRESOURCE_DATA sd{};
    sd.pSysMem = pSrcData;
    sd.SysMemPitch = SrcRowPitch;
    sd.SysMemSlicePitch = SrcDepthPitch;
    QueueTextureUpload(pDst, &sd, 1);
}

void STDMETHODCALLTYPE Dx12Context::ResolveSubresource(
    ID3D11Resource* pDst, UINT DstSubresource, ID3D11Resource* pSrc, UINT SrcSubresource, DXGI_FORMAT Format)
{
    if (g_present_redraw) return;
    g_present_snapshots.NoteWrite(pDst);
    const Dx12Bridge& b = GetDx12Bridge();
    if (!b.cl) {
        NotImplOnce("ResolveSubresource (no DX12 command list bound)");
        return;
    }
    WrapResource* d = AsWrapResource(pDst);
    WrapResource* s = AsWrapResource(pSrc);
    if (!d || !s || !d->dx12 || !s->dx12) {
        NotImplOnce("ResolveSubresource with a resource that is not one of our wrappers");
        return;
    }
    FlushPendingUploads(b.cl);
    TransitionResource(b.cl, pSrc, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    TransitionResource(b.cl, pDst, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    b.cl->ResolveSubresource(d->dx12, DstSubresource, s->dx12, SrcSubresource, Format);
}

void STDMETHODCALLTYPE Dx12Context::ClearRenderTargetView(ID3D11RenderTargetView* pRTV, const FLOAT ColorRGBA[4])
{
    const Dx12Bridge& b = GetDx12Bridge();
    WrapView* v = AsWrapView(pRTV);
    if (!b.cl || !v || !v->wres) {
        NotImplOnce("ClearRenderTargetView (no command list or not ours)");
        return;
    }
    FlushPendingUploads(b.cl);
    TransitionResource(b.cl, v->res, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const FLOAT black[4] = { 0, 0, 0, 0 };
    b.cl->ClearRenderTargetView(v->cpu, ColorRGBA ? ColorRGBA : black, 0, nullptr);
}

void STDMETHODCALLTYPE Dx12Context::ClearDepthStencilView(
    ID3D11DepthStencilView* pDSV, UINT ClearFlags, FLOAT Depth, UINT8 Stencil)
{
    const Dx12Bridge& b = GetDx12Bridge();
    WrapView* v = AsWrapView(pDSV);
    if (!b.cl || !v || !v->wres) {
        NotImplOnce("ClearDepthStencilView (no command list or not ours)");
        return;
    }
    FlushPendingUploads(b.cl);
    TransitionResource(b.cl, v->res, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CLEAR_FLAGS f = (D3D12_CLEAR_FLAGS)0;
    if (ClearFlags & D3D11_CLEAR_DEPTH) f |= D3D12_CLEAR_FLAG_DEPTH;
    if (ClearFlags & D3D11_CLEAR_STENCIL) f |= D3D12_CLEAR_FLAG_STENCIL;
    b.cl->ClearDepthStencilView(v->cpu, f, Depth, Stencil, 0, nullptr);
}

namespace {

bool PrepareUavClear(ID3D11UnorderedAccessView* uav, WrapView** out_view, D3D12_GPU_DESCRIPTOR_HANDLE* gpu,
    D3D12_CPU_DESCRIPTOR_HANDLE* cpu, UINT* scratch_slot)
{
    const Dx12Bridge& b = GetDx12Bridge();
    WrapView* v = AsWrapView(uav);
    if (!b.cl || !b.device || !v || !v->wres || !v->wres->dx12) return false;
    FlushPendingUploads(b.cl);

    D3D12_CPU_DESCRIPTOR_HANDLE src = v->cpu;
    *scratch_slot = UINT_MAX;
    if (v->wres->dim == D3D11_RESOURCE_DIMENSION_BUFFER) {
        const D3D12_RESOURCE_DESC rd = v->wres->dx12->GetDesc();
        UINT slot = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE tmp = AllocStagingDescriptor('u', &slot);
        if (!tmp.ptr) return false;
        D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
        d.Format = DXGI_FORMAT_R32_UINT;
        d.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        d.Buffer.NumElements = (UINT)(rd.Width / 4);
        b.device->CreateUnorderedAccessView(v->wres->dx12, nullptr, &d, tmp);
        src = tmp;
        *scratch_slot = slot;
    }

    if (!ShaderVisibleCopy(src, gpu, cpu, b.cl)) {
        if (*scratch_slot != UINT_MAX) ReleaseStagingDescriptor('u', *scratch_slot);
        return false;
    }
    TransitionResource(b.cl, v->res, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    *out_view = v;
    return true;
}

} // namespace

void STDMETHODCALLTYPE Dx12Context::ClearUnorderedAccessViewFloat(
    ID3D11UnorderedAccessView* pUAV, const FLOAT Values[4])
{
    if (g_present_redraw) return;
    WrapView* v = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    UINT scratch = UINT_MAX;
    if (!PrepareUavClear(pUAV, &v, &gpu, &cpu, &scratch)) {
        NotImplOnce("ClearUnorderedAccessViewFloat (no command list, or the view is not ours)");
        return;
    }
    g_present_snapshots.NoteWrite(v->res);
    const FLOAT zero[4] = { 0, 0, 0, 0 };
    if (scratch != UINT_MAX) {
        const FLOAT* src = Values ? Values : zero;
        UINT bits[4];
        for (int i = 0; i < 4; ++i) memcpy(&bits[i], &src[i], sizeof(UINT));
        GetDx12Bridge().cl->ClearUnorderedAccessViewUint(gpu, cpu, v->wres->dx12, bits, 0, nullptr);
        ReleaseStagingDescriptor('u', scratch);
    } else {
        GetDx12Bridge().cl->ClearUnorderedAccessViewFloat(gpu, cpu, v->wres->dx12, Values ? Values : zero, 0, nullptr);
    }
    OweGameState(GetDx12Bridge()); // ShaderVisibleCopy set our heaps
}

void STDMETHODCALLTYPE Dx12Context::ClearUnorderedAccessViewUint(ID3D11UnorderedAccessView* pUAV, const UINT Values[4])
{
    if (g_present_redraw) return;
    WrapView* v = nullptr;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
    D3D12_CPU_DESCRIPTOR_HANDLE cpu{};
    UINT scratch = UINT_MAX;
    if (!PrepareUavClear(pUAV, &v, &gpu, &cpu, &scratch)) {
        NotImplOnce("ClearUnorderedAccessViewUint (no command list, or the view is not ours)");
        return;
    }
    g_present_snapshots.NoteWrite(v->res);
    const UINT zero[4] = { 0, 0, 0, 0 };
    GetDx12Bridge().cl->ClearUnorderedAccessViewUint(gpu, cpu, v->wres->dx12, Values ? Values : zero, 0, nullptr);
    if (scratch != UINT_MAX) ReleaseStagingDescriptor('u', scratch);
    OweGameState(GetDx12Bridge());
}

// ---- state QUERIES ----------------------------------------------------------------------------
// Their engine saves state before running a custom shader and restores it after
// (RunCustomShaderCommand::run, ~35 D3D11 calls). The Set* calls record into PipeState, so the Get*
// calls answer honestly from the same place -- and their save/restore genuinely works, instead of
// restoring nothing and leaving a mod's own bindings live for the next one.
//
// These MUST fill their out-parameters: a getter that leaves them has the caller read uninitialised
// stack and then dereference it.
#define ZERO_OUT_PTRS(arr, n) \
    do { \
        if (arr) \
            for (UINT _i = 0; _i < (n); ++_i) (arr)[_i] = nullptr; \
    } while (0)

namespace {

// Get* hands back a reference in COM, and their code Releases what it gets.
template <class T> void GetSlots(T** out, UINT num, T* const* src, UINT src_max, UINT start)
{
    if (!out) return;
    for (UINT i = 0; i < num; ++i) {
        const UINT slot = start + i;
        T* v = (slot < src_max) ? src[slot] : nullptr;
        out[i] = v;
        if (v) v->AddRef();
    }
}

void GetShader(char stage, void** out)
{
    if (!out) return;
    const int si = StageIndex(stage);
    ID3D11DeviceChild* s = (si >= 0) ? State().shader[si] : nullptr;
    *out = s;
    if (s) s->AddRef();
}

} // namespace

void STDMETHODCALLTYPE Dx12Context::VSGetShader(ID3D11VertexShader** s, ID3D11ClassInstance** ci, UINT* n)
{
    GetShader('v', (void**)s);
    if (n) {
        ZERO_OUT_PTRS(ci, *n);
        *n = 0;
    }
}
void STDMETHODCALLTYPE Dx12Context::HSGetShader(ID3D11HullShader** s, ID3D11ClassInstance** ci, UINT* n)
{
    GetShader('h', (void**)s);
    if (n) {
        ZERO_OUT_PTRS(ci, *n);
        *n = 0;
    }
}
void STDMETHODCALLTYPE Dx12Context::DSGetShader(ID3D11DomainShader** s, ID3D11ClassInstance** ci, UINT* n)
{
    GetShader('d', (void**)s);
    if (n) {
        ZERO_OUT_PTRS(ci, *n);
        *n = 0;
    }
}
void STDMETHODCALLTYPE Dx12Context::GSGetShader(ID3D11GeometryShader** s, ID3D11ClassInstance** ci, UINT* n)
{
    GetShader('g', (void**)s);
    if (n) {
        ZERO_OUT_PTRS(ci, *n);
        *n = 0;
    }
}
void STDMETHODCALLTYPE Dx12Context::PSGetShader(ID3D11PixelShader** s, ID3D11ClassInstance** ci, UINT* n)
{
    GetShader('p', (void**)s);
    if (n) {
        ZERO_OUT_PTRS(ci, *n);
        *n = 0;
    }
}
void STDMETHODCALLTYPE Dx12Context::CSGetShader(ID3D11ComputeShader** s, ID3D11ClassInstance** ci, UINT* n)
{
    GetShader('c', (void**)s);
    if (n) {
        ZERO_OUT_PTRS(ci, *n);
        *n = 0;
    }
}

void STDMETHODCALLTYPE Dx12Context::VSGetConstantBuffers(UINT start, UINT n, ID3D11Buffer** out)
{
    GetSlots(out, n, State().cb[0], kMaxCB, start);
}
void STDMETHODCALLTYPE Dx12Context::CSGetConstantBuffers(UINT start, UINT n, ID3D11Buffer** out)
{
    GetSlots(out, n, State().cb[5], kMaxCB, start);
}
void STDMETHODCALLTYPE Dx12Context::PSGetShaderResources(UINT start, UINT n, ID3D11ShaderResourceView** out)
{
    if (out) InheritSrvsForRead('p', start, n);
    GetSlots(out, n, State().srv[4], kMaxSRV, start);
}
void STDMETHODCALLTYPE Dx12Context::CSGetShaderResources(UINT start, UINT n, ID3D11ShaderResourceView** out)
{
    GetSlots(out, n, State().srv[5], kMaxSRV, start);
}
void STDMETHODCALLTYPE Dx12Context::PSGetSamplers(UINT start, UINT n, ID3D11SamplerState** out)
{
    GetSlots(out, n, State().samp[4], kMaxSamp, start);
}

void STDMETHODCALLTYPE Dx12Context::IAGetVertexBuffers(
    UINT start, UINT NumBuffers, ID3D11Buffer** ppVBs, UINT* pStrides, UINT* pOffsets)
{
    PipeState& st = State();
    GetSlots(ppVBs, NumBuffers, st.vb, kMaxVB, start);
    for (UINT i = 0; i < NumBuffers; ++i) {
        const UINT slot = start + i;
        if (pStrides) pStrides[i] = (slot < kMaxVB) ? st.vb_stride[slot] : 0;
        if (pOffsets) pOffsets[i] = (slot < kMaxVB) ? st.vb_offset[slot] : 0;
    }
}

void STDMETHODCALLTYPE Dx12Context::IAGetIndexBuffer(ID3D11Buffer** pIB, DXGI_FORMAT* fmt, UINT* offset)
{
    PipeState& st = State();
    if (pIB) {
        *pIB = st.ib;
        if (st.ib) st.ib->AddRef();
    }
    if (fmt) *fmt = st.ib_format;
    if (offset) *offset = st.ib_offset;
}

void STDMETHODCALLTYPE Dx12Context::IAGetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY* topo)
{
    if (topo) *topo = State().topology;
}

void STDMETHODCALLTYPE Dx12Context::OMGetRenderTargets(
    UINT NumViews, ID3D11RenderTargetView** ppRTVs, ID3D11DepthStencilView** ppDSV)
{
    PipeState& st = State();
    GetSlots(ppRTVs, NumViews, st.rtv, kMaxRTV, 0);
    if (ppDSV) {
        *ppDSV = st.dsv;
        if (st.dsv) st.dsv->AddRef();
    }
}

void STDMETHODCALLTYPE Dx12Context::OMGetRenderTargetsAndUnorderedAccessViews(UINT NumRTVs,
    ID3D11RenderTargetView** ppRTVs, ID3D11DepthStencilView** ppDSV, UINT UAVStart, UINT NumUAVs,
    ID3D11UnorderedAccessView** ppUAVs)
{
    OMGetRenderTargets(NumRTVs, ppRTVs, ppDSV);
    GetSlots(ppUAVs, NumUAVs, State().uav, kMaxUAV, UAVStart);
}

void STDMETHODCALLTYPE Dx12Context::OMGetBlendState(ID3D11BlendState** bs, FLOAT bf[4], UINT* mask)
{
    PipeState& st = State();
    if (bs) {
        *bs = st.blend;
        if (st.blend) st.blend->AddRef();
    }
    if (bf)
        for (int i = 0; i < 4; ++i) bf[i] = st.blend_factor[i];
    if (mask) *mask = st.sample_mask;
}

void STDMETHODCALLTYPE Dx12Context::OMGetDepthStencilState(ID3D11DepthStencilState** ds, UINT* ref)
{
    PipeState& st = State();
    if (ds) {
        *ds = st.depth;
        if (st.depth) st.depth->AddRef();
    }
    if (ref) *ref = st.stencil_ref;
}

void STDMETHODCALLTYPE Dx12Context::RSGetState(ID3D11RasterizerState** rs)
{
    PipeState& st = State();
    if (rs) {
        *rs = st.raster;
        if (st.raster) st.raster->AddRef();
    }
}

void STDMETHODCALLTYPE Dx12Context::RSGetViewports(UINT* pNum, D3D11_VIEWPORT* vp)
{
    if (!pNum) return;
    const PipeState& st = State();
    if (!vp) {
        *pNum = st.has_viewport ? 1u : 0u;
        return;
    }
    if (*pNum == 0) return;
    if (st.has_viewport) vp[0] = st.viewport;
    *pNum = st.has_viewport ? 1u : 0u;
}

void STDMETHODCALLTYPE Dx12Context::SOGetTargets(UINT NumBuffers, ID3D11Buffer** ppSOTargets)
{
    ZERO_OUT_PTRS(ppSOTargets, NumBuffers);
}

// ---- Get* : REPORT what we recorded, and ALWAYS write every out-slot -------------------------------
//
// Their frame-analysis engine does:
//
//     ID3D11Buffer *buffers[14];                                   // uninitialised stack
//     GetPassThroughOrigContext1()->VSGetConstantBuffers(0, 14, buffers);
//     _DumpCBs('v', compute, buffers);                             // then calls methods on each
//
// so a getter that writes nothing hands their code 14 stack-garbage pointers to dereference.
//
// D3D11's contract is also explicit: an unbound slot returns NULL, not "unchanged". So writing every
// slot is the correct behaviour, not merely the safe one.
namespace {

void GetCbsOut(char stage, UINT start, UINT num, ID3D11Buffer** out)
{
    if (!out) return;
    PipeState& st = State();
    const int si = StageIndex(stage);
    for (UINT i = 0; i < num; ++i) {
        const UINT slot = start + i;
        ID3D11Buffer* b = (si >= 0 && slot < kMaxCB) ? st.cb[si][slot] : nullptr;
        out[i] = b;
        if (b) b->AddRef(); // Get* AddRefs; their engine Releases what it is handed
    }
}

void GetSrvsOut(char stage, UINT start, UINT num, ID3D11ShaderResourceView** out)
{
    if (!out) return;
    InheritSrvsForRead(stage, start, num);
    PipeState& st = State();
    const int si = StageIndex(stage);
    for (UINT i = 0; i < num; ++i) {
        const UINT slot = start + i;
        ID3D11ShaderResourceView* v = (si >= 0 && slot < kMaxSRV) ? st.srv[si][slot] : nullptr;
        out[i] = v;
        if (v) v->AddRef();
    }
}

void GetSamplersOut(char stage, UINT start, UINT num, ID3D11SamplerState** out)
{
    if (!out) return;
    PipeState& st = State();
    const int si = StageIndex(stage);
    for (UINT i = 0; i < num; ++i) {
        const UINT slot = start + i;
        ID3D11SamplerState* s = (si >= 0 && slot < kMaxSamp) ? st.samp[si][slot] : nullptr;
        out[i] = s;
        if (s) s->AddRef();
    }
}

} // namespace

void STDMETHODCALLTYPE Dx12Context::PSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer** o)
{
    GetCbsOut('p', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::GSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer** o)
{
    GetCbsOut('g', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::HSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer** o)
{
    GetCbsOut('h', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::DSGetConstantBuffers(UINT s, UINT n, ID3D11Buffer** o)
{
    GetCbsOut('d', s, n, o);
}

void STDMETHODCALLTYPE Dx12Context::VSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView** o)
{
    GetSrvsOut('v', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::GSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView** o)
{
    GetSrvsOut('g', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::HSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView** o)
{
    GetSrvsOut('h', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::DSGetShaderResources(UINT s, UINT n, ID3D11ShaderResourceView** o)
{
    GetSrvsOut('d', s, n, o);
}

void STDMETHODCALLTYPE Dx12Context::VSGetSamplers(UINT s, UINT n, ID3D11SamplerState** o)
{
    GetSamplersOut('v', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::GSGetSamplers(UINT s, UINT n, ID3D11SamplerState** o)
{
    GetSamplersOut('g', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::HSGetSamplers(UINT s, UINT n, ID3D11SamplerState** o)
{
    GetSamplersOut('h', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::DSGetSamplers(UINT s, UINT n, ID3D11SamplerState** o)
{
    GetSamplersOut('d', s, n, o);
}
void STDMETHODCALLTYPE Dx12Context::CSGetSamplers(UINT s, UINT n, ID3D11SamplerState** o)
{
    GetSamplersOut('c', s, n, o);
}

void STDMETHODCALLTYPE Dx12Context::GetDevice(ID3D11Device** ppDevice)
{
    if (!ppDevice) return;
    *ppDevice = PortDeviceAsD3D11();
    (*ppDevice)->AddRef();
}

static void PortGetCbs1(UINT NumBuffers, UINT* pFirstConstant, UINT* pNumConstants)
{
    for (UINT i = 0; i < NumBuffers; ++i) {
        if (pFirstConstant) pFirstConstant[i] = 0;
        if (pNumConstants) pNumConstants[i] = 0; // 0 = whole buffer, theirs' own convention
    }
}
void STDMETHODCALLTYPE Dx12Context::VSGetConstantBuffers1(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants)
{
    VSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
    PortGetCbs1(NumBuffers, pFirstConstant, pNumConstants);
}
void STDMETHODCALLTYPE Dx12Context::HSGetConstantBuffers1(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants)
{
    HSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
    PortGetCbs1(NumBuffers, pFirstConstant, pNumConstants);
}
void STDMETHODCALLTYPE Dx12Context::DSGetConstantBuffers1(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants)
{
    DSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
    PortGetCbs1(NumBuffers, pFirstConstant, pNumConstants);
}
void STDMETHODCALLTYPE Dx12Context::GSGetConstantBuffers1(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants)
{
    GSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
    PortGetCbs1(NumBuffers, pFirstConstant, pNumConstants);
}
void STDMETHODCALLTYPE Dx12Context::PSGetConstantBuffers1(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants)
{
    PSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
    PortGetCbs1(NumBuffers, pFirstConstant, pNumConstants);
}
void STDMETHODCALLTYPE Dx12Context::CSGetConstantBuffers1(
    UINT StartSlot, UINT NumBuffers, ID3D11Buffer** ppConstantBuffers, UINT* pFirstConstant, UINT* pNumConstants)
{
    CSGetConstantBuffers(StartSlot, NumBuffers, ppConstantBuffers);
    PortGetCbs1(NumBuffers, pFirstConstant, pNumConstants);
}

void STDMETHODCALLTYPE Dx12Context::IAGetInputLayout(ID3D11InputLayout** ppInputLayout)
{
    if (!ppInputLayout) return;
    *ppInputLayout = CurrentInputLayout();
}

D3D11_DEVICE_CONTEXT_TYPE STDMETHODCALLTYPE Dx12Context::GetType(void)
{
    return D3D11_DEVICE_CONTEXT_IMMEDIATE;
}

} // namespace MigotoPort
