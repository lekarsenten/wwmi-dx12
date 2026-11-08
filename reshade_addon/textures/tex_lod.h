#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

// Lower->target links: proven from the game's own data, or look-alikes from tex_look. One line per target in our
// cache file, each low under it with its flags (mip, exact, chain, ssim), and one line of degenerate lows. The
// table (tex_lod_table.cpp) serves the lock-free lookups; the file (tex_lod_file.cpp) is read at load and written
// when quiet; learning (tex_lod_learn.cpp) is what the uploads and copies teach.
class TexLodCache {
public:
    struct Link {
        uint32_t high = 0;
        uint32_t mip = 0;
        bool exact = false;
        bool chain = false;
        bool place = false; // a look-alike by where it is drawn (tex_look's places), its ssim 1
        float ssim = 0; // a look-alike's: 0 is one from before tex_look, dropped at load
    };
    struct Rec {
        std::vector<Link> links;
    };
    using Records = std::unordered_map<uint32_t, Rec>;

    void Init(const std::wstring& cache_dir = std::wstring());
    void TargetsReady(bool (*is_target)(uint32_t hash, void* user), void* user);
    void OnExit();
    void FlushIfQuiet(unsigned quiet_ms);
    void SetResemblance(bool on);
    void SetCache(bool on);

    uint32_t Lookup(uint32_t hash) const;
    uint32_t Known(uint32_t hash) const;
    unsigned MipOf(
        uint32_t hash) const; // a proven low: which mip of its target it is; 0 = not a proven low, or not known
    bool Linked(uint32_t hash) const; // any link, exact or a look-alike
    bool Degenerate(uint32_t hash) const;
    bool LearnVisual(uint32_t low, uint32_t high, float ssim); // a tex_look match: a look-alike link
    bool LearnPlace(uint32_t low, uint32_t high); // drawn where HD draws `high`: a look-alike link
    bool Export(const std::wstring& path, bool (*keep)(uint32_t high, void* user), void* user); // proofs, as a seed

    void Feeds(uint32_t lower, uint32_t big, uint32_t big_rule, uint32_t steps); // (`small` is a windows.h macro)
    uint32_t AddMipExact(uint32_t low, uint32_t high, unsigned mip, bool* now_degenerate);
    void Refute(uint32_t low, uint32_t high, const char* why);

private:
    struct Entry;
    struct Slot;
    struct EntryTab;
    struct Feeder {
        uint32_t low = 0;
        uint32_t steps = 0;
    };
    // the table
    static EntryTab* TabNew(uint32_t bits);
    const Entry* EntryOf(uint32_t low) const;
    void TabGrow();
    void TabSet(uint32_t low, const Entry* e);
    static const Link* ClearBest(const Rec& r);
    void Publish(uint32_t low);
    void PublishDegenerate();
    static uint32_t ExactHigh(const Entry* e);
    // the records
    bool ExactRecord(uint32_t low) const;
    uint32_t SingleHigh(uint32_t low) const;
    uint32_t OneTexture(uint32_t a, uint32_t b) const;
    void MarkDirty();
    // the file
    std::wstring CachePath() const;
    bool WriteRecords(const std::wstring& path, bool (*keep)(const Link& k, void* user), void* user);
    void RewriteCache();
    void ImportSeed(const std::wstring& path, const wchar_t* name);
    void LoadAll();
    // learning
    void Proven(uint32_t low);
    bool LearnExact(uint32_t low, uint32_t high, uint32_t mip, bool chain, bool identity, const char* how);
    bool LearnLookalike(uint32_t low, uint32_t high, uint32_t mip, float ssim, bool chain, bool place);
    void ChainAlias(uint32_t id, uint32_t rule, int depth, bool look, uint32_t mip, float ssim);

    std::mutex mutex_; // WRITERS only (records, table writes, dirs); see tab_
    std::atomic<EntryTab*> tab_{ TabNew(17) };
    uint32_t tab_used_ = 0; // slots taken, tombstones included -- under mutex_
    std::wstring cache_dir_; // OURS, outside the mod tree -- the only place we read or write
    std::vector<std::pair<std::wstring, std::wstring>>
        imported_; // seed taken -> its DISABLED_ name, after the next write
    Records records_; // low -> every target it belongs to
    std::unordered_map<uint32_t, uint32_t> refuted_;
    std::set<uint32_t> degenerate_; // identical to the mips of two targets or more: never swapped
    std::atomic<const std::vector<uint32_t>*> degen_snap_{
        nullptr
    }; // degenerate_ for lock-free readers; old ones kept
    bool dirty_ = false; // learned since the file was last written -- under mutex_
    uint64_t dirty_at_ = 0; // GetTickCount64 of the latest change
    std::unordered_map<uint32_t, std::vector<Feeder>> feeders_;
};
extern TexLodCache g_tex_lod;
