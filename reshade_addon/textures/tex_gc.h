#pragma once
#include <d3d12.h>
#include <cstdint>
#include <deque>
#include <string>
#include <unordered_map>
#include <vector>

// Auto (A19): mod textures kept within what the game leaves free. Nothing changes while they fit. Short of room for
// a texture on screen that is below its size, what has gone unseen a while in generation 0 or 1 goes a level down,
// and out once it can go no lower; generation 2 unseen for a minute is generation 1 again. Short of memory, then
// the rest off screen, then, once short for 3 s, what is on screen. What is seen again comes back up. With A20,
// what the game has drawn smaller through 5 s of being drawn goes down as many levels, and back up when the game
// draws it nearer. Decides levels only; tex_budget turns them into orders.
class TexGc {
public:
    static constexpr unsigned kLevels = 5; // stored size and up to 4 levels down
    struct Item { // one loaded mod texture, as the planner sees it
        const std::wstring* fq_name;
        const std::wstring* mod; // as TEXFIT names it
        ID3D12Resource* dx12;
        unsigned now; // levels below its stored size
        unsigned ceiling; // the per-mod budget's level for it; 0 = stored size
        unsigned most; // the furthest down it may go
        bool fixed; // never lowered
        uint64_t bytes[kLevels];
        unsigned want = 0; // out: the level to end at
        bool evict = false; // out
    };
    struct Reading {
        bool valid = false;
        uint64_t budget = 0; // Windows' budget for the process
        uint64_t used = 0; // the process's usage
        uint64_t mods = 0; // mod textures
        uint64_t game = 0; // used - mods, its highest over the last 10 s
        uint64_t game_now = 0; // used - mods, the last sample
        uint64_t available = 0; // for mod textures, by game_now; more than this is short
        uint64_t room = 0; // available less a band (2.5% of the budget): short ends below it
        uint64_t raise_room =
            0; // the room by game: what raises may fill, so a raise is not short at the game's next peak
    };
    void Tick(); // present thread, every frame
    void Stop();
    bool Due() const; // a collection would act now
    void Decide(std::vector<Item>& items);
    void Swapped(const std::wstring& fq_name, ID3D12Resource* now); // nullptr: evicted
    void Held(const std::wstring& fq_name); // never let go: freeing it frees nothing
    bool Tight() const { return tight_; }
    const Reading& Now() const { return reading_; }

private:
    struct Tracked {
        unsigned gen = 0; // collections it was drawn before, up to 2: how long it may go unseen before it gives way
        unsigned level = 0; // levels below the stored size the collector asks for
        unsigned ceiling = 0; // the ceiling's level at the last collection
        unsigned most = 0; // the furthest down it may go, at the last collection; 0 when fixed
        ID3D12Resource* res = nullptr;
        uint64_t last_ms = 0; // last draw seen; first tracked counts as one
        bool returned = false; // let go and asked for again
        uint64_t back_ms = 0; // ... when: with no draw seen since, it is in use unstamped and not let go again
        bool told = false;
        bool held = false; // another [Resource] holds it by `= ref`: never let go
        uint64_t run_ms = 0; // when its current unbroken run of draws began; 0 = not drawn now
        std::wstring mod; // as TEXFIT names it
    };
    struct GameSample {
        uint64_t ms;
        uint64_t game;
    };
    static void OnUse(ID3D12Resource* res); // drawing threads
    static unsigned Far(const Tracked& t, int index, uint64_t now); // A20: the level to go down to
    static unsigned Floor(const Tracked& t, int index, uint64_t now); // the lowest level a raise ends at
    void Sample(uint64_t now, bool loading);
    bool Watched(const Tracked& t, uint64_t now) const;

    bool observing_ = false;
    bool mips_ = false; // A20's draw stamps are on
    Reading reading_;
    std::deque<GameSample> game_; // the last 10 s
    std::unordered_map<std::wstring, Tracked> tracked_;
    std::unordered_map<std::wstring, unsigned> let_go_; // this ini load's: textures it let go of, at their level then
    unsigned seen_gen_ = 0;
    uint64_t next_sample_ms_ = 0;
    uint64_t loading_ms_ = 0; // last time a mod texture was loading (tex_defer)
    uint64_t collect_ms_ = 0; // last collection
    uint64_t sweep_ms_ = 0; // last collection not asked for by A20 alone: generations move at these
    unsigned decided_loads_ = ~0u; // tex_defer's loads at the last collection
    uint64_t quiet_ms_ = 0; // last collection that could not act on tight or a raise
    uint64_t raise_quiet_ms_ = 0; // last collection that lowered or let go while tight
    uint64_t tight_ms_ = 0; // when tight began
    bool tight_ = false;
    bool raise_ready_ = false;
    bool far_due_ = false; // A20: something to go down, or textures it has not tracked yet
    uint64_t census_ms_ = 0; // A20: last count of the mips the game draws its own at
    std::wstring census_; // ... as last logged
};
extern TexGc g_tex_gc;
