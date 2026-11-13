// port_api: the game's buffers, textures and targets, published to their engine as D3D11 objects.
#include "port_api_internal.h"
#include "globals.h"
#include "IniHandler.h"
#include "CommandList.h"
#include "ResourceHash.h"
#include "util.h" // their crc32c_hw -- the buffer hash is theirs, computed here for a DX12 buffer
#include "DrawCallInfo.h"
#include "HackerDevice.h"
#include "HackerContext.h"
#include "frame_analysis_engine.h"
#include "dx12_context.h"
#include "dx12_pipeline.h"
#include "dx12_wrap.h"
#include "../core/log.h"
#include "../core/buffer_view_fit.h"

#include <map>
#include <mutex>
#include <string>
#include <tuple>
#include <vector>

namespace PortApi {

static D3D11_BUFFER_DESC GameConstantBufferDesc(unsigned dx11_byte_width)
{
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = dx11_byte_width;
    d.Usage = D3D11_USAGE_DYNAMIC;
    d.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    d.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    d.MiscFlags = 0;
    d.StructureByteStride = 0;
    return d;
}

unsigned GameConstantBufferHash(unsigned dx11_byte_width)
{
    // Their two lines, HackerDevice::CreateBuffer. crc32c_hw is their hash, from their util.h.
    const D3D11_BUFFER_DESC d = GameConstantBufferDesc(dx11_byte_width);
    return crc32c_hw(0, &d, sizeof(D3D11_BUFFER_DESC));
}

unsigned Crc32c(unsigned seed, const void* data, size_t n)
{
    return crc32c_hw(seed, data, n);
}

namespace {
template <class Key, class Wrap> struct SlotWrappers {
    struct Slot {
        Wrap* current = nullptr;
        std::vector<Wrap*> retired;
    };
    std::mutex mtx;
    std::map<Key, Slot> slots;
    Wrap* Get(const Key& k)
    {
        std::lock_guard<std::mutex> l(mtx);
        Slot& s = slots[k];
        if (!s.current) s.current = new Wrap();
        return s.current;
    }
    Wrap* Find(const Key& k)
    {
        std::lock_guard<std::mutex> l(mtx);
        auto it = slots.find(k);
        return it == slots.end() ? nullptr : it->second.current;
    }
    Wrap* Unheld(const Key& k, const void* bound, size_t* moved_to)
    {
        std::lock_guard<std::mutex> l(mtx);
        Slot& s = slots[k];
        auto unheld = [&](Wrap* w) { return w->RefCount() <= (bound == (const void*)w ? 2 : 1); };
        *moved_to = 0;
        if (!s.current) s.current = new Wrap();
        if (unheld(s.current)) return s.current;
        s.retired.push_back(s.current);
        s.current = nullptr;
        for (size_t i = 0; i < s.retired.size() && !s.current; ++i)
            if (unheld(s.retired[i])) {
                s.current = s.retired[i];
                s.retired.erase(s.retired.begin() + (ptrdiff_t)i);
            }
        if (!s.current) s.current = new Wrap();
        *moved_to = s.retired.size() + 1;
        return s.current;
    }
};
using StageSlot = std::pair<int, unsigned>;
using StageSlotCopy = std::tuple<int, unsigned, unsigned>;

void ForgetOverrideCandidates(ResourceHandleInfo& info)
{
    info.texture_override_candidates_valid = false;
    info.texture_override_candidates.hash_matches = nullptr;
    info.texture_override_candidates.fuzzy_matches.clear();
}

void RegisterGameBuffer(MigotoPort::WrapBuffer* w, unsigned hash)
{
    ResourceHandleInfo& info = G->mResources[(ID3D11Resource*)w];
    if (info.type != D3D11_RESOURCE_DIMENSION_BUFFER || info.hash != hash) ForgetOverrideCandidates(info);
    info.type = D3D11_RESOURCE_DIMENSION_BUFFER;
    info.hash = hash;
    info.orig_hash = hash;
}
void RegisterGameTexture2D(MigotoPort::WrapTexture2D* w, unsigned content_hash)
{
    ResourceHandleInfo& info = G->mResources[(ID3D11Resource*)w];
    if (info.type != D3D11_RESOURCE_DIMENSION_TEXTURE2D || memcmp(&info.desc2D, &w->desc, sizeof w->desc) != 0)
        ForgetOverrideCandidates(info);
    info.type = D3D11_RESOURCE_DIMENSION_TEXTURE2D;
    info.data_hash = content_hash;
    info.hash = content_hash ? content_hash : CalcTexture2DDescHash(0, &w->desc);
    info.orig_hash = info.hash;
    info.desc2D = w->desc;
}

bool SetViaTarget(ResourceCopyTarget& tgt, ID3D11Resource* res, ID3D11View* view)
{
    CommandListState cls{};
    cls.mHackerDevice = &g_dev;
    cls.mHackerContext = &g_ctx;
    cls.mOrigDevice1 = g_dev.GetPassThroughOrigDevice1();
    cls.mOrigContext1 = g_ctx.GetPassThroughOrigContext1();
    if (!cls.mOrigContext1) return false;
    tgt.SetResource(&cls, res, view, 0, 0, DXGI_FORMAT_UNKNOWN, 0);
    return true;
}
} // namespace

void PushGameVertexBuffer(unsigned slot, ID3D12Resource* res, unsigned long long gpu_va, unsigned dx11_byte_width,
    unsigned stride, unsigned offset)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    if (slot >= (unsigned)MigotoPort::kMaxVB) return;

    MigotoPort::PipeState& st = MigotoPort::State();

    static auto* slots = new SlotWrappers<unsigned, MigotoPort::WrapBuffer>();
    MigotoPort::WrapBuffer* w = slots->Get(slot);
    if (w->dx12 != res) w->heap_type = (D3D12_HEAP_TYPE)0;
    w->foreign = true;
    w->dx12 = res;
    if (!res) {
        w->gpu_va = 0;
        MigotoPort::BindSlot(st.vb[slot], (ID3D11Buffer*)nullptr);
        st.vb_stride[slot] = 0;
        st.vb_offset[slot] = 0;
        st.shim_vb[slot] = {};
        unsigned n = 0;
        for (unsigned s = 0; s < (unsigned)MigotoPort::kMaxVB; ++s)
            if (st.vb[s]) n = s + 1;
        st.num_vb = n;
        return;
    }
    w->gpu_va = (D3D12_GPU_VIRTUAL_ADDRESS)gpu_va;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = dx11_byte_width;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    bd.StructureByteStride = 0; // a vertex buffer is not STRUCTURED; D3D11 GetDesc returns 0 here
    w->desc = bd;

    unsigned hash = GameBufferHashSearch(dx11_byte_width, stride, nullptr);
    if (!hash) hash = GameConstantBufferHash(dx11_byte_width);
    RegisterGameBuffer(w, hash);

    MigotoPort::BindSlot(st.vb[slot], (ID3D11Buffer*)w);
    st.vb_stride[slot] = stride;
    st.vb_offset[slot] = offset;
    if (slot + 1 > st.num_vb) st.num_vb = slot + 1;
}

void PushGameIndexBuffer(ID3D12Resource* res, unsigned long long gpu_va_base, unsigned dx11_byte_width,
    unsigned index_bytes, unsigned offset)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    MigotoPort::PipeState& st = MigotoPort::State();
    static MigotoPort::WrapBuffer* w = nullptr;
    if (!w) w = new MigotoPort::WrapBuffer();
    if (w->dx12 != res) w->heap_type = (D3D12_HEAP_TYPE)0; // foreign, as the VB wrapper above
    w->foreign = true;
    w->dx12 = res;
    if (!res) {
        w->gpu_va = 0;
        MigotoPort::BindSlot(st.ib, (ID3D11Buffer*)nullptr);
        st.ib_format = DXGI_FORMAT_UNKNOWN;
        st.ib_offset = 0;
        return;
    }
    w->gpu_va = (D3D12_GPU_VIRTUAL_ADDRESS)gpu_va_base;
    D3D11_BUFFER_DESC bd{};
    bd.ByteWidth = dx11_byte_width;
    bd.Usage = D3D11_USAGE_DEFAULT;
    bd.BindFlags = D3D11_BIND_INDEX_BUFFER;
    w->desc = bd;
    unsigned hash = GameBufferHashSearch(dx11_byte_width, 0, nullptr);
    if (!hash) hash = GameConstantBufferHash(dx11_byte_width);
    RegisterGameBuffer(w, hash);
    MigotoPort::BindSlot(st.ib, (ID3D11Buffer*)w);
    st.ib_format = (index_bytes == 2) ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT;
    st.ib_offset = offset;
}

bool PipeStateVbBound(unsigned slot)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || slot >= (unsigned)MigotoPort::kMaxVB) return false;
    return MigotoPort::State().vb[slot] != nullptr;
}

bool PipeStateCbBound(char stage, unsigned slot)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || slot >= (unsigned)MigotoPort::kMaxCB) return false;
    const int si = MigotoPort::StageIndex(stage);
    if (si < 0) return false;
    return MigotoPort::State().cb[si][slot] != nullptr;
}

void PushGameConstantBufferRecord(
    char stage, unsigned slot, ID3D12Resource* res, unsigned long long gpu_va, unsigned dx11_byte_width)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded || slot >= (unsigned)MigotoPort::kMaxCB) return;
    const int si = MigotoPort::StageIndex(stage);
    if (si < 0) return;
    MigotoPort::PipeState& st = MigotoPort::State();

    static auto* slots = new SlotWrappers<StageSlot, MigotoPort::WrapBuffer>();
    MigotoPort::WrapBuffer* w = slots->Get({ si, slot });
    if (!res) {
        // Forget the record, and only the record: a slot the ini has since bound keeps its buffer.
        if (st.cb[si][slot] == (ID3D11Buffer*)w) {
            MigotoPort::BindSlot(st.cb[si][slot], (ID3D11Buffer*)nullptr);
            st.cb_window[si][slot] = {};
        }
        return;
    }
    if (w->dx12 != res) w->heap_type = (D3D12_HEAP_TYPE)0; // foreign, as the VB wrapper above
    w->foreign = true;
    MigotoPort::BindSlot(w->dx12, res);
    w->gpu_va = (D3D12_GPU_VIRTUAL_ADDRESS)gpu_va; // the window into the ring, not the ring
    w->desc = GameConstantBufferDesc(dx11_byte_width);
    RegisterGameBuffer(w, GameConstantBufferHash(dx11_byte_width));
    MigotoPort::BindSlot(st.cb[si][slot], (ID3D11Buffer*)w);
    st.cb_window[si][slot] = {};
}

void PushGameConstantBuffer(char stage, unsigned slot, ID3D12Resource* res, unsigned long long gpu_va,
    unsigned dx11_byte_width, unsigned copy)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;

    const int si = MigotoPort::StageIndex(stage);
    if (si < 0) return;
    if (slot >= (unsigned)MigotoPort::kMaxCB) return;

    ResourceCopyTarget tgt;
    tgt.type = ResourceCopyTargetType::CONSTANT_BUFFER;
    tgt.shader_type = (wchar_t)stage;
    tgt.slot = slot;

    if (!g_ctx.GetPassThroughOrigContext1()) return;
    if (!res) {
        SetViaTarget(tgt, nullptr, nullptr);
        return;
    }

    static auto* slots = new SlotWrappers<StageSlotCopy, MigotoPort::WrapBuffer>();
    size_t moved_to = 0;
    MigotoPort::WrapBuffer* w = slots->Unheld({ si, slot, copy }, MigotoPort::State().cb[si][slot], &moved_to);
    if (moved_to && logger::enabled(1)) {
        static std::mutex m;
        static std::set<size_t> seen;
        bool first;
        {
            std::lock_guard<std::mutex> l(m);
            first = seen.insert(((size_t)si << 40) | ((size_t)slot << 32) | moved_to).second;
        }
        if (first)
            logger::write(
                "PORT-CB-HELD %c%c-cb%u: a reference (a pool or resource slot) holds the buffer bound before -- "
                "it keeps that buffer, and the slot publishes through another wrapper (%zu for this slot)",
                "vhdgpc"[si], 's', slot, moved_to);
    }
    if (w->dx12 != res) w->heap_type = (D3D12_HEAP_TYPE)0; // foreign, as the VB wrapper above
    w->foreign = true;
    MigotoPort::BindSlot(w->dx12, res); // re-point it at what the game has bound NOW
    w->gpu_va = (D3D12_GPU_VIRTUAL_ADDRESS)gpu_va; // the window into the ring, not the ring
    w->desc = GameConstantBufferDesc(dx11_byte_width);

    const unsigned hash = GameConstantBufferHash(dx11_byte_width);
    RegisterGameBuffer(w, hash);

    if (logger::enabled(1)) {
        static std::mutex m;
        static std::set<unsigned> seen;
        bool first;
        {
            std::lock_guard<std::mutex> l(m);
            first = seen.insert(((unsigned)si << 8) | slot).second;
        }
        if (first) {
            TextureOverrideMatches matches;
            find_texture_overrides_for_resource((ID3D11Resource*)w, &matches, nullptr);
            float filter = matches.empty() ? 0.0f : 1.0f;
            for (auto rit = matches.rbegin(); rit != matches.rend(); ++rit)
                if ((*rit)->filter_index != FLT_MAX) {
                    filter = (*rit)->filter_index;
                    break;
                }
            logger::write("PORT-RECOG %c%c-cb%u hash=%08x -> %zu [TextureOverride] match(es), "
                          "filter_index=%.4f %s",
                "vhdgpc"[si], 's', slot, hash, matches.size(), filter,
                matches.empty() ? "(no section names its hash: a filter_index test on this slot is false)"
                                : "(recognised)");
        }
    }
    SetViaTarget(tgt, (ID3D11Resource*)w, nullptr);
}

static D3D11_BUFFER_DESC GameComputeUavBufferDesc(unsigned byte_width)
{
    D3D11_BUFFER_DESC d{};
    d.ByteWidth = byte_width;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    d.StructureByteStride = 0;
    return d;
}

bool GetShimVertexBuffer(unsigned slot, void** out_res, unsigned long long* out_va, unsigned* out_size,
    unsigned* out_stride, unsigned* out_state)
{
    if (slot >= (unsigned)MigotoPort::kMaxVB) return false;
    const MigotoPort::PipeState& st = MigotoPort::State();
    const MigotoPort::ShimVbRecord& r = st.shim_vb[slot];
    if (!r.res || !r.va || !r.size) return false; // nothing substituted here
    if (out_res) *out_res = (void*)r.res;
    if (out_va) *out_va = (unsigned long long)r.va;
    if (out_size) *out_size = r.size;
    if (out_stride) *out_stride = r.stride;
    if (out_state) *out_state = MigotoPort::QueryResourceState((void*)r.res);
    return true;
}

unsigned GameComputeUavBufferHash(unsigned byte_width)
{
    const D3D11_BUFFER_DESC d = GameComputeUavBufferDesc(byte_width);
    return crc32c_hw(0, &d, sizeof(D3D11_BUFFER_DESC));
}

unsigned GameBufferHashSearch(unsigned byte_width, unsigned ini_stride, bool* out_ambiguous)
{
    if (out_ambiguous) *out_ambiguous = false;
    if (!g_loaded || !byte_width) return 0;

    struct Found {
        unsigned hash;
        bool ambiguous;
    };
    thread_local std::unordered_map<uint64_t, Found> t_found;
    thread_local unsigned t_found_gen = ~0u;
    const unsigned gen = LoadGeneration();
    if (gen != t_found_gen) {
        t_found.clear();
        t_found_gen = gen;
    }
    const uint64_t key = ((uint64_t)ini_stride << 32) | byte_width;
    if (auto it = t_found.find(key); it != t_found.end()) {
        if (out_ambiguous) *out_ambiguous = it->second.ambiguous;
        return it->second.hash;
    }

    static const UINT kUsage[] = { D3D11_USAGE_DEFAULT, D3D11_USAGE_IMMUTABLE, D3D11_USAGE_DYNAMIC,
        D3D11_USAGE_STAGING };
    static const UINT kBind[] = {
        D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, // the solved one
        D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
        D3D11_BIND_UNORDERED_ACCESS,
        D3D11_BIND_SHADER_RESOURCE,
        D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_SHADER_RESOURCE,
        D3D11_BIND_INDEX_BUFFER | D3D11_BIND_SHADER_RESOURCE,
        D3D11_BIND_CONSTANT_BUFFER,
    };
    const UINT kStride[] = { 0u, ini_stride };
    static const UINT kMisc[] = { 0u, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,
        D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS };

    unsigned found = 0;
    unsigned hits = 0;
    for (UINT u : kUsage)
        for (UINT b : kBind)
            for (UINT m : kMisc)
                for (UINT st : kStride) {
                    if (st && m != D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) continue; // stride implies structured
                    if (!st && m == D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) continue;
                    D3D11_BUFFER_DESC d{};
                    d.ByteWidth = byte_width;
                    d.Usage = (D3D11_USAGE)u;
                    d.BindFlags = b;
                    d.CPUAccessFlags = (u == D3D11_USAGE_DYNAMIC) ? D3D11_CPU_ACCESS_WRITE
                        : (u == D3D11_USAGE_STAGING)              ? (D3D11_CPU_ACCESS_READ | D3D11_CPU_ACCESS_WRITE)
                                                                  : 0u;
                    d.MiscFlags = m;
                    d.StructureByteStride = st;
                    const unsigned h = crc32c_hw(0, &d, sizeof(D3D11_BUFFER_DESC));
                    if (G->mTextureOverrideMap.find(h) == G->mTextureOverrideMap.end()) continue;
                    if (h != found) {
                        ++hits;
                        found = h;
                    }
                }

    if (hits > 1) { // two different declared hashes are reachable -- say so
        t_found[key] = Found{ 0, true };
        if (out_ambiguous) *out_ambiguous = true;
        return 0;
    }
    t_found[key] = Found{ found, false };
    return found; // 0 = nothing the ini declares matches this buffer
}

namespace {
SlotWrappers<unsigned, MigotoPort::WrapBuffer> g_gameuav_res; // slot -> stable wrapper
SlotWrappers<unsigned, MigotoPort::WrapUAV> g_gameuav_view;
}

int PushGameComputeUav(unsigned slot, ID3D12Resource* res, unsigned byte_width, unsigned long long gpu_va)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return kPushUavNotLoaded;
    if (slot >= (unsigned)MigotoPort::kMaxUAV) return kPushUavBadSlot;

    int result = kPushUavPublished;
    {
        ID3D11UnorderedAccessView* cur = nullptr;
        if (ID3D11DeviceContext1* octx = g_ctx.GetPassThroughOrigContext1())
            octx->CSGetUnorderedAccessViews(slot, 1, &cur);
        if (cur) {
            const bool ours = (ID3D11UnorderedAccessView*)g_gameuav_view.Find(slot) == cur;
            cur->Release();
            if (!ours) result = kPushUavOverMod;
        }
    }

    if (!res) byte_width = 0; // clearing the slot: ignore whatever the caller passed
    MigotoPort::WrapBuffer* w = g_gameuav_res.Get(slot);
    MigotoPort::BindSlot(w->dx12, res);
    if (res) {
        w->desc = GameComputeUavBufferDesc(byte_width);
        w->foreign = true; // the GAME made this buffer; our `state` field has no authority over it
        w->gpu_va = (D3D12_GPU_VIRTUAL_ADDRESS)gpu_va;
    }

    // Register the hash for a resource we did not create -- their CreateBuffer does this at creation time.
    if (res) RegisterGameBuffer(w, GameComputeUavBufferHash(byte_width));

    MigotoPort::WrapUAV* v = g_gameuav_view.Get(slot);
    v->res = res ? (ID3D11Resource*)w : nullptr;
    v->wres = res ? (MigotoPort::WrapResource*)w : nullptr;
    v->desc.Format = DXGI_FORMAT_R32_UINT;
    v->desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    v->desc.Buffer.NumElements = byte_width / 4;

    if (res) {
        if (!v->cpu.ptr) v->cpu = MigotoPort::AllocStagingDescriptor('u', &v->heap_slot);
        if (v->cpu.ptr) {
            D3D12_UNORDERED_ACCESS_VIEW_DESC ud{};
            ud.Format = DXGI_FORMAT_R32_UINT; // TYPED: RWBuffer<uint>
            ud.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
            ud.Buffer.FirstElement = 0;
            ud.Buffer.NumElements = (UINT)BufferViewFit(res, 0, byte_width / 4, 4, "game-cs-uav");
            ud.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE; // RAW is only legal with TYPELESS
            if (const MigotoPort::Dx12Bridge& br = MigotoPort::GetDx12Bridge(); br.device && ud.Buffer.NumElements)
                br.device->CreateUnorderedAccessView(res, nullptr, &ud, v->cpu);
        }
    }

    ID3D11DeviceContext1* octx = g_ctx.GetPassThroughOrigContext1();
    if (!octx) return kPushUavNoContext; // the setter would be a no-op; say so rather than "published"
    ID3D11UnorderedAccessView* views[1] = { res ? (ID3D11UnorderedAccessView*)v : nullptr };
    octx->CSSetUnorderedAccessViews(slot, 1, views, nullptr);
    return res ? result : kPushUavCleared;
}

// ---- the game's TEXTURES, same transition as PushGameConstantBuffer -------------------------------
namespace {

D3D11_TEXTURE2D_DESC GameTexture2DDesc(ID3D12Resource* res)
{
    D3D11_TEXTURE2D_DESC d{};
    if (!res) return d;
    const D3D12_RESOURCE_DESC rd = res->GetDesc();
    d.Width = (UINT)rd.Width;
    d.Height = rd.Height;
    d.MipLevels = rd.MipLevels;
    d.ArraySize = rd.DepthOrArraySize;
    d.Format = rd.Format;
    d.SampleDesc = rd.SampleDesc;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    d.CPUAccessFlags = 0;
    d.MiscFlags = 0;
    return d;
}

SlotWrappers<StageSlot, MigotoPort::WrapTexture2D> g_gametex_res;
SlotWrappers<StageSlot, MigotoPort::WrapSRV> g_gametex_view;

ID3D11ShaderResourceView* PublishedTextureView(int si, unsigned slot)
{
    return (ID3D11ShaderResourceView*)g_gametex_view.Find({ si, slot });
}

} // namespace

int PushGameTexture(char stage, unsigned slot, ID3D12Resource* res, unsigned content_hash, unsigned view_format)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return kPushTexNotLoaded;

    const int si = MigotoPort::StageIndex(stage);
    if (si < 0) return kPushTexBadSlot;
    if (slot >= (unsigned)MigotoPort::kMaxSRV) return kPushTexBadSlot;

    int result = kPushTexPublished;
    {
        ID3D11ShaderResourceView* cur = nullptr;
        if (ID3D11DeviceContext1* octx = g_ctx.GetPassThroughOrigContext1()) {
            switch (stage) {
            case 'v': octx->VSGetShaderResources(slot, 1, &cur); break;
            case 'h': octx->HSGetShaderResources(slot, 1, &cur); break;
            case 'd': octx->DSGetShaderResources(slot, 1, &cur); break;
            case 'g': octx->GSGetShaderResources(slot, 1, &cur); break;
            case 'p': octx->PSGetShaderResources(slot, 1, &cur); break;
            case 'c': octx->CSGetShaderResources(slot, 1, &cur); break;
            }
        }
        if (cur) {
            const bool ours = MigotoPort::AsWrapView(cur) && MigotoPort::AsWrapView(cur)->kind == 't' &&
                PublishedTextureView(si, slot) == cur;
            cur->Release();
            if (!ours) result = kPushTexOverMod; // a mod override (or something else) held this slot
        }
    }

    // The per-slot caches live at file scope (see above) so the guard and this step share them.
    MigotoPort::WrapTexture2D* w = g_gametex_res.Get({ si, slot });
    w->foreign = true; // the game's texture: its state is what the game's barriers say (Transition)
    MigotoPort::BindSlot(w->dx12, res);
    if (res) w->desc = GameTexture2DDesc(res);

    if (res) RegisterGameTexture2D(w, content_hash);

    MigotoPort::WrapSRV* v = g_gametex_view.Get({ si, slot });
    v->res = res ? (ID3D11Resource*)w : nullptr;
    v->wres = res ? (MigotoPort::WrapResource*)w : nullptr; // our side of the same object
    v->desc.Format = view_format ? (DXGI_FORMAT)view_format : EnsureNotTypeless(w->desc.Format);
    v->desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    v->desc.Texture2D.MipLevels = w->desc.MipLevels;

    ResourceCopyTarget tgt;
    tgt.type = ResourceCopyTargetType::SHADER_RESOURCE;
    tgt.shader_type = (wchar_t)stage;
    tgt.slot = slot;
    SetViaTarget(tgt, res ? (ID3D11Resource*)w : nullptr, res ? (ID3D11View*)v : nullptr);
    return result;
}

void UnpushGameTexture(char stage, unsigned slot)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    const int si = MigotoPort::StageIndex(stage);
    if (si < 0 || slot >= (unsigned)MigotoPort::kMaxSRV) return;
    ID3D11DeviceContext1* octx = g_ctx.GetPassThroughOrigContext1();
    if (!octx) return;
    ID3D11ShaderResourceView* cur = nullptr;
    switch (stage) {
    case 'v': octx->VSGetShaderResources(slot, 1, &cur); break;
    case 'h': octx->HSGetShaderResources(slot, 1, &cur); break;
    case 'd': octx->DSGetShaderResources(slot, 1, &cur); break;
    case 'g': octx->GSGetShaderResources(slot, 1, &cur); break;
    case 'p': octx->PSGetShaderResources(slot, 1, &cur); break;
    case 'c': octx->CSGetShaderResources(slot, 1, &cur); break;
    }
    const bool ours = cur && PublishedTextureView(si, slot) == cur;
    if (cur) cur->Release();
    if (ours) PushGameTexture(stage, slot, nullptr, 0);
}

void PushGameDepthTarget(ID3D12Resource* res)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;

    static MigotoPort::WrapTexture2D* w = nullptr;
    static MigotoPort::WrapDSV* v = nullptr;
    if (!w) w = new MigotoPort::WrapTexture2D();
    if (!v) v = new MigotoPort::WrapDSV();

    w->foreign = true; // the game's depth target, as the texture slots above
    MigotoPort::BindSlot(w->dx12, res);
    if (res) {
        w->desc = GameTexture2DDesc(res);
        w->desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;

        RegisterGameTexture2D(w, 0); // no content hash: never uploaded, GPU-written

        v->res = (ID3D11Resource*)w;
        v->wres = (MigotoPort::WrapResource*)w;
        v->desc.Format = EnsureNotTypeless(w->desc.Format);
        v->desc.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
    }

    ResourceCopyTarget tgt;
    tgt.type = ResourceCopyTargetType::DEPTH_STENCIL_TARGET;
    SetViaTarget(tgt, res ? (ID3D11Resource*)w : nullptr, res ? (ID3D11View*)v : nullptr);
}

void PushGameRenderTarget(unsigned slot, ID3D12Resource* res)
{
    if (slot >= 8) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;

    static MigotoPort::WrapTexture2D* w[8] = {};
    static MigotoPort::WrapRTV* v[8] = {};
    if (!w[slot]) w[slot] = new MigotoPort::WrapTexture2D();
    if (!v[slot]) v[slot] = new MigotoPort::WrapRTV();
    MigotoPort::WrapTexture2D* wt = w[slot];
    MigotoPort::WrapRTV* vw = v[slot];

    MigotoPort::BindSlot(wt->dx12, res);
    if (res) {
        wt->desc = GameTexture2DDesc(res);
        // Say what it is, or a later BindFlags test reads a render target as a sampled texture.
        wt->desc.BindFlags = D3D11_BIND_RENDER_TARGET;

        RegisterGameTexture2D(wt, 0); // no content hash: GPU-written, never uploaded

        vw->res = (ID3D11Resource*)wt;
        vw->wres = (MigotoPort::WrapResource*)wt;
        vw->desc.Format = EnsureNotTypeless(wt->desc.Format);
        vw->desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    }

    ResourceCopyTarget tgt;
    tgt.type = ResourceCopyTargetType::RENDER_TARGET;
    tgt.slot = slot;
    SetViaTarget(tgt, res ? (ID3D11Resource*)wt : nullptr, res ? (ID3D11View*)vw : nullptr);
}
} // namespace PortApi
