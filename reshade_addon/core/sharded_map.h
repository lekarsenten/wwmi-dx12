#pragma once
// A self-synchronized sharded map: N independent {mutex, submap} buckets keyed by hash(key) % N.
#include <mutex>
#include <unordered_map>
#include <cstdint>

template <class K, class V, class H = std::hash<K>> struct ShardedMap {
    static constexpr uint32_t N = 64; // shards -- MUST be a power of two
    struct alignas(64) Shard {
        std::mutex m;
        std::unordered_map<K, V, H> map;
    };
    Shard sh[N];
    Shard& shard(const K& k) { return sh[(uint32_t)H{}(k) & (N - 1)]; }
    bool find(const K& k, V& out)
    {
        Shard& s = shard(k);
        std::lock_guard l(s.m);
        auto it = s.map.find(k);
        if (it == s.map.end()) return false;
        out = it->second;
        return true;
    }
    void put(const K& k, const V& v)
    {
        Shard& s = shard(k);
        std::lock_guard l(s.m);
        s.map[k] = v;
    }
    void erase(const K& k)
    {
        Shard& s = shard(k);
        std::lock_guard l(s.m);
        s.map.erase(k);
    }
    template <class F> void for_each(F f)
    {
        for (auto& s : sh) {
            std::lock_guard l(s.m);
            for (auto& kv : s.map) f(kv.first, kv.second);
        }
    }
    template <class F> void update(const K& k, F f)
    {
        Shard& s = shard(k);
        std::lock_guard l(s.m);
        f(s.map[k]);
    }
    template <class F> bool with(const K& k, F f)
    {
        Shard& s = shard(k);
        std::lock_guard l(s.m);
        auto it = s.map.find(k);
        if (it == s.map.end()) return false;
        f(it->second);
        return true;
    }
    template <class F> void erase_if(F pred)
    {
        for (auto& s : sh) {
            std::lock_guard l(s.m);
            for (auto it = s.map.begin(); it != s.map.end();) {
                if (pred(it->second))
                    it = s.map.erase(it);
                else
                    ++it;
            }
        }
    }
};
