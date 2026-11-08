#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>
#include "wwmi_mod.h" // WwmiMod::Mod -- GeoMod
#include "../core/frame_clock.h" // FrameCounter -- GeoMod; g_frame_id -- ClaimAge
#include "../core/ns_hash.h"

// ONE LOADED CHARACTER MOD, shared by the hook modules.
struct GeoMod {
    WwmiMod::Mod mod;
    std::wstring fwdmap;
    std::wstring wcl_ns; // THE mod key: their engine's namespace, from PortApi::NamespaceForIni
    uint64_t ns_hash = 0;
    float* det_slot = nullptr;
    uint32_t last_char_frame = 0; // g_frame_id of the last claimed draw of this unit's character; 0 = never
    float det_at_keys = 0.f; // $object_detected sampled at key-poll time, before [Present]'s post pass clears it
    bool geom_enabled = true; // $mod_enabled sampled after [Present]'s post pass; what GeomEnabled answers
    // this unit's share of the heartbeat's sect= / skip= / replaced=
    FrameCounter sect_runs;
    FrameCounter skip_orig;
    FrameCounter replaced;
};

// One mod: the top-most folder under one of d3dx.ini's include_recursive roots holding an ini their engine parsed,
// with every such ini under it. An ini directly in a root is a mod of its own.
struct ModIni {
    std::wstring path; // absolute
    std::wstring ns; // their engine's namespace for it
};
struct DiscoveredMod {
    std::wstring dir; // absolute mod folder
    std::wstring name; // its folder from the root; the ini's file name when it sits in the root
    std::vector<ModIni> inis;
};

class ModRegistry {
public:
    size_t Count() const { return units_.size(); }
    GeoMod& At(size_t mod_id) { return units_[mod_id]; }
    const GeoMod& At(size_t mod_id) const { return units_[mod_id]; }
    std::deque<GeoMod>& Units() { return units_; }
    // The mod whose namespace this is (GeoMod::wcl_ns); -1 for none.
    int IdOfNs(const std::wstring& ns) const;
    int IdOfNsHash(uint64_t ns_hash) const;
    unsigned ParseGen() const { return parse_gen_; }
    // A loaded mod asks what the game bound in ps-tN.
    bool PsSlotQueries() const { return ps_slot_queries_.load(std::memory_order_relaxed); }
    const WwmiMod::Component* Comp(int mod_id, int idx) const;
    uint32_t IndexEnd(int mod_id, uint32_t ib_hash) const;
    // WWMI enable-flag compliance: is this mod's geometry meant to be applied right now?
    bool GeomEnabled(int mod_id) const;
    uint32_t ClaimAge(int mod_id) const;
    bool OnScreen(int mod_id) const { return ClaimAge(mod_id) <= 4u; }
    // The geometry units discovered and described from their engine's parse, and the component table filled.
    void Build();
    static std::vector<DiscoveredMod> Discover();
    int DiscoveredCount() const { return discovered_; }

private:
    std::deque<GeoMod> units_;
    std::unordered_map<std::wstring, int> by_ns_;
    std::unordered_map<uint64_t, int> by_ns_hash_;
    unsigned parse_gen_ = 0;
    int discovered_ = 0;
    std::vector<std::unordered_map<uint32_t, uint32_t>> index_end_; // per mod: ib hash -> the last index it declares
    std::atomic<bool> ps_slot_queries_{ false };
};
