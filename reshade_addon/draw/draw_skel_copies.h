#pragma once
// Two copies of one character on screen draw with two skeletons (B20): which copy a component draw belongs to,
// told apart by its per-object CB and, when that address is new, by its bones.
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>
#include <d3d12.h>
#include <reshade.hpp>

constexpr unsigned kSkelMatchBones = 32;
struct SkelBones {
    unsigned n = 0;
    float t[kSkelMatchBones * 3] = {};
};

class SkeletonCopyTracker {
public:
    // The skeleton slot this component draw merges into; first_sight: the mod's first draw since the load.
    unsigned CopySlot(reshade::api::command_list* cl, int dm, int comp, ID3D12Resource* b4_res, uint64_t b4_va,
        bool shadow, bool* first_sight);
    // The component's bone CB at its colour pass this frame; what its shadow draw binds in place of the inherited one.
    void NoteComponentBone(int dm, int comp, uint64_t va);
    uint64_t ComponentBone(int dm, int comp); // noted within the last two frames, else 0

private:
    struct CompBoneCb {
        uint64_t va = 0;
        uint32_t frame = 0;
    };
    struct SkelCopyCb {
        uint64_t va = 0; // the component's bone CB this frame
        SkelBones bones; // read at its draw: by the next frame the game may have released the CB
    };
    struct SkelCopySeen {
        uint64_t key = 0;
        unsigned slot = 0;
        std::vector<SkelCopyCb> cb4;
        std::vector<uint64_t> keys;
        std::vector<uint64_t> cb4_drawn;
    };
    struct SkelCopyMod {
        uint32_t frame = 0;
        bool noted = false;
        bool off = false;
        unsigned oversplit = 0;
        std::vector<SkelCopySeen> cur;
        std::vector<unsigned> prev_slots;
        std::vector<std::vector<SkelBones>> prev_bones;
        std::vector<std::pair<uint64_t, unsigned>> prev_keys; // last frame's vs-cb2 -> skeleton, joined ones included
    };
    static constexpr unsigned kOversplitFrames = 3;
    static void Roll(SkelCopyMod& m, int dm);
    struct PerSet;

    std::mutex comp_bone_mutex_;
    std::mutex mutex_; // PerSet::mods
};
extern SkeletonCopyTracker g_skel_copies;
