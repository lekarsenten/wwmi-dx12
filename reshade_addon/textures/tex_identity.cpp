// The identity table: what is known about each game texture by its resource. See tex_identity.h.
#include "tex_identity.h"

TexIdentityTable g_tex_identity;

uint32_t TexIdentityTable::HashOf(uint64_t res)
{
    const uint32_t gen = gen_.load(std::memory_order_acquire);
    Memo& m = MemoAt(res);
    if (m.res == res && m.gen == gen) return m.hash;
    uint32_t h = 0;
    {
        std::shared_lock l(mutex_);
        auto it = hash_of_.find(res);
        if (it != hash_of_.end()) h = it->second;
    }
    m = Memo{ res, h, gen };
    return h;
}

bool TexIdentityTable::HashKnown(uint64_t res)
{
    std::shared_lock l(mutex_);
    return hash_of_.count(res) != 0;
}

bool TexIdentityTable::DimsOf(uint64_t res, TexInfo* out)
{
    std::shared_lock l(mutex_);
    auto it = dims_.find(res);
    if (it == dims_.end()) return false;
    *out = it->second;
    return true;
}

void TexIdentityTable::HashesOf(
    ID3D12Resource* const* res, const uint32_t* miss, uint32_t nm, uint32_t* hash, uint32_t gen)
{
    std::shared_lock lt(mutex_); // PURE READ, once per table
    for (uint32_t k = 0; k < nm; ++k) {
        const uint64_t r = reinterpret_cast<uint64_t>(res[miss[k]]);
        auto h = hash_of_.find(r);
        hash[miss[k]] = h != hash_of_.end() ? h->second : 0;
        MemoAt(r) = Memo{ r, hash[miss[k]], gen };
    }
}

uint32_t TexIdentityTable::HashAndHashable(uint64_t res, bool want_hashable, bool* hashable)
{
    uint32_t hash = 0;
    std::shared_lock l(mutex_); // PURE READ (per-descriptor, hot) -> shared so worker threads parallelize
    auto h = hash_of_.find(res);
    if (h != hash_of_.end()) hash = h->second;
    if (want_hashable) {
        auto d = dims_.find(res);
        if (d != dims_.end()) *hashable = d->second.hashable;
    }
    return hash;
}

void TexIdentityTable::Insert(uint64_t res, const TexInfo& info)
{
    std::lock_guard lock(mutex_);
    dims_[res] = info;
}

void TexIdentityTable::Forget(uint64_t res)
{
    bool tex_known;
    {
        std::shared_lock l(mutex_);
        tex_known = hash_of_.count(res) || modtex_.count(res) || inherited_.count(res) || dims_.count(res);
    }
    if (tex_known) {
        std::lock_guard l(mutex_);
        if (hash_of_.erase(res)) gen_.fetch_add(1, std::memory_order_acq_rel);
        modtex_.erase(res);
        inherited_.erase(res);
        dims_.erase(res);
    }
}

bool TexIdentityTable::SetHash(uint64_t res, uint32_t hash)
{
    std::lock_guard l(mutex_);
    hash_of_[res] = hash;
    gen_.fetch_add(1, std::memory_order_acq_rel);
    return inherited_.erase(res) != 0;
}

void TexIdentityTable::KeepHash(uint64_t res, uint32_t hash)
{
    std::lock_guard l(mutex_);
    hash_of_[res] = hash;
    gen_.fetch_add(1, std::memory_order_acq_rel);
}

bool TexIdentityTable::Inherit(uint64_t dst, uint32_t hash)
{
    std::lock_guard l(mutex_);
    auto it = hash_of_.find(dst);
    if (it != hash_of_.end() && it->second == hash) return false;
    hash_of_[dst] = hash; // dest now swaps as the mod target on next placement
    gen_.fetch_add(1, std::memory_order_acq_rel);
    modtex_.insert(dst);
    inherited_.insert(dst); // a belief until its own mip0 says otherwise
    return true;
}

void TexIdentityTable::NoteModTex(uint64_t res)
{
    std::lock_guard l(mutex_);
    modtex_.insert(res);
}

bool TexIdentityTable::NoteModTexIfHash(uint64_t res, uint32_t hash)
{
    std::lock_guard l(mutex_); // the resource must still hold this texture
    auto it = hash_of_.find(res);
    if (it == hash_of_.end() || it->second != hash) return false;
    modtex_.insert(res);
    return true;
}

bool TexIdentityTable::Inherited(uint64_t res)
{
    std::shared_lock l(mutex_);
    return inherited_.count(res) != 0;
}

void TexIdentityTable::NoteMatchProfile(uint64_t prof)
{
    std::lock_guard l(match_mutex_);
    match_profiles_.insert(prof);
}

bool TexIdentityTable::MatchProfile(uint64_t prof)
{
    std::lock_guard l(match_mutex_);
    return match_profiles_.count(prof) != 0;
}

bool TexIdentityTable::MatchProfileWithin(uint32_t w, uint32_t h, uint32_t fmt, unsigned shifts)
{
    bool shaped = false;
    std::lock_guard l(match_mutex_);
    for (uint32_t k = 0; k <= shifts && !shaped; ++k)
        shaped = match_profiles_.count((((uint64_t)w << k) << 40) | (((uint64_t)h << k) << 16) | (fmt & 0xffffu)) != 0;
    return shaped;
}

void TexIdentityTable::NoteModSrv(uint64_t res, SIZE_T srv)
{
    std::lock_guard l(mod_srv_mutex_);
    mod_srv_of_[res] = srv;
}

bool TexIdentityTable::HasModSrv(uint64_t res)
{
    std::lock_guard l(mod_srv_mutex_);
    return mod_srv_of_.count(res) != 0;
}

void TexIdentityTable::ForgetModSrv(uint64_t res)
{
    std::lock_guard l(mod_srv_mutex_);
    mod_srv_of_.erase(res);
}
