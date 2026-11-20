#pragma once
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "../instrumentation/diag_gate.h"

// A21 and the debug pipe's `texrefs`: the textures game character draws read, and every drawn texture a section
// names, over two frames, read back from the graphics card and kept as texture-match references for the seed. With
// the gallery crawl on (A15), each such texture without a reference is taken at its first sight, no button.
class TexRefTake {
public:
    WWMI_DEBUG_FN void Arm() WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN bool Watching() const WWMI_DEBUG_BODY(return false;); // a take collecting, or the crawl on
    WWMI_DEBUG_FN void Note(ID3D12Resource* res) WWMI_DEBUG_BODY(); // a texture a game character draw reads
    WWMI_DEBUG_FN void NoteNamed(ID3D12Resource* res) WWMI_DEBUG_BODY(); // a texture a draw reads, if a section names it
    WWMI_DEBUG_FN void Tick() WWMI_DEBUG_BODY(); // the frame step: once the frames are collected, the take
    WWMI_DEBUG_FN std::string Last() const WWMI_DEBUG_BODY(return {};);
    WWMI_DEBUG_FN std::string Command(const std::vector<std::string>& args) WWMI_DEBUG_BODY(return {};);
    // The debug pipe's `texsave`: the loaded texture with that hash, its mip 0 read back and written as a .dds.
    WWMI_DEBUG_FN std::string Save(uint32_t hash) WWMI_DEBUG_BODY(return {};);

private:
    using Seen = std::unordered_map<ID3D12Resource*, uint32_t>; // texture -> its hash when noted, held by a reference
    static bool FirstSight(uint32_t hash, bool named);
    void Keep(Seen& take, Seen& crawl, ID3D12Resource* res, uint32_t hash);
    void Take(const Seen& characters, const Seen& named, bool crawl);

    std::atomic<bool> collecting_{ false };
    mutable std::mutex mutex_;
    uint32_t until_ = 0; // the last frame collected
    Seen seen_;
    Seen named_;
    Seen crawl_seen_; // the crawl's, taken at the next frame step
    Seen crawl_named_;
    std::unordered_set<uint32_t> attempted_; // every hash the crawl has queued this session
    std::string last_; // the last take's summary
    std::string detail_; // the last take's named textures that are too flat or of a size signatures skip
};
WWMI_DEBUG_VAR TexRefTake g_tex_ref_take;
