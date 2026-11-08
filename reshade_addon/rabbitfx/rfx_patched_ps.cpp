#include "rfx_patched_ps.h"

RfxPatchedPsStore g_patched_ps;

CodeBlob RfxPatchedPsStore::Decensored(uint64_t src_pso) const
{
    std::lock_guard l(decensor_mutex_);
    auto it = decensor_.find(src_pso);
    return it == decensor_.end() ? nullptr : it->second;
}

CodeBlob RfxPatchedPsStore::Rfx(uint64_t src_pso, bool decensor) const
{
    std::lock_guard l(rfx_mutex_);
    auto it = rfx_.find(src_pso | ((uint64_t)decensor << 60));
    return it == rfx_.end() ? nullptr : it->second;
}

void RfxPatchedPsStore::SetDecensored(uint64_t src_pso, CodeBlob ps)
{
    std::lock_guard d(decensor_mutex_);
    decensor_[src_pso] = std::move(ps);
}

void RfxPatchedPsStore::SetRfx(uint64_t key, CodeBlob ps)
{
    std::lock_guard r(rfx_mutex_);
    rfx_[key] = std::move(ps);
}

void RfxPatchedPsStore::Forget(uint64_t src_pso)
{
    {
        std::lock_guard d(decensor_mutex_);
        decensor_.erase(src_pso);
    }
    std::lock_guard r(rfx_mutex_);
    rfx_.erase(src_pso);
    rfx_.erase(src_pso | (1ull << 60));
}
