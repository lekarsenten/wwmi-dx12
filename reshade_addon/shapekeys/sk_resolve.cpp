// The resolver: which buffer is this dispatch's u0, and whose character owns it. See sk_internal.h.
#include "sk_internal.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/removal_pin.h" // Refs: the buffers the chain picks by identity are named on the game's list
#include "../instrumentation/sk_ring.h" // Note: every decision below into the post-hoc ring (Numpad 1 / J3 dumps it)
#include "../dispatch/dispatch_toggles.h"
#include "../present/present_toggles.h"
#include "../hooks/hooks_toggles.h"
#include "DrawCallInfo.h"
#include "../hooks/command_list_registry.h"
#include "../mods/layout_registry.h"
#include "../mods/mod_set.h"

ShapekeyResolver g_sk_resolver(g_sk_store, g_sk_identity);

bool ShapekeyResolver::U0ResourceNow(command_list* cmd_list, ID3D12Resource** res, unsigned* size)
{
    *res = nullptr;
    *size = 0;
    const uint64_t clay = g_command_lists.CmpLayout(cmd_list);
    if (!clay) return false;
    // Root descriptor: the VA names the buffer through the memory map (liveness included).
    const int root = g_layouts.CsU0Root(clay);
    if (root >= 0 && root < 64) {
        ID3D12Resource* r = nullptr;
        uint64_t va = 0;
        {
            auto& stx = g_command_lists.Gfx(cmd_list);
            r = stx.root[root].res;
            va = stx.root[root].va;
        }
        if (r) g_resources.VaOf(reinterpret_cast<uint64_t>(r), &va);
        if (va) {
            ResInfo si{};
            if (g_resources.AtVa(va, &si) && si.res) {
                *res = si.res;
                *size = si.size;
                return true;
            }
        }
    }
    U0CsTable tbl{};
    if (!g_layouts.U0Table(clay, &tbl) || tbl.param >= 64) return false;
    descriptor_table dt = {};
    {
        auto& stx = g_command_lists.Gfx(cmd_list);
        dt = stx.root[tbl.param].compute_table();
    }
    if (!dt.handle) return false;
    descriptor_heap heap = { 0 };
    uint32_t off = 0;
    cmd_list->get_device()->get_descriptor_heap_offset(dt, tbl.offset, 0, &heap, &off);
    ResInfo ri;
    if (!heap.handle || !g_descriptors.Find({ heap.handle, off }, ri) || !ri.res) return false;
    *res = ri.res;
    *size = ri.size; // the size recorded while the view was live; never GetDesc() on ri.res
    return true;
}

bool ShapekeyResolver::U0Record(command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_x,
    uint32_t group_count_y, uint32_t group_count_z, SkU0Ident& ck, ResInfo& ri, uint64_t& clay,
    ID3D12Resource*& ck_res_for_ring)
{
    clay = g_command_lists.CmpLayout(cmd_list);
    U0CsTable u0t{};
    const bool have = clay && g_layouts.U0Table(clay, &u0t);
    ck.layout = clay;
    ck.have = (int)have;
    if (have) {
        ck.param = u0t.param;
        ck.off = u0t.offset;
    }
    if (g_debug && (!clay || !have)) {
        static ProbeBudget s_q;
        if (s_q.take(1))
            logger::write("CSU0-PUBLISH: no layout/table entry (cmpLayout=%llx have=%d) -- nothing to resolve",
                (unsigned long long)clay, (int)have);
    }
    if (have && u0t.param < 64) {
        descriptor_table dt = {};
        {
            auto& stx = g_command_lists.Gfx(cmd_list);
            const RootParam& rp = stx.root[u0t.param];
            dt = rp.compute_table();
            if (rp.ctable.handle && rp.ctable_note.frame) {
                ck.ct_match = (rp.ctable_note.layout == ck.layout) ? 1 : 0;
                ck.ct_age = (int)(g_frame_id - rp.ctable_note.frame);
                ck.ct_seqd = (int)(stx.ctable_seq_next - rp.ctable_note.seq);
            }
            // DEBUG ONLY: these three feed the heartbeat's U0TABLE line and nothing else.
            if (g_debug) {
                if (rp.ctable.handle)
                    g_sk_stats.u0tab_compute.fetch_add(1, std::memory_order_relaxed);
                else if (rp.table.handle)
                    g_sk_stats.u0tab_fallback.fetch_add(1, std::memory_order_relaxed);
                else
                    g_sk_stats.u0tab_none.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (g_debug && !dt.handle) {
            static ProbeBudget s_nt;
            if (s_nt.take(1))
                logger::write("CSU0-PUBLISH: no table bound at param=%u (compute or graphics) this dispatch",
                    (unsigned)u0t.param);
        }
        if (dt.handle) {
            descriptor_heap uheap = { 0 };
            uint32_t uoff = 0;
            cmd_list->get_device()->get_descriptor_heap_offset(dt, u0t.offset, 0, &uheap, &uoff);
            ck.heap = uheap.handle;
            ck.slot = uoff;
            if (g_debug) {
                ResInfo probe{};
                const bool hit = uheap.handle && g_descriptors.Find({ uheap.handle, uoff }, probe);
                char psig[128];
                _snprintf_s(psig, sizeof(psig), _TRUNCATE, "%d|%u|%u", (int)hit, hit ? probe.size : 0u,
                    hit ? probe.wrote_src : 0u);
                if (DiagChanged("U0IDENT", (int)u0t.param, Fnv1a64Str(psig)))
                    logger::write("U0IDENT param=%u off=%u heap=%llx slot=%u -> %s res=%p size=%u "
                                  "buf=%d texcand=%d hash=%08x age=%d src=%s | %s",
                        (unsigned)u0t.param, (unsigned)u0t.offset, (unsigned long long)uheap.handle, uoff,
                        hit ? "HIT" : "MISS (nothing recorded at this slot)", hit ? probe.res : nullptr,
                        hit ? probe.size : 0u, hit ? (int)probe.is_buffer : -1, hit ? (int)probe.tex_cand : -1,
                        hit ? PortApi::GameComputeUavBufferHash(probe.size) : 0u,
                        (hit && probe.wrote_frame) ? (int)(g_frame_id - probe.wrote_frame) : -1,
                        (!hit || !probe.wrote_src)   ? "NEVER-WRITTEN-BY-A-HOOK-WE-OWN"
                            : (probe.wrote_src == 1) ? "update_descriptor_tables"
                            : (probe.wrote_src == 2) ? "copy_descriptor_tables"
                                                     : "a view created in the slot",
                        (!hit)                  ? "MAP MISS -- a descriptor write we never saw"
                            : (probe.size == 0) ? "recorded with size 0 -- entry exists but carries no extent"
                                                : "recorded; if this is not the mod's buffer the slot was REUSED");
            }
            bool u_hit = (uheap.handle != 0) && g_descriptors.Find({ uheap.handle, uoff }, ri);
            if (uheap.handle && !u_hit)
                ck.why = 2; // heap lookup MISS
            else if (u_hit && !ri.res)
                ck.why = 3; // cache entry has no resource
            identity_.GeomExpect(cmd_list, cs_hash, group_count_y, &ck.geo_lo, &ck.geo_hi);
            if (ck.geo_hi) {
                const unsigned was = (u_hit && ri.res) ? ri.size : 0;
                if (u_hit && ri.res && ri.size >= ck.geo_lo && ri.size <= ck.geo_hi) {
                    ck.geo = 1;
                } else {
                    ID3D12Resource* g = nullptr;
                    unsigned gb = 0;
                    const int n = identity_.FindU0ByGeometry(cmd_list, ck.geo_lo, ck.geo_hi, &g, &gb);
                    if (n == 1) {
                        ri = ResInfo{};
                        ri.res = g;
                        ri.size = gb;
                        ri.is_buffer = true;
                        u_hit = true;
                        ck.geo = 2;
                        RemovalPin::Refs(
                            reinterpret_cast<const void*>(cmd_list->get_native()), g, "u0 fitting live buffer");
                    } else {
                        ri = ResInfo{};
                        u_hit = false;
                        ck.geo = (n == 0) ? 3 : 4;
                    }
                    if (g_debug) {
                        char sig[96];
                        _snprintf_s(sig, sizeof(sig), _TRUNCATE, "%d|%u|%u|%u|%d", ck.geo, was, ck.geo_hi, gb, n);
                        if (DiagChanged(
                                "U0GEOM", (cs_hash == g_dispatch_toggles.morph_cs_hash) ? 0 : 1, Fnv1a64Str(sig)))
                            logger::write("U0GEOM: cs=%016llx groups=%u,%u,%u expects %u..%u B for u0; the slot's "
                                          "record says %u B -> %s (%d live declared buffer(s) fit%s)",
                                (unsigned long long)cs_hash, group_count_x, group_count_y, group_count_z, ck.geo_lo,
                                ck.geo_hi, was,
                                (ck.geo == 2)       ? "REPLACED by the fitting live buffer"
                                    : (ck.geo == 3) ? "DROPPED, no live declared buffer fits"
                                                    : "DROPPED, more than one fits and this list's UAV "
                                                      "transitions did not single one out",
                                n, (n == 1) ? ", chosen" : "");
                    }
                }
            }
            // Record WHAT the slot answered with, before anything downstream filters on it.
            if (u_hit) ck_res_for_ring = ri.res;
            if (u_hit) {
                ck.ri_buf = (int)ri.is_buffer;
                ck.ri_tex = (int)ri.tex_cand;
                ck.ri_age = ri.wrote_frame ? (int)(g_frame_id - ri.wrote_frame) : -1;
                ck.ri_src = (int)ri.wrote_src;
            }
            return u_hit && ri.res;
        }
    }
    return false;
}

// The candidate's extent when its record carries none (size 0), recovered only from a live resource.
static unsigned SkU0Extent(
    command_list* cmd_list, SkU0Ident& ck, const ResInfo& ri, ID3D12Resource* cand, unsigned cand_bytes)
{
    if (!cand_bytes && g_sk_toggles.u0_size_recover && cand) {
        const bool live = g_resources.Live(cand);
        if (!live)
            ck.why = 4;
        else
            ck.why = 5; // 5 downgraded to 0 below on success

        if (!live && g_debug && ProbeArmed()) {
            static ProbeBudget s_pb;
            if (s_pb.take(4)) {
                reshade::api::device* dev = cmd_list->get_device();
                reshade::api::resource vres{};
                reshade::api::resource_desc vrd{};
                if (ri.view_handle && dev)
                    vres = dev->get_resource_from_view(reshade::api::resource_view{ ri.view_handle });
                if (vres.handle && dev) vrd = dev->get_resource_desc(vres);
                const size_t nmap = g_resources.BufferCount();
                logger::write("U0CREATE: res=%p ABSENT from g_res_to_va | view_handle=%llx -> "
                              "res_from_view=%llx type=%d buf_size=%llu | wrote_frame=%u src=%u "
                              "cur_frame=%u | map=%zu init_res_all=%u init_res_buf=%u "
                              "init_view_all=%u | is_buffer=%d tex_cand=%d rec_size=%u",
                    (void*)cand, (unsigned long long)ri.view_handle, (unsigned long long)vres.handle, (int)vrd.type,
                    (unsigned long long)vrd.buffer.size, ri.wrote_frame, (unsigned)ri.wrote_src, g_frame_id, nmap,
                    g_hooks_stats.init_res_all.load(std::memory_order_relaxed),
                    g_hooks_stats.init_res_buf.load(std::memory_order_relaxed),
                    g_hooks_stats.init_view_all.load(std::memory_order_relaxed), (int)ri.is_buffer, (int)ri.tex_cand,
                    ri.size);
            }
        }
        if (live) {
            const D3D12_RESOURCE_DESC rd = cand->GetDesc();
            if (rd.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER && rd.Width && rd.Width <= 0xFFFFFFFFull) {
                cand_bytes = (unsigned)rd.Width;
                if (g_debug) {
                    static ProbeBudget s_rc;
                    if (s_rc.take(8))
                        logger::write("U0SIZE: recovered extent %u B for res=%p (record had "
                                      "size 0) -> hash %08x. Without this the dispatch is "
                                      "refused with decl=0.",
                            cand_bytes, (void*)cand, PortApi::GameComputeUavBufferHash(cand_bytes));
                }
            }
        }
    }
    return cand_bytes;
}

// Which enabled, on-screen mod owns the candidate: ck.owner / ck.claims / ck.declared.
void ShapekeyResolver::U0Claim(SkU0Ident& ck, unsigned cand_hash, unsigned cand_bytes)
{
    ck.declared = 0;
    ck.owner = -1;
    ck.claims = 0;
    const bool is_cb_hash = PortApi::ShapeKeyCallbackDeclaresHash(cand_hash);
    // The four claim_* counters feed only the heartbeat's CLAIMGATE line: debug only.
    if (g_debug && !is_cb_hash) g_sk_stats.claim_nodecl.fetch_add(1, std::memory_order_relaxed);
    if (is_cb_hash) {
        // NO `break`. Every claimant is counted, because the count is the answer.
        for (int dmo = 0; dmo < (int)Mods().Count(); ++dmo) {
            if (!Mods().GeomEnabled(dmo)) continue;
            if (!Mods().OnScreen(dmo)) continue;
            if (!identity_.ModDeclaresCb(dmo, cand_hash)) continue;
            if (ck.claims == 0) ck.owner = dmo;
            ++ck.claims;
        }
        ck.declared = (ck.claims == 1) ? 1 : 0;
        if (g_debug) {
            if (ck.claims == 1)
                g_sk_stats.claim_ok.fetch_add(1, std::memory_order_relaxed);
            else if (ck.claims == 0)
                g_sk_stats.claim_offscreen.fetch_add(1, std::memory_order_relaxed);
            else
                g_sk_stats.claim_multi.fetch_add(1, std::memory_order_relaxed);
        }
        if (ck.claims > 1) {
            if (ProbeArmed() && DiagOnce("CSU0-AMBIGUOUS", (int)cand_hash)) {
                std::wstring who;
                for (int dmo = 0; dmo < (int)Mods().Count(); ++dmo) {
                    if (!Mods().GeomEnabled(dmo)) continue;
                    if (!Mods().OnScreen(dmo)) continue;
                    if (!identity_.ModDeclaresCb(dmo, cand_hash)) continue; // same test as the count above
                    if (!who.empty()) who += L", ";
                    who += Mods().At(dmo).wcl_ns;
                }
                logger::write("CSU0-AMBIGUOUS: desc=%08x (%u B) is declared by %d ON-SCREEN "
                              "mods (%ls) -- REFUSING to publish. A descriptor hash answers "
                              "WHOSE, never WHICH; picking one applies its `handling = skip` "
                              "to the other character's dispatch.",
                    cand_hash, cand_bytes, ck.claims, who.c_str());
            }
        }
        if (!ck.declared && g_debug) {
            if (ProbeArmed() && DiagOnce("CSU0-OWNER", (int)cand_hash))
                logger::write("CSU0-OWNER: desc=%08x (%u B) is a callback hash, but no "
                              "ENABLED, ON-SCREEN mod declares it -- NOT publishing. "
                              "Publishing on a library-wide match cancels an unmodded "
                              "character's own morph dispatch via a foreign "
                              "`handling = skip`.",
                    cand_hash, cand_bytes);
        }
    }
}

void ShapekeyResolver::ResolveU1(command_list* cmd_list, SkU0Ident& ck, ID3D12Resource* cand, unsigned cand_hash,
    std::wstring& owner_ns, unsigned& want_scale, ID3D12Resource*& u1_res, unsigned& u1_bytes)
{
    if (ck.declared && ck.owner >= 0 && ck.owner < (int)Mods().Count()) {
        owner_ns = Mods().At(ck.owner).wcl_ns;
        want_scale = identity_.ModScaleFor(ck.owner, cand_hash); // resolved at ini load
    } else {
        for (int dmx = 0; dmx < (int)Mods().Count(); ++dmx) {
            if (!identity_.ModDeclaresCb(dmx, cand_hash)) continue;
            want_scale = identity_.ModScaleFor(dmx, cand_hash);
            owner_ns = Mods().At(dmx).wcl_ns;
            break;
        }
    }
    ID3D12Resource* rb_res = nullptr;
    uint64_t rb_va = 0;
    uint32_t rb_size = 0;
    const bool have_rb = ResolveBound(cmd_list, 'c', TargetClass::UnorderedAccessView, 1, &rb_res, &rb_va, &rb_size) &&
        rb_res && rb_size;

    uint64_t live1 = 0;
    bool foreign1 = false;
    if (have_rb) g_resources.VaOf(reinterpret_cast<uint64_t>(rb_res), &live1); // liveness
    if (live1 && want_scale) {
        const unsigned got = PortApi::GameComputeUavBufferHash(g_buffer_reloc.IdentityBytes(rb_res, rb_size));
        if (got != want_scale) {
            if (logger::enabled(1) && DiagChanged("U1FOREIGN", (int)want_scale, got))
                logger::write("U1FOREIGN owner=%08x wantScale=%08x: register u1 holds %08x (%u B), not the declared "
                              "scale -- searching by identity",
                    cand_hash, want_scale, got, rb_size);
            live1 = 0;
            rb_res = nullptr;
            rb_size = 0;
            rb_va = 0;
            foreign1 = true;
        }
    }
    if (g_debug && want_scale && (!have_rb || foreign1)) {
        BindTrace bt;
        ResolveBound(cmd_list, 'c', TargetClass::UnorderedAccessView, 1, nullptr, nullptr, nullptr, &bt);
        SkRing::Note(SkRing::U1Reg, (uint64_t)cand_hash | ((uint64_t)want_scale << 32), bt.heap, (uint64_t)bt.res,
            (uint64_t)(bt.slot & 0xFFFFFF) | ((uint64_t)(bt.step & 0xFF) << 24) | ((uint64_t)bt.param << 32) |
                ((uint64_t)bt.table << 40) | ((uint64_t)(bt.size >> 4) << 41));
        if (DiagChanged("U1REG", (int)want_scale, (unsigned long long)bt.step)) {
            char tab[640] = {};
            size_t pos = 0;
            for (int k = 0; k < 8 && pos < sizeof(tab); ++k) {
                const unsigned id = bt.table_size[k] ? PortApi::GameComputeUavBufferHash(bt.table_size[k]) : 0u;
                const int w = _snprintf_s(tab + pos, sizeof(tab) - pos, _TRUNCATE, " [%d]%p/%u/%08x@f%u/s%u", k,
                    (void*)bt.table_res[k], bt.table_size[k], id, bt.table_wrote[k], (unsigned)bt.table_src[k]);
                if (w < 0) break;
                pos += (size_t)w;
            }
            logger::write("U1REG owner=%08x wantScale=%08x: register u1 lookup stopped at step %d (param=%u table=%d "
                          "heap=%llx slot=%u offset=%u slot-res=%p slot-size=%u) now=f%u | "
                          "table[k]=res/size/identity@written-frame/source(1 update,2 copy):%s",
                cand_hash, want_scale, bt.step, (unsigned)bt.param, (int)bt.table, (unsigned long long)bt.heap, bt.slot,
                bt.offset, (void*)bt.res, bt.size, g_frame_id, tab);
        }
    }
    int u1_ident = -1;
    if (!live1 && g_sk_toggles.u1_by_identity && want_scale && !owner_ns.empty()) {
        ID3D12Resource* ir = nullptr;
        unsigned ib = 0;
        char iwhy[256] = {};
        const bool want_why = logger::enabled(1);
        u1_ident = identity_.FindU1ByIdentity(
            cmd_list, want_scale, cand, &ir, &ib, want_why ? iwhy : nullptr, want_why ? sizeof(iwhy) : 0);
        if (u1_ident == 1 && ir && ib) {
            uint64_t va = 0;
            if (g_resources.VaOf(reinterpret_cast<uint64_t>(ir), &va)) {
                rb_res = ir;
                rb_size = ib;
                rb_va = va;
                live1 = va;
            }
            RemovalPin::Refs(reinterpret_cast<const void*>(cmd_list->get_native()), ir, "u1 by identity");
        }
        if (logger::enabled(1) &&
            DiagChanged("U1IDENT", (int)want_scale, ((unsigned long long)(unsigned)u1_ident << 32) | ib))
            logger::write("U1IDENT owner=%08x wantScale=%08x: register u1 is unbound or foreign (U1REG says which) "
                          "-> %s | %s",
                cand_hash, want_scale,
                (u1_ident == 1)       ? "ONE live buffer with the declared identity, USING IT"
                    : (u1_ident == 0) ? "no live buffer with the declared identity -- u1 stays unbound"
                                      : "AMBIGUOUS -- u1 stays unbound",
                iwhy);
    }
    if (live1) {
        ID3D12Resource* pub1 = rb_res;
        unsigned b1 = rb_size;
        if (g_present_toggles.allow_buffer_resize) {
            const unsigned h1 = PortApi::GameComputeUavBufferHash(rb_size);
            const BufResize w1 = g_buffer_reloc.Declared(h1);
            if (w1.bytes() > (uint64_t)rb_size) {
                ID3D12Resource* big1 = nullptr;
                uint64_t bva1 = 0;
                if (g_buffer_reloc.Ensure(cmd_list, rb_res, rb_size, h1, w1, &big1, &bva1)) {
                    pub1 = big1;
                    b1 = (unsigned)w1.bytes();
                }
            }
        }
        u1_res = pub1;
        u1_bytes = b1;
    } else {
        u1_res = nullptr;
        u1_bytes = 0;
    }
    SkRing::Note(SkRing::U1, (uint64_t)cand_hash | ((uint64_t)want_scale << 32), (uint64_t)u1_res, u1_bytes,
        (uint64_t)((u1_ident + 1) & 0xFF) | ((uint64_t)(live1 ? 1 : 0) << 8));

    if (g_debug) {
        if (LogBudget("U1RESOLVE", owner_ns, 8))
            logger::write("U1RESOLVE owner=%08x wantScale=%08x -> %s (%u B, identity hash %08x, "
                          "va=%llx)%s%s",
                cand_hash, want_scale, u1_res ? "bound" : "UNBOUND", u1_bytes,
                u1_res ? PortApi::GameComputeUavBufferHash(g_buffer_reloc.IdentityBytes(u1_res, u1_bytes)) : 0u,
                (unsigned long long)live1,
                (u1_res && want_scale &&
                    PortApi::GameComputeUavBufferHash(g_buffer_reloc.IdentityBytes(u1_res, u1_bytes)) != want_scale)
                    ? "  <== NOT THIS MOD'S BUFFER"
                    : "",
                (u1_ident == 1) ? "  [by declared identity, the register had no record]" : "");
    }
}

// cb0 of the owner's dispatch, by register, and its bytes snapshotted while they are still the game's.
void ShapekeyResolver::ResolveCb0(command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_y,
    const std::wstring& owner_ns, ID3D12Resource* inst, ID3D12Resource*& cb0_res, uint64_t& cb0_va, unsigned& cb0_size)
{
    {
        ID3D12Resource* cb = nullptr;
        uint64_t cbva = 0;
        uint32_t cbsz = 0;
        if (ResolveBound(cmd_list, 'c', TargetClass::ConstantBuffer, 0, &cb, &cbva, &cbsz) && cb && cbva) {
            bool from_reflection = false;
            if (MigotoPort::ShaderCode* scs = MigotoPort::AsShaderCode(MigotoPort::State().shader[5]))
                if (scs->cb_size[0]) {
                    cbsz = scs->cb_size[0];
                    from_reflection = true;
                }
            constexpr uint32_t kGameShapeKeyCb0Bytes = 4096u;
            if (!from_reflection && cbsz > kGameShapeKeyCb0Bytes) cbsz = kGameShapeKeyCb0Bytes;
            cb0_res = cb;
            cb0_va = cbva;
            cb0_size = cbsz;
            ID3D12Resource* auth_cb = cb;
            uint64_t auth_va = cbva;
            {
                const uint64_t bclay = g_command_lists.CmpLayout(cmd_list);
                const int b0p = bclay ? g_layouts.CsCb0Root(bclay) : -1;
                if (b0p >= 0 && b0p < 64) {
                    auto& cbs = g_command_lists.Cbv(cmd_list);
                    if (cbs.binds[b0p].res && cbs.binds[b0p].va) {
                        auth_cb = reinterpret_cast<ID3D12Resource*>(cbs.binds[b0p].res);
                        auth_va = cbs.binds[b0p].va;
                    }
                }
                if (g_debug) {
                    if (LogBudget("CB0SRC", owner_ns, 6))
                        logger::write("CB0SRC ns='%ls' cs=%016llx pushDescB0=p%d res=%p va=%llx | "
                                      "ResolveBound res=%p va=%llx | %s",
                            owner_ns.c_str(), (unsigned long long)cs_hash, b0p, (void*)auth_cb,
                            (unsigned long long)auth_va, (void*)cb, (unsigned long long)cbva,
                            (auth_cb == cb && auth_va == cbva) ? "AGREE"
                                                               : "DISAGREE -- snapshotting the push-descriptor one");
                }
            }
            store_.SnapshotCb0(
                cmd_list, owner_ns, inst, auth_cb, auth_va, cbsz, (unsigned long long)cs_hash, group_count_y);
        } else {
            cb0_res = nullptr;
            cb0_va = 0;
            cb0_size = 0;
        }
        if (g_debug) {
            if (LogBudget("CB0RESOLVE", owner_ns, 8))
                logger::write("CB0RESOLVE register b0 -> %s (%u B, va=%llx) "
                              "[DX11 reference for this dispatch is 4096 B]",
                    cb0_res ? "bound" : "UNBOUND", cb0_size, (unsigned long long)cb0_va);
        }
    }
}

// Everything the game had bound for compute on this list, stored for the owner's replay.
void ShapekeyResolver::SnapshotState(command_list* cmd_list, uint64_t clay, ID3D12Resource* cand, unsigned cand_bytes,
    ID3D12Resource* u1_res, unsigned u1_bytes, const std::wstring& owner_ns, ID3D12Resource* inst)
{
    {
        SkComputeState s;
        {
            CsCbvRoots roots;
            const bool have_roots = g_layouts.CsCbvRootsOf(clay, &roots);
            if (have_roots) {
                auto* ci = &g_command_lists.Cbv(cmd_list);
                if (ci)
                    for (int reg = 0;
                        reg < (int)RegisterFileWidth(TargetClass::ConstantBuffer) && reg < (int)std::size(roots.reg);
                        ++reg) {
                        const int rp = roots.reg[reg];
                        if (rp < 0 || rp >= 64) continue;
                        if (!ci->binds[rp].res || !ci->binds[rp].va) continue;
                        SkBufferBinding& b = SkRegMut(s, TargetClass::ConstantBuffer, (unsigned)reg);
                        b.res = ci->binds[rp].res;
                        b.va = ci->binds[rp].va;
                        b.bytes = ci->binds[rp].size;
                    }
            }
        }
        {
            SkBufferBinding& u0 = SkRegMut(s, TargetClass::UnorderedAccessView, 0);
            u0.res = cand;
            u0.bytes = cand_bytes;
        }
        if (u1_res) {
            SkBufferBinding& u1 = SkRegMut(s, TargetClass::UnorderedAccessView, 1);
            u1.res = u1_res;
            u1.bytes = u1_bytes;
        }
        s.valid = true;
        store_.SetState(owner_ns, inst, s); // the per-instance store is the only reader
    }
}

void ShapekeyResolver::Publish(command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_y, SkU0Ident& ck,
    ID3D12Resource* cand, unsigned cand_bytes, unsigned cand_hash, uint64_t clay)
{
    if (ck.declared) {
        uint64_t live_va = 0;
        g_resources.VaOf(reinterpret_cast<uint64_t>(cand), &live_va);
        ck.live = live_va ? 1 : 0;
        if (live_va) {
            ck.inst = cand;
            ID3D12Resource* u1_res = nullptr;
            unsigned u1_bytes = 0;
            ID3D12Resource* cb0_res = nullptr;
            uint64_t cb0_va = 0;
            unsigned cb0_size = 0;

            ID3D12Resource* pub_res = cand;
            unsigned pub_bytes = cand_bytes;
            uint64_t pub_va = live_va;
            if (g_present_toggles.allow_buffer_resize) {
                const BufResize want = g_buffer_reloc.Declared(cand_hash);
                if (want.bytes() > (uint64_t)cand_bytes) {
                    ID3D12Resource* big = nullptr;
                    uint64_t big_va = 0;
                    if (g_buffer_reloc.Ensure(cmd_list, cand, cand_bytes, cand_hash, want, &big, &big_va)) {
                        pub_res = big;
                        pub_bytes = (unsigned)want.bytes();
                        pub_va = big_va;
                    }
                }
            }
            ck.push = PortApi::PushGameComputeUav(0, pub_res, pub_bytes, pub_va);
            cand = pub_res;
            cand_bytes = pub_bytes; // downstream records the one in use
            ck.size = pub_bytes;

            unsigned want_scale = 0;
            std::wstring owner_ns; // WHOSE dispatch this is -- the key u1 is stored under
            ResolveU1(cmd_list, ck, cand, cand_hash, owner_ns, want_scale, u1_res, u1_bytes);
            ResolveCb0(cmd_list, cs_hash, group_count_y, owner_ns, ck.inst, cb0_res, cb0_va, cb0_size);
            SnapshotState(cmd_list, clay, cand, cand_bytes, u1_res, u1_bytes, owner_ns, ck.inst);
            if (logger::enabled(1)) {
                static std::atomic<bool> said{ false };
                if (!said.exchange(true))
                    logger::write("SKSTATE captured from the skipped dispatch: u1=%p(%u B) "
                                  "cb0=%p va=%llx size=%u -- the state DX11 inherits from "
                                  "the immediate context and our replay does not",
                        (void*)u1_res, u1_bytes, (void*)cb0_res, (unsigned long long)cb0_va, cb0_size);
            }
        }
    } else if (g_debug) {
        if (ProbeArmed() && DiagOnce("SHAPEKEYU0-REFUSED", (int)cand_hash))
            logger::write("SHAPEKEYU0 REFUSED to publish: ByteWidth=%llu -> descHash=%08x, "
                          "which no [TextureOverride] declares. The RESOLVE is wrong; "
                          "publishing it would be noise at best (and a stale-cache "
                          "pointer at worst).",
                (unsigned long long)cand_bytes, cand_hash);
    }
}

static void SkU0ChainLog(
    uint64_t cs_hash, uint32_t group_count_x, uint32_t group_count_y, uint32_t group_count_z, SkU0Ident& ck)
{
    if (g_debug && ck.shapekey) {
        uint64_t bf_hash = 0;
        int bf_handle = 0;
        int bf_inmap = 0;
        ck.filter = PortApi::BoundShaderFilter('c', &bf_hash, &bf_handle, &bf_inmap);
        wchar_t so[128] = { 0 };
        PortApi::ShaderOverrideSection(bf_hash, so, std::size(so));

        if (ck.shapekey && MigotoPort::FrameAnalysisArmed()) {
            DrawCallInfo dci{};
            dci.type = DrawCall::Dispatch;
            dci.ThreadGroupCountX = group_count_x;
            dci.ThreadGroupCountY = group_count_y;
            dci.ThreadGroupCountZ = group_count_z;
            MigotoPort::FrameAnalysisAfterDraw(true, &dci, 0, 0, cs_hash); // cs_hash NAMES the file
            if (logger::enabled(1)) {
                static std::atomic<bool> said{ false };
                if (!said.exchange(true))
                    logger::write("SKDUMP: game shapekey dispatch cs=%016llx dumped from PipeState only "
                                  "(nothing published, see above) -- the UNMODDED reference to diff ours against",
                        (unsigned long long)cs_hash);
            }
        }

        char sig[320];
        _snprintf_s(sig, sizeof(sig), _TRUNCATE,
            "%d%d%d|%llx|%d%d|%llx|%d|%u|%u|%08x|%d|%d|%d|%d|%d|%d|%d|%u,%u,%u|%d/%u",
            (int)g_sk_toggles.shader_hash_alias, (int)g_sk_toggles.publish_shader_state,
            (int)g_sk_toggles.publish_cs_u0, (unsigned long long)cs_hash, bf_handle, bf_inmap,
            (unsigned long long)ck.layout, ck.have, ck.param, ck.size, ck.desc, ck.declared, ck.live, ck.push, ck.why,
            ck.owner, ck.claims, ck.by_owner, group_count_x, group_count_y, group_count_z, ck.geo, ck.geo_hi);
        if (DiagChanged(
                "CSU0-CHAIN", (int)(cs_hash ^ (cs_hash >> 32) ^ (group_count_y * 2654435761u)), Fnv1a64Str(sig)))
            logger::write("CSU0-CHAIN cs=%016llx I7=%d I8=%d I9=%d | csFilter=%g boundHash=%016llx "
                          "handle=%d mS=%d so='%ls' | layout=%llx have=%d p=%u/%u | size=%u desc=%08x "
                          "decl=%d live=%d push=%d why=%s owner=%d claims=%d byOwner=%d "
                          "groups=%u,%u,%u geo=%d/%u..%u",
                (unsigned long long)cs_hash, (int)g_sk_toggles.shader_hash_alias,
                (int)g_sk_toggles.publish_shader_state, (int)g_sk_toggles.publish_cs_u0, ck.filter,
                (unsigned long long)bf_hash, bf_handle, bf_inmap, so, (unsigned long long)ck.layout, ck.have, ck.param,
                ck.off, ck.size, ck.desc, ck.declared, ck.live, ck.push,
                (ck.why == 0)       ? "ok"
                    : (ck.why == 1) ? "NO-U0-DESCRIPTOR-BOUND"
                    : (ck.why == 2) ? "HEAP-LOOKUP-MISS"
                    : (ck.why == 3) ? "CACHE-ENTRY-HAS-NO-RESOURCE"
                    : (ck.why == 4) ? "EXTENT-0-RESOURCE-NOT-LIVE"
                                    : "EXTENT-0-RECOVERY-FAILED",
                ck.owner, ck.claims, ck.by_owner, group_count_x, group_count_y, group_count_z, ck.geo, ck.geo_lo,
                ck.geo_hi);
    }
}

void ShapekeyResolver::ResolveU0(command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_x,
    uint32_t group_count_y, uint32_t group_count_z, SkU0Ident& ck)
{
    ID3D12Resource* ck_res_for_ring = nullptr; // the u0 record's resource, for the ring note at the end
    if (cs_hash == g_dispatch_toggles.clear_cs_hash) identity_.NoteClear(cmd_list, group_count_x);
    if (g_sk_toggles.publish_cs_u0 && ck.shapekey) {
        PROF(PROF_CsU0);
        ResInfo ri;
        uint64_t clay = 0;
        if (U0Record(cmd_list, cs_hash, group_count_x, group_count_y, group_count_z, ck, ri, clay, ck_res_for_ring)) {
            auto* cand = reinterpret_cast<ID3D12Resource*>(ri.res);
            unsigned cand_bytes = ri.size;
            cand_bytes = SkU0Extent(cmd_list, ck, ri, cand, cand_bytes);
            if (cand_bytes) ck.why = 0; // an extent exists, however it was obtained
            unsigned cand_hash = PortApi::GameComputeUavBufferHash(cand_bytes);
            ck.size = cand_bytes;
            ck.desc = cand_hash;
            U0Claim(ck, cand_hash, cand_bytes);
            Publish(cmd_list, cs_hash, group_count_y, ck, cand, cand_bytes, cand_hash, clay);
        }
    }

    if (g_sk_toggles.publish_cs_u0 && ck.shapekey && ck.push < 0) {
        const int cleared = PortApi::PushGameComputeUav(0, nullptr, 0, 0);
        if (g_debug) {
            if (ProbeArmed() && DiagOnce("CSU0-CLEAR", (int)ck.desc))
                logger::write("CSU0-CLEAR: declined to publish (desc=%08x decl=%d live=%d) -- cs-u0 "
                              "CLEARED so their checktextureoverride cannot match a stale resource "
                              "from an earlier dispatch (clear rc=%d)",
                    ck.desc, ck.declared, ck.live, cleared);
        }
    }

    SkU0ChainLog(cs_hash, group_count_x, group_count_y, group_count_z, ck);
    if (!ck_res_for_ring && !ck.declared && ck.owner < 0) return;
    SkRing::Note(SkRing::Resolve, cs_hash, (uint64_t)ck_res_for_ring, (uint64_t)ck.size | ((uint64_t)ck.desc << 32),
        (uint64_t)(ck.geo & 0xF) | ((uint64_t)(ck.why & 0xF) << 4) | ((uint64_t)(ck.live ? 1 : 0) << 8) |
            ((uint64_t)(ck.declared ? 1 : 0) << 9) | ((uint64_t)((ck.push + 128) & 0xFF) << 16) |
            ((uint64_t)((ck.owner + 128) & 0xFF) << 24) | ((uint64_t)(ck.claims & 0xFF) << 32) |
            ((uint64_t)(group_count_y & 0xFFFFF) << 40));
}

void SkResolveU0(command_list* cmd_list, uint64_t cs_hash, uint32_t group_count_x, uint32_t group_count_y,
    uint32_t group_count_z, SkU0Ident& ck)
{
    g_sk_resolver.ResolveU0(cmd_list, cs_hash, group_count_x, group_count_y, group_count_z, ck);
}

bool SkU0ResourceNow(command_list* cmd_list, ID3D12Resource** res, unsigned* size)
{
    return g_sk_resolver.U0ResourceNow(cmd_list, res, size);
}
