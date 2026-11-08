#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>
#include <d3d12.h>
#include <reshade.hpp>
#include "../hooks/command_list_registry.h" // CmdState

// RTSKINREAD (g_debug): once a session, 48-byte windows of every buffer her ring-writing dispatch binds, copied
// through the tracked state and logged four frames later.
class RtSkinRead {
public:
    void Drain();
    void Record(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash, uint32_t groups_x);

private:
    struct Slot {
        char label[48];
    };
    ID3D12Resource* rb_ = nullptr;
    std::vector<Slot> slots_;
    uint32_t ready_ = 0;
    bool done_ = false;
    std::atomic<bool> pending_{ false }; // lets every other dispatch skip the drain without a lock
    std::mutex mutex_;
};
extern RtSkinRead g_rt_skin_read;

// RTBONES (g_debug): for each dispatch writing her ring over two frames, its r3 constants, bone view and first bones.
class RtBonesLog {
public:
    // While RTBONES logs, every dispatch is checked for her ring, not only the learned skin shader.
    bool WindowOpen() const;
    void Log(
        reshade::api::command_list* cmd_list, const CmdState& state, uint32_t groups_x, uint64_t cs_hash, bool learned);

private:
    std::mutex mutex_;
    uint32_t f0_ = 0;
    uint32_t frame_ = 0;
    uint32_t seq_ = 0;
    uint32_t lines_ = 0;
    std::atomic<uint32_t> open_until_{ 0 }; // frame the window closes; 0 = not opened yet
};
extern RtBonesLog g_rt_bones;
