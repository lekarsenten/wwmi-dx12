// section_ps_stamp.cpp -- see section_ps_stamp.h.
#include "../addon_common.h"
#include "section_ps_stamp.h"
#include "../mods/mod_set.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/stamp_ledger.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../hooks/pipeline_registry.h" // PipelineRegistry::SrvRegs -- what the bound shaders read (PSV0)
#include "../textures/tex_defer.h" // a mod's textures still loading: its null is "not yet", not "unbind"
#include "../rabbitfx/rabbitfx_bind.h"
#include <algorithm>
#include "../addon/settings.h"
#include "draw_toggles.h"
#include "../textures/tex_toggles.h"
#include "../textures/mod_texture_store.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/dx12_wrap.h"
#include "../addon/addon.h"

SectionPsStamper g_ps_stamps;

struct SectionPsStamper::PerSet : SetState {
    std::unordered_map<uint64_t, uint64_t> game_side; // (mod, component, ps register)
};

void SectionPsStamper::ForThisFrame()
{
    if (stamped_frame_ == g_frame_id) return;
    stamped_.clear();
    stamped_frame_ = g_frame_id;
}

// A20: how many mips below the largest it has had there the game draws the texture it bound at `at`.
unsigned SectionPsStamper::GameMip(int dm, int comp, unsigned reg, const HeapKey& at)
{
    ResInfo ri{};
    if (!g_descriptors.Find(at, ri) || !ri.res) return ModTextureStore::kMipUnknown;
    const D3D12_RESOURCE_DESC d = ri.res->GetDesc();
    if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return ModTextureStore::kMipUnknown;
    const uint64_t side = (std::max)(d.Width, (UINT64)d.Height);
    const uint64_t key = (uint64_t)(uint32_t)CompKey(dm, comp) * MigotoPort::kMaxSRV + reg;
    std::unordered_map<uint64_t, uint64_t>& game_side = CurrentSet().State<PerSet>().game_side;
    uint64_t most = 0;
    {
        std::shared_lock l(game_side_mutex_);
        const auto it = game_side.find(key);
        if (it != game_side.end()) most = it->second;
    }
    if (side > most) {
        std::unique_lock l(game_side_mutex_);
        uint64_t& m = game_side[key];
        m = (std::max)(m, side);
        return 0;
    }
    unsigned k = 0;
    while ((side << (k + 1)) <= most) ++k;
    return k;
}

void SectionPsStamper::PlanStamps(SectionPsStamps& out, device* dev, const RootParam* root, const RegBind* t_ps,
    uint64_t pso, uint64_t layh, uint32_t gen0, const std::vector<uint32_t>& this_gen)
{
    const int dm = out.dm;
    const int comp = out.comp;
    const MigotoPort::PipeState& st = MigotoPort::State();
    const int si = MigotoPort::StageIndex('p');
    const DxilSrvRegs ps_regs = g_pipelines.SrvRegs(pso, 'p');
    for (unsigned r = 0; r < MigotoPort::kMaxSRV; ++r) {
        if ((int32_t)(st.srv_gen[si][r] - gen0) <= 0) continue;
        if (!ps_regs.reads(r)) {
            if (g_debug && DiagOnce("PS-SLOT-UNREAD", CompKey(dm, comp) * 128 + (int)r))
                logger::write("PS-SLOT-STAMP mod=%d comp=%d ps-t%u: the pixel shader does not read it -- not stamped",
                    dm, comp, r);
            continue;
        }
        if (r < this_gen.size() && this_gen[r] && st.srv_gen[si][r] == this_gen[r]) {
            if (g_debug && DiagOnce("PS-SLOT-THIS", CompKey(dm, comp) * 128 + (int)r))
                logger::write("PS-SLOT-STAMP mod=%d comp=%d ps-t%u: bound by a texture section's `this =` -- not "
                              "stamped, the texture module places texture swaps",
                    dm, comp, r);
            continue;
        }
        D3D12_CPU_DESCRIPTOR_HANDLE src{ 0 };
        ID3D12Resource* res = nullptr;
        bool stored = false;
        int index = -1;
        if (ID3D11ShaderResourceView* v = st.srv[si][r]) {
            MigotoPort::WrapView* wv = MigotoPort::AsWrapView(v);
            if (!wv || wv->kind != 't' || !wv->cpu.ptr) continue; // a dump publication, or not a view of ours
            src = wv->cpu;
            res = wv->wres ? wv->wres->dx12 : nullptr;
            index = g_mod_textures.IndexOf(res);
            g_mod_textures.Touch(index);
            stored = index >= 0;
        } else {
            if (!g_draw_toggles.ps_slot_stamp_nulls || g_tex_defer.Pending(dm) || PortApi::BoundLoading()) continue;
            src = g_stamps.NullSrv(g_addon.d3d12_device, ps_regs.parsed ? ps_regs.kind[r] : 0);
            if (!src.ptr) continue;
        }
        const RegBind& b = t_ps[r];
        if (b.param == 0xFF || b.param >= 64 || !b.is_table) {
            if (g_debug && DiagOnce("PS-SLOT-UNDECLARED", (int)((layh ^ (layh >> 32)) * 131u + r)))
                logger::write(
                    "PS-SLOT-STAMP mod=%d comp=%d ps-t%u: layout %llx declares no table slot for it (param=%u) -- not applied",
                    dm, comp, r, (unsigned long long)layh, (unsigned)b.param);
            continue;
        }
        const TableStart ts = TableStartOf(dev, root[b.param].table);
        if (!ts) continue;
        const uint32_t slot = ts.base + b.offset;
        if (slot >= ts.Heap()->GetDesc().NumDescriptors) continue;
        if (stored && g_settings.tex_distance) g_mod_textures.TouchAt(index, GameMip(dm, comp, r, { ts.heap, slot }));
        SectionPsSlot s;
        s.reg = r;
        s.param = b.param;
        s.offset = b.offset;
        s.heap = ts.heap;
        s.slot = slot;
        s.src = src;
        s.res = res;
        s.stored = stored;
        out.slots.push_back(s);
    }
}

SectionPsStamps SectionPsStamper::Plan(
    command_list* cmd_list, uint32_t gen0, const std::vector<uint32_t>& this_gen, int dm, int comp)
{
    SectionPsStamps out;
    out.dm = dm;
    out.comp = comp;
    if (!g_draw_toggles.ps_slot_stamp || g_tex_toggles.disable || !g_addon.d3d12_device || !cmd_list) return out;
    const MigotoPort::PipeState& st = MigotoPort::State();
    const int si = MigotoPort::StageIndex('p');
    bool any = false;
    for (unsigned r = 0; r < MigotoPort::kMaxSRV && !any; ++r) any = (int32_t)(st.srv_gen[si][r] - gen0) > 0;
    device* dev = cmd_list->get_device();
    const uint64_t layh = g_command_lists.GfxLayout(cmd_list);
    static_assert(MigotoPort::kMaxSRV == RegisterFileWidth(TargetClass::ShaderResource),
        "PipeState and the register map disagree on the SRV file width");
    RegBind t_ps[MigotoPort::kMaxSRV];
    if (!g_layouts.WithRegMap(layh, [&](const LayoutRegMap& m) { memcpy(t_ps, m.t_ps, sizeof t_ps); })) {
        if (any && g_debug && DiagOnce("PS-SLOT-NOMAP", (int)(layh ^ (layh >> 32))))
            logger::write(
                "PS-SLOT-STAMP mod=%d comp=%d: no register map for layout %llx -- the section's ps-tN not applied", dm,
                comp, (unsigned long long)layh);
        return out;
    }
    const CmdState& gs = g_command_lists.Gfx(cmd_list);
    if (any) PlanStamps(out, dev, gs.root, t_ps, gs.gfx_pso, layh, gen0, this_gen);
    std::vector<HeapKey> read_as_bound;
    const uint64_t reads = g_pipelines.PsReadMask(gs.gfx_pso);
    for (unsigned r = 0; r < 64; ++r) {
        if (!((reads >> r) & 1u)) continue;
        if (std::any_of(out.slots.begin(), out.slots.end(), [r](const SectionPsSlot& s) { return s.reg == r; }))
            continue;
        const RegBind& b = t_ps[r];
        if (b.param == 0xFF || b.param >= 64 || !b.is_table) continue;
        const TableStart ts = TableStartOf(dev, gs.root[b.param].table);
        if (ts) read_as_bound.push_back(HeapKey{ ts.heap, ts.base + b.offset });
    }
    {
        std::lock_guard l(stamped_mutex_);
        ForThisFrame();
        for (const SectionPsSlot& s : out.slots) {
            auto it = stamped_.find({ s.heap, s.slot });
            if (it != stamped_.end() && it->second.src != s.src.ptr) out.shared = true;
        }
        for (const HeapKey& k : read_as_bound) stamped_.emplace(k, StampedSlot{ kReadAsBound, g_frame_id });
    }
    if (out.shared && g_debug && DiagOnce("PS-SLOT-SHARED", CompKey(dm, comp)))
        logger::write(
            "PS-SLOT-STAMP mod=%d comp=%d f=%u: a slot of this draw's table was stamped, or read unstamped, earlier "
            "this frame -- this draw binds through a private table",
            dm, comp, g_frame_id);
    return out;
}

void SectionPsStamper::StampInPlace(const SectionPsStamps& stamps)
{
    if (stamps.slots.empty()) return;
    const int dm = stamps.dm;
    const int comp = stamps.comp;
    const UINT inc = g_addon.d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::lock_guard<std::mutex> l(stamped_mutex_);
    ForThisFrame();
    for (const SectionPsSlot& s : stamps.slots) {
        ID3D12DescriptorHeap* heap = reinterpret_cast<ID3D12DescriptorHeap*>(s.heap);
        const D3D12_CPU_DESCRIPTOR_HANDLE dst{ heap->GetCPUDescriptorHandleForHeapStart().ptr + (SIZE_T)s.slot * inc };
        g_addon.d3d12_device->CopyDescriptorsSimple(1, dst, s.src, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (s.res) g_descriptors.SetSwapped({ s.heap, s.slot }, s.res); // the frame dump publishes what the GPU reads
        if (s.stored) g_stamps.StampedStoreTexture(s.heap, s.slot, s.res);
        stamped_[{ s.heap, s.slot }] = StampedSlot{ s.src.ptr, g_frame_id };
        g_draw_stats.ps_slot_stamp_count.fetch_add(1, std::memory_order_relaxed);
        if (g_debug &&
            DiagChanged("PS-SLOT-STAMP", CompKey(dm, comp) * 128 + (int)s.reg, (unsigned long long)s.src.ptr))
            logger::write("PS-SLOT-STAMP mod=%d comp=%d ps-t%u <- %s (res=%p) at heap %p slot %u (p%u+%u) f=%u", dm,
                comp, s.reg, s.res ? "section resource" : "null", (void*)s.res, (void*)heap, s.slot, (unsigned)s.param,
                s.offset, g_frame_id);
    }
}

void SectionPsStamps::QueueForShadow() const
{
    std::vector<ShadowSlotOvr> extra;
    extra.reserve(slots.size());
    for (const SectionPsSlot& s : slots) extra.push_back(ShadowSlotOvr{ s.param, s.offset, s.src });
    g_rfx_bind.AddExtraSlots(extra.data(), (unsigned)extra.size());
}
