#pragma once
#include <d3d12.h>
#include <reshade.hpp>
#include <atomic>
#include <cstdint>
#include <shared_mutex>
#include <unordered_map>

constexpr uint32_t kBoneStride = 48; // float3x4
constexpr uint32_t kMaxBones = 256;
constexpr uint32_t kBoneCBBytes = kMaxBones * kBoneStride; // 12288

// A float3x4 whose rotation rows are unit-ish and orthogonal: a bone, not arbitrary constants.
bool LooksLikeBone(const float* m);

// The game's constant buffers read back on the CPU: each UPLOAD-heap CB the game binds is mapped once and kept
// mapped, so a draw's skeleton can be copied out at the address the root CBV names.
class GameCbReader {
public:
    uint32_t Snapshot(ID3D12Resource* res, uint64_t va, uint8_t* dst, uint32_t max_bytes, bool map_on_demand = true);
    bool ReadRootCbv(reshade::api::command_list* cmd_list, uint32_t param, void* dst, uint32_t n);
    // Diagnostic: bitmask of root params (0..15) whose bound CBV first matrix LooksLikeBone.
    uint32_t BoneRootMask(reshade::api::command_list* cmd_list);
    void Forget(ID3D12Resource* res);

private:
    struct Mapping {
        uint8_t* mapped = nullptr; // nullptr = looked at and not mappable (not an UPLOAD heap, or Map failed)
        uint64_t base = 0;
        uint64_t width = 0;
    };
    uint8_t* At(ID3D12Resource* res, uint64_t va, bool map_on_demand, uint64_t* bytes_left);

    std::unordered_map<ID3D12Resource*, Mapping> maps_;
    std::shared_mutex mutex_;
    std::atomic<uint64_t> forgotten_{ 0 }; // debug: entries erased by Forget
};
extern GameCbReader g_game_cbs;
