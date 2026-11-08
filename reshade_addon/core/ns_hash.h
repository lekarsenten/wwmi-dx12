#pragma once
#include <cstdint>

inline uint64_t NsHash(const wchar_t* ns)
{
    if (!ns || !*ns) return 0;
    uint64_t h = 0xcbf29ce484222325ull;
    for (const wchar_t* c = ns; *c; ++c) h = (h ^ (uint64_t)*c) * 0x100000001b3ull;
    return h;
}
