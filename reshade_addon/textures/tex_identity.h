#pragma once
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <set>
#include <shared_mutex>
#include <unordered_map>
#include <unordered_set>

inline constexpr uint64_t kTexHashFloorPx = 64ull * 64ull;

inline uint32_t TexTypelessBC(uint32_t f)
{
    if (f >= 70 && f <= 72) return 70;
    if (f >= 73 && f <= 75) return 73;
    if (f >= 76 && f <= 78) return 76;
    if (f >= 79 && f <= 81) return 79;
    if (f >= 82 && f <= 84) return 82;
    if (f >= 94 && f <= 96) return 94;
    if (f >= 97 && f <= 99) return 97;
    if (f == 87 || f == 91) return 90;
    if (f == 88 || f == 93) return 92;
    if (f == 28 || f == 29) return 27;
    return f;
}

struct TexInfo {
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint32_t mips;
    uint32_t arr;
    bool hashable = false;
}; // no RT/DS/UAV usage and mip0 at or above kTexHashFloorPx: an upload can name it

// What is known about each game texture by its resource: its dimensions, the 3dmigoto hash its mip0 produced (or
// the one it inherited through a copy), and the size/format profiles the rule targets have shown.
class TexIdentityTable {
public:
    struct Memo {
        uint64_t res;
        uint32_t hash;
        uint32_t gen;
    };
    static Memo& MemoAt(uint64_t res)
    {
        thread_local Memo t_memo[1024] = {};
        return t_memo[((res >> 4) ^ (res >> 14)) & 1023];
    }
    // Bumped under the lock by every hash write: a thread's remembered hash holds while it is unchanged.
    uint32_t Gen() const { return gen_.load(std::memory_order_acquire); }
    uint32_t HashOf(uint64_t res); // 0 = never hashed
    bool HashKnown(uint64_t res);
    bool DimsOf(uint64_t res, TexInfo* out);
    // The hashes of the misses one table scan left, under one shared lock, each remembered on this thread.
    void HashesOf(ID3D12Resource* const* res, const uint32_t* miss, uint32_t nm, uint32_t* hash, uint32_t gen);
    // The hash and, when asked, whether an upload can name the texture, under one shared lock.
    uint32_t HashAndHashable(uint64_t res, bool want_hashable, bool* hashable);
    void Insert(uint64_t res, const TexInfo& info);
    void Forget(uint64_t res);
    bool SetHash(uint64_t res, uint32_t hash); // true: it had been inherited, which it no longer is
    void KeepHash(uint64_t res, uint32_t hash); // the belief kept: the hash written back, inherited untouched
    bool Inherit(
        uint64_t dst, uint32_t hash); // a copy's destination believed to be its source's texture; false = already
    void NoteModTex(uint64_t res);
    bool NoteModTexIfHash(uint64_t res, uint32_t hash); // ... only while it still holds that texture
    bool Inherited(uint64_t res);
    template <class F> void ForEachHash(F&& f)
    {
        std::shared_lock l(mutex_);
        for (const auto& kv : hash_of_) f(kv.first, kv.second);
    }
    void NoteMatchProfile(uint64_t prof);
    bool MatchProfile(uint64_t prof);
    bool MatchProfileWithin(uint32_t w, uint32_t h, uint32_t fmt, unsigned shifts); // ... at w,h scaled up to 2^shifts
    // Resource Identity Tracking: a vanilla resource pointer -> the mod SRV it was last swapped for (MIDTEX probe).
    void NoteModSrv(uint64_t res, SIZE_T srv);
    bool HasModSrv(uint64_t res);
    void ForgetModSrv(uint64_t res);

private:
    std::shared_mutex mutex_;
    std::unordered_map<uint64_t, TexInfo> dims_; // resource handle -> dims
    std::unordered_map<uint64_t, uint32_t> hash_of_; // ID3D12Resource* -> 3dmigoto hash
    std::atomic<uint32_t> gen_{ 1 };
    std::unordered_set<uint64_t> modtex_;
    std::unordered_set<uint64_t> inherited_;
    std::mutex match_mutex_;
    std::set<uint64_t> match_profiles_;
    std::mutex mod_srv_mutex_;
    std::unordered_map<uint64_t, SIZE_T> mod_srv_of_;
};
extern TexIdentityTable g_tex_identity;
