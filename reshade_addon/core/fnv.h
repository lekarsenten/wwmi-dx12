#pragma once
// FNV-1a. The 64-bit byte form is 3dmigoto's shader hash (HashShader), so these constants are not a choice.
#include <cstddef>
#include <cstdint>

inline constexpr uint64_t kFnv64Basis = 0xcbf29ce484222325ull;
inline constexpr uint64_t kFnv64Prime = 0x100000001b3ull;
inline constexpr uint32_t kFnv32Basis = 0x811c9dc5u;
inline constexpr uint32_t kFnv32Prime = 0x01000193u;

constexpr uint64_t Fnv1a64Str(const char* s, uint64_t h = kFnv64Basis)
{
    for (; s && *s; ++s) h = (h ^ (unsigned char)*s) * kFnv64Prime;
    return h;
}

constexpr uint32_t Fnv1a32Str(const char* s, uint32_t h = kFnv32Basis)
{
    for (; s && *s; ++s) h = (h ^ (unsigned char)*s) * kFnv32Prime;
    return h;
}

inline uint64_t Fnv1a64(const void* data, size_t n, uint64_t h = kFnv64Basis)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kFnv64Prime;
    return h;
}

inline uint32_t Fnv1a32(const void* data, size_t n, uint32_t h = kFnv32Basis)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * kFnv32Prime;
    return h;
}

// One step per 32-bit WORD, not per byte: a different hash, for content ids of dword buffers.
inline uint64_t Fnv1a64Words(const uint32_t* w, size_t n, uint64_t h = kFnv64Basis)
{
    for (size_t i = 0; i < n; ++i) h = (h ^ w[i]) * kFnv64Prime;
    return h;
}

// Published FNV-1a test vectors (isthe.com/chongo/tech/comp/fnv).
static_assert(Fnv1a64Str("") == 0xcbf29ce484222325ull);
static_assert(Fnv1a64Str("a") == 0xaf63dc4c8601ec8cull);
static_assert(Fnv1a64Str("foobar") == 0x85944171f73967e8ull);
static_assert(Fnv1a32Str("a") == 0xe40c292cu);
static_assert(Fnv1a32Str("foobar") == 0xbf9cf968u);

// 64-bit FNV-1a over shader bytecode.  Zero means "no shader".
inline uint64_t HashShader(const void* data, size_t size)
{
    if (!data || size == 0) return 0;
    return Fnv1a64(data, size);
}
