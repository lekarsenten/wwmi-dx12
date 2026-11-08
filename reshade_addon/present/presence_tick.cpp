// presence_tick.cpp -- see presence_tick.h.
#include "../addon_common.h"
#include "../mods/character_recognition.h"
#include "../mods/port_section.h"
#include "presence_tick.h"
#include "../textures/tex_rules.h"
#include "../textures/tex_slot_log.h"
#include "../migoto/port_api.h"
#include "../mods/mod_set.h"
#include <algorithm>
#include <unordered_set>

using namespace reshade::api;

PresenceTriggers g_presence;

struct PresenceTriggers::PerSet : SetState {
    std::vector<std::pair<std::wstring, std::wstring>> trigs; // (ns, section)
    bool cleared = false;
    bool built = false;
    std::vector<uint32_t> last_log; // RECOVER: per unit, grown to the load
    struct ModelFlag {
        uint32_t hash = 0;
        std::wstring fq;
        std::vector<int> mods; // units whose components carry the hash: their claim is a draw of the model
    };
    std::vector<ModelFlag> model_flags; // other mods' variable-only sections keyed to a model's hash
};

void PresenceTriggers::Tick()
{
    PerSet& ps = CurrentSet().State<PerSet>();
    if (!ps.cleared) {
        ps.cleared = true;
        g_recognition.SetSkipVbHashes(nullptr); // rebuilt below
        g_recognition.SetSmallMeshHashes(nullptr);
        g_recognition.SetModelHashes(nullptr);
        std::lock_guard<std::mutex> l(g_cs_trigs_mtx);
        g_cs_trigs.clear(); // rebuilt below with ps.trigs
        g_cs_trigs_snap = std::make_shared<const std::vector<CsTrig>>(); // nothing fires until the rebuild publishes
    }
    if (!ps.built && g_tex_rules.Ready()) {
        PortApi::ForEachTextureOverride(
            [](uint32_t hash, const wchar_t* fq, bool ranged, const wchar_t*, bool, void* user) {
                auto* out = (std::vector<std::pair<std::wstring, std::wstring>>*)user;
                if (!fq) return;
                if (ranged) return; // driven per-draw by draw_comp
                const wchar_t* leaf = wcsrchr(fq, L'\\');
                if (!leaf) return; // no namespace: a global section
                if (_wcsnicmp(leaf + 1, L"component", 9) == 0) return; // draw-triggered
                if (hash && g_tex_rules.Now().hash_to_rule.count(hash)) return;
                const wchar_t* ns_beg = wcschr(fq, L'\\'); // skip the type prefix
                if (!ns_beg || ns_beg >= leaf) return;
                if (SectionReadsPipelineState(fq)) {
                    std::lock_guard<std::mutex> l(g_cs_trigs_mtx);
                    g_cs_trigs.push_back({ std::wstring(ns_beg + 1, leaf), std::wstring(fq), hash });
                    return;
                }
                out->emplace_back(std::wstring(ns_beg + 1, leaf), std::wstring(fq));
            },
            &ps.trigs);
        ps.built = true;
        { // the skip sections: texture targets are the texture pipeline's, the rest are meshes
            auto skips = std::make_shared<std::unordered_set<uint32_t>>();
            PortApi::ForEachSkipTextureOverride(
                [](uint32_t hash, const wchar_t* fq, void* user) {
                    if (!hash || g_tex_rules.Now().hash_to_rule.count(hash)) return;
                    ((std::unordered_set<uint32_t>*)user)->insert(hash);
                    logger::write("SKIPONLY: %08x [%ls] -- a game draw of this mesh is dropped", hash, fq ? fq : L"?");
                },
                skips.get());
            g_recognition.SetSkipVbHashes(
                skips->empty() ? nullptr : std::shared_ptr<const std::unordered_set<uint32_t>>(skips));
        }
        {
            auto meshes = std::make_shared<std::unordered_set<uint32_t>>();
            PortApi::ForEachVarTextureOverride(
                [](uint32_t hash, const wchar_t* fq, void* user) {
                    if (!hash || g_tex_rules.Now().hash_to_rule.count(hash)) return;
                    if (((std::unordered_set<uint32_t>*)user)->insert(hash).second)
                        logger::write(
                            "SMALLMESH: %08x [%ls] -- run when a small unskinned draw's vertex buffer hashes to it",
                            hash, fq ? fq : L"?");
                },
                meshes.get());
            g_recognition.SetSmallMeshHashes(
                meshes->empty() ? nullptr : std::shared_ptr<const std::unordered_set<uint32_t>>(meshes));
        }
        {
            PortApi::ForEachVarTextureOverride(
                [](uint32_t hash, const wchar_t* fq, void* user) {
                    if (!hash || !fq || g_tex_rules.Now().hash_to_rule.count(hash)) return;
                    const wchar_t* a = wcschr(fq, L'\\');
                    const wchar_t* z = wcsrchr(fq, L'\\');
                    if (!a || z <= a) return;
                    const size_t n = (size_t)(z - a - 1);
                    for (const GeoMod& gm : Mods().Units())
                        if (gm.wcl_ns.size() == n && _wcsnicmp(gm.wcl_ns.c_str(), a + 1, n) == 0) return;
                    ((std::vector<PerSet::ModelFlag>*)user)->push_back(PerSet::ModelFlag{ hash, fq, {} });
                },
                &ps.model_flags);
            auto hashes = std::make_shared<std::unordered_set<uint32_t>>();
            size_t on_mods = 0;
            for (PerSet::ModelFlag& mf : ps.model_flags) {
                hashes->insert(mf.hash);
                for (const MergedComp& mc : Comps().All()) {
                    const WwmiMod::Component* c = Mods().Comp(mc.mod_id, mc.comp_idx);
                    if (!c || c->ib_hash != mf.hash) continue;
                    if (std::find(mf.mods.begin(), mf.mods.end(), mc.mod_id) == mf.mods.end())
                        mf.mods.push_back(mc.mod_id);
                }
                if (!mf.mods.empty()) ++on_mods;
            }
            g_recognition.SetModelHashes(
                hashes->empty() ? nullptr : std::shared_ptr<const std::unordered_set<uint32_t>>(hashes));
            logger::write("MODELFLAGS: %zu variable-only section(s) of other mods keyed to a mesh hash, %zu on a loaded "
                          "mod's mesh -- run once per frame while that mesh is drawn, as DX11 runs them at its draws",
                ps.model_flags.size(), on_mods);
        }
        size_t ncs = 0;
        {
            std::lock_guard<std::mutex> l(g_cs_trigs_mtx);
            ncs = g_cs_trigs.size();
            g_cs_trigs_snap = std::make_shared<const std::vector<CsTrig>>(g_cs_trigs);
        } // the dispatch hook's copy, made once
        logger::write("Wcl: %zu presence-trigger sections (+%zu DISPATCH-bound: bodies that "
                      "read cs/vs/ps, moved off the per-frame path)",
            ps.trigs.size(), ncs);
    }
    constexpr uint32_t kRetryFrames = 60;
    if (ps.built)
        for (int md = 0; md < (int)Mods().Count(); ++md) {
            const uint32_t age = Mods().ClaimAge(md);
            const bool fresh = (age <= 2u);
            const bool retry = !fresh && (g_frame_id % kRetryFrames == (uint32_t)md % kRetryFrames);
            if (!fresh && !retry) continue;
            if (retry && logger::enabled(1)) {
                if (ps.last_log.size() <= (size_t)md) ps.last_log.resize(md + 1);
                if (g_frame_id - ps.last_log[md] > 600) {
                    ps.last_log[md] = g_frame_id;
                    logger::write("RECOVER: mod %d claim stale (age=%u) -- re-running presence triggers", md, age);
                }
            }
            const std::wstring& uns = Mods().At(md).wcl_ns;
            if (uns.empty()) continue;
            for (auto& [tns, tfq] : ps.trigs)
                if (_wcsicmp(tns.c_str(), uns.c_str()) == 0) PortApi::RunSectionByName(tfq.c_str(), nullptr);
        }
    if (!ps.built || ps.model_flags.empty()) return;
    constexpr uint32_t kModelFrames = 2;
    std::unordered_set<uint32_t> drawn;
    g_recognition.ModelsDrawnWithin(kModelFrames, drawn);
    for (const PerSet::ModelFlag& mf : ps.model_flags) {
        bool on = drawn.count(mf.hash) != 0;
        for (const int md : mf.mods) on = on || Mods().ClaimAge(md) <= kModelFrames;
        if (!on) continue;
        PortApi::RunSectionByName(mf.fq.c_str(), nullptr);
#ifdef WWMI_DEBUG
        if (g_tex_trace.Armed())
            g_tex_trace.Line("model flag %08x [%ls] run: %s", mf.hash, mf.fq.c_str(),
                drawn.count(mf.hash) ? "its mesh was drawn" : "its mod claimed a draw");
#endif
        if (g_debug && DiagOnce("MODELFLAG-RUN", (int)mf.hash))
            logger::write("MODELFLAG-RUN f=%u %08x [%ls]: its mesh is drawn (%s) -- run", g_frame_id, mf.hash,
                mf.fq.c_str(), drawn.count(mf.hash) ? "unclaimed" : "by its mod");
    }
}
