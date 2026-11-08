#pragma once
#include <cstdint>
inline uint32_t DxgiBcBlockBytes(uint32_t f)
{
    if ((f >= 70 && f <= 72) || (f >= 79 && f <= 81)) return 8;
    if ((f >= 73 && f <= 78) || (f >= 82 && f <= 84) || (f >= 94 && f <= 99)) return 16;
    return 0; // not block-compressed
}
inline bool DxgiIsBc(uint32_t f)
{
    return DxgiBcBlockBytes(f) != 0;
}
inline uint32_t DxgiPlainBytesPerPixel(uint32_t f)
{
    if (f >= 1 && f <= 4) return 16;
    if (f >= 9 && f <= 18) return 8;
    if ((f >= 23 && f <= 43) || f == 67 || f == 87 || f == 88 || (f >= 90 && f <= 93)) return 4;
    if ((f >= 48 && f <= 59) || f == 85 || f == 86 || f == 115) return 2;
    if (f >= 60 && f <= 65) return 1;
    return 0;
}
