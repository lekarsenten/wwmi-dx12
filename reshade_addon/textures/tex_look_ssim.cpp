// A texture's look: the signature and the SSIM maths. See tex_look_sig.h.
#define BCDEC_IMPLEMENTATION
#include "../../third_party/bcdec/bcdec.h"
#include "tex_look_sig.h"
#include "../core/dxgi_bc.h"
#include <algorithm>
#include <cmath>
#include <cstring>

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

struct Corner {
    uint32_t x = 0;
    uint32_t y = 0;
};
struct Geometry {
    uint32_t sw = 0; // patch side in mip0 texels
    uint32_t sh = 0;
    uint32_t fx = 0; // texels per cell
    uint32_t fy = 0;
    uint32_t pw = 0; // cells per patch side
    uint32_t ph = 0;
    Corner at[kPatches];
};
Geometry geometry_of(uint32_t w, uint32_t h)
{
    Geometry g;
    g.sw = w / 16;
    g.sh = h / 16;
    g.fx = w / (std::min)(w, kRefSide);
    g.fy = h / (std::min)(h, kRefSide);
    g.pw = g.sw / g.fx;
    g.ph = g.sh / g.fy;
    const uint32_t iw = w / 8;
    const uint32_t ih = h / 8;
    g.at[0] = { (w - g.sw) / 2, (h - g.sh) / 2 };
    g.at[1] = { iw, ih };
    g.at[2] = { w - iw - g.sw, ih };
    g.at[3] = { iw, h - ih - g.sh };
    g.at[4] = { w - iw - g.sw, h - ih - g.sh };
    return g;
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
    return pow2(w) && pow2(h) && (std::min)(w, h) >= 128 && codec_of(fmt).id;
}

uint32_t CellsOf(uint32_t w, uint32_t h)
{
    return ((std::min)(w, kRefSide) / 16) * ((std::min)(h, kRefSide) / 16);
}

bool SigOf(const TexLookCut& cut, LookSig& s)
{
    if (!Eligible(cut.w, cut.h, cut.fmt)) return false;
    const Codec c = codec_of(cut.fmt);
    const Geometry g = geometry_of(cut.w, cut.h);
    const uint32_t bb = DxgiBcBlockBytes(cut.fmt);
    const uint32_t nbx = g.sw / 4;
    const uint32_t nby = g.sh / 4;
    if (cut.blocks.size() != (size_t)kPatches * nbx * nby * bb) return false;
    s.codec = c.id;
    s.nch = c.nch;
    s.w = cut.w;
    s.h = cut.h;
    s.pw = g.pw;
    s.ph = g.ph;
    const size_t cells = (size_t)g.pw * g.ph * c.nch;
    s.px.assign(kPatches * cells, 0);
    std::vector<uint32_t> sum(cells);
    const uint8_t* blk = cut.blocks.data();
    const double per = (double)g.fx * g.fy;
    for (int k = 0; k < kPatches; ++k) {
        std::fill(sum.begin(), sum.end(), 0u);
        for (uint32_t by = 0; by < nby; ++by)
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
        uint8_t* out = &s.px[k * cells];
        for (size_t i = 0; i < cells; ++i) out[i] = (uint8_t)std::nearbyint(sum[i] / per);
    }
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

TexLookCut TexLookCutOf(const uint8_t* bc, size_t bc_size, uint32_t w, uint32_t h, uint32_t fmt)
{
    TexLookCut cut;
    if (!bc || !texlook::Eligible(w, h, fmt)) return cut;
    const uint32_t bb = DxgiBcBlockBytes(fmt);
    const size_t row = (size_t)(w / 4) * bb;
    if (bc_size < row * (h / 4)) return cut;
    const texlook::Geometry g = texlook::geometry_of(w, h);
    const size_t run = (size_t)(g.sw / 4) * bb;
    cut.blocks.resize(texlook::kPatches * (size_t)(g.sh / 4) * run);
    uint8_t* d = cut.blocks.data();
    for (const texlook::Corner& p : g.at)
        for (uint32_t r = 0; r < g.sh / 4; ++r, d += run)
            memcpy(d, bc + (size_t)(p.y / 4 + r) * row + (size_t)(p.x / 4) * bb, run);
    cut.fmt = fmt;
    cut.w = w;
    cut.h = h;
    return cut;
}
