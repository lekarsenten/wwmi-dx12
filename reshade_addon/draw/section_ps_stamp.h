#pragma once
// A section's own `ps-tN = ResourceX` / `ps-tN = null` stamped into the injected draw's table.
#include <cstdint>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <vector>
#include <d3d12.h>
#include <reshade.hpp>
#include "../hooks/descriptor_shadow.h" // HeapKey
#include "../hooks/command_list_registry.h" // RootParam
#include "../mods/layout_registry.h" // RegBind

struct SectionPsSlot {
    unsigned reg = 0;
    uint8_t param = 0;
    uint32_t offset = 0;
    uint64_t heap = 0;
    uint32_t slot = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE src{ 0 };
    ID3D12Resource* res = nullptr;
    bool stored = false; // res is in the texture store: checked after the stamp
};
// The section's own ps-tN for one injected draw, each resolved to the slot of the game's table it lands in.
struct SectionPsStamps {
    std::vector<SectionPsSlot> slots;
    bool shared = false; // an earlier draw of this frame stamped one of these slots with another view
    int dm = -1;
    int comp = -1;
    void QueueForShadow() const; // the draw's next private-table bind writes them
};

// What was stamped where this frame (so a second draw on the same slot binds through a private table), and the
// largest the game's own texture has been at each (component, register) for the A20 mip distance.
class SectionPsStamper {
public:
    SectionPsStamps Plan(
        reshade::api::command_list* cmd_list, uint32_t gen0, const std::vector<uint32_t>& this_gen, int dm, int comp);
    void StampInPlace(const SectionPsStamps& stamps);

private:
    struct StampedSlot {
        uint64_t src = 0;
        uint32_t frame = 0;
    };
    static constexpr uint64_t kReadAsBound = 1; // StampedSlot::src: a draw read the slot unstamped
    struct PerSet;
    void ForThisFrame(); // caller holds stamped_mutex_
    unsigned GameMip(int dm, int comp, unsigned reg, const HeapKey& at);
    void PlanStamps(SectionPsStamps& out, reshade::api::device* dev, const RootParam* root, const RegBind* t_ps,
        uint64_t pso, uint64_t layh, uint32_t gen0, const std::vector<uint32_t>& this_gen);

    std::mutex stamped_mutex_;
    std::unordered_map<HeapKey, StampedSlot, HeapKeyHash> stamped_;
    uint32_t stamped_frame_ = 0;
    std::shared_mutex game_side_mutex_;
};
extern SectionPsStamper g_ps_stamps;
