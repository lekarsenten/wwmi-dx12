#pragma once
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

// Mip-exact identities: the resident the game streams in at distance is a resource whose mip0 IS the asset's
// mip K, with N-K mips, so its identity hash is exactly computable from the full texture's mip K as it uploads.
// Recorded for every mip K of a mod target whose standalone size still clears the hashing floor; mips that arrive
// before mip0 wait until mip0's identity says whether the texture is a target at all.
class TexMipIdentity {
public:
    void FromUpload(ID3D12Resource* dest, uint32_t sub, const uint8_t* src);
    void Forget(uint64_t res); // on destroy: whatever was held for it
    void Record(ID3D12Resource* dest, uint32_t sub, uint32_t id, uint32_t rule);
    std::vector<std::pair<uint32_t, uint32_t>> TakeHeld(uint64_t dest); // (mip, identity) that waited for mip0
    // A stream-copy edge whose bigger side has no hash yet: parked until it does.
    void ParkFeed(uint64_t bigger, uint32_t small_id, uint32_t steps);
    std::vector<std::pair<uint32_t, uint32_t>> TakeFed(uint64_t dest); // (small identity, mips between)

private:
    std::mutex mip_mutex_;
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> mip_pending_; // dest -> (mip, identity)
    std::mutex feed_mutex_;
    std::unordered_map<uint64_t, std::vector<std::pair<uint32_t, uint32_t>>> feed_pending_; // bigger resource -> ...
};
extern TexMipIdentity g_tex_mip_identity;
