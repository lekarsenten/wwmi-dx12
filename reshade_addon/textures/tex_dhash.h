#pragma once
#include <cstdint>
#include <functional>

// XXMI's 64-bit difference hash of one texture mip (3DMigoto 1.3.3, DHash64Gpu.cpp): the [Filter] sections'
// match_dhash. Nine bilinear samples a row at ((x + 0.5) / 9, (y + 0.5) / 8), eight rows, each the mean of R, G, B
// and A as an SRV of `view_fmt` returns them; bit y * 8 + x is sample x > sample x + 1. A mip whose 64 bits are all
// zero takes its centre colour as four 16-bit thermometer codes instead.
namespace texdhash {

// One mip's data: `row(r)` is block row r (block-compressed) or pixel row r, null when it cannot be read.
struct Mip {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t fmt = 0; // the resource's DXGI format
    std::function<const uint8_t*(uint32_t row)> row;
};

bool Decodes(uint32_t fmt);
// The format reads as linear through one view and as sRGB through another (BC1-3, BC7, 8-bit RGBA / BGRA).
bool HasSrgbView(uint32_t fmt);
bool IsSrgb(uint32_t view_fmt);
// The rows of `m` a hash reads, ascending: what a caller fetches before hashing.
void RowsRead(uint32_t w, uint32_t h, uint32_t fmt, std::function<void(uint32_t row)> fn);
bool Hash(const Mip& m, bool srgb, uint64_t* out);

} // namespace texdhash
