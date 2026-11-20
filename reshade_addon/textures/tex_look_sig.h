#pragma once
// A texture's look: five patches of its mip0 (by default the centre and four corners inset 1/8; side 1/16, in whole
// 4x4 blocks) box-averaged to one density, their histograms, and SSIM per patch between two of them. Pure functions,
// depending on core/ only.
#include <array>
#include <cstdint>
#include <vector>

namespace texlook {

constexpr int kPatches = 5;

// A patch's top-left texel in mip0, on a 4x4 block.
struct Spot {
    uint32_t x = 0;
    uint32_t y = 0;
    bool operator==(const Spot& o) const { return x == o.x && y == o.y; }
};
using Spots = std::array<Spot, kPatches>;

} // namespace texlook

// The BC blocks a signature reads, cut on the upload thread: the patches at `spots`, or the whole mip0.
struct TexLookCut {
    uint32_t fmt = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    bool whole = false; // blocks: the whole mip0, block row by block row
    std::vector<texlook::Spot> spots; // else: these patches, each block row by block row
    std::vector<uint8_t> blocks;
};
// The patches at `spots` (none: the default five); the whole mip0 when they would hold as many blocks.
TexLookCut TexLookCutOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt,
    const std::vector<texlook::Spot>& spots = {});
TexLookCut TexLookWholeOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt);

namespace texlook {

constexpr uint32_t kRefSide = 512; // a patch holds at most kRefSide/16 cells a side
constexpr uint32_t kMaxCells = kRefSide / 16;
constexpr float kHistMax = 0.08f;
constexpr float kSsimMin = 0.80f;
constexpr float kLiveStd = 4.0f;
constexpr int kLiveMin = 3;

struct LookSig {
    uint8_t codec = 0;
    uint8_t nch = 0;
    uint32_t w = 0; // the mip it was taken from
    uint32_t h = 0;
    uint32_t pw = 0;
    uint32_t ph = 0;
    Spots at{}; // where its patches are in that mip
    std::vector<uint8_t> px; // kPatches x ph x pw x nch
};

struct LookStats {
    float hist[4][32] = {};
    float ch_std[4] = {};
    float patch_std[kPatches][4] = {};
};

struct LookView {
    uint32_t pw = 0;
    uint32_t ph = 0;
    uint32_t nch = 0;
    std::vector<float> v; // kPatches x ph x pw x nch
    LookStats st;
};

struct Ref {
    LookSig sig;
    LookStats st; // at the signature's own density
};

struct Score {
    float hist = 1.f;
    float ssim = -1.f; // below 0: not measured, or too flat to tell
    bool match() const { return hist <= kHistMax && ssim >= kSsimMin; }
};

bool Eligible(uint32_t w, uint32_t h, uint32_t fmt);
// Both sides a power of two: such a texture compares with any of its aspect, another one only at its own size and
// patch spots.
bool PowerOfTwo(uint32_t w, uint32_t h);
uint8_t CodecOf(uint32_t fmt); // 0: a format signatures skip
uint64_t ShapeOf(uint8_t codec, uint32_t w, uint32_t h);
uint32_t CellsOf(uint32_t w, uint32_t h);
Spots DefaultSpots(uint32_t w, uint32_t h);
bool SpotsFit(uint32_t w, uint32_t h, const Spots& at); // every patch on a block and inside the texture
bool SigOf(const TexLookCut& cut, const Spots& at, LookSig& s);
// A texture not a power of two a side: the five patches that do not overlap with the most structure (the highest
// channel std of their cells), on a grid of half a patch. `cut` is whole.
bool BusiestSpots(const TexLookCut& cut, Spots& at);
// The signature at a coarser (or its own) cell density: each cell the mean of the cells it covers.
LookView ViewOf(const LookSig& s, uint32_t pw, uint32_t ph);
// `cv` is `cand` at its own density.
Score Compare(const LookSig& cand, const LookView& cv, const Ref& r);
int LivePatches(const LookStats& st, uint32_t nch);

} // namespace texlook
