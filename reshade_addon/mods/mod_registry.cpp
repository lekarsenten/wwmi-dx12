// The mod registry and the geometry loader. See mod_registry.h.
#include "../addon_common.h"
#include "mod_registry.h"
#include "component_table.h"
#include "port_section.h"
#include "../addon/addon.h"
#include "../core/log.h"
#include "../textures/mod_texture_store.h"
#include "../draw/draw_toggles.h"
#include "../migoto/port_api.h"
#include <algorithm>
#include <cwctype>
#include <filesystem>
#include <map>
#include <unordered_set>
#include "mod_set.h"


int ModRegistry::IdOfNs(const std::wstring& ns) const
{
    auto it = by_ns_.find(ns);
    return it == by_ns_.end() ? -1 : it->second;
}

int ModRegistry::IdOfNsHash(uint64_t ns_hash) const
{
    auto it = by_ns_hash_.find(ns_hash);
    return it == by_ns_hash_.end() ? -1 : it->second;
}

const WwmiMod::Component* ModRegistry::Comp(int mod_id, int idx) const
{
    if (mod_id < 0 || mod_id >= (int)units_.size()) return nullptr;
    for (auto& c : units_[mod_id].mod.comps)
        if (c.index == idx) return &c;
    return nullptr;
}

uint32_t ModRegistry::IndexEnd(int mod_id, uint32_t ib_hash) const
{
    if (mod_id < 0 || mod_id >= (int)index_end_.size()) return 0u;
    const auto it = index_end_[mod_id].find(ib_hash);
    return it == index_end_[mod_id].end() ? 0u : it->second;
}

bool ModRegistry::GeomEnabled(int mod_id) const
{
    if (mod_id < 0 || mod_id >= (int)units_.size()) return true;
    if (g_draw_toggles.wcl_draw && !units_[mod_id].wcl_ns.empty()) {
        return units_[mod_id].geom_enabled;
    }
    // Vars live under the ENGINE namespace.
    constexpr float kUnset = -1e30f;
    const float v = PortApi::GetVarNs(units_[mod_id].wcl_ns.c_str(), L"enable_mods", kUnset);
    if (v == kUnset) return true; // mod ships no enable toggle -> auto-on (author intent)
    return v != 0.0f; // manual mod -> honor $enable_mods (0 until the mod's enable key)
}

uint32_t ModRegistry::ClaimAge(int mod_id) const
{
    if (mod_id < 0 || mod_id >= (int)units_.size()) return 0xFFFFFFFFu;
    const uint32_t f = units_[mod_id].last_char_frame;
    return f == 0 ? 0xFFFFFFFFu : (uint32_t)(g_frame_id - f);
}

void ModRegistry::Build()
{
    parse_gen_ = PortApi::LoadGeneration();
    const std::vector<DiscoveredMod> discovered = Discover();
    discovered_ = (int)discovered.size();
    const std::unordered_map<std::wstring, WwmiMod::NsEntries> by_ns = WwmiMod::GroupByNamespace();
    for (auto& dm : discovered) {
        struct ObjGroup {
            std::wstring ns;
            std::vector<std::wstring> inis;
        };
        std::vector<ObjGroup> groups;
        for (auto& ini : dm.inis) {
            auto git =
                std::find_if(groups.begin(), groups.end(), [&](const ObjGroup& g) { return g.ns == ini.ns; });
            if (git == groups.end())
                groups.push_back({ ini.ns, { ini.path } });
            else
                git->inis.push_back(ini.path);
        }
        if (groups.empty()) continue;
        {
            auto has_lod = [](const std::wstring& p) {
                auto lo = [](wchar_t x) { return (x >= L'A' && x <= L'Z') ? (wchar_t)(x + 32) : x; };
                for (size_t i = 0; i + 3 <= p.size(); ++i)
                    if (lo(p[i]) == L'l' && lo(p[i + 1]) == L'o' && lo(p[i + 2]) == L'd') return true;
                return false;
            };
            std::stable_partition(groups.begin(), groups.end(),
                [&](const ObjGroup& g) { return !(has_lod(g.ns) || has_lod(g.inis.front())); });
        }
        for (size_t oi = 0; oi < groups.size(); ++oi) {
            units_.emplace_back(); // popped again below if their engine cannot describe it
            GeoMod& gm = units_.back();
            const int mod_id = (int)units_.size() - 1;
            gm.wcl_ns = groups[oi].ns; // their namespace for this unit's inis, asked once above
            if (WwmiMod::ReadMod(gm.wcl_ns, by_ns, gm.mod) <= 0 || gm.mod.comps.empty()) {
                units_.pop_back();
                continue;
            }
            if (gm.mod.dir.empty()) gm.mod.dir = dm.dir; // only if their SectionFolder had none
            WwmiMod::LoadResources(gm.mod, CurrentSet().held);
            if (gm.mod.queries_ps_slots) ps_slot_queries_.store(true, std::memory_order_relaxed);
            if (!gm.mod.forward_map_file.empty()) {
                std::wstring rel = gm.mod.forward_map_file;
                for (auto& c : rel)
                    if (c == L'/') c = L'\\';
                const bool rooted = (rel.size() > 2 && rel[1] == L':') || (rel.size() > 1 && rel[0] == L'\\');
                gm.fwdmap = rooted ? rel : (gm.mod.dir + L"\\" + rel);
            }
            by_ns_[gm.wcl_ns] = mod_id;
            gm.ns_hash = NsHash(gm.wcl_ns.c_str());
            if (gm.ns_hash) {
                by_ns_hash_[gm.ns_hash] = mod_id;
                gm.det_slot = PortApi::VarSlotNs(gm.wcl_ns.c_str(), L"object_detected");
            }
            for (auto& c : units_[mod_id].mod.comps) {
                MergedComp mc;
                mc.mod_id = mod_id;
                mc.comp_idx = c.index;
                mc.first_index = c.match_first_index;
                mc.match_count = c.match_index_count;
                mc.remap_size = 0;
                Comps().All().push_back(mc);
            }
            logger::write("WwmiGeom: geometry unit #%d ns='%ls' guid=%d '%ls' (%zu comps) fwdmap=%ls", mod_id,
                units_[mod_id].wcl_ns.c_str(), units_[mod_id].mod.object_guid, units_[mod_id].mod.dir.c_str(),
                units_[mod_id].mod.comps.size(), units_[mod_id].fwdmap.c_str());
        }
    }
    if (!units_.empty()) {
        int reachable = 0;
        int inert = 0;
        for (size_t i = 0; i < units_.size(); ++i) {
            const auto& gm = units_[i];
            if (gm.mod.comps.empty()) continue;
            const WwmiMod::Component& c0 = gm.mod.comps.front();
            const std::wstring fq = PortComponentSection((int)i, c0.index, c0.tail, gm.wcl_ns);
            if (!fq.empty()) {
                ++reachable;
                continue;
            }
            ++inert;
            logger::write("PORT-NS INERT mod %zu ns='%ls' -- their engine has no [TextureOverride...%ls] "
                          "under that namespace, so this mod will never run a section",
                i, gm.wcl_ns.c_str(), c0.tail.c_str());
        }
        logger::write("PORT-NS audit: %d of %zu geometry mods can resolve their own component sections%s",
            reachable, units_.size(), inert ? "  *** SOME ARE INERT, see above ***" : "");
    }
    Comps().Index();
    index_end_.assign(units_.size(), {});
    for (size_t i = 0; i < Comps().Count(); ++i) {
        const MergedComp& mc = Comps().At(i);
        const WwmiMod::Component* wc = Comp(mc.mod_id, mc.comp_idx);
        if (mc.mod_id >= 0 && mc.mod_id < (int)index_end_.size()) {
            uint32_t& end = index_end_[mc.mod_id][wc ? wc->ib_hash : 0u];
            end = (std::max)(end, mc.first_index + mc.match_count);
        }
    }
    for (MergedComp& mc : Comps().All()) mc.with.clear();
    for (size_t a = 0; a < Comps().Count(); ++a) {
        const MergedComp& ma = Comps().At(a);
        const std::vector<int>* shared = Comps().SharedSlots(ma.first_index, ma.match_count);
        if (!shared || shared->front() != (int)a) continue;
        for (const int x : *shared) {
            const WwmiMod::Component* ca = Comp(Comps().At(x).mod_id, Comps().At(x).comp_idx);
            if (!ca || !ca->ib_hash) continue;
            for (const int b : *shared) {
                const WwmiMod::Component* cb = Comp(Comps().At(b).mod_id, Comps().At(b).comp_idx);
                if (b != x && cb && cb->ib_hash == ca->ib_hash) Comps().At(x).with.push_back(b);
            }
            if (x == shared->front() && !Comps().At(x).with.empty())
                logger::write(
                    "WwmiGeom: hash=%08x first=%u count=%u is declared by %zu mods -- every one's section runs "
                    "on its draw, as on DX11",
                    ca->ib_hash, Comps().At(x).first_index, Comps().At(x).match_count,
                    Comps().At(x).with.size() + 1);
        }
    }
    {
        auto priority = [this](int s) {
            const WwmiMod::Component* c = Comp(Comps().At(s).mod_id, Comps().At(s).comp_idx);
            return c ? c->match_priority : 0;
        };
        auto section = [this](int s) {
            const MergedComp& m = Comps().At(s);
            const WwmiMod::Component* c = Comp(m.mod_id, m.comp_idx);
            return c ? PortComponentSection(m.mod_id, c->index, c->tail, units_[m.mod_id].wcl_ns) : std::wstring();
        };
        for (size_t a = 0; a < Comps().Count(); ++a) {
            MergedComp& mc = Comps().At(a);
            mc.run_order.assign(1, (int)a);
            mc.run_order.insert(mc.run_order.end(), mc.with.begin(), mc.with.end());
            if (mc.run_order.size() < 2) continue;
            std::stable_sort(mc.run_order.begin(), mc.run_order.end(), [&](int x, int y) {
                const int px = priority(x);
                const int py = priority(y);
                return px != py ? px < py : section(x) < section(y);
            });
            if ((int)a != mc.run_order.front()) continue;
            std::string order;
            for (const int s : mc.run_order)
                order +=
                    " mod" + std::to_string(Comps().At(s).mod_id) + "(" + std::to_string(priority(s)) + ")";
            logger::write("WwmiGeom: first=%u count=%u -- its sections run lowest match_priority first:%s",
                mc.first_index, mc.match_count, order.c_str());
        }
    }
    if (!units_.empty()) Comps().Finalize();
}

std::vector<DiscoveredMod> ModRegistry::Discover()
{
    namespace fs = std::filesystem;
    struct Found {
        fs::path root;
        ModIni ini;
    };
    std::vector<Found> found;
    PortApi::ForEachModIni(
        [](const wchar_t* ini, const wchar_t* ns, const wchar_t* root, void* user) {
            static_cast<std::vector<Found>*>(user)->push_back({ root, { ini, ns } });
        },
        &found);
    auto lower = [](std::wstring s) {
        for (auto& c : s) c = (wchar_t)towlower(c);
        return s;
    };
    std::unordered_set<std::wstring> ini_dirs;
    for (const auto& f : found) ini_dirs.insert(lower(fs::path(f.ini.path).parent_path().wstring()));
    std::map<fs::path, DiscoveredMod> mods;
    for (auto& f : found) {
        const fs::path folder = fs::path(f.ini.path).parent_path();
        fs::path key = f.ini.path;
        fs::path dir = f.root;
        fs::path walk = f.root;
        for (const auto& part : folder.lexically_relative(f.root)) {
            if (part == L".") break;
            walk /= part;
            if (ini_dirs.count(lower(walk.wstring()))) {
                key = walk;
                dir = walk;
                break;
            }
        }
        DiscoveredMod& m = mods[key];
        m.dir = dir.wstring();
        m.name = (dir == f.root ? fs::path(f.ini.path).filename() : dir.lexically_relative(f.root)).wstring();
        m.inis.push_back(std::move(f.ini));
    }
    std::vector<DiscoveredMod> out;
    for (auto& kv : mods) {
        std::sort(kv.second.inis.begin(), kv.second.inis.end(),
            [](const ModIni& a, const ModIni& b) { return a.path < b.path; });
        out.push_back(std::move(kv.second));
    }
    return out;
}

