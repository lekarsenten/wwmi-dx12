#pragma once
#include <d3d12.h>
#include <cstdint>
#include <unordered_map>
#include <vector>

struct MergedComp {
    int mod_id = 0; // which geometry mod (ModRegistry index) owns this component
    int comp_idx = -1;
    uint32_t first_index = 0; // game match_first_index
    uint32_t match_count = 0; // game match_index_count -- disambiguates (first_index alone collides)
    int remap_size = 0; // # of local bones in this component's RemappedSkeleton
    bool hide = false;
    bool vanilla = false;
    bool freeze = false;
    std::vector<int> with; // other mods' components on this range of the same mesh: their sections run on its draw too
    std::vector<int> run_order; // this slot and `with`, as DX11 runs them: match_priority, then section name
};
inline uint64_t FirstCountKey(uint32_t first_index, uint32_t match_count)
{
    return ((uint64_t)first_index << 32) | match_count;
}

// Every component of every loaded mod, in one table the draw hooks index by the game's (first index, index count)
// range, and what is shared across them: the identity skeleton a frozen component binds.
class ComponentTable {
public:
    size_t Count() const { return comps_.size(); }
    MergedComp& At(size_t slot) { return comps_[slot]; }
    const MergedComp& At(size_t slot) const { return comps_[slot]; }
    std::vector<MergedComp>& All() { return comps_; }
    int IndexOf(const MergedComp* mc) const { return (int)(mc - comps_.data()); }
    // The slot declared on this range; -1 for none. Of several, the last declared (SharedSlots lists them all).
    int SlotOf(uint32_t first_index, uint32_t index_count) const;
    const std::vector<int>* SharedSlots(uint32_t first_index, uint32_t index_count) const;
    int SlotOfIbHash(uint32_t ib_hash) const;
    bool Loaded() const { return loaded_; }
    uint64_t IdentityCbVa() const { return identity_cb_va_; }
    // The lookups rebuilt from the table: after the loader filled it.
    void Index();
    void Finalize();

private:
    std::vector<MergedComp> comps_;
    std::unordered_map<uint64_t, int> by_range_;
    std::unordered_map<uint64_t, std::vector<int>> shared_by_range_;
    std::unordered_map<uint32_t, int> by_ib_hash_;
    bool loaded_ = false;
    uint64_t identity_cb_va_ = 0;
};
