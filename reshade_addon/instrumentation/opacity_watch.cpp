#include "opacity_watch.h"
#include "../hooks/cmd_bind_hooks.h" // s_iid_ps_hash
#include "../hooks/command_list_registry.h"
#include "../mods/game_cb_reader.h"
#include "../mods/layout_registry.h"
#include "../core/log.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>

OpacityWatch g_opacity_watch;

namespace {

std::string Read(reshade::api::command_list* cmd_list, uint64_t layout, const RfxCbRef& r, float* v)
{
    *v = NAN;
    if (r.reg == 0xFF) return "(not a CB load)";
    char name[32];
    snprintf(name, sizeof name, "cb%u[%u].%c", (unsigned)r.reg, (unsigned)r.row, "xyzw"[r.comp & 3]);
    RegBind rb;
    g_layouts.WithRegMap(layout, [&](const LayoutRegMap& m) {
        if (r.reg < std::size(m.b_ps)) rb = m.b_ps[r.reg];
    });
    if (rb.param >= 64) return std::string(name) + "=(no root)";
    if (rb.is_table) return std::string(name) + "=(table)";
    const RootCbvBind& b = g_command_lists.Cbv(cmd_list).binds[rb.param];
    float row[4];
    if (!b.res || !b.va ||
        g_game_cbs.Snapshot(b.res, b.va + r.row * 16ull, reinterpret_cast<uint8_t*>(row), sizeof row) < sizeof row)
        return std::string(name) + "=(unread)";
    *v = row[r.comp & 3];
    char out[64];
    snprintf(out, sizeof out, "%s=%g", name, *v);
    return out;
}

} // namespace

void OpacityWatch::NoteShader(uint64_t ps_hash, const std::vector<RfxOpacitySite>& sites)
{
    if (sites.empty()) return;
    std::lock_guard l(mutex_);
    sites_[ps_hash] = sites;
}

void OpacityWatch::NoteDraw(reshade::api::command_list* cmd_list, uint64_t ps_hash, int mod_id, int comp)
{
    std::vector<RfxOpacitySite> sites;
    {
        std::lock_guard l(mutex_);
        const auto it = sites_.find(ps_hash);
        if (it == sites_.end()) return;
        sites = it->second;
    }
    const uint64_t layout = g_command_lists.GfxLayout(cmd_list);
    std::string text;
    unsigned long long h = 1469598103934665603ull;
    for (const RfxOpacitySite& s : sites) {
        float sv = NAN;
        float av = NAN;
        float bv = NAN;
        const std::string st = Read(cmd_list, layout, s.sw, &sv);
        const std::string at = Read(cmd_list, layout, s.a, &av);
        const std::string bt = Read(cmd_list, layout, s.b, &bv);
        const char* branch = std::isnan(sv) ? "?" : sv < 0.5f ? "fixed" : "camera fade";
        if (!text.empty()) text += "; ";
        text += st + " -> " + branch + ", min(" + at + ", " + bt + ")";
        for (const float f : { sv, av, bv }) {
            uint32_t u = 0;
            memcpy(&u, &f, sizeof u);
            h = (h ^ u) * 1099511628211ull;
        }
    }
    {
        std::lock_guard l(mutex_);
        Seen& e = seen_[{ ps_hash, mod_id, comp }];
        if (e.text != text) {
            ++e.changes;
            e.text = text;
        }
        e.frame = g_frame_id;
    }
    const int key = (int)(uint32_t)(ps_hash ^ (ps_hash >> 32)) ^ CompKey(mod_id, comp);
    if (DiagChanged(&g_opacity_watch, key, h))
        logger::write("OPACITY f=%u ps=%016llx mod=%d comp=%d: %s", g_frame_id, (unsigned long long)ps_hash, mod_id,
            comp, text.c_str());
}

CbRowsWatch g_cb_rows;

void CbRowsWatch::NoteDraw(reshade::api::command_list* cmd_list)
{
    const uint64_t want = ps_.load(std::memory_order_relaxed);
    if (!want) return;
    uint64_t ps = 0;
    cmd_list->get_private_data(s_iid_ps_hash, &ps);
    if (ps != want) return;
    unsigned reg = 0;
    unsigned first = 0;
    unsigned rows = 0;
    {
        std::lock_guard l(mutex_);
        reg = reg_;
        first = first_;
        rows = rows_;
    }
    RegBind rb;
    g_layouts.WithRegMap(g_command_lists.GfxLayout(cmd_list), [&](const LayoutRegMap& m) {
        if (reg < std::size(m.b_ps)) rb = m.b_ps[reg];
    });
    std::string text;
    unsigned long long h = 1469598103934665603ull;
    if (rb.param >= 64 || rb.is_table) {
        text = rb.param >= 64 ? "(no root)" : "(table)";
    } else {
        const RootCbvBind& b = g_command_lists.Cbv(cmd_list).binds[rb.param];
        for (unsigned r = first; r < first + rows; ++r) {
            float v[4];
            char row[96];
            if (!b.res || !b.va ||
                g_game_cbs.Snapshot(b.res, b.va + r * 16ull, reinterpret_cast<uint8_t*>(v), sizeof v) < sizeof v) {
                snprintf(row, sizeof row, " [%u]=(unread)", r);
            } else {
                snprintf(row, sizeof row, " [%u]=(%g, %g, %g, %g)", r, v[0], v[1], v[2], v[3]);
                for (const float f : v) {
                    uint32_t u = 0;
                    memcpy(&u, &f, sizeof u);
                    h = (h ^ u) * 1099511628211ull;
                }
            }
            text += row;
        }
    }
    {
        std::lock_guard l(mutex_);
        if (text != last_) {
            ++changes_;
            last_ = text;
        }
        frame_ = g_frame_id;
    }
    if (DiagChanged(&g_cb_rows, (int)(uint32_t)(ps ^ (ps >> 32)), h))
        logger::write("CBROWS f=%u ps=%016llx cb%u:%s", g_frame_id, (unsigned long long)ps, reg, text.c_str());
}

std::string CbRowsWatch::Command(const std::vector<std::string>& args)
{
    if (args.size() == 1 && args[0] == "off") {
        ps_.store(0, std::memory_order_relaxed);
        return "stopped";
    }
    if (args.size() == 3 || args.size() == 4) {
        char* end = nullptr;
        const uint64_t ps = std::strtoull(args[0].c_str(), &end, 16);
        if (!end || *end || !ps) return "ERR not a shader hash: " + args[0];
        const unsigned reg = (unsigned)std::strtoul(args[1].c_str(), nullptr, 10);
        const unsigned first = (unsigned)std::strtoul(args[2].c_str(), nullptr, 10);
        const unsigned rows = args.size() == 4 ? (unsigned)std::strtoul(args[3].c_str(), nullptr, 10) : 8u;
        if (reg >= 14 || !rows || rows > 64) return "ERR cbrows <ps> <register 0-13> <first row> [rows 1-64]";
        {
            std::lock_guard l(mutex_);
            reg_ = reg;
            first_ = first;
            rows_ = rows;
            changes_ = 0;
            frame_ = 0;
            last_.clear();
        }
        ps_.store(ps, std::memory_order_relaxed);
        logger::write("PIPE: cbrows %s cb%u rows %u..%u", args[0].c_str(), reg, first, first + rows - 1);
        return "watching " + args[0] + " cb" + args[1] + " rows " + std::to_string(first) + ".." +
            std::to_string(first + rows - 1);
    }
    if (!args.empty()) return "ERR cbrows <ps> <register> <first row> [rows], cbrows off, or cbrows";
    std::lock_guard l(mutex_);
    if (!ps_.load(std::memory_order_relaxed)) return "not watching";
    if (!frame_) return "no draw of that shader yet";
    return "f=" + std::to_string(g_frame_id) + ", last draw at f=" + std::to_string(frame_) + ", " +
        std::to_string(changes_) + " change(s):" + last_;
}

std::string OpacityWatch::Command(const std::vector<std::string>& args)
{
    const uint32_t frames = args.empty() ? 120u : (uint32_t)std::strtoul(args[0].c_str(), nullptr, 10);
    std::lock_guard l(mutex_);
    std::string out = "f=" + std::to_string(g_frame_id) + ", " + std::to_string(sites_.size()) +
        " shader(s) with a fade switch\n";
    for (const auto& [k, e] : seen_) {
        if (g_frame_id - e.frame > frames) continue;
        char head[112];
        snprintf(head, sizeof head, "ps=%016llx mod=%d comp=%d age=%u changes=%u: ",
            (unsigned long long)std::get<0>(k), std::get<1>(k), std::get<2>(k), g_frame_id - e.frame, e.changes);
        out += head + e.text + "\n";
    }
    return out;
}
