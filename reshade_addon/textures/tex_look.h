#pragma once
#include "tex_look_sig.h"
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

// A texture recognised by how it looks: its signature against every reference (a mod target, or a proven lower
// mip of one), a histogram shortlist first. A match becomes a look-alike link in the LOD cache. References and
// one verdict per content hash persist in tex_look.bin under the algorithm version: a hash is decoded once, again
// only when the references changed since. The places HD draws each target at are kept too, so a texture drawn at
// one of them is that target's look-alike by place.
class TexLook {
public:
    void Init(const std::wstring& cache_dir = std::wstring());
    void FlushIfQuiet(unsigned quiet_ms);
    void OnExit();
    bool Wants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target);
    void OnCut(uint32_t hash, const TexLookCut& cut, bool is_target);
    void AtPlace(uint64_t pass, uint32_t slot, uint32_t hash, uint32_t target); // a component draw's slot
    void AtMeshPlace(uint64_t place, uint32_t hash, uint32_t target); // a game character draw's
    void AddMeshPlace(uint64_t place, uint32_t target);
    uint32_t MeshMask(
        uint64_t pass, unsigned regs) const; // the registers below `regs` with a mesh place of one texture
    bool HasMeshPlaces() const { return has_mesh_.load(std::memory_order_relaxed); }

private:
    enum class Kind : uint8_t {
        NoMatch = 0,
        Match = 1,
        Ref = 2, // offered as a reference: a target, or a proven lower mip of one
    };
    struct Verdict {
        uint32_t gen = 0; // the reference generation it was decided against
        Kind kind = Kind::NoMatch;
    };
    using PlaceMap = std::unordered_map<uint64_t, uint32_t>;

    std::wstring Path() const { return dir_.empty() ? std::wstring() : dir_ + L"\\tex_look.bin"; }
    bool Weaker(uint32_t target, uint32_t w, uint32_t h) const;
    uint32_t TargetOf(uint32_t hash, bool is_target) const;
    bool Offered(uint32_t hash) const;
    bool KeepRef(uint32_t target, texlook::Ref&& r);
    void Offer(uint32_t source, uint32_t target, texlook::LookSig&& sig);
    void Match(uint32_t hash, const texlook::LookSig& sig);
    void MarkDirty();
    bool KeepPlace(PlaceMap& m, uint64_t place, uint32_t target);
    void AtPlaceIn(PlaceMap& m, uint64_t place, uint32_t hash, uint32_t target);
    size_t Load(const std::wstring& file, bool seed);
    void Write();

    mutable std::shared_mutex mtx_;
    std::unordered_map<uint32_t, texlook::Ref> refs_; // target -> its reference
    std::unordered_map<uint32_t, Verdict> verdicts_; // content hash -> what its decode decided
    PlaceMap places_; // a pass's slot -> the target HD draws there, or kMixed
    PlaceMap mesh_places_; // the same for a game character draw's slot
    std::atomic<bool> has_mesh_{ false };
    std::atomic<uint32_t> mesh_gen_{ 1 }; // bumped by every change to mesh_places_
    std::unordered_set<uint64_t> placed_; // (hash, target) a place linked this session
    uint32_t gen_ = 1; // bumped by every reference added or replaced
    bool loaded_ = false;
    bool dirty_ = false;
    uint64_t dirty_at_ = 0;
    std::wstring dir_;
    std::vector<std::pair<std::wstring, std::wstring>>
        imported_; // seed taken -> its DISABLED_ name, after the next write
    std::atomic<uint32_t> decoded_{ 0 };
    std::atomic<uint32_t> skipped_{ 0 };
    std::atomic<uint32_t> matched_{ 0 };
    std::atomic<uint32_t> refs_new_{ 0 };
    std::atomic<uint32_t> places_new_{ 0 };
    std::atomic<uint32_t> by_place_{ 0 };
};
extern TexLook g_tex_look;

// The place keys: one per component draw's pass and slot, and per game character draw's.
uint64_t TexLookPlaceOf(const wchar_t* ns, int comp, uint64_t ps); // a component draw's pass: one key on every install
uint64_t TexLookMeshPassOf(uint32_t ib_hash, uint32_t first_index, uint32_t index_count, uint64_t ps);
uint64_t TexLookSlotOf(uint64_t pass, uint32_t slot);
