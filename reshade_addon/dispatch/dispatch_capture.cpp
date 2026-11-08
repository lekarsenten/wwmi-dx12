// dispatch_capture.cpp -- armed captures of a dispatch: the morph dump row and the game-side SKCAP.
// See dispatch_parts.h.
#include "../addon_common.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../mods/layout_registry.h"
#include "../hooks/resource_tracker.h"
#include "../core/d3d12_barrier.h" // CopySourceState
#include "../mods/shader_alias.h"
#include "dispatch_parts.h"
#include "../migoto/dx12_pipeline.h"
#include "../migoto/dx12_skcap.h"
#include "../instrumentation/morph_dump.h"
#include "../addon/addon.h"
#include "../migoto/dx12_bridge.h"

using namespace reshade::api;

void DispatchCapture(const DispatchArgs& da, const SkU0Ident& ck, bool skip_dispatch, unsigned md_sections_fired)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t cs_hash = da.cs_hash;
    const uint32_t group_count_x = da.group_count_x;
    const uint32_t group_count_y = da.group_count_y;
    const uint32_t group_count_z = da.group_count_z;
    MorphDump::Record mr;
    const bool md_want = MorphDump::Armed();
    const bool sk_want = MigotoPort::SkCapArmed() && ck.shapekey;
    if (md_want || sk_want) {
        mr.frame = g_frame_id;
        mr.cs_hash = cs_hash;
        mr.gx = group_count_x;
        mr.gy = group_count_y;
        mr.gz = group_count_z;
        mr.cmd_list = reinterpret_cast<uint64_t>(cmd_list);
        mr.skip = skip_dispatch;
        mr.owner = ck.owner;
        // The chain's own numbers for this dispatch. Free: the caller already resolved ck.
        mr.ck_why = ck.why;
        mr.ck_size = ck.size;
        mr.ck_decl = ck.declared;
        mr.ck_live = ck.live;
        mr.ck_push = ck.push;
        mr.ck_claims = ck.claims;
        mr.ck_buf = ck.ri_buf;
        mr.ck_tex = ck.ri_tex;
        mr.ct_match = ck.ct_match;
        mr.ct_age = ck.ct_age;
        mr.ct_seqd = ck.ct_seqd;
        mr.ck_off = ck.off;
        mr.ck_heap = ck.heap;
        mr.ck_slot = ck.slot;
        mr.ck_age = ck.ri_age;
        mr.ck_src = ck.ri_src;
        mr.geo_lo = ck.geo_lo;
        mr.geo_hi = ck.geo_hi;
        mr.geo_verdict = ck.geo;
        mr.sections_fired = md_sections_fired;
        {
            uint8_t qr = 0;
            mr.queued = SkTakeQueueOutcome(&qr);
            mr.queue_reason = qr;
        }

        std::wstring md_so;
        { // Record the matcher's ANSWER, not just its payload.
            const bool so_ok = ShaderOverrideMatches(cs_hash, &md_so);
            mr.so_match = !so_ok ? 0 : (md_so.empty() ? 1 : 2);
            if (so_ok && !md_so.empty()) mr.so_section = md_so.c_str();
        }

        uint64_t mclay = 0;
        mclay = g_command_lists.CmpLayout(cmd_list);
        mr.cmp_layout = mclay;

        mr.u0_why = 1;
        if (mclay) {
            const int mroot = g_layouts.CsU0Root(mclay);
            U0CsTable mtbl{};
            const bool mhave = g_layouts.U0Table(mclay, &mtbl);
            mr.b0_param = g_layouts.CsCb0Root(mclay);
            if (mroot < 0 && !mhave) mr.u0_why = 2;

            if (mroot >= 0 && mroot < 64) {
                mr.u0_param = mroot;
                ID3D12Resource* mres = nullptr;
                uint64_t mva = 0;
                {
                    auto& stx = g_command_lists.Gfx(cmd_list);
                    mres = stx.root[mroot].res;
                    mva = stx.root[mroot].va;
                }
                mr.u0_why = 3;
                if (mres) {
                    mr.u0_res = reinterpret_cast<uint64_t>(mres);
                    mr.u0_why = 4;
                    uint64_t va = 0;
                    if (g_resources.VaOf(reinterpret_cast<uint64_t>(mres), &va)) {
                        mr.u0_why = 5;
                        ResInfo si{};
                        if (g_resources.AtVa(va, &si)) mr.u0_size = si.size;
                    }
                } else if (mva) {
                    mr.u0_why = 6;
                    ResInfo si{};
                    if (g_resources.AtVa(mva, &si)) {
                        mr.u0_res = reinterpret_cast<uint64_t>(si.res);
                        mr.u0_size = si.size;
                    }
                }
            }
            if (!mr.u0_size && mhave && mtbl.param < 64) {
                mr.u0_param = mtbl.param;
                if (mr.u0_why < 4) mr.u0_why = 7;
                descriptor_table mdt = {};
                {
                    auto& stx = g_command_lists.Gfx(cmd_list);
                    mdt = stx.root[mtbl.param].compute_table();
                }
                if (mdt.handle) {
                    if (mr.u0_why < 4 || mr.u0_why == 7) mr.u0_why = 8;
                    descriptor_heap mh = { 0 };
                    uint32_t mo = 0;
                    cmd_list->get_device()->get_descriptor_heap_offset(mdt, mtbl.offset, 0, &mh, &mo);
                    ResInfo mri;
                    if (mh.handle && g_descriptors.Find({ mh.handle, mo }, mri) && mri.res) {
                        mr.u0_res = reinterpret_cast<uint64_t>(mri.res);
                        mr.u0_size = mri.size;
                        mr.u0_slot_age = mri.wrote_frame ? (int)(g_frame_id - mri.wrote_frame) : -1;
                        mr.u0_slot_src = mri.wrote_src;
                    }
                }
            }
            // Answer the ck_why=4 question on the same row that reports it.
            if (mr.u0_res) mr.u0_erased = MorphDump::WasResourceErased(mr.u0_res);
            mr.res_map_n = (unsigned)g_resources.BufferCount();
            if (mr.u0_size) {
                mr.u0_why = 10;
                mr.u0_desc_hash = PortApi::GameComputeUavBufferHash(mr.u0_size);
                mr.u0_declared = PortApi::TextureOverrideDeclaresHash(mr.u0_desc_hash) ? 1 : 0;
            }
            if (mr.b0_param >= 0 && mr.b0_param < 64) {
                auto& cbs = g_command_lists.Cbv(cmd_list);
                mr.cb0_res = reinterpret_cast<uint64_t>(cbs.binds[mr.b0_param].res);
                mr.cb0_va = cbs.binds[mr.b0_param].va;
                mr.cb0_size = cbs.binds[mr.b0_param].size;
            }
        }
        if (md_want) MorphDump::RecordDispatch(mr);
    }

    if (sk_want) {
        static std::atomic<unsigned> s_gseq{ 0 };
        const unsigned gseq = s_gseq.fetch_add(1);
        auto* dev12 = g_addon.d3d12_device;
        auto* cl12 = (ID3D12GraphicsCommandList*)cmd_list->get_native();
        auto stage = [&](const char* nm, uint64_t res, uint64_t va, unsigned size, const char* prov) {
            if (!res || !size) return;
            auto* r = reinterpret_cast<ID3D12Resource*>(res);
            uint64_t base = 0;
            if (!g_resources.VaOf(res, &base)) return; // destroyed since resolve -- never deref
            const uint64_t off = (va > base) ? (va - base) : 0;
            D3D12_RESOURCE_STATES st{};
            if (!CopySourceState(MigotoPort::QueryResourceState((void*)r), &st)) return; // no barrier names its state
            MigotoPort::SkCapSnapRaw(dev12, cl12, nm, r, off, size, (unsigned)st, (const void*)cs_hash, gseq,
                group_count_x, group_count_y, group_count_z, prov);
        };
        char pv0[72] = {};
        char pv1[72] = {};
        char pvb[72] = {};
        _snprintf_s(pv0, sizeof(pv0), _TRUNCATE, "u0 p%d", mr.u0_param);
        _snprintf_s(pvb, sizeof(pvb), _TRUNCATE, "b0 p%d", mr.b0_param);
        stage("game-u0", mr.u0_res, 0, mr.u0_size, pv0);
        stage("game-cb0", mr.cb0_res, mr.cb0_va, mr.cb0_size, pvb);

        {
            char pvn[72];
            for (uint32_t reg = 0; reg < 8; ++reg) {
                ID3D12Resource* rr = nullptr;
                uint64_t rva = 0;
                uint32_t rsz = 0;
                if (reg != (uint32_t)mr.u0_param &&
                    ResolveBound(cmd_list, 'c', TargetClass::UnorderedAccessView, reg, &rr, &rva, &rsz) && rr && rsz) {
                    char nm[32];
                    _snprintf_s(nm, sizeof(nm), _TRUNCATE, "game-u%u", reg);
                    _snprintf_s(pvn, sizeof(pvn), _TRUNCATE, "u%u ResolveBound", reg);
                    stage(nm, reinterpret_cast<uint64_t>(rr), rva, rsz, pvn);
                }
                rr = nullptr;
                rva = 0;
                rsz = 0;
                if (ResolveBound(cmd_list, 'c', TargetClass::ShaderResource, reg, &rr, &rva, &rsz) && rr && rsz) {
                    char nm[32];
                    _snprintf_s(nm, sizeof(nm), _TRUNCATE, "game-t%u", reg);
                    _snprintf_s(pvn, sizeof(pvn), _TRUNCATE, "t%u ResolveBound", reg);
                    stage(nm, reinterpret_cast<uint64_t>(rr), rva, rsz, pvn);
                }
            }
        }

        {
            ID3D12Resource* u1r = nullptr;
            uint64_t u1va = 0;
            uint32_t u1sz = 0;
            if (ResolveBound(cmd_list, 'c', TargetClass::UnorderedAccessView, 1, &u1r, &u1va, &u1sz) && u1r && u1sz) {
                _snprintf_s(pv1, sizeof(pv1), _TRUNCATE, "u1 ResolveBound va=%llx", (unsigned long long)u1va);
                stage("game-u1", reinterpret_cast<uint64_t>(u1r), u1va, u1sz, pv1);
            } else {
                static std::atomic<int> s_nu1{ 0 };
                if (ProbeArmed() && s_nu1.fetch_add(1) == 0)
                    logger::write("SKCAP: game u1 UNBOUND at this dispatch -- ResolveBound found no "
                                  "register-1 binding. That is a finding, not a miss.");
            }
        }
    }
}
