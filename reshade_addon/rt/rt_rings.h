#pragma once
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

// RTDBG: the vb0 range a claimed draw of (mod, comp) last read.
struct RtDrawVb {
    uint32_t frame = 0;
    uint64_t va = 0;
    uint32_t size = 0;
    uint32_t stride = 0;
    uint64_t vs = 0;
};
class RtDrawVbs {
public:
    void Note(int mod_id, int comp_idx, const RtDrawVb& v);
    bool Of(int mod_id, int comp_idx, RtDrawVb* out) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, RtDrawVb> by_comp_;
};
extern RtDrawVbs g_rt_draw_vbs;

// The skin-cache output rings mod characters' BLAS are built from. `mod`: ModRegistry index, -1 unknown.
class RtSkinRings {
public:
    void Note(uint64_t va, uint64_t bytes, int mod = -1);
    bool Overlaps(uint64_t va, uint64_t bytes) const; // does [va, va+bytes) overlap a recorded ring?
    // The mod whose ring is EXACTLY a view's window [va, va+bytes), or -1; `overlap_mod` gets one it merely overlaps.
    int ModOf(uint64_t va, uint64_t bytes, int* overlap_mod = nullptr) const;
    size_t Count() const;
    // A destroyed buffer's rings go with it.
    void ForgetRange(uint64_t va, uint64_t bytes);
    // Bumped whenever Note records a ring it did not have: a shader judged before then is judged again.
    uint32_t Gen() const;

private:
    struct Ring {
        uint64_t va;
        uint64_t bytes;
        uint64_t ns_hash;
    };
    mutable std::mutex mutex_;
    std::vector<Ring> rings_;
    uint32_t gen_ = 0;
};
extern RtSkinRings g_rt_rings;
