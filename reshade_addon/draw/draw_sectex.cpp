// draw_sectex.cpp -- see draw_sectex.h.
#include "../addon_common.h"
#include "draw_sectex.h"
#include "draw_toggles.h"
#include "../mods/character_recognition.h"
#include "../mods/ib_hash.h"
#include "../mods/mod_registry.h"
#include "../hooks/cmd_bind_hooks.h"
#include "../hooks/descriptor_shadow.h"
#include "../hooks/pipeline_registry.h"
#include "../instrumentation/profiler.h"
#include "../instrumentation/diag_arms.h"
#include "../migoto/port_api.h"
#include "../textures/tex_identity.h"
#include "../textures/tex_select.h"
#include "../textures/tex_crawl.h"
#include "../textures/tex_lod.h"
#include "../textures/tex_look.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <tuple>
#include <utility>
#include "../mods/mod_set.h"

using namespace reshade::api;

SectionTexturePublisher g_section_textures;

struct PsTexture {
    unsigned reg = 0;
    bool read = false;
    TableStart ts;
    uint32_t offset = 0;
    ResInfo ri;
};

// Each pixel-shader SRV register of the bound layout that the pipeline reads (with `unread`, the rest too) and
// `want(reg)` accepts, resolved to the resource behind it. False when the layout has no register map.
template <typename Want, typename Fn>
static bool ForEachPsTexture(command_list* cmd_list, bool unread, const DxilSrvRegs& ps_regs, Want&& want, Fn&& fn)
{
    const uint64_t layh = g_command_lists.GfxLayout(cmd_list);
    RegBind t_ps[kSectionTexRegs];
    if (!g_layouts.WithRegMap(layh, [&](const LayoutRegMap& m) { memcpy(t_ps, m.t_ps, sizeof t_ps); })) return false;
    device* dev = cmd_list->get_device();
    const auto& root = g_command_lists.Gfx(cmd_list).root;
    for (unsigned r = 0; r < kSectionTexRegs; ++r) {
        const RegBind& b = t_ps[r];
        if (b.param == 0xFF || b.param >= 64 || !b.is_table) continue;
        PsTexture t;
        t.reg = r;
        t.read = ps_regs.reads(r);
        if ((!t.read && !unread) || !want(r)) continue;
        t.ts = TableStartOf(dev, root[b.param].table);
        if (!t.ts) continue;
        t.offset = b.offset;
        if (!g_descriptors.Find({ t.ts.heap, t.ts.base + b.offset }, t.ri) || !t.ri.res) continue;
        fn(t);
    }
    return true;
}

void SectionTexturePublisher::CarryPsReads(const DrawArgs& da)
{
    if (!Mods().PsSlotQueries()) return;
    command_list* const cmd_list = da.cmd_list;
    CmdState& gs = g_command_lists.Gfx(cmd_list);
    if (!g_recognition.InSkinBand(gs.vb[0].va)) return;
    PROF(PROF_DI_PsCarry);
    const uint64_t layh = g_command_lists.GfxLayout(cmd_list);
    if (!layh) return;
    PsCarry& pc = gs.ps_carry;
    const uint64_t gen = g_command_lists.Gen(cmd_list);
    if (pc.layout != layh || pc.layout_gen != gen) {
        const bool mapped = g_layouts.WithRegMap(layh, [&](const LayoutRegMap& m) {
            for (unsigned r = 0; r < kSectionTexRegs; ++r) {
                const RegBind& b = m.t_ps[r];
                pc.param[r] = b.param < 64 && b.is_table ? b.param : 0xFF;
                pc.offset[r] = b.offset;
            }
            pc.layout = layh;
            pc.layout_gen = gen;
        });
        if (!mapped) return;
    }
    const uint64_t reads = g_pipelines.PsReadMask(gs.gfx_pso);
    for (unsigned r = 0; r < kSectionTexRegs; ++r)
        if (((reads >> r) & 1u) && pc.param[r] != 0xFF)
            pc.slot[r] = PsCarry::Slot{ gs.root[pc.param[r]].table, pc.offset[r], gen };
}

bool SectionTexturePublisher::CarryKey::operator<(const CarryKey& o) const
{
    return std::tie(dm, comp, ps, reg) < std::tie(o.dm, o.comp, o.ps, o.reg);
}

struct SectionTexturePublisher::CarryMemo::PerSet : SetState {
    std::map<CarryKey, CarriedTexture> map;
    ~PerSet() override
    {
        for (auto& kv : map) kv.second.res->Release();
    }
};

void SectionTexturePublisher::CarryMemo::Remember(const CarryKey& k, ID3D12Resource* res, uint32_t hash, unsigned fmt)
{
    std::lock_guard l(mutex_);
    CarriedTexture& e = CurrentSet().State<PerSet>().map[k];
    if (e.res != res) {
        res->AddRef();
        if (e.res) e.res->Release();
        e.res = res;
    }
    e.hash = hash;
    e.fmt = fmt;
    e.frame = g_frame_id;
}

bool SectionTexturePublisher::CarryMemo::Recall(const CarryKey& k, CarriedTexture& out)
{
    std::lock_guard l(mutex_);
    const std::map<CarryKey, CarriedTexture>& map = CurrentSet().State<PerSet>().map;
    auto it = map.find(k);
    if (it == map.end()) return false;
    out = it->second;
    out.res->AddRef();
    return true;
}

uint16_t SectionTexturePublisher::PublishCarried(
    command_list* cmd_list, int dm, int comp, const DxilSrvRegs& ps_regs, uint16_t pushed, uint64_t ps_hash)
{
    const PsCarry& pc = g_command_lists.Gfx(cmd_list).ps_carry;
    const uint64_t gen = g_command_lists.Gen(cmd_list);
    device* dev = cmd_list->get_device();
    uint16_t carried = 0;
    for (unsigned r = 0; r < kSectionTexRegs; ++r) {
        if (ps_regs.reads(r) || ((pushed >> r) & 1u)) continue;
        const CarryKey key{ dm, comp, ps_hash, r };
        const PsCarry::Slot& s = pc.slot[r];
        const TableStart ts = s.table.handle && s.gen == gen ? TableStartOf(dev, s.table) : TableStart{};
        ResInfo ri;
        const bool listed = ts && g_descriptors.Find({ ts.heap, ts.base + s.offset }, ri) && ri.res;
        CarriedTexture ct;
        if (listed) {
            const uint32_t resident = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(ri.res));
            const uint32_t hash =
                !resident || PortApi::TextureHashHasSections(resident) ? resident : g_tex_select.LodTargetOf(resident);
            ct.res = ri.res;
            ct.hash = hash && PortApi::TextureHashHasSections(hash) ? hash : resident;
            ct.fmt =
                ri.view_handle ? (unsigned)dev->get_resource_view_desc(resource_view{ ri.view_handle }).format : 0u;
            carry_memo_.Remember(key, ct.res, ct.hash, ct.fmt);
        }
        const bool recalled = !listed && carry_memo_.Recall(key, ct);
        if (listed || recalled) {
            PortApi::PushGameTexture('p', r, ct.res, ct.hash, ct.fmt);
            carried |= (uint16_t)(1u << r);
        }
        if (recalled) ct.res->Release();
        const unsigned src = listed ? 1u : recalled ? 2u : 0u;
        if (g_debug &&
            DiagChanged("SECTEX-CARRY",
                (int)(((unsigned)CompKey(dm, comp) * 16u + r) ^ (unsigned)(ps_hash * 0x9E3779B97F4A7C15ull >> 40)),
                (unsigned long long)ct.hash << 32 | ct.fmt << 2 | src))
            logger::write("SECTEX-CARRY f=%u mod=%d comp=%d ps-t%u ps=%016llx does not read it: %s%u -- %08x fmt=%u",
                g_frame_id, dm, comp, r, (unsigned long long)ps_hash,
                listed         ? "the list's last character draw that read it, frame "
                    : recalled ? "nothing on this list; remembered from frame "
                               : "nothing on this list, nothing remembered, frame ",
                recalled ? ct.frame : g_frame_id, ct.hash, ct.fmt);
    }
    return carried;
}

uint16_t SectionTexturePublisher::Publish(command_list* cmd_list, int dm, int comp, uint32_t rtv_count)
{
    const DxilSrvRegs ps_regs = g_pipelines.SrvRegs(g_command_lists.Gfx(cmd_list).gfx_pso, 'p');
    uint16_t pushed = 0;
    uint64_t ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    const uint64_t pass = TexLookPlaceOf(Mods().At(dm).wcl_ns.c_str(), comp, ps_hash);
    const bool every_slot = Mods().At(dm).mod.queries_ps_slots;
    device* dev = cmd_list->get_device();
    ForEachPsTexture(
        cmd_list, g_debug, ps_regs, [](unsigned) { return true; },
        [&](const PsTexture& t) {
            const unsigned r = t.reg;
            const bool read = t.read;
            const TableStart& ts = t.ts;
            const ResInfo& ri = t.ri;
            const uint32_t resident = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(ri.res));
            if (read && resident)
                g_tex_look.AtPlace(pass, r, resident,
                    PortApi::TextureHashHasSections(resident) ? resident : g_tex_lod.Known(resident));
            const uint32_t hash =
                !resident || PortApi::TextureHashHasSections(resident) ? resident : g_tex_select.LodTargetOf(resident);
            const bool known = hash && PortApi::TextureHashHasSections(hash);
            if (!known && !(every_slot && read)) return;
            if (read) {
                const unsigned fmt = every_slot && ri.view_handle
                    ? (unsigned)dev->get_resource_view_desc(resource_view{ ri.view_handle }).format
                    : 0u;
                PortApi::PushGameTexture('p', r, ri.res, known ? hash : resident, fmt);
                pushed |= (uint16_t)(1u << r);
            }
            if (!known) return;
            if (g_debug) {
                bool appeared;
                {
                    std::lock_guard l(sectex_mutex_);
                    uint32_t& last = sectex_last_[(uint64_t)hash | (read ? 0ull : 1ull << 32)];
                    appeared = !last || g_frame_id - last > kSectexGap;
                    last = g_frame_id;
                }
                if (appeared || g_diag_arms.sectex_watch.load(std::memory_order_relaxed) > 0) {
                    char lod[24] = "";
                    if (hash != resident) sprintf_s(lod, " (its mip %08x)", resident);
                    logger::write(
                        "SECTEX f=%u mod=%d comp=%d ps-t%u rtv=%u: %08x%s res=%p at %llx+%u, written %u frame(s) "
                        "ago (src %u), ps=%016llx %s -- %s",
                        g_frame_id, dm, comp, r, rtv_count, hash, lod, (void*)ri.res, (unsigned long long)ts.heap,
                        ts.base + t.offset, g_frame_id - ri.wrote_frame, (unsigned)ri.wrote_src,
                        (unsigned long long)ps_hash,
                        !read                ? "does NOT read it: not published"
                            : ps_regs.parsed ? "reads it"
                                             : "(no PSV) published",
                        appeared ? "first sighting in 30+ frames" : "watch: a mod switched itself back on");
                }
            }
        });
    if (every_slot) pushed |= PublishCarried(cmd_list, dm, comp, ps_regs, pushed, ps_hash);
    return pushed;
}

#ifdef WWMI_DEBUG
TexDrawWatch g_tex_draw_watch;

bool TexDrawWatch::Key::operator<(const Key& o) const
{
    return std::tie(ps, vs, reg, hash, indexed) < std::tie(o.ps, o.vs, o.reg, o.hash, o.indexed);
}

void TexDrawWatch::Note(const DrawArgs& da)
{
    Record(da.cmd_list, da.vs_hash, da.index_count, true);
}

void TexDrawWatch::NoteNonIndexed(command_list* cmd_list, uint32_t vertex_count)
{
    if (!on_.load(std::memory_order_relaxed)) return;
    uint64_t vs = 0;
    cmd_list->get_private_data(s_iid_vs_hash, &vs);
    Record(cmd_list, vs, vertex_count, false);
}

void TexDrawWatch::Record(command_list* cmd_list, uint64_t vs_hash, uint32_t count, bool indexed)
{
    if (!on_.load(std::memory_order_relaxed)) return;
    uint32_t want = 0;
    {
        std::lock_guard l(mutex_);
        if (last_frame_ && g_frame_id > last_frame_) {
            on_.store(false, std::memory_order_relaxed);
            return;
        }
        want = hash_;
    }
    uint64_t ps = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps);
    std::vector<std::pair<Key, Seen>> hits;
    ForEachPsTexture(
        cmd_list, false, g_pipelines.SrvRegs(g_command_lists.Gfx(cmd_list).gfx_pso, 'p'), [](unsigned) { return true; },
        [&](const PsTexture& t) {
            const uint64_t res = reinterpret_cast<uint64_t>(t.ri.res);
            const uint32_t hash = g_tex_identity.HashOf(res);
            if (want && hash != want) return;
            Seen s;
            s.draws = 1;
            s.count = count;
            TexInfo ti{};
            if (g_tex_identity.DimsOf(res, &ti)) {
                s.width = ti.width;
                s.height = ti.height;
                s.format = ti.format;
            }
            hits.push_back({ Key{ ps, vs_hash, t.reg, hash, indexed }, s });
        });
    if (hits.empty()) return;
    std::lock_guard l(mutex_);
    if (!on_.load(std::memory_order_relaxed) || hash_ != want) return;
    for (const auto& [k, s] : hits) {
        Seen& d = seen_[k];
        if (d.draws)
            ++d.draws;
        else
            d = s;
    }
}

std::string TexDrawWatch::Command(const std::vector<std::string>& args)
{
    std::lock_guard l(mutex_);
    if (args.size() == 1 && args[0] == "off") {
        on_.store(false);
        return "stopped; " + std::to_string(seen_.size()) + " finding(s) kept";
    }
    if (args.size() == 1) {
        const bool all = args[0] == "all";
        uint32_t hash = 0;
        if (!all) {
            char* end = nullptr;
            hash = (uint32_t)std::strtoul(args[0].c_str(), &end, 16);
            if (!end || *end || !hash) return "ERR not a texture hash: " + args[0];
        }
        hash_ = hash;
        last_frame_ = all ? g_frame_id + 1 : 0;
        seen_.clear();
        on_.store(true);
        logger::write("PIPE: texdraws %s", args[0].c_str());
        return all ? "recording every draw's ps-t0..t8 for about two frames"
                   : "watching draws for texture " + args[0] + " at ps-t0..t8";
    }
    std::string out;
    for (const auto& [k, s] : seen_) {
        char b[192];
        std::snprintf(b, sizeof b, "ps=%016llx vs=%016llx t%u tex=%08x %ux%u fmt=%u draws=%u %s=%u\n",
            (unsigned long long)k.ps, (unsigned long long)k.vs, k.reg, k.hash, s.width, s.height, s.format, s.draws,
            k.indexed ? "indices" : "vertices", s.count);
        out += b;
    }
    if (seen_.empty())
        out = hash_ ? "nothing yet (a texture under 256x256 has no hash: try texdraws all)\n" : "nothing\n";
    out += on_.load() ? "(still recording)" : "(stopped)";
    return out;
}
#endif

void SectionTexturePublisher::NoteMeshPlaces(const DrawArgs& da)
{
    const bool crawl = g_tex_crawl.On();
    if (!crawl && !g_tex_look.HasMeshPlaces()) return;
    command_list* const cmd_list = da.cmd_list;
    ID3D12Resource* ibr = nullptr;
    ID3D12Resource* vbr = nullptr;
    uint64_t ibva = 0;
    uint64_t vbva = 0;
    uint32_t idxb = 2;
    uint32_t vstride = 0;
    {
        const auto& st = g_command_lists.Gfx(cmd_list);
        if (!g_recognition.InSkinBand(st.vb[0].va) && !st.vb[6].res) return;
        ibr = st.ib.res;
        ibva = st.ib.va;
        if (st.ib.index_bytes) idxb = st.ib.index_bytes;
        vbr = st.vb[0].res;
        vbva = st.vb[0].va;
        vstride = st.vb[0].stride;
    }
    if (!ibr || !vbr || !vstride) return;
    const uint32_t ib_hash = IbHash::Lookup(ibr, (uint32_t)(ibva - ibr->GetGPUVirtualAddress()),
        (da.first_index + da.index_count) * idxb, idxb, vbr, (uint32_t)(vbva - vbr->GetGPUVirtualAddress()), vstride,
        /*grow*/ true);
    if (!ib_hash) return;
    uint64_t ps_hash = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps_hash);
    const uint64_t pass = TexLookMeshPassOf(ib_hash, da.first_index, da.index_count, ps_hash);
    const uint32_t mask = crawl ? ~0u : g_tex_look.MeshMask(pass, kSectionTexRegs);
    if (!mask) return;
    const DxilSrvRegs ps_regs = g_pipelines.SrvRegs(g_command_lists.Gfx(cmd_list).gfx_pso, 'p');
    ForEachPsTexture(
        cmd_list, false, ps_regs, [&](unsigned r) { return ((mask >> r) & 1u) != 0; },
        [&](const PsTexture& t) {
            const uint32_t resident = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(t.ri.res));
            if (!resident) return;
            const uint64_t place = TexLookSlotOf(pass, t.reg);
            if (crawl)
                g_tex_crawl.Note(place, resident);
            else
                g_tex_look.AtMeshPlace(
                    place, resident, PortApi::TextureHashHasSections(resident) ? resident : g_tex_lod.Known(resident));
        });
}
