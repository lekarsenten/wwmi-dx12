#pragma once
// A texture's look: five patches of its mip0 (centre, four corners inset 1/8, side 1/16) box-averaged to one
// density, their histograms, and SSIM per patch between two of them. Pure functions, depending on core/ only.
#include <cstdint>
#include <vector>

// The BC blocks under a mip0's five patches, cut on the upload thread: all a signature reads.
struct TexLookCut {
    uint32_t fmt = 0;
    uint32_t w = 0;
    uint32_t h = 0;
    std::vector<uint8_t> blocks; // patch by patch, block row by block row
};
TexLookCut TexLookCutOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt);

namespace texlook {

constexpr int kPatches = 5;
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
uint32_t CellsOf(uint32_t w, uint32_t h);
bool SigOf(const TexLookCut& cut, LookSig& s);
// The signature at a coarser (or its own) cell density: each cell the mean of the cells it covers.
LookView ViewOf(const LookSig& s, uint32_t pw, uint32_t ph);
// `cv` is `cand` at its own density.
Score Compare(const LookSig& cand, const LookView& cv, const Ref& r);
int LivePatches(const LookStats& st, uint32_t nch);

} // namespace texlook
