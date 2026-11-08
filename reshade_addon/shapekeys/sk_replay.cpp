// The replay: one mod instance's queued lists, run on its first draw of the frame. See sk_internal.h.
#include "sk_internal.h"
#include "../mods/mod_set.h"
#include "../instrumentation/sk_ring.h"
#include "../instrumentation/morph_dump.h"
#include "../instrumentation/profiler.h"

ShapekeyReplay g_sk_replay(g_sk_store, g_sk_queue, g_sk_identity);

static int SkInstDiagKey(int mod_id, ID3D12Resource* inst)
{
    return (int)(((uint64_t)(unsigned)mod_id << 24) ^ (reinterpret_cast<uintptr_t>(inst) >> 4));
}

bool ShapekeyReplay::FirstDrawThisFrame(int mod_id, ID3D12Resource* inst, const void* cl, MorphDump::DrainRecord& dr)
{
    // The ended frame's numbers, copied out under the lock and reported after it.
    bool report = false;
    uint32_t rep_frame = 0;
    uint32_t rep_draws = 0;
    uint32_t rep_nlists = 0;
    const void* rep_pass_cl = nullptr;
    const void* rep_lists[8] = {};
    {
        std::lock_guard<std::mutex> l(ord_mutex_);
        if (ord_st_.size() > 64)
            for (auto it = ord_st_.begin(); it != ord_st_.end();)
                if ((uint32_t)(g_frame_id - it->second.frame) > 300u)
                    it = ord_st_.erase(it);
                else
                    ++it;
        OrdState& s = ord_st_[{ mod_id, inst }];
        if (s.frame == g_frame_id) {
            // A later draw of this mod in the same frame: the passes already ran on the first list.
            ++s.draws;
            bool known = false;
            for (uint32_t i = 0; i < s.nlists; ++i)
                if (s.lists[i] == cl) {
                    known = true;
                    break;
                }
            if (!known && s.nlists < 8) s.lists[s.nlists++] = cl;
            dr.exit = 2;
            MorphDump::RecordDrain(dr);
            return false;
        } else {
            if (s.frame && logger::enabled(1)) {
                report = true;
                rep_frame = s.frame;
                rep_draws = s.draws;
                rep_nlists = s.nlists;
                rep_pass_cl = s.pass_cl;
                for (uint32_t i = 0; i < s.nlists; ++i) rep_lists[i] = s.lists[i];
            }
            s.frame = g_frame_id;
            s.draws = 1;
            s.nlists = 0;
            s.pass_cl = cl;
            s.lists[s.nlists++] = cl;
        }
    }
    if (report) {
        const int pass_at = SubmitIndexOf(rep_frame, rep_pass_cl);
        const void* disp_cl = queue_.DispListForFrame(rep_frame); // per-frame slot, not the global
        const int disp_at = disp_cl ? SubmitIndexOf(rep_frame, disp_cl) : -2; // -2 = no dispatch seen
        int earliest = 0x7fffffff;
        int unsub = 0;
        int before = 0;
        for (uint32_t i = 0; i < rep_nlists; ++i) {
            const int at = SubmitIndexOf(rep_frame, rep_lists[i]);
            if (at < 0) {
                ++unsub;
                continue;
            }
            if (at < earliest) earliest = at;
            if (pass_at >= 0 && at < pass_at) ++before;
        }
        if (earliest == 0x7fffffff) earliest = -1;
        const uint64_t key = (uint64_t)rep_nlists * 1000000000ull + (uint64_t)(pass_at + 1) * 1000000ull +
            (uint64_t)(disp_at + 3) * 1000ull + (uint64_t)(before + 1);
        if (DiagChanged("SHAPEKEY-ORDER", SkInstDiagKey(mod_id, inst), key))
            logger::write("SHAPEKEY-ORDER mod=%d inst=%p f=%u: %u draw(s) across %u list(s); pass@%d "
                          "earliest@%d disp@%d before=%d unsubmitted=%d (submissions seen: %llu). "
                          "before>0 counts lists whose draws read last frame's positions. "
                          "disp@ is the GAME's shapekey dispatch list. "
                          "-2 = no dispatch seen that frame",
                mod_id, (void*)inst, rep_frame, rep_draws, rep_nlists, pass_at, earliest, disp_at, before, unsub,
                (unsigned long long)g_sub_seen.load());
    }
    return true;
}

// u0 from the mod's own declared hash when the dispatch left no live binding.
void ShapekeyReplay::OwnBufFallback(
    const std::wstring& ns, int mod_id, unsigned u0hash, ID3D12Resource*& u0_res, unsigned& u0_bytes)
{
    {
        PROF(PROF_SK_OwnBuf);
        const unsigned want = identity_.ModCbFor(mod_id, u0hash);
        if (want) {
            ID3D12Resource* own = nullptr;
            unsigned own_bytes = 0;
            uint64_t own_va = 0;
            g_resources.ForEachBuffer([&](uint64_t va, const ResInfo& ri) {
                if (!ri.size || !ri.res) return false;
                if (PortApi::GameComputeUavBufferHash(ri.size) != want) return false;
                own = ri.res;
                own_bytes = ri.size;
                own_va = va;
                return true;
            });
            bool bound_live = false; // a dispatch binding the game has since destroyed is as good as none
            if (u0_res) bound_live = g_resources.Live(u0_res);
            if ((!u0_res || !bound_live) && own && own_va) {
                u0_res = own;
                u0_bytes = own_bytes;
            } else if (own && own_va && own != u0_res && logger::enabled(1) &&
                DiagChanged("SHAPEKEY-OWNBUF-DISAGREES", mod_id,
                    reinterpret_cast<unsigned long long>(own) ^ reinterpret_cast<unsigned long long>(u0_res)))
                logger::write("SHAPEKEY-OWNBUF: the declared-hash search met %p (%u B) first, the dispatch bound %p "
                              "(%u B) -- the dispatch wins (two live instances of this size, or a relocation)",
                    (void*)own, own_bytes, (void*)u0_res, u0_bytes);
            if (logger::enabled(1) && DiagChanged("SHAPEKEY-OWNBUF", mod_id, own_bytes))
                logger::write("SHAPEKEY-OWNBUF mod=%d ns='%ls' declares %08x -> %s (%u bytes)", mod_id, ns.c_str(),
                    want, own ? "found its own buffer" : "NOT FOUND among live buffers", own_bytes);
        }
    }
}

// cs-u0, u1 and cb0 republished for this mod instance before its passes run.
void ShapekeyReplay::RepublishBindings(
    const std::wstring& ns, ID3D12Resource* inst, int mod_id, ID3D12Resource* u0_res, unsigned u0_bytes)
{
    if (u0_res && u0_bytes) {
        uint64_t va = 0;
        g_resources.VaOf(reinterpret_cast<uint64_t>(u0_res), &va);
        if (va) PortApi::PushGameComputeUav(0, u0_res, u0_bytes, va);
        {
            ID3D12Resource* u1r = nullptr;
            unsigned u1b = 0;
            uint64_t u1va = 0;
            SkBufferBinding u1_bind{};
            if (store_.GetBinding(ns, inst, TargetClass::UnorderedAccessView, 1, &u1_bind)) {
                u1r = u1_bind.res;
                u1b = u1_bind.bytes;
            } else {
                u1r = nullptr;
                u1b = 0;
            }
            if (u1r && u1b)
                g_resources.VaOf(reinterpret_cast<uint64_t>(u1r), &u1va); // liveness + the live VA, one lookup
            if (u1r && u1va) PortApi::PushGameComputeUav(1, u1r, u1b, u1va);
            SkRing::Note(SkRing::U1Bind, (uint64_t)mod_id, (uint64_t)u1r, u1b, (u1r && u1va) ? 1u : 0u);
            if (logger::enabled(1) && DiagChanged("SHAPEKEY-U1", mod_id, u1b))
                logger::write("SHAPEKEY-U1 mod=%d ns='%ls' -> %s (%u bytes) [from the dispatch's own "
                              "table, by declared ShapeKeyScale hash]",
                    mod_id, ns.c_str(), (u1r && u1va) ? "bound" : "NOT CAPTURED at the dispatch", u1b);
        }
        ID3D12Resource* ns_cb0_res = nullptr;
        uint64_t ns_cb0_va = 0;
        unsigned ns_cb0_size = 0;
        {
            SkBufferBinding cb{};
            if (store_.GetBinding(ns, inst, TargetClass::ConstantBuffer, 0, &cb)) {
                ns_cb0_res = cb.res;
                ns_cb0_va = cb.va;
                ns_cb0_size = cb.bytes;
            }
        }
        if (ns_cb0_res && ns_cb0_va) {
            uint64_t vac = 0;
            g_resources.VaOf(reinterpret_cast<uint64_t>(ns_cb0_res), &vac);
            constexpr unsigned kShapeKeyCB0Bytes = ((66u * 16u) + 255u) & ~255u; // cb0[66] -> 1280
            if (vac) PortApi::PushGameConstantBuffer('c', 0, ns_cb0_res, ns_cb0_va, kShapeKeyCB0Bytes);
        } else if (logger::enabled(1)) {
            static std::atomic<bool> said{ false };
            if (!said.exchange(true))
                logger::write("SHAPEKEY-PASSES: no cb0 recorded for this instance at draw time -- cb0 NOT "
                              "republished.");
        }
    }
}

// The game's whole compute state for this mod instance replayed under the passes (UAVs past u1, CBVs).
void ShapekeyReplay::ReplayComputeState(const std::wstring& ns, ID3D12Resource* inst, int mod_id)
{
    {
        SkComputeState s;
        if (!store_.GetState(ns, inst, &s)) s = SkComputeState{};
        if (s.valid) {
            unsigned n_uav = 0;
            unsigned n_cbv = 0;
            for (unsigned u = 0; u < RegisterFileWidth(TargetClass::UnorderedAccessView); ++u) {
                if (u <= 1) continue;
                if (!SkReg(s, TargetClass::UnorderedAccessView, u).res ||
                    !SkReg(s, TargetClass::UnorderedAccessView, u).bytes)
                    continue;
                uint64_t va = 0;
                g_resources.VaOf(reinterpret_cast<uint64_t>(SkReg(s, TargetClass::UnorderedAccessView, u).res), &va);
                if (!va) continue;
                if ((SkReg(s, TargetClass::UnorderedAccessView, u).bytes & 3u) || (va & 3ull)) continue;
                PortApi::PushGameComputeUav(u, SkReg(s, TargetClass::UnorderedAccessView, u).res,
                    SkReg(s, TargetClass::UnorderedAccessView, u).bytes, va);
                ++n_uav;
            }
            for (unsigned b = 0; b < RegisterFileWidth(TargetClass::ConstantBuffer); ++b) {
                if (b == 0 && g_sk_toggles.cb0_snapshot) {
                    if (g_debug && LogBudget("CB0SNAP-SKIP", ns, 4))
                        logger::write("CB0SNAP: state replay SKIPPED b0 for ns='%ls' -- each pass "
                                      "publishes its own snapshot just before it runs (CB0PASS).",
                            ns.c_str());
                    continue;
                }
                if (!SkReg(s, TargetClass::ConstantBuffer, b).res || !SkReg(s, TargetClass::ConstantBuffer, b).va)
                    continue;
                uint64_t live = 0;
                g_resources.VaOf(reinterpret_cast<uint64_t>(SkReg(s, TargetClass::ConstantBuffer, b).res), &live);
                if (!live) continue; // destroyed since capture: bind nothing, never deref
                unsigned sz = SkReg(s, TargetClass::ConstantBuffer, b).bytes;
                if (b == 0) sz = ((66u * 16u) + 255u) & ~255u;
                if (sz == 0u || sz == 0xFFFFFFFFu) {
                    // Safe to dereference ONLY because `live` above proved it is still alive.
                    const D3D12_RESOURCE_DESC rd = SkReg(s, TargetClass::ConstantBuffer, b).res->GetDesc();
                    const uint64_t base = live; // the VA the map just gave us
                    const uint64_t off = (SkReg(s, TargetClass::ConstantBuffer, b).va >= base)
                        ? (SkReg(s, TargetClass::ConstantBuffer, b).va - base)
                        : 0;
                    sz = (rd.Width > off) ? (unsigned)(rd.Width - off) : 0;
                    if (sz > 65536u) sz = 65536u; // D3D12 CBV maximum
                    sz &= ~255u; // CBVs must be 256-byte aligned
                }
                if (!sz) continue;
                if (SkReg(s, TargetClass::ConstantBuffer, b).va & 255ull) {
                    if (logger::enabled(1)) {
                        static std::atomic<bool> said{ false };
                        if (!said.exchange(true))
                            logger::write("SKREPLAY: b%u SKIPPED -- va=%llx is not 256-byte aligned, and "
                                          "CreateConstantBufferView rejects that (INVALID_CALL -> device "
                                          "removed). Binding nothing is the safe answer.",
                                b, (unsigned long long)SkReg(s, TargetClass::ConstantBuffer, b).va);
                    }
                    continue;
                }
                PortApi::PushGameConstantBuffer('c', b, SkReg(s, TargetClass::ConstantBuffer, b).res,
                    SkReg(s, TargetClass::ConstantBuffer, b).va, sz);
                ++n_cbv;
            }
            if (logger::enabled(1) && DiagChanged("SKREPLAY", mod_id, (unsigned long long)n_uav * 100u + n_cbv))
                logger::write("SKREPLAY mod=%d ns='%ls' republished the game's compute state: "
                              "%u UAV, %u CBV (SRVs are never replayed) -- the mod's own binds "
                              "are applied after this and win, exactly as on DX11's immediate context",
                    mod_id, ns.c_str(), n_uav, n_cbv);
        }
    }
}

// The mod's lists run, each on its own cb0 snapshot and batch selector; one SKPASS row per pass.
void ShapekeyReplay::RunLists(const std::wstring& ns, ID3D12Resource* inst, int mod_id,
    reshade::api::command_list* cmd_list, const std::vector<SkPendingList>& lists, MorphDump::DrainRecord& dr,
    ID3D12Resource* u0_res, unsigned u0_bytes)
{
    if (u0_res && u0_bytes) identity_.NoteChainWroteU0(u0_res, g_frame_id);
    PortApi::DrawInfo di;
    di.dispatch = true;
    {
        PROF(PROF_SK_Lists); // the four passes themselves; IssueDispatch nests inside this
        for (size_t i = 0; i < lists.size(); ++i) {
            ++dr.ran;
            /* Stamp the capture with WHICH game dispatch queued this pass, before it issues. */
            MigotoPort::SkCapSetCaller(ns.c_str(), lists[i].cs, lists[i].tgy, mod_id);
            uint64_t sk_snap_res = 0;
            uint64_t sk_snap_va = 0;
            unsigned sk_snap_bytes = 0;
            int sk_snap_hit = -1;
            if (g_sk_toggles.cb0_snapshot) {
                PROF(PROF_SK_Cb0);
                sk_snap_hit = 0; // attempted; 1 only once GetSnap actually answers
                /* GetSnap refuses an entry with no res or no va, so what it returns is usable as-is. */
                SkBufferBinding sb{};
                if (store_.GetSnap(ns, inst, 0, lists[i].cs, lists[i].tgy, &sb)) {
                    PortApi::PushGameConstantBuffer('c', 0, sb.res, sb.va, sb.bytes);
                    sk_snap_hit = 1;
                    sk_snap_res = reinterpret_cast<uint64_t>(sb.res);
                    sk_snap_va = sb.va;
                    sk_snap_bytes = sb.bytes;
                    if (g_debug) {
                        if (LogBudget("CB0PASS", ns, 12))
                            logger::write("CB0PASS: pass %zu <- cb0 snapshot of cs=%016llx (%u B, va=%llx)", i,
                                lists[i].cs, sb.bytes, (unsigned long long)sb.va);
                    }
                } else if (g_debug) {
                    if (LogBudget("CB0PASS-MISS", ns, 12))
                        logger::write("CB0PASS: pass %zu has NO snapshot for cs=%016llx -- it will run on "
                                      "whatever b0 the previous pass left bound.",
                            i, lists[i].cs);
                }
            }
            if (g_sk_toggles.freeze && i + 1 == lists.size()) {
                if (store_.ZeroBinding(ns, inst, TargetClass::UnorderedAccessView, 0, cmd_list, "SKFREEZE") == 1 &&
                    g_debug && LogBudget("SKFREEZE-ON", ns, 4))
                    logger::write("SKFREEZE: ns='%ls' -- u0 zeroed before the final pass (%zu of %zu); the "
                                  "Applier will write base positions with no morph.",
                        ns.c_str(), i + 1, lists.size());
            }
            di.thread_group_y = lists[i].tgy;
            MigotoPort::ResetEmittedDispatch();
            {
                PROF(PROF_SK_Run);
                PortApi::RunResolvedCommandList(lists[i].list, &di);
            }
            uint32_t em_x = 0;
            uint32_t em_y = 0;
            uint32_t em_z = 0;
            const unsigned em_n = MigotoPort::TakeEmittedDispatch(&em_x, &em_y, &em_z);
            PROF(PROF_SK_Row); // everything below, to the end of this iteration: the row and its log
            const int csi = 5; // 'c' -- v h d g p c (MigotoPort::StageIndex)
            MigotoPort::PipeState& ps = MigotoPort::State();
            auto srv = [&](int r) { return ps.srv[csi][r] ? 1 : 0; };
            auto uav = [&](int r) { return ps.uav[r] ? 1 : 0; };
            const float ck = PortApi::GetVarNs(L"WWMIv1", L"shapekey_checksum", -1.f);
            const float skv = PortApi::GetVarNs(L"WWMIv1", L"shapekey_vertex_count", -1.f);
            const float cvc = PortApi::GetVarNs(L"WWMIv1", L"custom_vertex_count", -1.f);
            if (MorphDump::Armed()) {
                MorphDump::PassRecord pr{};
                pr.frame = g_frame_id;
                pr.mod_id = mod_id;
                pr.owner_mod = Mods().IdOfNsHash(lists[i].owner_ns_hash);
                pr.pass_index = (unsigned)(i + 1);
                pr.pass_count = (unsigned)lists.size();
                pr.queued_cs = lists[i].cs;
                pr.queued_tgy = lists[i].tgy;
                pr.emitted_n = em_n;
                pr.emitted_x = em_x;
                pr.emitted_y = em_y;
                pr.emitted_z = em_z;
                pr.t0 = srv(0);
                pr.t1 = srv(1);
                pr.t6 = srv(6);
                pr.t33 = srv(33);
                pr.u5 = uav(5);
                pr.u6 = uav(6);
                pr.checksum = ck;
                pr.shapekey_vtx = skv;
                pr.custom_vtx = cvc;
                // S6: their live IniParams, two texels = x0y0z0w0 / x1y1z1w1.
                {
                    float ip[8] = {};
                    PortApi::GetIniParams(ip, 2);
                    pr.x0 = ip[0];
                    pr.y0 = ip[1];
                    pr.z0 = ip[2];
                    pr.x1 = ip[4];
                    pr.y1 = ip[5];
                    pr.z1 = ip[6];
                }
                pr.cb0_res = reinterpret_cast<uint64_t>(ps.cb[csi][0]);
                pr.snap_hit = sk_snap_hit;
                pr.snap_res = sk_snap_res;
                pr.snap_va = sk_snap_va;
                pr.snap_bytes = sk_snap_bytes;
                // S17 identity: WHICH PipeState we just read, against the one the shim binds through.
                pr.ps_addr = reinterpret_cast<uint64_t>(&ps);
                MigotoPort::VbBindStats(&pr.vb_binds, &pr.vb_nonnull, &pr.vb_last_ps);
                // S17. Both views, because they disagree exactly when it matters.
                pr.vb0_state = reinterpret_cast<uint64_t>(ps.vb[0]);
                pr.vb0_shim = reinterpret_cast<uint64_t>(ps.shim_vb[0].res);
                pr.vb0_shim_size = ps.shim_vb[0].size;
                pr.vb0_shim_stride = ps.shim_vb[0].stride;
                static_assert(MigotoPort::kMaxVB > 6);
                pr.vb6_state = reinterpret_cast<uint64_t>(ps.vb[6]);
                pr.vb6_shim = reinterpret_cast<uint64_t>(ps.shim_vb[6].res);
                pr.ns = ns.c_str();
                MorphDump::RecordPass(pr);
            }
            if (logger::enabled(1)) {
                char line[256];
                _snprintf_s(line, sizeof(line), _TRUNCATE,
                    "t0=%d t1=%d t6=%d t33=%d u5=%d u6=%d | checksum=%g shapekey_vtx=%g "
                    "custom_vtx=%g | emitted=%u x %u,%u,%u tgy=%u",
                    srv(0), srv(1), srv(6), srv(33), uav(5), uav(6), ck, skv, cvc, em_n, em_x, em_y, em_z,
                    lists[i].tgy);
                // on change of the whole line, per (mod, pass)
                if (DiagChanged("SKPASS", mod_id * 16 + (int)i, Fnv1a64Str(line)))
                    logger::write(
                        "SKPASS mod=%d ns='%ls' pass=%zu/%zu | %s", mod_id, ns.c_str(), i + 1, lists.size(), line);
            }
        }
    }
}

// The drain row and the SHAPEKEY-PASSES line for what ran.
void ShapekeyReplay::ReportDrain(const std::wstring& ns, int mod_id, const std::vector<SkPendingList>& lists,
    MorphDump::DrainRecord& dr, size_t left_for_others, size_t dropped_unowned, ID3D12Resource* u0_res,
    unsigned u0_bytes)
{
    { // THE DRAIN ROW. Sampled here, at the draw, where the mod's own vb0/vb6 binds have landed.
        dr.matched = (unsigned)lists.size();
        dr.left = (unsigned)left_for_others;
        dr.dropped = (unsigned)dropped_unowned;
        dr.disp_seen = queue_.DispListForFrame(g_frame_id) ? 1 : 0;
        dr.u0_res = reinterpret_cast<uint64_t>(u0_res);
        dr.u0_bytes = u0_bytes;
        dr.u0_identity = u0_res ? g_buffer_reloc.IdentityBytes(u0_res, u0_bytes) : 0;
        if (u0_res && dr.u0_identity != u0_bytes) {
            dr.reloc_from = dr.u0_identity;
            dr.reloc_to = u0_bytes;
        }
        MigotoPort::PipeState& dps = MigotoPort::State();
        dr.ps_addr = reinterpret_cast<uint64_t>(&dps);
        MigotoPort::VbBindStats(&dr.vb_binds, &dr.vb_nonnull, &dr.vb_last_ps);
        dr.vb0_state = reinterpret_cast<uint64_t>(dps.vb[0]);
        dr.vb0_shim = reinterpret_cast<uint64_t>(dps.shim_vb[0].res);
        static_assert(MigotoPort::kMaxVB > 6);
        dr.vb6_state = reinterpret_cast<uint64_t>(dps.vb[6]);
        dr.vb6_shim = reinterpret_cast<uint64_t>(dps.shim_vb[6].res);
        MorphDump::RecordDrain(dr);
    }
    if (logger::enabled(1) && DiagChanged("SHAPEKEY-PASSES", mod_id, (unsigned long long)lists.size())) {
        const size_t named =
            (size_t)std::count_if(lists.begin(), lists.end(), [](const SkPendingList& e) { return e.owner_ns_hash != 0; });
        logger::write(
            "SHAPEKEY-PASSES mod=%d ns='%ls' ran %zu of ITS OWN list(s) (%zu named this mod at their dispatch, %zu fell back to hash/name), %zu left queued for "
            "other mods or instances, %zu dropped-unowned, cs-u0 %s (%u bytes), cmdlist=%p",
            mod_id, ns.c_str(), lists.size(), named, lists.size() - named, left_for_others, dropped_unowned,
            u0_res ? "republished" : "NOT AVAILABLE", u0_bytes, (void*)MigotoPort::GetDx12Bridge().cl);
    }
}

void ShapekeyReplay::RunPasses(
    const std::wstring& ns, int mod_id, ID3D12Resource* inst, reshade::api::command_list* cmd_list)
{
    const void* cl = cmd_list ? reinterpret_cast<const void*>(cmd_list->get_native())
                              : nullptr; // every record/order key below is the native list
    // Every exit records: "never called" and "called and did nothing" are indistinguishable by absence.
    MorphDump::DrainRecord dr{};
    dr.frame = g_frame_id;
    dr.mod_id = mod_id;
    dr.ns = ns.c_str();
    if (ns.empty()) {
        dr.exit = 1;
        MorphDump::RecordDrain(dr);
        return;
    }
    PROF(PROF_DI_ShapeKeys);
    if (!FirstDrawThisFrame(mod_id, inst, cl, dr)) return;
    size_t left_for_others = 0;
    size_t dropped_unowned = 0; // lists whose queuing dispatch named no mod
    ID3D12Resource* u0_res = nullptr;
    unsigned u0_bytes = 0;
    size_t other_inst_lists = 0;
    ID3D12Resource* other_inst = nullptr;
    std::vector<SkPendingList> lists =
        queue_.TakeOwnLists(ns, mod_id, inst, dr, left_for_others, dropped_unowned, other_inst_lists, other_inst);
    // THIS INSTANCE'S u0, or nothing. Inheriting the previous dispatcher's position buffer is the same leak.
    {
        SkBufferBinding b0b{};
        if (store_.GetBinding(ns, inst, TargetClass::UnorderedAccessView, 0, &b0b)) {
            u0_res = b0b.res;
            u0_bytes = b0b.bytes;
        } else {
            u0_res = nullptr;
            u0_bytes = 0;
        }
    }
    if (!lists.empty() && DiagOnce("SKINST", SkInstDiagKey(mod_id, inst)))
        logger::write("SKINST mod=%d ns='%ls' inst=%p: first drain of this instance -- %zu list(s), u0=%p (%u B); "
                      "%zu of this mod's lists wait for another instance",
            mod_id, ns.c_str(), (void*)inst, lists.size(), (void*)u0_res, u0_bytes, other_inst_lists);
    else if (lists.empty() && other_inst_lists && DiagOnce("SKINST-WAIT", SkInstDiagKey(mod_id, inst)))
        logger::write("SKINST-WAIT mod=%d ns='%ls' inst=%p: nothing queued for this draw's vb6; %zu of this mod's "
                      "lists wait for instance %p",
            mod_id, ns.c_str(), (void*)inst, other_inst_lists, (void*)other_inst);
    if (lists.empty()) {
        dr.matched = 0;
        dr.left = (unsigned)left_for_others;
        dr.dropped = (unsigned)dropped_unowned;
        dr.disp_seen = queue_.DispListForFrame(g_frame_id) ? 1 : 0;
        MorphDump::RecordDrain(dr);
        return;
    }

    if (g_sk_toggles.u0_clear)
        dr.u0_clear = store_.ZeroBinding(ns, inst, TargetClass::UnorderedAccessView, 0, cmd_list, "U0CLEAR");
    if (g_sk_toggles.u1_clear)
        dr.u1_clear = store_.ZeroBinding(ns, inst, TargetClass::UnorderedAccessView, 1, cmd_list, "U1CLEAR");

    OwnBufFallback(ns, mod_id, lists.front().u0hash, u0_res, u0_bytes);

    RepublishBindings(ns, inst, mod_id, u0_res, u0_bytes);

    ReplayComputeState(ns, inst, mod_id);

    RunLists(ns, inst, mod_id, cmd_list, lists, dr, u0_res, u0_bytes);

    ReportDrain(ns, mod_id, lists, dr, left_for_others, dropped_unowned, u0_res, u0_bytes);
}

void RunShapeKeyPasses(const std::wstring& ns, int mod_id, ID3D12Resource* inst, reshade::api::command_list* cmd_list)
{
    g_sk_replay.RunPasses(ns, mod_id, inst, cmd_list);
}
