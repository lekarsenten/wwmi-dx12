// A texture's look: the signature and the SSIM maths. See tex_look_sig.h.
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"
#include "tex_look_sig.h"
#include "../core/dxgi_bc.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <numeric>

namespace texlook {

namespace {

constexpr double kC1 = (0.01 * 255) * (0.01 * 255);
constexpr double kC2 = (0.03 * 255) * (0.03 * 255);

struct Codec {
    uint8_t id = 0;
    uint8_t nch = 0;
};
Codec codec_of(uint32_t fmt)
{
    if (fmt >= 70 && fmt <= 72) return { 1, 3 };
    if (fmt >= 76 && fmt <= 78) return { 3, 4 };
    if (fmt == 79 || fmt == 80) return { 4, 1 };
    if (fmt == 82 || fmt == 83) return { 5, 2 };
    if (fmt >= 97 && fmt <= 99) return { 7, 4 };
    return {};
}
bool pow2(uint32_t v)
{
    return v && !(v & (v - 1));
}

struct Geometry {
    uint32_t sw = 0; // patch side in mip0 texels
    uint32_t sh = 0;
    uint32_t fx = 0; // texels per cell
    uint32_t fy = 0;
    uint32_t pw = 0; // cells per patch side
    uint32_t ph = 0;
    Spots at{}; // the default spots
};
// A patch side holds whole 4x4 blocks and whole cells, and every corner starts on a block.
Geometry geometry_of(uint32_t w, uint32_t h)
{
    Geometry g;
    if (w < 16 || h < 16) return g;
    g.fx = (w + kRefSide - 1) / kRefSide;
    g.fy = (h + kRefSide - 1) / kRefSide;
    const uint32_t ux = std::lcm(4u, g.fx);
    const uint32_t uy = std::lcm(4u, g.fy);
    g.sw = w / 16 / ux * ux;
    g.sh = h / 16 / uy * uy;
    g.pw = g.sw / g.fx;
    g.ph = g.sh / g.fy;
    const uint32_t iw = (w / 8) & ~3u;
    const uint32_t ih = (h / 8) & ~3u;
    g.at[0] = { ((w - g.sw) / 2) & ~3u, ((h - g.sh) / 2) & ~3u };
    g.at[1] = { iw, ih };
    g.at[2] = { w - iw - g.sw, ih };
    g.at[3] = { iw, h - ih - g.sh };
    g.at[4] = { w - iw - g.sw, h - ih - g.sh };
    return g;
}

bool fits(uint32_t w, uint32_t h, const Geometry& g, const Spot& p)
{
    return g.sw && p.x % 4 == 0 && p.y % 4 == 0 && p.x + g.sw <= w && p.y + g.sh <= h;
}

// A patch's first block row in a cut, and the bytes from one block row to the next; null when the cut lacks it.
struct PatchRows {
    const uint8_t* first = nullptr;
    size_t stride = 0;
};
PatchRows rows_of(const TexLookCut& cut, const Geometry& g, uint32_t bb, const Spot& p)
{
    PatchRows r;
    if (cut.whole) {
        r.stride = (size_t)(cut.w / 4) * bb;
        r.first = cut.blocks.data() + (size_t)(p.y / 4) * r.stride + (size_t)(p.x / 4) * bb;
        return r;
    }
    const auto it = std::find(cut.spots.begin(), cut.spots.end(), p);
    if (it == cut.spots.end()) return r;
    r.stride = (size_t)(g.sw / 4) * bb;
    r.first = cut.blocks.data() + (size_t)(it - cut.spots.begin()) * (g.sh / 4) * r.stride;
    return r;
}

bool cut_complete(const TexLookCut& cut, const Geometry& g, uint32_t bb)
{
    const size_t want = cut.whole ? (size_t)(cut.w / 4) * (cut.h / 4) * bb
                                  : cut.spots.size() * (g.sw / 4) * (g.sh / 4) * bb;
    return want && cut.blocks.size() == want;
}

void decode_block(uint8_t codec, const uint8_t* blk, uint8_t rgba[64])
{
    memset(rgba, 0, 64);
    if (codec == 1)
        bcdec_bc1(blk, rgba, 16);
    else if (codec == 3)
        bcdec_bc3(blk, rgba, 16);
    else if (codec == 7)
        bcdec_bc7(blk, rgba, 16);
    else if (codec == 4) {
        uint8_t r[16];
        bcdec_bc4(blk, r, 4);
        for (int i = 0; i < 16; ++i) rgba[i * 4] = r[i];
    } else if (codec == 5) {
        uint8_t rg[32];
        bcdec_bc5(blk, rg, 8);
        for (int i = 0; i < 16; ++i) {
            rgba[i * 4] = rg[i * 2];
            rgba[i * 4 + 1] = rg[i * 2 + 1];
        }
    }
}

void stats_of(LookView& o)
{
    const size_t cells = (size_t)o.pw * o.ph;
    const size_t all = kPatches * cells;
    for (uint32_t c = 0; c < o.nch; ++c) {
        double s = 0;
        double ss = 0;
        for (int k = 0; k < kPatches; ++k) {
            double ps = 0;
            double pss = 0;
            for (size_t i = 0; i < cells; ++i) {
                const float v = o.v[(k * cells + i) * o.nch + c];
                ++o.st.hist[c][(std::min)(31, (int)v >> 3)];
                ps += v;
                pss += (double)v * v;
            }
            const double pm = ps / cells;
            o.st.patch_std[k][c] = (float)std::sqrt((std::max)(0.0, pss / cells - pm * pm));
            s += ps;
            ss += pss;
        }
        const double m = s / all;
        o.st.ch_std[c] = (float)std::sqrt((std::max)(0.0, ss / all - m * m));
        for (float& b : o.st.hist[c]) b /= (float)all;
    }
}

float hist_dist(const LookStats& a, const LookStats& b, uint32_t nch)
{
    double sum = 0;
    int n = 0;
    for (uint32_t c = 0; c < nch; ++c) {
        if (a.ch_std[c] < 1.f && b.ch_std[c] < 1.f) continue;
        double in = 0;
        for (int i = 0; i < 32; ++i) in += (std::min)(a.hist[c][i], b.hist[c][i]);
        sum += in;
        ++n;
    }
    return n ? (float)(1.0 - sum / n) : 1.f;
}

double ssim_plane(const LookView& a, const LookView& b, int k, uint32_t c, uint32_t win)
{
    const size_t base = (size_t)k * a.ph * a.pw;
    const double n = (double)win * win;
    double total = 0;
    uint32_t windows = 0;
    for (uint32_t y0 = 0; y0 + win <= a.ph; ++y0)
        for (uint32_t x0 = 0; x0 + win <= a.pw; ++x0) {
            double sa = 0;
            double sb = 0;
            double saa = 0;
            double sbb = 0;
            double sab = 0;
            for (uint32_t y = y0; y < y0 + win; ++y)
                for (uint32_t x = x0; x < x0 + win; ++x) {
                    const size_t i = (base + (size_t)y * a.pw + x) * a.nch + c;
                    const double va = a.v[i];
                    const double vb = b.v[i];
                    sa += va;
                    sb += vb;
                    saa += va * va;
                    sbb += vb * vb;
                    sab += va * vb;
                }
            const double ma = sa / n;
            const double mb = sb / n;
            const double var_a = saa / n - ma * ma;
            const double var_b = sbb / n - mb * mb;
            const double cov = sab / n - ma * mb;
            total += ((2 * ma * mb + kC1) * (2 * cov + kC2)) / ((ma * ma + mb * mb + kC1) * (var_a + var_b + kC2));
            ++windows;
        }
    return windows ? total / windows : 0;
}

// Lowest SSIM over the (patch, channel) planes with structure on either side; below 0 if fewer than
// kLiveMin patches have any.
float ssim_min(const LookView& a, const LookView& b)
{
    const uint32_t win = (std::min)({ 7u, a.pw, a.ph });
    int live = 0;
    double lo = 2;
    for (int k = 0; k < kPatches; ++k) {
        bool any = false;
        for (uint32_t c = 0; c < a.nch; ++c) {
            if ((std::max)(a.st.patch_std[k][c], b.st.patch_std[k][c]) < kLiveStd) continue;
            lo = (std::min)(lo, ssim_plane(a, b, k, c, win));
            any = true;
        }
        live += any;
    }
    return live >= kLiveMin ? (float)lo : -1.f;
}

} // namespace

bool Eligible(uint32_t w, uint32_t h, uint32_t fmt)
{
    return w % 4 == 0 && h % 4 == 0 && (std::min)(w, h) >= 128 && codec_of(fmt).id;
}

bool PowerOfTwo(uint32_t w, uint32_t h)
{
    return pow2(w) && pow2(h);
}

uint8_t CodecOf(uint32_t fmt)
{
    return codec_of(fmt).id;
}

uint64_t ShapeOf(uint8_t codec, uint32_t w, uint32_t h)
{
    return (uint64_t)codec << 56 | (uint64_t)(w & 0xfffffffu) << 28 | (h & 0xfffffffu);
}

uint32_t CellsOf(uint32_t w, uint32_t h)
{
    const Geometry g = geometry_of(w, h);
    return g.pw * g.ph;
}

Spots DefaultSpots(uint32_t w, uint32_t h)
{
    return geometry_of(w, h).at;
}

bool SpotsFit(uint32_t w, uint32_t h, const Spots& at)
{
    const Geometry g = geometry_of(w, h);
    return std::all_of(at.begin(), at.end(), [&](const Spot& p) { return fits(w, h, g, p); });
}

bool SigOf(const TexLookCut& cut, const Spots& at, LookSig& s)
{
    if (!Eligible(cut.w, cut.h, cut.fmt)) return false;
    const Codec c = codec_of(cut.fmt);
    const Geometry g = geometry_of(cut.w, cut.h);
    const uint32_t bb = DxgiBcBlockBytes(cut.fmt);
    const uint32_t nbx = g.sw / 4;
    const uint32_t nby = g.sh / 4;
    if (!cut_complete(cut, g, bb) || !SpotsFit(cut.w, cut.h, at)) return false;
    PatchRows rows[kPatches];
    for (int k = 0; k < kPatches; ++k) {
        rows[k] = rows_of(cut, g, bb, at[k]);
        if (!rows[k].first) return false;
    }
    s.codec = c.id;
    s.nch = c.nch;
    s.w = cut.w;
    s.h = cut.h;
    s.pw = g.pw;
    s.ph = g.ph;
    s.at = at;
    const size_t cells = (size_t)g.pw * g.ph * c.nch;
    s.px.assign(kPatches * cells, 0);
    std::vector<uint32_t> sum(cells);
    const double per = (double)g.fx * g.fy;
    for (int k = 0; k < kPatches; ++k) {
        std::fill(sum.begin(), sum.end(), 0u);
        for (uint32_t by = 0; by < nby; ++by) {
            const uint8_t* blk = rows[k].first + by * rows[k].stride;
            for (uint32_t bx = 0; bx < nbx; ++bx, blk += bb) {
                uint8_t t[64];
                decode_block(c.id, blk, t);
                for (uint32_t i = 0; i < 16; ++i) {
                    const uint32_t tx = bx * 4 + (i & 3);
                    const uint32_t ty = by * 4 + (i >> 2);
                    uint32_t* cell = &sum[((size_t)(ty / g.fy) * g.pw + tx / g.fx) * c.nch];
                    for (uint32_t ch = 0; ch < c.nch; ++ch) cell[ch] += t[i * 4 + ch];
                }
            }
        }
        uint8_t* out = &s.px[k * cells];
        for (size_t i = 0; i < cells; ++i) out[i] = (uint8_t)std::nearbyint(sum[i] / per);
    }
    return true;
}

bool BusiestSpots(const TexLookCut& cut, Spots& at)
{
    if (!cut.whole || !Eligible(cut.w, cut.h, cut.fmt)) return false;
    const Codec c = codec_of(cut.fmt);
    const Geometry g = geometry_of(cut.w, cut.h);
    const uint32_t bb = DxgiBcBlockBytes(cut.fmt);
    if (!g.sw || !cut_complete(cut, g, bb)) return false;
    const uint32_t gw = cut.w / g.fx;
    const uint32_t gh = cut.h / g.fy;
    std::vector<uint32_t> sum((size_t)gw * gh * c.nch, 0u);
    const uint8_t* blk = cut.blocks.data();
    for (uint32_t by = 0; by < cut.h / 4; ++by)
        for (uint32_t bx = 0; bx < cut.w / 4; ++bx, blk += bb) {
            uint8_t t[64];
            decode_block(c.id, blk, t);
            for (uint32_t i = 0; i < 16; ++i) {
                const uint32_t cx = (bx * 4 + (i & 3)) / g.fx;
                const uint32_t cy = (by * 4 + (i >> 2)) / g.fy;
                if (cx >= gw || cy >= gh) continue;
                uint32_t* cell = &sum[((size_t)cy * gw + cx) * c.nch];
                for (uint32_t ch = 0; ch < c.nch; ++ch) cell[ch] += t[i * 4 + ch];
            }
        }
    const double per = (double)g.fx * g.fy;
    const uint32_t ux = std::lcm(4u, g.fx);
    const uint32_t uy = std::lcm(4u, g.fy);
    const uint32_t step_x = (std::max)(ux, g.sw / 2 / ux * ux);
    const uint32_t step_y = (std::max)(uy, g.sh / 2 / uy * uy);
    struct Scored {
        Spot p;
        double detail = 0;
    };
    std::vector<Scored> all;
    const double n = (double)g.pw * g.ph;
    for (uint32_t y = 0; y + g.sh <= cut.h; y += step_y)
        for (uint32_t x = 0; x + g.sw <= cut.w; x += step_x) {
            double detail = 0;
            for (uint32_t ch = 0; ch < c.nch; ++ch) {
                double s = 0;
                double ss = 0;
                for (uint32_t j = 0; j < g.ph; ++j)
                    for (uint32_t i = 0; i < g.pw; ++i) {
                        const double v = std::nearbyint(sum[(((size_t)(y / g.fy) + j) * gw + x / g.fx + i) * c.nch + ch] / per);
                        s += v;
                        ss += v * v;
                    }
                const double m = s / n;
                detail = (std::max)(detail, std::sqrt((std::max)(0.0, ss / n - m * m)));
            }
            all.push_back(Scored{ Spot{ x, y }, detail });
        }
    std::stable_sort(all.begin(), all.end(), [](const Scored& a, const Scored& b) { return a.detail > b.detail; });
    int got = 0;
    for (const Scored& e : all) {
        if (got == kPatches) break;
        bool overlaps = false;
        for (int k = 0; k < got; ++k)
            if ((std::max)(e.p.x, at[k].x) - (std::min)(e.p.x, at[k].x) < g.sw &&
                (std::max)(e.p.y, at[k].y) - (std::min)(e.p.y, at[k].y) < g.sh)
                overlaps = true;
        if (!overlaps) at[got++] = e.p;
    }
    if (got < kPatches) return false;
    std::sort(at.begin(), at.end(), [](const Spot& a, const Spot& b) { return a.y != b.y ? a.y < b.y : a.x < b.x; });
    return true;
}

LookView ViewOf(const LookSig& s, uint32_t pw, uint32_t ph)
{
    LookView o;
    o.pw = pw;
    o.ph = ph;
    o.nch = s.nch;
    const uint32_t fx = s.pw / pw;
    const uint32_t fy = s.ph / ph;
    o.v.assign((size_t)kPatches * pw * ph * s.nch, 0.f);
    for (int k = 0; k < kPatches; ++k)
        for (uint32_t y = 0; y < s.ph; ++y)
            for (uint32_t x = 0; x < s.pw; ++x)
                for (uint32_t c = 0; c < s.nch; ++c)
                    o.v[(((size_t)k * ph + y / fy) * pw + x / fx) * s.nch + c] +=
                        s.px[(((size_t)k * s.ph + y) * s.pw + x) * s.nch + c];
    const float per = (float)(fx * fy);
    for (float& v : o.v) v /= per;
    stats_of(o);
    return o;
}

Score Compare(const LookSig& cand, const LookView& cv, const Ref& r)
{
    Score s;
    if (cand.codec != r.sig.codec || (uint64_t)cand.w * r.sig.h != (uint64_t)cand.h * r.sig.w) return s;
    if ((!PowerOfTwo(cand.w, cand.h) || !PowerOfTwo(r.sig.w, r.sig.h)) &&
        (cand.w != r.sig.w || cand.h != r.sig.h || cand.pw != r.sig.pw || cand.ph != r.sig.ph || cand.at != r.sig.at))
        return s;
    const uint32_t pw = (std::min)(cand.pw, r.sig.pw);
    const uint32_t ph = (std::min)(cand.ph, r.sig.ph);
    const bool ref_native = pw == r.sig.pw && ph == r.sig.ph;
    LookView down;
    if (pw != cand.pw || ph != cand.ph) down = ViewOf(cand, pw, ph);
    const LookView& a = down.v.empty() ? cv : down;
    if (ref_native) {
        s.hist = hist_dist(a.st, r.st, cand.nch);
        if (s.hist > kHistMax) return s;
    }
    const LookView b = ViewOf(r.sig, pw, ph);
    if (!ref_native) {
        s.hist = hist_dist(a.st, b.st, cand.nch);
        if (s.hist > kHistMax) return s;
    }
    s.ssim = ssim_min(a, b);
    return s;
}

int LivePatches(const LookStats& st, uint32_t nch)
{
    int live = 0;
    for (int k = 0; k < kPatches; ++k)
        for (uint32_t c = 0; c < nch; ++c)
            if (st.patch_std[k][c] >= kLiveStd) {
                ++live;
                break;
            }
    return live;
}

} // namespace texlook

TexLookCut TexLookWholeOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt)
{
    TexLookCut cut;
    if (!bc || !texlook::Eligible(w, h, fmt)) return cut;
    const size_t bytes = (size_t)(w / 4) * (h / 4) * DxgiBcBlockBytes(fmt);
    if (bc_size < bytes) return cut;
    cut.blocks.assign(bc, bc + bytes);
    cut.whole = true;
    cut.fmt = fmt;
    cut.w = w;
    cut.h = h;
    return cut;
}

TexLookCut TexLookCutOf(
    const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt, const std::vector<texlook::Spot>& spots)
{
    TexLookCut cut;
    if (!bc || !texlook::Eligible(w, h, fmt)) return cut;
    const uint32_t bb = DxgiBcBlockBytes(fmt);
    const size_t row = (size_t)(w / 4) * bb;
    if (bc_size < row * (h / 4)) return cut;
    const texlook::Geometry g = texlook::geometry_of(w, h);
    if (spots.empty())
        cut.spots.assign(g.at.begin(), g.at.end());
    else
        for (const texlook::Spot& p : spots)
            if (texlook::fits(w, h, g, p)) cut.spots.push_back(p);
    if (cut.spots.size() * g.sw * g.sh >= (size_t)w * h) return TexLookWholeOf(bc, bc_size, w, h, fmt);
    const size_t run = (size_t)(g.sw / 4) * bb;
    cut.blocks.resize(cut.spots.size() * (g.sh / 4) * run);
    uint8_t* d = cut.blocks.data();
    for (const texlook::Spot& p : cut.spots)
        for (uint32_t r = 0; r < g.sh / 4; ++r, d += run)
            memcpy(d, bc + (size_t)(p.y / 4 + r) * row + (size_t)(p.x / 4) * bb, run);
    cut.fmt = fmt;
    cut.w = w;
    cut.h = h;
    return cut;
}
