#pragma once
#include "pso_clone.h"
#include <cstdint>
#include <mutex>
#include <unordered_map>

// A source PSO's patched pixel shaders once their variants have settled: the decensored one, and the
// RabbitFX-patched one with and without the decensor. The blend and input-layout variants swap them in.
class RfxPatchedPsStore {
public:
    CodeBlob Decensored(uint64_t src_pso) const;
    CodeBlob Rfx(uint64_t src_pso, bool decensor) const;
    void SetDecensored(uint64_t src_pso, CodeBlob ps);
    void SetRfx(uint64_t key, CodeBlob ps); // key: src_pso | decensor << 60
    void Forget(uint64_t src_pso);

private:
    mutable std::mutex decensor_mutex_;
    std::unordered_map<uint64_t, CodeBlob> decensor_;
    mutable std::mutex rfx_mutex_;
    std::unordered_map<uint64_t, CodeBlob> rfx_;
};
extern RfxPatchedPsStore g_patched_ps;
