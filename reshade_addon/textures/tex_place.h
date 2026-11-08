#pragma once
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

// Mod textures into the game's descriptor heaps at placement time (PIPELINE 1): the swap, the placements it could
// not serve yet and their retry, the selections remembered from a mod's draw-time state, and the hand-back of a
// mod that switched itself off.
class TexPlacement {
public:
    // At descriptor placement: if res's hash matches a rule, overwrite the heap slot with the mod SRV. True = swapped.
    bool Swap(ID3D12Device* dev, uint64_t heap_handle, uint32_t slot, ID3D12Resource* res);
    const char* Decline(); // why the last Swap on this thread declined
    void Report();
    void SnapshotSelections(const wchar_t* ns);
    void SnapshotsMove(const std::unordered_map<int, int>& index_to); // store indices of replaced textures
    void RetryMissed(const char* why, bool quiet_if_none = false);
    void RestampOnVarChange(); // Present, between their [Present] passes
    void RetryMissedBetweenPasses(const char* why); // at the next frame's [Present] passes
    void RunRetryBetweenPasses(); // Present, between their [Present] passes
    void ApplyQueued(); // new mod textures described: a pick made while its texture loaded is stamped now
    // Holds a resource for the next RetryMissed: one of its slots was given back to the game.
    static void HoldForRetry(uint64_t res);
    void ForgetMissed(uint64_t res); // destroyed
    // A mod switched itself off: its stamped slots in CPU-only heaps get the game's descriptor back and are held.
    void UnstampSwitchedOff(const wchar_t* ns);
    void SwapsSwitched(bool were_live);
    void LookAlikesSwitched(bool on); // A13 changed: slots of look-alike textures swapped or given back
    void WatchSwitchedOff(const wchar_t* ns);
    void WatchSwitchedOn();
    bool OffWatching() const;
    uint32_t OffRuleOf(ID3D12Resource* res); // the watched mod's rule this game texture swaps under, else 0

private:
    struct PlaceStat {
        uint32_t placed = 0;
        uint32_t missed = 0;
        std::set<std::pair<uint64_t, uint32_t>> slots;
    };
    struct SelSnap {
        int sel;
        uint32_t frame;
    };
    struct OffWatch { // a switched-off mod's rules, every draw checked for a slot still stamped under one
        std::mutex mutex;
        std::unordered_set<uint32_t> rules;
        std::atomic<bool> watching{ false };
    };
    struct QueuedPick { // its section picked a texture still loading
        const void* custom;
        unsigned gen;
        uint32_t rule;
        uint64_t seq;
    };
    void NotePlacement(uint32_t rule, uint64_t heap, uint32_t slot, bool ok);
    int SnapshotSelection(uint32_t rule);

    std::atomic<const char*> retry_why_{ nullptr };
    std::mutex missed_mutex_;
    std::unordered_set<uint64_t> missed_; // placements with a rule but an empty selection; a destroyed resource leaves
    std::unordered_map<uint64_t, QueuedPick> queued_; // under missed_mutex_
    uint64_t queued_seq_ = 0; // under missed_mutex_
    std::atomic<size_t> queued_n_{ 0 };
    std::mutex place_mutex_;
    std::unordered_map<uint32_t, PlaceStat> place_stats_;
    std::shared_mutex sel_snap_mutex_;
    std::unordered_map<uint32_t, SelSnap> sel_snap_;
    OffWatch off_;
};
extern TexPlacement g_tex_place;
