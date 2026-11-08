#pragma once
#include <cstdint>
#include <mutex>
#include <set>
#include <unordered_set>

struct StalkerResult {
    uint64_t cs_hash;
    uint32_t root;
    uint32_t slot;
    bool operator<(const StalkerResult& o) const
    {
        if (cs_hash != o.cs_hash) return cs_hash < o.cs_hash;
        if (root != o.root) return root < o.root;
        return slot < o.slot;
    }
};

// The UAV stalker: the vb0s the character vertex shaders draw from this frame, and the compute tables whose
// entries sit near one of them (the overlay's discovery list).
class UavStalker {
public:
    void NoteCharVb(uint64_t va);
    void ForgetCharVbs();
    // A compute table entry at (root, slot) of this dispatch resolves to res_va.
    void Probe(uint64_t cs_hash, uint32_t root, uint32_t slot, uint64_t res_va);
    template <class F> void ForEachResult(F&& f)
    {
        std::lock_guard lock(results_mutex_);
        for (const StalkerResult& r : results_) f(r);
    }
    void Clear();

private:
    std::mutex char_vb_mutex_;
    std::unordered_set<uint64_t> char_vb_vas_;
    std::mutex results_mutex_;
    std::set<StalkerResult> results_;
};
extern UavStalker g_uav_stalker;
