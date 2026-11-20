#pragma once
#include "tex_look_sig.h"
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <mutex>
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
    enum class RefTaken : uint8_t {
        Kept,
        AsDense, // its target has a reference as dense already
        Flat,
        Unread, // no signature: off, degenerate, or a cut that does not fit
    };

    void Init(const std::wstring& cache_dir = std::wstring());
    void FlushIfQuiet(unsigned quiet_ms);
    void OnExit();
    bool Wants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target);
    // Where an upload Wants says to look at is cut: none for the default five, else every spot the references of
    // its size use.
    std::vector<texlook::Spot> SpotsFor(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target) const;
    void OnCut(uint32_t hash, const TexLookCut& cut, bool is_target);
    // `cut` is whole: a texture not a power of two a side, too flat at the default spots, is taken at its busiest.
    RefTaken TakeRef(uint32_t hash, uint32_t target, const TexLookCut& cut);
    bool Taken(uint32_t hash) const; // offered as a reference already: kept, as dense, or too flat
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
        uint32_t gen = 0; // the reference generation it was decided against; 0: decided again at the next upload
        Kind kind = Kind::NoMatch;
    };
    struct ShapeSpots {
        texlook::Spots at{};
        uint32_t refs = 0;
    };
    enum class RefGroup : uint8_t {
        PowerOfTwo,
        FreeDefault, // not a power of two a side, at the default spots
        FreePlaced, // at spots of its own, written with them
    };
    using PlaceMap = std::unordered_map<uint64_t, uint32_t>;

    std::wstring Path() const { return dir_.empty() ? std::wstring() : dir_ + L"\\tex_look.bin"; }
    bool Weaker(uint32_t target, uint32_t w, uint32_t h) const;
    uint32_t TargetOf(uint32_t hash, bool is_target) const;
    bool Offered(uint32_t hash) const;
    void PutRef(uint32_t target, texlook::Ref&& r);
    bool KeepRef(uint32_t target, texlook::Ref&& r);
    RefTaken Offer(uint32_t source, uint32_t target, texlook::LookSig&& sig);
    void Match(uint32_t hash, const TexLookCut& cut);
    void NoteSize(uint32_t hash, uint32_t w, uint32_t h, uint32_t target, uint32_t ref_w, uint32_t ref_h);
    void MarkDirty();
    bool KeepPlace(PlaceMap& m, uint64_t place, uint32_t target);
    void AtPlaceIn(PlaceMap& m, uint64_t place, uint32_t hash, uint32_t target);
    static RefGroup GroupOf(const texlook::LookSig& s);
    bool ReadRefs(FILE* f, uint32_t n, bool seed, bool with_spots, size_t* taken);
    bool WriteRefs(FILE* f, RefGroup group) const;
    size_t Load(const std::wstring& file, bool seed);
    void Write();

    mutable std::shared_mutex mtx_;
    std::unordered_map<uint32_t, texlook::Ref> refs_; // target -> its reference
    std::unordered_map<uint64_t, std::vector<ShapeSpots>>
        free_shapes_; // references not a power of two a side, per ShapeOf: their spot sets, each with its count
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
    std::wstring shipped_taken_; // texture_matches\looks.texlook taken; marked so after the next write
    std::atomic<uint32_t> decoded_{ 0 };
    std::atomic<uint32_t> skipped_{ 0 };
    std::atomic<uint32_t> matched_{ 0 };
    std::atomic<uint32_t> refs_new_{ 0 };
    std::atomic<uint32_t> places_new_{ 0 };
    std::atomic<uint32_t> by_place_{ 0 };
    std::mutex sized_mtx_;
    std::unordered_set<uint32_t> sized_; // look-alikes NoteSize has seen
    std::atomic<uint32_t> sized_same_{ 0 }; // look-alikes seen at their reference's size (debug builds)
    std::atomic<uint32_t> sized_other_{ 0 };
};
extern TexLook g_tex_look;

// The place keys: one per component draw's pass and slot, and per game character draw's.
uint64_t TexLookPlaceOf(const wchar_t* ns, int comp, uint64_t ps); // a component draw's pass: one key on every install
uint64_t TexLookMeshPassOf(uint32_t ib_hash, uint32_t first_index, uint32_t index_count, uint64_t ps);
uint64_t TexLookSlotOf(uint64_t pass, uint32_t slot);
