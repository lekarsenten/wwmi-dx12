#pragma once
#include "tex_replace.h"
#include <chrono>
#include <string>
#include <unordered_map>
#include <unordered_set>

// Every loaded mod texture at the size its mod's budget (A19) allows: the largest textures of a mod over it go down a
// level at a time, from the mips in their file or re-encoded on the graphics card; a raised budget brings them back.
// From Auto up, the collector (tex_gc) decides below that ceiling, and which textures are let go.
class TexBudget : public TexReplace::Client {
public:
    void Tick(); // present thread, after the replacer's
    bool Owns(const std::wstring& fq_name) const override;
    void Swapped(const std::wstring& fq_name, ID3D12Resource* now) override;
    void Kept(const std::wstring& fq_name) override;
    void Held(const std::wstring& fq_name) override;
    void Ended() override;

private:
    struct Stored { // as the mod's file holds it
        UINT64 width = 0;
        UINT height = 0;
        UINT16 mips = 0;
    };
    struct Lowered {
        Stored stored;
        unsigned level; // levels below the stored size
        ID3D12Resource* now; // what the [Resource] got; anything else there is at its stored size
    };
    bool Due(std::chrono::steady_clock::time_point now);
    void Fit();

    std::unordered_map<std::wstring, Lowered> lowered_; // [Resource]s holding a copy below their stored size
    std::unordered_map<std::wstring, Lowered> planned_; // what the pass under way asked for; level 0 = stored size
    std::unordered_map<std::wstring, Lowered>
        returning_; // this ini load's: let go of below their stored size, to load back at it
    std::unordered_map<std::wstring, std::wstring> mods_of_ns_; // this ini load's: namespace (lower case) -> mod
    std::unordered_set<std::wstring> kept_; // this ini load's: re-encodes refused, left at their size
    unsigned seen_gen_ = 0;
    int step_ = -1; // A19 as last seen
    unsigned loads_seen_ = 0;
    bool wanted_ = false;
    std::chrono::steady_clock::time_point step_moved_{};
    std::chrono::steady_clock::time_point loads_moved_{};
    std::chrono::steady_clock::time_point last_fit_{};
};
extern TexBudget g_tex_budget;
