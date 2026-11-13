#pragma once
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../instrumentation/diag_gate.h"

// A21 and the debug pipe's `texrefs`: the textures game character draws read over two frames, read back from the
// graphics card and kept as texture-match references, so a character no mod here targets reaches the seed too.
class TexRefTake {
public:
    WWMI_DEBUG_FN void Arm() WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN bool Collecting() const WWMI_DEBUG_BODY(return false;);
    WWMI_DEBUG_FN void Note(ID3D12Resource* res) WWMI_DEBUG_BODY(); // a texture a game character draw reads
    WWMI_DEBUG_FN void Tick() WWMI_DEBUG_BODY(); // the frame step: once the frames are collected, the take
    WWMI_DEBUG_FN std::string Last() const WWMI_DEBUG_BODY(return {};);
    WWMI_DEBUG_FN std::string Command(const std::vector<std::string>& args) WWMI_DEBUG_BODY(return {};);

private:
    void Take(const std::unordered_map<ID3D12Resource*, uint32_t>& seen);

    std::atomic<bool> collecting_{ false };
    mutable std::mutex mutex_;
    uint32_t until_ = 0; // the last frame collected
    std::unordered_map<ID3D12Resource*, uint32_t> seen_; // texture -> its hash when noted, each held by a reference
    std::string last_; // the last take's summary
};
WWMI_DEBUG_VAR TexRefTake g_tex_ref_take;
