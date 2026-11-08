#pragma once
// The textures a component section sees: the game's at the draw's ps-tN published to their engine for the
// section's `ps-tN` reads, the ones the list carried from its last character draw, and where a game character
// draw binds each texture (for the crawl and the look-alike seed).
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <d3d12.h>
#include <reshade.hpp>
#include "draw_context.h"
#include "../hooks/dxil_psv.h" // DxilSrvRegs
#include "../instrumentation/diag_gate.h"

class SectionTexturePublisher {
public:
    // Which textures a character draw read at ps-t0..t8, kept per list as DX11 keeps its slots.
    void CarryPsReads(const DrawArgs& da);
    // Where a game character draw binds each texture: recorded while crawling, linked from a seed.
    void NoteMeshPlaces(const DrawArgs& da);
    // The draw's textures published for the section; the registers pushed, as a mask.
    uint16_t Publish(reshade::api::command_list* cmd_list, int dm, int comp, uint32_t rtv_count);

private:
    struct CarryKey {
        int dm = 0;
        int comp = 0;
        uint64_t ps = 0;
        unsigned reg = 0;
        bool operator<(const CarryKey& o) const;
    };
    struct CarriedTexture {
        ID3D12Resource* res = nullptr;
        uint32_t hash = 0;
        unsigned fmt = 0;
        uint32_t frame = 0;
    };
    // What the list carry last published for one component's pass and register, referenced so it outlives the
    // game's release of it.
    class CarryMemo {
    public:
        void Remember(const CarryKey& k, ID3D12Resource* res, uint32_t hash, unsigned fmt);
        bool Recall(const CarryKey& k, CarriedTexture& out); // with a reference the caller releases

    private:
        struct PerSet;
        std::mutex mutex_;
    };
    static constexpr uint32_t kSectexGap = 30; // frames unseen before a texture's next sighting is logged again
    uint16_t PublishCarried(reshade::api::command_list* cmd_list, int dm, int comp, const DxilSrvRegs& ps_regs,
        uint16_t pushed, uint64_t ps_hash);

    std::mutex sectex_mutex_;
    std::unordered_map<uint64_t, uint32_t> sectex_last_; // hash | unread << 32 -> last frame a section saw it
    CarryMemo carry_memo_;
};
extern SectionTexturePublisher g_section_textures;

// Debug pipe `texdraws`: the textures draws read at ps-t0..t8, indexed or not, watched by texture hash or all of them
// for about two frames.
class TexDrawWatch {
public:
    WWMI_DEBUG_FN void Note(const DrawArgs& da) WWMI_DEBUG_BODY();
    WWMI_DEBUG_FN void NoteNonIndexed(reshade::api::command_list* cmd_list, uint32_t vertex_count) WWMI_DEBUG_BODY();
    // texdraws <hash> | all: starts watching; off: stops; nothing: what was found.
    WWMI_DEBUG_FN std::string Command(const std::vector<std::string>& args) WWMI_DEBUG_BODY(return {};);

private:
    struct Key {
        uint64_t ps = 0;
        uint64_t vs = 0;
        unsigned reg = 0;
        uint32_t hash = 0;
        bool indexed = true;
        bool operator<(const Key& o) const;
    };
    struct Seen {
        uint32_t draws = 0;
        uint32_t count = 0;
        uint32_t width = 0;
        uint32_t height = 0;
        uint32_t format = 0;
    };
    void Record(reshade::api::command_list* cmd_list, uint64_t vs_hash, uint32_t count, bool indexed);
    std::mutex mutex_;
    std::atomic<bool> on_{ false };
    uint32_t hash_ = 0; // 0 = every texture
    uint32_t last_frame_ = 0; // every texture: recorded through this frame
    std::map<Key, Seen> seen_;
};
WWMI_DEBUG_VAR TexDrawWatch g_tex_draw_watch;
