#pragma once
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

// The crawl: with it on, every texture's mips are proven and every game character draw's slots recorded, and the
// proofs written out as a seed once the game has been quiet a while.
class TexCrawl {
public:
    bool On() const { return on_.load(std::memory_order_relaxed); }
    void Set(bool on);
    void Note(uint64_t place, uint32_t hash); // a texture a game character draw binds at that mesh place
    void FlushIfQuiet(unsigned quiet_ms); // saved once no new texture was seen for quiet_ms, or a minute on

private:
    void Save();

    std::atomic<bool> on_{ false };
    std::mutex mutex_;
    std::unordered_map<uint64_t, std::vector<uint32_t>> seen_; // mesh place -> every texture drawn there
    bool dirty_ = false; // a texture seen since the last save -- under mutex_
    uint64_t new_at_ = 0; // GetTickCount64 of the latest one
    uint64_t saved_at_ = 0; // GetTickCount64 of the last save
    std::wstring file_; // this session's proofs file, named at its first save
};
extern TexCrawl g_tex_crawl;
