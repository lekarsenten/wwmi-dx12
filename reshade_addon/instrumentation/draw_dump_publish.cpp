// draw_dump_publish.cpp -- what a frame-analysis capture is told about a draw: the game's bound targets,
// textures, geometry and VS constant buffers, published into their engine's PipeState. See draw_dump_publish.h.
#include "../addon_common.h"
#include "../hooks/resource_tracker.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../hooks/pipeline_registry.h" // PipelineRegistry::SrvRegs -- what the bound shaders read (PSV0)
#include "../rabbitfx/rabbitfx_bind.h" // RfxBinder::BindsForDump
#include "draw_dump_publish.h"
#include "diag_arms.h"
#include "../mods/buffer_relocation.h"
#include "../rabbitfx/rfx_patch.h" // kRfxParamsReg
#include "../textures/tex_toggles.h"
#include "../textures/tex_rules.h"
#include "../textures/tex_identity.h"
#include "../textures/tex_select.h"
#include "../textures/mod_texture_store.h"
#include "../migoto/dx12_input_layout.h"
#include "../migoto/frame_analysis_driver.h"
#include "../migoto/port_api.h"
#include "../hooks/command_list_registry.h"

void PublishGameBoundStateForDump(command_list* cmd_list)
{
    // DEPTH: the DSV this command list has bound, resolved to its resource.
    uint64_t dsv_handle = 0;
    CmdRT snap = g_command_lists.Rt(cmd_list);
    if (snap.has_dsv) dsv_handle = snap.dsv;
    if (dsv_handle) {
        if (device* dev = cmd_list->get_device()) {
            resource r = dev->get_resource_from_view(resource_view{ dsv_handle });
            PortApi::PushGameDepthTarget(r.handle ? reinterpret_cast<ID3D12Resource*>(r.handle) : nullptr);
        }
    } else {
        PortApi::PushGameDepthTarget(nullptr);
    }

    if (device* dev = cmd_list->get_device()) {
        for (uint32_t i = 0; i < 8; ++i) {
            ID3D12Resource* rt = nullptr;
            if (i < snap.rtv_count && snap.rtv[i]) {
                resource r = dev->get_resource_from_view(resource_view{ snap.rtv[i] });
                rt = r.handle ? reinterpret_cast<ID3D12Resource*>(r.handle) : nullptr;
            }
            PortApi::PushGameRenderTarget(i, rt);
        }
    }
}

void PublishGameTexturesForDump(command_list* cmd_list)
{
    if (!g_diag_arms.dump_game_textures) return;
    device* dev = cmd_list->get_device();
    if (!dev) return;

    const uint64_t layh = g_command_lists.GfxLayout(cmd_list);
    static thread_local LayoutRegMap rm;
    if (!g_layouts.WithRegMap(layh, [&](const LayoutRegMap& m) { rm = m; })) {
        char line[160];
        snprintf(line, sizeof line, "%06u textures NOT PUBLISHED: no register map for layout %llx",
            MigotoPort::FrameAnalysisDrawIndex(), (unsigned long long)layh);
        MigotoPort::FrameAnalysisNote(line);
        return;
    }
    const auto& root = g_command_lists.Gfx(cmd_list).root;
    const unsigned di = MigotoPort::FrameAnalysisDrawIndex();
    const bool swap_live =
        g_tex_toggles.placement_swap && !g_tex_toggles.disable && g_tex_rules.Ready(); // TexPlacement::Swap's own gate
    const uint64_t pso = g_command_lists.Gfx(cmd_list).gfx_pso;
    const DxilSrvRegs ps_regs = g_pipelines.SrvRegs(pso, 'p');
    const DxilSrvRegs vs_regs = g_pipelines.SrvRegs(pso, 'v');
    auto publish_stage = [&](char stage, const RegBind* regs, const DxilSrvRegs& bound) {
        if (bound.parsed) {
            unsigned n = 0;
            for (unsigned r = 0; r < 128; ++r) n += bound.has(r) ? 1 : 0;
            char line[160];
            snprintf(line, sizeof line,
                "%06u %cs textures: the shader binds %u t register(s)%s (PSV0); the rest of the root signature's are not published",
                di, stage, n, bound.unbounded ? " plus an unbounded range" : "");
            MigotoPort::FrameAnalysisNote(line);
        }
        for (uint32_t r = 0; r < 128; ++r) {
            const RegBind& b = regs[r];
            if (b.param == 0xFF || b.param >= 64 || !b.is_table) continue; // a root SRV is a buffer, not a texture
            if (!bound.reads(r)) continue; // declared by the layout, never read by the shader
            const TableStart ts = TableStartOf(dev, root[b.param].table);
            if (!ts) continue;
            const uint32_t slot = ts.base + b.offset;
            ResInfo ri;
            if (!g_descriptors.Find({ ts.heap, slot }, ri) || !ri.res) continue;
            const uint64_t rk = reinterpret_cast<uint64_t>(ri.res);
            TexInfo d{};
            if (!g_tex_identity.DimsOf(rk, &d) || d.width == 0) continue; // a texture still alive, not a buffer view
            const uint32_t chash = g_tex_identity.HashOf(rk); // content hash == the ini's hash=
            ID3D12Resource* res = ri.res;
            uint32_t hash = chash;
            const char* src = "GAME";
            if (const uint32_t asset = g_tex_select.LodTargetOf(chash)) hash = asset;
            const uint32_t rule = swap_live && chash ? g_tex_select.RuleHashFor(chash) : 0;
            const bool rep_now = rule && g_tex_select.Replacement(rule).ptr != 0;
            if (ri.swapped) {
                if (g_mod_textures.IndexOf(ri.swapped) >= 0) {
                    res = ri.swapped;
                    hash = 0;
                    src = "MOD (stamped)";
                } else
                    src = "GAME (stamp record is not a live mod texture -- ignored)";
            } else if (rep_now)
                src = ri.miss_frame
                    ? "GAME -- NOT STAMPED: the placement's selection was EMPTY (see miss_f), the rule selects a replacement now"
                    : "GAME -- NOT STAMPED though the rule selects a replacement now (no placement miss recorded)";
            const int pushed = PortApi::PushGameTexture(stage, r, res, hash);
            char line[320];
            snprintf(line, sizeof line,
                "%06u %cs-t%u <- %s res=%p stamped=%p miss_f=%u placed_f=%u now_f=%u gamehash=%08x rule=%08x rep_now=%d game=%ux%u fmt=%u p%u+%u%s",
                di, stage, r, src, (void*)res, (void*)ri.swapped, ri.miss_frame, ri.wrote_frame, g_frame_id, chash,
                rule, rep_now ? 1 : 0, d.width, d.height, d.format, (unsigned)b.param, b.offset,
                pushed == PortApi::kPushTexOverMod ? " (over a PipeState mod bind the game PSO never reads)" : "");
            MigotoPort::FrameAnalysisNote(line);
        }
    };
    publish_stage('p', rm.t_ps, ps_regs);
    publish_stage('v', rm.t_vs, vs_regs);
    if (const auto* rb = g_rfx_bind.BindsForDump(di)) {
        for (const RfxDumpBind& b : *rb) {
            char line[240];
            if (b.res) {
                const int pushed = PortApi::PushGameTexture('p', (unsigned)b.reg, b.res, 0);
                snprintf(line, sizeof line,
                    "%06u ps-t%d <- RFX shadow bind: MOD res=%p (over the game's register above)%s", di, b.reg,
                    (void*)b.res, pushed == PortApi::kPushTexOverMod ? " (over a PipeState mod bind)" : "");
            } else {
                snprintf(line, sizeof line, "%06u ps-t%d <- RFX shadow bind: %s (not published)", di, b.reg,
                    b.srv.ptr
                        ? (b.reg == (int)kRfxParamsReg ? "the glow params buffer"
                                                       : "a descriptor that is not a mod texture (null, or the game's)")
                        : "no descriptor");
            }
            MigotoPort::FrameAnalysisNote(line);
        }
    }
}

thread_local unsigned t_x24_vb_mask = 0;
thread_local bool t_x24_ib = false;

uint64_t X27ResolveVsCbs(command_list* cmd_list, X27Cb out[8])
{
    for (int r = 0; r < 8; ++r) out[r] = X27Cb{};
    uint64_t layh = 0;
    layh = g_command_lists.GfxLayout(cmd_list);
    const auto& root = g_command_lists.Gfx(cmd_list).root;
    if (!layh) return 0;
    RegBind rb[8];
    if (!g_layouts.WithRegMap(layh, [&](const LayoutRegMap& m) {
            for (int r = 0; r < 8; ++r) rb[r] = m.b_vs[r];
        }))
        return layh;
    const CmdCbvBinds& cb = g_command_lists.Cbv(cmd_list);
    device* dev = cmd_list->get_device();
    for (int r = 0; r < 8; ++r) {
        if (rb[r].param == 0xFF || rb[r].param >= 64) continue;
        X27Cb& o = out[r];
        o.param = rb[r].param;
        o.off = rb[r].offset;
        if (!rb[r].is_table) {
            o.kind = 1;
            o.res = cb.binds[o.param].res;
            o.va = cb.binds[o.param].va;
            o.size = cb.binds[o.param].size;
            continue;
        }
        o.kind = 3;
        if (!dev) continue;
        const TableStart ts = TableStartOf(dev, root[o.param].table);
        HeapCbv hc;
        if (ts && g_descriptors.FindCbv({ ts.heap, ts.base + o.off }, hc)) {
            o.kind = 2;
            o.res = hc.res;
            o.va = hc.va;
            o.size = hc.size;
        }
    }
    return layh;
}
void X27Format(char* buf, size_t cap, const X27Cb cbs[8])
{
    size_t n = 0;
    buf[0] = 0;
    for (int r = 0; r < 8 && n + 72 < cap; ++r) {
        const X27Cb& o = cbs[r];
        if (o.kind == 0) continue;
        const int k = _snprintf_s(buf + n, cap - n, _TRUNCATE, " b%d=%s p%u+%u va=%llx sz=%u", r,
            o.kind == 1       ? "root"
                : o.kind == 2 ? "table"
                              : "table-unrecorded",
            (unsigned)o.param, o.off, (unsigned long long)o.va, o.size);
        if (k < 0) break;
        n += (size_t)k;
    }
}
thread_local unsigned t_x27_cb_mask = 0;

void PublishGameDrawForDump(command_list* cmd_list, bool injected, ID3D12Resource* sub6_res, uint64_t sub6_va,
    uint32_t sub6_bytes, uint8_t sub6_verdict)
{
    if (!MigotoPort::FrameAnalysisArmed()) return;
    t_x24_vb_mask = 0;
    t_x24_ib = false;
    uint64_t gva[8] = {};
    uint32_t gsz[8] = {};
    uint32_t gstr[8] = {};
    ID3D12Resource* gres[8] = {};
    uint64_t ib_va = 0;
    uint32_t ib_size = 0;
    uint32_t ib_bytes = 0;
    ID3D12Resource* ib_res = nullptr;
    uint64_t pso = 0;
    {
        auto const& gs = g_command_lists.Gfx(cmd_list);
        for (unsigned s = 0; s < 8; ++s) {
            gva[s] = gs.vb[s].va;
            gsz[s] = gs.vb[s].size;
            gstr[s] = gs.vb[s].stride;
            gres[s] = gs.vb[s].res;
        }
        ib_va = gs.ib.va;
        ib_size = gs.ib.size;
        ib_bytes = gs.ib.index_bytes;
        ib_res = gs.ib.res;
        pso = gs.gfx_pso;
    }
    // A CommandListRegistry::Gfx pointer is a bind-time record, not a reference: accept it only while the VA maps still know it.
    auto alive = [](ID3D12Resource* r, uint64_t* base) -> bool {
        if (!r) return false;
        uint64_t va = 0;
        if (!g_resources.VaOf(reinterpret_cast<uint64_t>(r), &va)) return false;
        if (base) *base = va;
        return true;
    };
    char line[360];
    const unsigned di = MigotoPort::FrameAnalysisDrawIndex();
    if (!injected) {
        MigotoPort::SetCurrentInputLayout(pso);
        PublishGameBoundStateForDump(cmd_list);
        PublishGameTexturesForDump(cmd_list);
        uint64_t base = 0;
        if (ib_va && ib_bytes && alive(ib_res, &base) && ib_va >= base) {
            const unsigned off = (unsigned)(ib_va - base);
            PortApi::PushGameIndexBuffer(ib_res, base, ib_size + off, ib_bytes, off);
            t_x24_ib = true;
            snprintf(line, sizeof line, "%06u ib <- GAME res=%p va=%llx bytes=%u index_bytes=%u", di, (void*)ib_res,
                (unsigned long long)ib_va, ib_size, ib_bytes);
            MigotoPort::FrameAnalysisNote(line);
        }
    }
    for (unsigned s = 0; s < 8; ++s) {
        if (s == 6 && injected && sub6_verdict) {
            // What RELOCBIND put on the native list for THIS draw. PipeState never saw it.
            if (sub6_res && sub6_va) {
                const uint64_t base = sub6_res->GetGPUVirtualAddress(); // ours (widened / frozen copy): alive
                const unsigned off = (sub6_va >= base) ? (unsigned)(sub6_va - base) : 0u;
                PortApi::PushGameVertexBuffer(6, sub6_res, base, sub6_bytes + off, gstr[6], off);
                t_x24_vb_mask |= 1u << 6;
            }
            snprintf(line, sizeof line,
                "%06u vb6 <- PORT %s res=%p va=%llx bytes=%u stride=%u (game had bound %p va=%llx)", di,
                sub6_verdict == 1       ? "SUBSTITUTE: this mod's WIDENED buffer (the chain's output)"
                    : sub6_verdict == 6 ? "FROZEN COPY of the widened buffer"
                                        : "NULL view (zeros)",
                (void*)sub6_res, (unsigned long long)sub6_va, sub6_bytes, gstr[6], (void*)gres[6],
                (unsigned long long)gva[6]);
            MigotoPort::FrameAnalysisNote(line);
            continue;
        }
        if (injected && PortApi::PipeStateVbBound(s)) continue; // the ini bound it: already in the dump as theirs
        if (!gva[s] || !gstr[s]) continue;
        uint64_t base = 0;
        if (!alive(gres[s], &base) || gva[s] < base) {
            snprintf(line, sizeof line,
                "%06u vb%u <- GAME va=%llx stride=%u NOT PUBLISHED: resource %p no longer tracked", di, s,
                (unsigned long long)gva[s], gstr[s], (void*)gres[s]);
            MigotoPort::FrameAnalysisNote(line);
            continue;
        }
        const unsigned off = (unsigned)(gva[s] - base);
        PortApi::PushGameVertexBuffer(s, gres[s], base, gsz[s] + off, gstr[s], off);
        t_x24_vb_mask |= 1u << s;
        RelocBuf rb{};
        if (g_buffer_reloc.Resolve(gres[s], &rb))
            snprintf(line, sizeof line,
                "%06u vb%u <- GAME res=%p va=%llx bytes=%u stride=%u -- the VANILLA buffer the port "
                "WIDENED (widened copy %p, %llu B); this draw reads the game's own contents, not the chain's",
                di, s, (void*)gres[s], (unsigned long long)gva[s], gsz[s], gstr[s], (void*)rb.res,
                (unsigned long long)rb.bytes);
        else
            snprintf(line, sizeof line, "%06u vb%u <- GAME res=%p va=%llx bytes=%u stride=%u", di, s, (void*)gres[s],
                (unsigned long long)gva[s], gsz[s], gstr[s]);
        MigotoPort::FrameAnalysisNote(line);
    }
    {
        X27Cb cbs[8];
        const uint64_t layh = X27ResolveVsCbs(cmd_list, cbs);
        char desc[420];
        X27Format(desc, sizeof desc, cbs);
        char x27[560];
        snprintf(x27, sizeof x27, "%06u vs-cb map <- %s layout=%llx:%s", di, injected ? "INJECTED" : "GAME",
            (unsigned long long)layh, desc[0] ? desc : " (layout not mapped)");
        MigotoPort::FrameAnalysisNote(x27);
        for (unsigned r = 0; r < 8; ++r) {
            const X27Cb& o = cbs[r];
            if (!o.res || !o.va) continue;
            if (PortApi::PipeStateCbBound('v', r)) continue; // an ini bind: already in the dump as theirs
            uint64_t base = 0;
            if (!alive(o.res, &base) || o.va < base) {
                snprintf(x27, sizeof x27, "%06u vs-cb%u <- GAME va=%llx NOT PUBLISHED: resource %p no longer tracked",
                    di, r, (unsigned long long)o.va, (void*)o.res);
                MigotoPort::FrameAnalysisNote(x27);
                continue;
            }
            const unsigned bytes = (o.size && o.size <= 65536u) ? o.size : 4096u;
            PortApi::PushGameConstantBufferRecord('v', r, o.res, o.va, bytes);
            t_x27_cb_mask |= 1u << r;
        }
    }
}

void PublishGameDrawForDumpEnd()
{
    for (unsigned s = 0; s < 8; ++s)
        if (t_x24_vb_mask & (1u << s)) PortApi::PushGameVertexBuffer(s, nullptr, 0, 0, 0, 0);
    if (t_x24_ib) PortApi::PushGameIndexBuffer(nullptr, 0, 0, 0, 0);
    for (unsigned r = 0; r < 8; ++r)
        if (t_x27_cb_mask & (1u << r)) PortApi::PushGameConstantBufferRecord('v', r, nullptr, 0, 0);
    t_x24_vb_mask = 0;
    t_x24_ib = false;
    t_x27_cb_mask = 0;
}
