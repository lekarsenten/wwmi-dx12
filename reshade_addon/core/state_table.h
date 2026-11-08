#pragma once
#include <mutex>
#include <vector>
#include <cstdint>

class ShardedStateTable {
    static constexpr uint32_t N = 64;
    static constexpr uint64_t kEmpty = 0;
    static constexpr uint64_t kGone = 1;
    struct Slot {
        uint64_t key;
        uint32_t val;
    };
    struct alignas(64) Shard {
        std::mutex m;
        std::vector<Slot> slots;
        uint32_t used = 0;
        uint32_t live = 0;
    };
    Shard sh[N];

    static uint64_t Mix(uint64_t k)
    {
        k ^= k >> 33;
        k *= 0xff51afd7ed558ccdull;
        k ^= k >> 33;
        return k;
    }
    static Shard& ShardOf(Shard (&s)[N], uint64_t k) { return s[(Mix(k) >> 58) & (N - 1)]; }
    static void Rehash(Shard& s)
    {
        size_t cap = 64;
        while (cap < (size_t)s.live * 4) cap <<= 1;
        std::vector<Slot> old;
        old.swap(s.slots);
        s.slots.assign(cap, Slot{ kEmpty, 0 });
        s.used = s.live;
        const size_t mask = cap - 1;
        for (const Slot& o : old) {
            if (o.key <= kGone) continue;
            for (size_t i = Mix(o.key) & mask;; i = (i + 1) & mask)
                if (s.slots[i].key == kEmpty) {
                    s.slots[i] = o;
                    break;
                }
        }
    }
    static Slot* Probe(Shard& s, uint64_t k)
    {
        if (s.slots.empty()) return nullptr;
        const size_t mask = s.slots.size() - 1;
        for (size_t i = Mix(k) & mask;; i = (i + 1) & mask) {
            Slot& x = s.slots[i];
            if (x.key == k) return &x;
            if (x.key == kEmpty) return nullptr;
        }
    }

public:
    bool find(uint64_t k, uint32_t& out)
    {
        if (k <= kGone) return false;
        Shard& s = ShardOf(sh, k);
        std::lock_guard l(s.m);
        const Slot* x = Probe(s, k);
        if (!x) return false;
        out = x->val;
        return true;
    }
    void put(uint64_t k, uint32_t v)
    {
        if (k <= kGone) return;
        Shard& s = ShardOf(sh, k);
        std::lock_guard l(s.m);
        if (Slot* x = Probe(s, k)) {
            x->val = v;
            return;
        }
        if ((size_t)(s.used + 1) * 2 > s.slots.size()) Rehash(s);
        const size_t mask = s.slots.size() - 1;
        for (size_t i = Mix(k) & mask;; i = (i + 1) & mask) {
            Slot& x = s.slots[i];
            if (x.key == kEmpty) {
                x = Slot{ k, v };
                ++s.used;
                ++s.live;
                return;
            }
        }
    }
    void erase(uint64_t k)
    {
        if (k <= kGone) return;
        Shard& s = ShardOf(sh, k);
        std::lock_guard l(s.m);
        if (Slot* x = Probe(s, k)) {
            x->key = kGone;
            --s.live;
        }
    }
};
