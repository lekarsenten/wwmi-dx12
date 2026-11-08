// dispatch_overrides.cpp -- a compute dispatch's [ShaderOverride] and cs-reading sections run, and whether the
// game's dispatch is cancelled (handling = skip, the shapekey clear). See dispatch_parts.h.
#include "../addon_common.h"
#include "../mods/character_recognition.h"
#include "../mods/port_section.h"
#include "../mods/shader_alias.h"
#include "dispatch_parts.h"
#include "dispatch_toggles.h"
#include "../shapekeys/sk_toggles.h"
#include "../mods/mod_set.h"

using namespace reshade::api;

DispatchVerdict RunDispatchOverrides(const DispatchArgs& da, SkU0Ident& ck)
{
    reshade::api::command_list* const cmd_list = da.cmd_list;
    const uint64_t cs_hash = da.cs_hash;
    const uint32_t group_count_x = da.group_count_x;
    const uint32_t group_count_y = da.group_count_y;
    const uint32_t group_count_z = da.group_count_z;
    bool skip_dispatch = false;
    unsigned md_sections_fired = 0; // MorphDump: sections OUR loop ran for this dispatch
    if (g_dispatch_toggles.cancel_game_clear && cs_hash == g_dispatch_toggles.clear_cs_hash) {
        ID3D12Resource* cres = nullptr;
        unsigned csz = 0;
        if (SkU0ResourceNow(cmd_list, &cres, &csz) && SkChainWroteU0Recently(cres, g_frame_id)) {
            skip_dispatch = true;
            g_sk_stats.clear_cancelled.fetch_add(1, std::memory_order_relaxed);
            if (g_debug && DiagOnce("SK-CLEAR-CANCEL", (int)(reinterpret_cast<uint64_t>(cres) >> 4)))
                logger::write("DISPATCH-CLEAR-CANCELLED cs=%016llx u0=%p (%u B, %u groups x 256 B): the chain wrote "
                              "this buffer within two frames, so the draw lists submitted before its pass list keep "
                              "last frame's deltas instead of reading zeros",
                    (unsigned long long)cs_hash, (void*)cres, csz, group_count_x);
        }
    }
    if (g_dispatch_toggles.shader_override_dispatch) {
        PortApi::DrawInfo ddi;
        ddi.dispatch = true;
        ddi.thread_group_x = group_count_x;
        ddi.thread_group_y = group_count_y;
        ddi.thread_group_z = group_count_z;

        if (ck.shapekey) SkNoteDispatchList((const void*)cmd_list->get_native(), g_frame_id);

        SkBeginDispatchScope(cs_hash, group_count_y, ck);
        if (RunShaderOverrides(cs_hash, &ddi, cmd_list)) skip_dispatch = true;
        if (ck.shapekey) {
            std::shared_ptr<const std::vector<CsTrig>> cs_trigs_snap;
            {
                std::lock_guard<std::mutex> l(g_cs_trigs_mtx);
                cs_trigs_snap = g_cs_trigs_snap;
            } // a pointer, not the vector
            static const std::vector<CsTrig> s_none;
            const std::vector<CsTrig>& cs_trigs = cs_trigs_snap ? *cs_trigs_snap : s_none;
            if (g_dispatch_toggles.engine_dispatch && ck.desc) {
                md_sections_fired += PortApi::DispatchTextureOverride(ck.desc, &ddi);
                if (ddi.skip) {
                    ddi.skip = false;
                    if (g_dispatch_toggles.honour_skip && ck.owner >= 0) skip_dispatch = true;
                    if (DiagOnce((const void*)&g_dispatch_toggles.engine_dispatch,
                            (int)ck.desc * 2 + (g_dispatch_toggles.honour_skip ? 1 : 0)))
                        logger::write(
                            "DISPATCH-SKIP cs=%016llx u0 hash=%08x owner=%d (engine lookup, I19) asked handling=skip -> %s",
                            (unsigned long long)cs_hash, ck.desc, ck.owner,
                            (g_dispatch_toggles.honour_skip && ck.owner >= 0)
                                ? "the game's dispatch is CANCELLED (DX11 BeforeDispatch)"
                                : "IGNORED (I14 off, or no owner)");
                }
            } else
                for (const CsTrig& trig : cs_trigs) {
                    if (trig.hash != ck.desc) {
                        if (DiagOnce((const void*)&g_cs_trigs, (int)trig.hash))
                            logger::write("SKFIRE: shapekey dispatch cs=%016llx u0 hash=%08x -- not firing %ls "
                                          "(declared for %08x)",
                                (unsigned long long)cs_hash, ck.desc, trig.fq.c_str(), trig.hash);
                        continue;
                    }
                    for (int md = 0; md < (int)Mods().Count(); ++md) {
                        const std::wstring& uns = Mods().At(md).wcl_ns;
                        if (uns.empty() || _wcsicmp(trig.ns.c_str(), uns.c_str()) != 0) continue;
                        if (!Mods().OnScreen(md)) break;
                        PortApi::RunSectionByName(trig.fq.c_str(), &ddi);
                        ++md_sections_fired;
                        if (ddi.skip) {
                            ddi.skip = false;
                            const bool own = (ck.owner == md);
                            if (g_dispatch_toggles.honour_skip && own) skip_dispatch = true;
                            if (DiagOnce((const void*)&g_dispatch_toggles.honour_skip,
                                    md * 8 + (own ? 4 : 0) + (g_dispatch_toggles.honour_skip ? 2 : 0)))
                                logger::write(
                                    "DISPATCH-SKIP cs=%016llx mod=%d owner=%d section=%ls asked handling=skip -> %s",
                                    (unsigned long long)cs_hash, md, ck.owner, trig.fq.c_str(),
                                    (g_dispatch_toggles.honour_skip && own)
                                        ? "the game's dispatch is CANCELLED (DX11 BeforeDispatch)"
                                        : own ? "IGNORED (g_sk_honour_skip off)"
                                              : "IGNORED (this dispatch's u0 is not that mod's)");
                        }
                        break;
                    }
                }
        }
        SkEndDispatchScope(ck.shapekey);
        PortApi::RunPendingPost();
    }
    return { skip_dispatch, md_sections_fired };
}
