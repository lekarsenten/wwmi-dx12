#pragma once
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <d3d12.h>
#include <wrl.h>
#include "descriptor_shadow.h"
#include "../core/sharded_map.h"

struct UnstampTally {
    unsigned recorded = 0;
    unsigned cpu_only = 0;
    size_t cpu_only_stamped = 0;
};
// A mod texture replaced by another: what the slots stamped with it get instead.
struct StampMove {
    ID3D12Resource* to;
    D3D12_CPU_DESCRIPTOR_HANDLE to_srv;
    D3D12_CPU_DESCRIPTOR_HANDLE from_srv;
};
struct StampTally {
    unsigned moved_cpu_only = 0;
    unsigned moved_visible = 0;
    unsigned restored = 0; // the game's descriptor back
    unsigned nulled = 0; // a null SRV: nothing of the game's to put back
    unsigned raced = 0; // the game wrote the slot between the look and the write
    unsigned backups = 0; // unstamp backups now naming the new SRV
};

// Which heap slot each game texture was last placed in, and the game's own descriptor kept from before a stamp
// went over it: everything needed to take a mod texture out of a slot again.
class StampLedger {
public:
    // The slot `res` was placed in (a stamp may follow; a re-stamp targets it).
    void NotePlacement(uint64_t res, const HeapKey& slot);
    void ForgetResource(uint64_t res);
    void BackupBeforeStamp(uint64_t heap_h, uint32_t offset, uint64_t mod_srv);
    // Put the game's own descriptor back in resH's recorded slot. True = restored. log=false for bulk callers.
    bool Unstamp(uint64_t resH, bool log = true);
    // The same for one heap slot, whatever resource it holds. True = restored.
    bool UnstampSlot(uint64_t heap_h, uint32_t offset, bool log = false);
    // Re-run the placement swap on the slot resH was last placed in. True = the slot now holds a mod SRV.
    bool Restamp(uint64_t resH);
    std::vector<std::pair<HeapKey, uint64_t>> StampedCpuOnlySlots();
    UnstampTally UnstampAll(void (*hold)(uint64_t res));
    // Every slot stamped with a key of `moves` holds its `to` instead; the unstamp backups follow.
    StampTally Move(const std::unordered_map<ID3D12Resource*, StampMove>& moves);
    // Mod textures whose last reference dropped, before the GPU fence frees them: no slot may keep one. Called
    // after they left the texture store, so a stamp after the walk sees that (StampedStoreTexture).
    StampTally UnstampDying(const std::unordered_set<ID3D12Resource*>& dying);
    // Drawing threads, right after stamping a texture taken from the store into a slot: if the store let go of it
    // meanwhile, the slot is given back as UnstampDying would have.
    void StampedStoreTexture(uint64_t heap, uint32_t offset, ID3D12Resource* res);
    // A null view of a DXIL resource kind (0 = 2D), in a CPU-only heap of its own.
    D3D12_CPU_DESCRIPTOR_HANDLE NullSrv(ID3D12Device* dev, uint8_t kind);
    // Every placement forgotten: the add-on was bypassed and they went stale.
    void Clear();

private:
    struct Backup {
        uint32_t idx;
        uint64_t mod_srv;
    };
    static constexpr uint32_t kBackupBlock = 16384; // one CPU-only heap per block; blocks are added as needed
    static uint32_t DescInc();
    static void BackupsOfRetiredHeap(uint64_t heap);
    D3D12_CPU_DESCRIPTOR_HANDLE BackupSlot(uint32_t idx); // under backup_mutex_
    bool RestoreFromBackup(const HeapKey& key, D3D12_CPU_DESCRIPTOR_HANDLE dst);
    bool GiveBack(const HeapKey& key, uint64_t lo, FlatHeapMap::Slot* slot, ID3D12Resource* res, StampTally& t);

    ShardedMap<uint64_t, HeapKey> slot_of_; // texture resource -> the slot it was last placed in (re-stamp)
    std::mutex backup_mutex_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> backup_heaps_;
    std::unordered_map<HeapKey, Backup, HeapKeyHash> backup_of_;
    std::vector<uint32_t> backup_free_;
    uint32_t backup_next_ = 0;
    std::mutex null_mutex_;
    ID3D12DescriptorHeap* null_heap_ = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE null_srv_[8] = {}; // by view dimension, NullSrv's slot order
    UINT null_inc_ = 0;
};
extern StampLedger g_stamps;

// A1: every stamped slot given back and the hooks bypassed, or the records cleared and the hooks resumed.
void SetAddonDisabled(bool dis);
