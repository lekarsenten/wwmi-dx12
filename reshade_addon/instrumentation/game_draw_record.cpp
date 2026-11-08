#include "../addon_common.h"
#include "game_draw_record.h"
#include "draw_dump_publish.h" // X27ResolveVsCbs -- the game's bone CBs per game draw
#include "../draw/draw_hook.h" // t_x23_*
#include "../hooks/cmd_bind_hooks.h" // s_iid_vs_hash / s_iid_ps_hash
#include "../hooks/command_list_registry.h"
#include "../hooks/pipeline_registry.h"
#include "../mods/component_table.h"
#include "../mods/mod_registry.h"
#include "../mods/mod_set.h"

GameDrawRecorder g_game_draw_recorder;

bool GameDrawRecorder::Begin(command_list* cmd_list, uint32_t index_count, uint32_t instance_count,
    uint32_t first_index, MorphDump::GameDrawRecord* r)
{
    const int hit = Comps().SlotOf(first_index, index_count);
    const bool is_comp = !(hit < 0 || hit >= (int)Comps().Count());
    int mod_id = -1;
    {
        const auto& gs = g_command_lists.Gfx(cmd_list);
        r->pso = gs.gfx_pso;
        if (gs.vp_count) {
            r->vp_w = gs.vp[0].Width;
            r->vp_h = gs.vp[0].Height;
        }
        r->stencil_ref = gs.stencil_ref;
        r->stencil_ref_set = gs.stencil_ref_set ? 1 : 0;
        r->vb0_va = gs.vb[0].va;
        r->vb6_va = gs.vb[6].va;
        r->vb6_stride = gs.vb[6].stride;
        r->ib_va = gs.ib.va;
        const CmdRT& rt = g_command_lists.Rt(cmd_list);
        if (rt.valid) {
            r->rtv_count = rt.rtv_count;
            r->has_dsv = rt.has_dsv ? 1 : 0;
            r->dsv = rt.dsv;
            r->pass_gen = rt.pass_gen;
        }
    }
    if (is_comp) {
        const MergedComp& mc = Comps().At(hit);
        mod_id = mc.mod_id;
        r->comp = mc.comp_idx;
        if (r->vb0_va || r->ib_va) {
            std::lock_guard<std::mutex> l(mutex_);
            bool known = false;
            for (int i = 0; i < n_; ++i)
                if (meshes_[i].mod == mod_id && meshes_[i].vb0 == r->vb0_va && meshes_[i].ib == r->ib_va) {
                    known = true;
                    break;
                }
            if (!known && n_ < 16) meshes_[n_++] = Mesh{ r->vb0_va, r->ib_va, mod_id };
        }
    } else {
        // Not a component. The same character's mesh through another index range?
        if (!r->vb0_va && !r->ib_va) return false;
        uint8_t same = 0;
        {
            std::lock_guard<std::mutex> l(mutex_);
            for (int i = 0; i < n_; ++i) {
                uint8_t s = 0;
                if (r->vb0_va && meshes_[i].vb0 == r->vb0_va) s |= 1;
                if (r->ib_va && meshes_[i].ib == r->ib_va) s |= 2;
                if (s) {
                    same = s;
                    mod_id = meshes_[i].mod;
                    break;
                }
            }
        }
        if (!same) return false;
        r->same_mesh = same;
        r->comp = -1;
    }
    r->frame = g_frame_id;
    r->mod_id = mod_id;
    r->list = (uint64_t)cmd_list->get_native();
    r->thread = (uint32_t)GetCurrentThreadId();
    r->index_count = index_count;
    r->first_index = first_index;
    r->instance_count = instance_count;
    cmd_list->get_private_data(s_iid_vs_hash, &r->vs);
    cmd_list->get_private_data(s_iid_ps_hash, &r->ps);
    if (mod_id >= 0 && mod_id < (int)Mods().Count()) r->ns = Mods().At(mod_id).wcl_ns.c_str();
    X27Cb cbs[8];
    X27ResolveVsCbs(cmd_list, cbs);
    r->cb3_va = cbs[3].va;
    r->cb4_va = cbs[4].va;
    t_x23_claimed = t_x23_shadow = t_x23_cast = t_x23_hidden = 0;
    t_x23_injected = 0;
    t_x23_merges = 0;
    return true;
}
