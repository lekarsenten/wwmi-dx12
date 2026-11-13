// draw_restore.cpp -- the game's bindings put back after an injected draw. See draw_inject.h.
#include "../addon_common.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../hooks/descriptor_shadow.h"
#include "../hooks/pipeline_registry.h"
#include "../mods/layout_registry.h"
#include "../migoto/dx12_bridge.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/port_api.h"
#include "../instrumentation/gpu_timer.h"
#include "../rabbitfx/pso_variant.h"
#include "../textures/mod_texture_store.h"
#include "../textures/tex_identity.h"
#include "../textures/tex_select.h"
#include "../textures/tex_slot_log.h"
#include "../addon/addon.h"
#include "draw_inject.h"
#include "draw_parts.h"
#include "draw_toggles.h"
#include "../hooks/command_list_registry.h"
#include "../hooks/stamp_ledger.h"
#include "../instrumentation/diag_gate.h"
#include "../migoto/dx12_wrap.h"
#include "../core/frame_clock.h"
#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>
#include <mutex>
#include <unordered_map>

bool RestoreGameRoot(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl)
{
    const CmdGfxRoot& g = g_command_lists.GfxRoot(cmd_list);
    if (!g.layout) return false;
    cl->SetGraphicsRootSignature(reinterpret_cast<ID3D12RootSignature*>(g.layout));
    for (UINT p = 0; p < 64; ++p) {
        switch (g.kind[p]) {
        case CmdGfxRoot::kTable:
            cl->SetGraphicsRootDescriptorTable(p, D3D12_GPU_DESCRIPTOR_HANDLE{ g.value[p] });
            break;
        case CmdGfxRoot::kCbv:
            if (g.value[p]) cl->SetGraphicsRootConstantBufferView(p, g.value[p]);
            break;
        case CmdGfxRoot::kSrv:
            if (g.value[p]) cl->SetGraphicsRootShaderResourceView(p, g.value[p]);
            break;
        case CmdGfxRoot::kUav:
            if (g.value[p]) cl->SetGraphicsRootUnorderedAccessView(p, g.value[p]);
            break;
        case CmdGfxRoot::kNone: break;
        }
    }
    for (uint32_t i = 0; i < g.constant_count; ++i)
        cl->SetGraphicsRoot32BitConstant(g.constants[i].param, g.constants[i].value, g.constants[i].offset);
    return true;
}

void RestoreOverrideDraw(void* user)
{
    auto* const cmd_list = static_cast<reshade::api::command_list*>(user);
    ID3D12GraphicsCommandList* const cl = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    const CmdState& gs = g_command_lists.Gfx(cmd_list);
    if (const uint64_t pso = gs.gfx_restore()) cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(pso));
    if (MigotoPort::TakeHeapsChanged()) {
        ID3D12DescriptorHeap* h[2] = { GameHeap(cmd_list, 0), GameHeap(cmd_list, 1) };
        if (h[0])
            cl->SetDescriptorHeaps(h[1] ? 2 : 1, h);
        else if (g_debug && DiagOnce("HEAPRESTORE-NOSRV", 1))
            logger::write(
                "HEAPRESTORE: our heaps were set on this list and the game's CBV_SRV_UAV heap is unknown -- ours stay set");
    }
    if (MigotoPort::g_touched_root_sig) {
        RestoreGameRoot(cmd_list, cl);
        MigotoPort::g_touched_root_sig = false;
    }
    if (MigotoPort::g_touched_rt) {
        const CmdRT& rt = g_command_lists.Rt(cmd_list);
        if (rt.valid) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv[8] = {};
            for (uint32_t i = 0; i < rt.rtv_count && i < 8; ++i) rtv[i].ptr = rt.rtv[i];
            const D3D12_CPU_DESCRIPTOR_HANDLE dsv{ rt.dsv };
            cl->OMSetRenderTargets(rt.rtv_count, rt.rtv_count ? rtv : nullptr, FALSE, rt.has_dsv ? &dsv : nullptr);
        }
        MigotoPort::g_touched_rt = false;
    }
    if (MigotoPort::g_touched_ia) {
        D3D12_VERTEX_BUFFER_VIEW vbv[16] = {};
        for (int s = 0; s < 16; ++s) vbv[s] = { gs.vb[s].va, gs.vb[s].size, gs.vb[s].stride };
        cl->IASetVertexBuffers(0, 16, vbv);
        if (gs.ib.va) {
            const D3D12_INDEX_BUFFER_VIEW ibv{ gs.ib.va, gs.ib.size,
                gs.ib.index_bytes == 2 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT };
            cl->IASetIndexBuffer(&ibv);
        }
        MigotoPort::g_touched_ia = false;
    }
    if (MigotoPort::g_touched_viewport) {
        if (gs.vp_count) cl->RSSetViewports(gs.vp_count, gs.vp);
        if (gs.scissor_count) cl->RSSetScissorRects(gs.scissor_count, gs.scissor);
        MigotoPort::g_touched_viewport = false;
    }
    if (gs.topology) cl->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)gs.topology);
    if (MigotoPort::g_touched_blend && gs.blend_factor_set) cl->OMSetBlendFactor(gs.blend_factor);
    if (MigotoPort::g_touched_stencil && gs.stencil_ref_set) cl->OMSetStencilRef(gs.stencil_ref);
    MigotoPort::g_touched_blend = false;
    MigotoPort::g_touched_stencil = false;
}

namespace {

RegBind GameRegister(const CmdGfxRoot& g, char stage, char kind, unsigned reg)
{
    RegBind rb;
    g_layouts.WithRegMap(g.layout, [&](const LayoutRegMap& m) {
        if (kind == 't' && reg < std::size(m.t_ps))
            rb = stage == 'p' ? m.t_ps[reg] : m.t_vs[reg];
        else if (kind == 'b' && reg < std::size(m.b_ps))
            rb = stage == 'p' ? m.b_ps[reg] : m.b_vs[reg];
    });
    return rb;
}

bool GameTableView(reshade::api::command_list* cmd_list, const CmdGfxRoot& g, const RegBind& rb, ResInfo& ri)
{
    if (rb.param >= 64 || !rb.is_table || g.kind[rb.param] != CmdGfxRoot::kTable) return false;
    const TableStart ts = TableStartOf(cmd_list->get_device(), reshade::api::descriptor_table{ g.value[rb.param] });
    return ts && g_descriptors.Find({ ts.heap, ts.base + rb.offset }, ri) && ri.res;
}

D3D12_CPU_DESCRIPTOR_HANDLE CopySource(const ResInfo& ri)
{
    ModTextureStore::Entry stamp{};
    if (ri.swapped && g_mod_textures.EntryAt(g_mod_textures.IndexOf(ri.swapped), &stamp)) return stamp.srv;
    HeapKey in{};
    if (!ri.view_handle || (g_descriptors.SlotOfCpu(ri.view_handle, &in) && g_descriptors.ShaderVisible(in.heap)))
        return {};
    return { (SIZE_T)ri.view_handle };
}

bool GameShaderReads(reshade::api::command_list* cmd_list, char stage, unsigned reg)
{
    return g_pipelines.SrvRegs(g_command_lists.Gfx(cmd_list).gfx_pso, stage).reads(reg);
}

bool InheritGameDescriptor(
    void* user, char stage, char kind, unsigned reg, unsigned cb_bytes, D3D12_CPU_DESCRIPTOR_HANDLE dst)
{
    auto* const cmd_list = static_cast<reshade::api::command_list*>(user);
    if (kind == 't' && !GameShaderReads(cmd_list, stage, reg)) return false;
    const CmdGfxRoot& g = g_command_lists.GfxRoot(cmd_list);
    const RegBind rb = GameRegister(g, stage, kind, reg);
    if (rb.param >= 64) return false;
    if (rb.is_table) {
        ResInfo ri;
        const D3D12_CPU_DESCRIPTOR_HANDLE src =
            GameTableView(cmd_list, g, rb, ri) ? CopySource(ri) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        if (!src.ptr) return false;
        g_addon.d3d12_device->CopyDescriptorsSimple(1, dst, src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        return true;
    }
    if (kind != 'b' || g.kind[rb.param] != CmdGfxRoot::kCbv) return false;
    const RootCbvBind& cb = g_command_lists.Cbv(cmd_list).binds[rb.param];
    const uint64_t va = g.value[rb.param];
    if (!va || (va & 255) || cb.va != va) return false;
    const uint64_t want = ((uint64_t)(cb_bytes ? cb_bytes : 256) + 255) & ~255ull;
    const uint64_t size =
        (std::min)({ want, (uint64_t)cb.size & ~255ull, (uint64_t)D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT * 16 });
    if (!size) return false;
    const D3D12_CONSTANT_BUFFER_VIEW_DESC d{ va, (UINT)size };
    g_addon.d3d12_device->CreateConstantBufferView(&d, dst);
    return true;
}

void InheritGameTexture(void* user, char stage, unsigned slot)
{
    auto* const cmd_list = static_cast<reshade::api::command_list*>(user);
    if (!GameShaderReads(cmd_list, stage, slot)) return;
    const CmdGfxRoot& g = g_command_lists.GfxRoot(cmd_list);
    ResInfo ri;
    if (!GameTableView(cmd_list, g, GameRegister(g, stage, 't', slot), ri) || ri.is_buffer) {
        PortApi::UnpushGameTexture(stage, slot);
        return;
    }
    const uint32_t resident = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(ri.res));
    const uint32_t lod =
        !resident || PortApi::TextureHashHasSections(resident) ? resident : g_tex_select.LodTargetOf(resident);
    const uint32_t hash = lod && PortApi::TextureHashHasSections(lod) ? lod : resident;
    const unsigned fmt = ri.view_handle
        ? (unsigned)cmd_list->get_device()->get_resource_view_desc(reshade::api::resource_view{ ri.view_handle }).format
        : 0u;
    [[maybe_unused]] const int pushed = PortApi::PushGameTexture(stage, slot, ri.res, hash, fmt);
#ifdef WWMI_DEBUG
    if (pushed == PortApi::kPushTexOverMod && g_tex_trace.On(hash))
        g_tex_trace.Line("%c-t%u: the game's texture handed to the sections over a mod texture an earlier "
                         "section left in that register",
            stage, slot);
#endif
}

ID3D12PipelineState* GamePipelineWith(void* user, const MigotoPort::ModPipeline& mp)
{
    auto* const cmd_list = static_cast<reshade::api::command_list*>(user);
    return g_pso_variants.ModShaders(cmd_list->get_device(), g_command_lists.Gfx(cmd_list).gfx_pso, mp);
}

} // namespace

void InheritGameBindings(PortApi::DrawListScope& scope, reshade::api::command_list* cmd_list)
{
    scope.Inherit(InheritGameDescriptor, InheritGameTexture, GamePipelineWith, cmd_list);
}

#ifdef WWMI_DEBUG
static void NoteCbNotPushed(
    reshade::api::command_list* cmd_list, char stage, unsigned reg, const RegBind& rb, const RootCbvBind* cb)
{
    const char* why = rb.param >= 64 ? "not in the root signature"
        : rb.is_table                ? "bound through a descriptor table"
        : !cb                        ? "no root CBV set"
        : !cb->res || !cb->va        ? "no buffer recorded for it"
        : (cb->va & 255)             ? "its address is not 256-byte aligned"
                                     : "the recorded address is not the root argument";
    uint64_t ps = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps);
    if (DiagOnce("CBPUSH-SKIP", (int)(uint32_t)(ps ^ (ps >> 32)) * 64 + (stage == 'p' ? 32 : 0) + (int)reg))
        logger::write("CBPUSH-SKIP f=%u ps=%016llx %cs-cb%u: %s -- sections see no game buffer there", g_frame_id,
            (unsigned long long)ps, stage, reg, why);
}
#endif

uint16_t PushGameConstantBuffers(reshade::api::command_list* cmd_list, char stage, uint16_t skip)
{
    const uint64_t pso = g_command_lists.Gfx(cmd_list).gfx_pso;
    const CmdGfxRoot& g = g_command_lists.GfxRoot(cmd_list);
    uint16_t pushed = 0;
    std::array<uint32_t, 16> declared_by_reg;
    if (!g_pipelines.CbufferBytes(pso, stage, declared_by_reg)) return pushed;
    for (unsigned reg = 0; reg < MigotoPort::kMaxCB; ++reg) {
        if ((skip >> reg) & 1u) continue;
        const uint32_t declared = declared_by_reg[reg];
        if (!declared) continue;
        const RegBind rb = GameRegister(g, stage, 'b', reg);
        const RootCbvBind* cb = rb.param < 64 && !rb.is_table && g.kind[rb.param] == CmdGfxRoot::kCbv
            ? &g_command_lists.Cbv(cmd_list).binds[rb.param]
            : nullptr;
        if (!cb || !cb->res || !cb->va || (cb->va & 255) || cb->va != g.value[rb.param]) {
#ifdef WWMI_DEBUG
            NoteCbNotPushed(cmd_list, stage, reg, rb, cb);
#endif
            continue;
        }
        PortApi::PushGameConstantBuffer(stage, reg, cb->res, cb->va, declared);
        pushed |= (uint16_t)(1u << reg);
    }
    return pushed;
}

#ifdef WWMI_DEBUG
static void NoteOverrideUnstamp(reshade::api::command_list* cmd_list, unsigned reg, const HeapKey& key,
    ID3D12Resource* was, ID3D11ShaderResourceView* now)
{
    ResInfo ri;
    const bool rec = g_descriptors.Find(key, ri);
    const uint32_t game_hash = rec && ri.res ? g_tex_identity.HashOf(reinterpret_cast<uint64_t>(ri.res)) : 0;
    const MigotoPort::WrapView* wv = MigotoPort::AsWrapView(now);
    const int why = !now ? 0 : !wv ? 1 : !wv->cpu.ptr ? 2 : 3;
    if (!DiagOnce("OVR-UNSTAMP", (int)(game_hash ^ (uint32_t)(why << 28) ^ reg))) return;
    uint64_t ps = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps);
    static const char* const kWhy[] = { "nothing", "an object that is not a port view",
        "a view with no descriptor yet", "a view with no DX12 resource" };
    logger::write("OVR-UNSTAMP f=%u ps=%016llx t%u heap=%llx off=%u: stamped with %p, the game's texture there "
                  "%08x (%p); this draw's sections left %s -- the game's descriptor goes back",
        g_frame_id, (unsigned long long)ps, reg, (unsigned long long)key.heap, (unsigned)key.offset, (void*)was,
        game_hash, (void*)ri.res, kWhy[why]);
}

static const wchar_t* const kPickLeft[] = { L"nothing", L"an object that is not a port view",
    L"a view with no descriptor yet", L"a view with no DX12 resource" };

struct OverridePick {
    uint64_t ps = 0;
    unsigned reg = 0;
    uint32_t game_hash = 0;
    int value = 0;
    int prev = 0;
    uint32_t seen_frame = 0;
    uint32_t changed_frame = 0;
    uint32_t changes = 0;
    uint32_t changes_at_query = 0;
    uint32_t logged_frame = 0;
    uint32_t unlogged = 0;
};
static std::mutex s_pick_mtx;
static std::unordered_map<uint64_t, OverridePick> s_picks;

static std::wstring PickName(int value)
{
    if (value >= 0) return L"picked " + g_mod_textures.NameOf(value);
    if (value == -5) return L"picked a texture not in the mod table";
    return std::wstring(L"left ") + kPickLeft[-1 - value];
}

static uint32_t NoteOverridePick(
    reshade::api::command_list* cmd_list, unsigned reg, const HeapKey& key, ID3D11ShaderResourceView* now)
{
    constexpr uint32_t kLogGap = 120;
    ResInfo ri;
    const bool rec = g_descriptors.Find(key, ri);
    const uint32_t game_hash = rec && ri.res ? g_tex_identity.HashOf(reinterpret_cast<uint64_t>(ri.res)) : 0;
    const MigotoPort::WrapView* wv = MigotoPort::AsWrapView(now);
    const bool ours = wv && wv->cpu.ptr && wv->wres && wv->wres->dx12;
    const int entry = ours ? g_mod_textures.IndexOf(wv->wres->dx12) : -1;
    const int why = !now ? 0 : !wv ? 1 : !wv->cpu.ptr ? 2 : 3;
    const int value = ours ? (entry >= 0 ? entry : -5) : -1 - why;
    uint64_t ps = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps);
    const uint64_t k = (ps * 31u) ^ ((uint64_t)game_hash << 8) ^ reg;
    uint32_t skipped = 0;
    {
        std::lock_guard<std::mutex> l(s_pick_mtx);
        auto [it, fresh] = s_picks.try_emplace(k);
        OverridePick& p = it->second;
        p.seen_frame = g_frame_id;
        if (!fresh && p.value == value) return game_hash;
        p.ps = ps;
        p.reg = reg;
        p.game_hash = game_hash;
        p.prev = fresh ? value : p.value;
        p.value = value;
        p.changed_frame = g_frame_id;
        if (!fresh) ++p.changes;
        if (!fresh && g_frame_id - p.logged_frame < kLogGap) {
            ++p.unlogged;
            return game_hash;
        }
        skipped = p.unlogged;
        p.unlogged = 0;
        p.logged_frame = g_frame_id;
    }
    logger::write("OVR-PICK f=%u ps=%016llx t%u: the game's %08x -- its sections %ls%s", g_frame_id,
        (unsigned long long)ps, reg, game_hash, PickName(value).c_str(),
        skipped ? (" (" + std::to_string(skipped) + " change(s) since the last line)").c_str() : "");
    return game_hash;
}

static std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.c_str(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::string OverridePickReport(uint32_t game_hash)
{
    std::vector<OverridePick> picks;
    {
        std::lock_guard<std::mutex> l(s_pick_mtx);
        for (auto& [k, p] : s_picks) {
            if (game_hash && p.game_hash != game_hash) continue;
            picks.push_back(p);
            p.changes_at_query = p.changes;
        }
    }
    std::sort(picks.begin(), picks.end(), [](const OverridePick& a, const OverridePick& b) {
        return a.seen_frame > b.seen_frame;
    });
    std::string out = "f=" + std::to_string(g_frame_id) + "\n";
    for (const OverridePick& p : picks) {
        char head[160];
        std::snprintf(head, sizeof head,
            "ps=%016llx t%u tex=%08x drawn f=%u, changed f=%u, %u change(s) since the last query: ",
            (unsigned long long)p.ps, p.reg, p.game_hash, p.seen_frame, p.changed_frame,
            p.changes - p.changes_at_query);
        out += head + Utf8(PickName(p.value)) + " (before: " + Utf8(PickName(p.prev)) + ")\n";
    }
    return picks.empty() ? "no checked draw has written a register yet" : out;
}

static void TraceCheck(uint32_t game_hash, reshade::api::command_list* cmd_list, unsigned reg, const HeapKey& key,
    const std::wstring& what)
{
    if (!g_tex_trace.On(game_hash)) return;
    uint64_t ps = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps);
    ResInfo ri;
    std::wstring now = L"not recorded";
    if (g_descriptors.Find(key, ri)) now = ri.swapped ? g_tex_trace.ModText(ri.swapped) : L"the game's texture";
    g_tex_trace.Line("checked draw ps=%016llx t%u heap=%llx off=%u: %ls; the slot now holds %ls",
        (unsigned long long)ps, reg, (unsigned long long)key.heap, (unsigned)key.offset, what.c_str(), now.c_str());
}

static void TraceUnwritten(reshade::api::command_list* cmd_list, const CmdGfxRoot& g, unsigned reg)
{
    if (!GameShaderReads(cmd_list, 'p', reg)) return;
    const RegBind rb = GameRegister(g, 'p', 't', reg);
    if (rb.param >= 64 || !rb.is_table || g.kind[rb.param] != CmdGfxRoot::kTable) return;
    const TableStart ts = TableStartOf(cmd_list->get_device(), reshade::api::descriptor_table{ g.value[rb.param] });
    if (!ts) return;
    const HeapKey key{ ts.heap, ts.base + rb.offset };
    ResInfo ri;
    if (!g_descriptors.Find(key, ri) || !ri.res) return;
    TraceCheck(g_tex_identity.HashOf(reinterpret_cast<uint64_t>(ri.res)), cmd_list, reg, key,
        L"its sections wrote nothing here");
}
#endif

void StampOverrideSrvs(reshade::api::command_list* cmd_list, uint32_t srv_gen0)
{
    static std::mutex s_mtx;
    static std::unordered_map<HeapKey, ID3D12Resource*, HeapKeyHash> s_stamped;
    const MigotoPort::PipeState& st = MigotoPort::State();
    const int si = MigotoPort::StageIndex('p');
    const CmdGfxRoot& g = g_command_lists.GfxRoot(cmd_list);
    reshade::api::device* const dev = cmd_list->get_device();
    const uint32_t inc =
        g_addon.d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    unsigned n_written = 0;
    unsigned n_unread = 0;
    unsigned n_noslot = 0;
    unsigned n_ours = 0;
    for (unsigned reg = 0; reg < (unsigned)MigotoPort::kMaxSRV; ++reg) {
        if ((int32_t)(st.srv_gen[si][reg] - srv_gen0) <= 0) {
#ifdef WWMI_DEBUG
            if (g_tex_trace.Armed()) TraceUnwritten(cmd_list, g, reg);
#endif
            continue;
        }
        ++n_written;
        if (!GameShaderReads(cmd_list, 'p', reg)) {
            ++n_unread;
            continue;
        }
        const RegBind rb = GameRegister(g, 'p', 't', reg);
        if (rb.param >= 64 || !rb.is_table || g.kind[rb.param] != CmdGfxRoot::kTable) {
            ++n_noslot;
            continue;
        }
        const TableStart ts = TableStartOf(dev, reshade::api::descriptor_table{ g.value[rb.param] });
        if (!ts) {
            ++n_noslot;
            continue;
        }
        const HeapKey key{ ts.heap, ts.base + rb.offset };
        const MigotoPort::WrapView* wv = MigotoPort::AsWrapView(st.srv[si][reg]);
        const bool ours = wv && wv->cpu.ptr && wv->wres && wv->wres->dx12;
#ifdef WWMI_DEBUG
        const uint32_t game_hash = g_debug ? NoteOverridePick(cmd_list, reg, key, st.srv[si][reg]) : 0;
#endif
        std::lock_guard<std::mutex> l(s_mtx);
        if (!ours) {
            const auto it = s_stamped.find(key);
            if (it == s_stamped.end()) {
#ifdef WWMI_DEBUG
                TraceCheck(game_hash, cmd_list, reg, key, L"its sections left no mod texture; no check stamped here");
#endif
                continue;
            }
#ifdef WWMI_DEBUG
            if (g_debug) NoteOverrideUnstamp(cmd_list, reg, key, it->second, st.srv[si][reg]);
#endif
            s_stamped.erase(it);
            g_stamps.UnstampSlot(key.heap, key.offset);
#ifdef WWMI_DEBUG
            TraceCheck(game_hash, cmd_list, reg, key,
                L"its sections left no mod texture; the game's texture given back");
#endif
            continue;
        }
        const D3D12_CPU_DESCRIPTOR_HANDLE dst{ ts.Heap()->GetCPUDescriptorHandleForHeapStart().ptr +
            (SIZE_T)key.offset * inc };
        g_stamps.BackupBeforeStamp(key.heap, key.offset, wv->cpu.ptr);
        g_addon.d3d12_device->CopyDescriptorsSimple(1, dst, wv->cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        g_descriptors.SetSwapped(key, wv->wres->dx12);
        s_stamped[key] = wv->wres->dx12;
        ++n_ours;
#ifdef WWMI_DEBUG
        if (g_tex_trace.On(game_hash))
            TraceCheck(game_hash, cmd_list, reg, key, L"stamped " + g_tex_trace.ModText(wv->wres->dx12));
#endif
    }
    if (g_debug && n_written) {
        uint64_t ps = 0;
        cmd_list->get_private_data(s_iid_ps_hash, &ps);
        if (DiagChanged("OVR-STAMP", (int)(ps ^ (ps >> 32)), (unsigned long long)n_ours))
            logger::write("OVR-STAMP ps=%016llx: %u ps-t slot(s) written by its sections, %u the shader does not "
                          "read, %u with no table slot, %u stamped with a mod texture",
                (unsigned long long)ps, n_written, n_unread, n_noslot, n_ours);
    }
}

void GameBackBuffer(PortApi::DrawListScope& scope, reshade::api::command_list* cmd_list, const CmdRT& rt)
{
    ID3D12Resource* const rt0 = rt.valid && rt.rtv_count && rt.rtv[0]
        ? reinterpret_cast<ID3D12Resource*>(cmd_list->get_device()->get_resource_from_view({ rt.rtv[0] }).handle)
        : nullptr;
    const bool bb = scope.BackBuffer(rt0);
#ifdef WWMI_DEBUG
    if (g_debug && DiagOnce("SCOPE-BB", bb ? 1 : 0))
        logger::write("SCOPE-BB: a draw scope on the game's list, its render target %p %s", (void*)rt0,
            bb ? "is a swap-chain buffer: `bb` names it" : "is not a swap-chain buffer: `bb` names nothing");
#else
    (void)bb;
#endif
}

void RestoreGameState(DrawContext& ctx)
{
    reshade::api::command_list* const cmd_list = ctx.args.cmd_list;
    ID3D12GraphicsCommandList* const d3d12_cl = ctx.args.d3d12_cl;
    const DrawClaim& dc = ctx.claim;
    PROF(PROF_DI_Restore); // runs on EVERY claimed draw: VBs + IB + 2 CBVs + non-PSO state
    const bool x32_skip_ia = ctx.x32_mid;
    if (ctx.x32_mid) {
        ++ctx.x32_mid_restores;
        if (x32_skip_ia) ++ctx.x32_mid_skipped;
    }
    if (!x32_skip_ia) {
        if (ctx.reloc_xit_n) {
            GpuBracket _gt(d3d12_cl, GT_RelocRestore);
            for (unsigned x = 0; x < ctx.reloc_xit_n; ++x)
                MigotoPort::OwnedTransition(d3d12_cl, ctx.reloc_xit[x].res, ctx.reloc_xit[x].back);
        }
        ctx.reloc_xit_n = 0;
        if (!g_draw_toggles.restore_vbib && ctx.reloc_vb_mask) {
            for (int s = 0; s < 8; ++s)
                if ((ctx.reloc_vb_mask & (1u << s)) && ctx.save_vb_set[s])
                    d3d12_cl->IASetVertexBuffers(s, 1, &ctx.save_vb[s]);
            if (ctx.save_ib_set) d3d12_cl->IASetIndexBuffer(&ctx.save_ib);
        }
        if (g_draw_toggles.restore_vbib) {
            for (int s = 0; s < 5; ++s) {
                if (ctx.save_vb_set[s])
                    d3d12_cl->IASetVertexBuffers(s, 1, &ctx.save_vb[s]);
                else {
                    D3D12_VERTEX_BUFFER_VIEW nullv = { 0, 0, 0 };
                    d3d12_cl->IASetVertexBuffers(s, 1, &nullv);
                }
            }
            for (int s = 5; s < 8; ++s)
                if (ctx.reloc_vb_mask & (1u << s)) d3d12_cl->IASetVertexBuffers(s, 1, &ctx.save_vb[s]);
            if (ctx.save_ib_set) d3d12_cl->IASetIndexBuffer(&ctx.save_ib);
        }
    } // !x32_skip_ia -- X32: a mid-section call leaves the IA state and the
    // relocation transitions to the hook's own end-of-draw restore.
    if (ctx.cb_ok && g_draw_toggles.restore_cb) {
        if (dc.b4_va && dc.b4_root >= 0 && dc.b4_root < 64)
            d3d12_cl->SetGraphicsRootConstantBufferView((UINT)dc.b4_root, dc.b4_va);
        if (dc.b3_root >= 0 && dc.b3_va) d3d12_cl->SetGraphicsRootConstantBufferView((UINT)dc.b3_root, dc.b3_va);
    }
    if (g_draw_toggles.restore_nonpso) {
        auto& gs = g_command_lists.Gfx(cmd_list);
        if (MigotoPort::g_touched_viewport) {
            if (gs.vp_count) d3d12_cl->RSSetViewports(gs.vp_count, gs.vp);
            if (gs.scissor_count) d3d12_cl->RSSetScissorRects(gs.scissor_count, gs.scissor);
            MigotoPort::g_touched_viewport = false;
        }
        if (gs.topology) d3d12_cl->IASetPrimitiveTopology((D3D12_PRIMITIVE_TOPOLOGY)gs.topology);
        if (MigotoPort::g_touched_blend && gs.blend_factor_set) d3d12_cl->OMSetBlendFactor(gs.blend_factor);
        if (MigotoPort::g_touched_stencil && gs.stencil_ref_set) d3d12_cl->OMSetStencilRef(gs.stencil_ref);
        // Cleared even when we could not restore, so a set flag never leaks into the next draw.
        MigotoPort::g_touched_blend = MigotoPort::g_touched_stencil = false;
        if (!gs.topology || !gs.blend_factor_set || !gs.stencil_ref_set) {
            static std::atomic<bool> once{ false };
            bool e = false;
            if (once.compare_exchange_strong(e, true))
                logger::write("RESTORE-GAP: injected draw could not restore non-PSO state never "
                              "observed from the game (topology=%d blend=%d stencil=%d) -- it "
                              "stays as our draw left it",
                    (int)(gs.topology != 0), (int)gs.blend_factor_set, (int)gs.stencil_ref_set);
        }
    }
}
