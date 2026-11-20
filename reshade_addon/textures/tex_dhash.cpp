// XXMI's dHash of a texture mip, on the CPU. See tex_dhash.h.
#include "tex_dhash.h"
#include "../../third_party/bcdec/bcdec.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <set>
#include <unordered_map>

namespace texdhash {

namespace {

enum class Kind { None, Bc1, Bc2, Bc3, Bc4, Bc5, Bc6, Bc7, Rgba8, Bgra8, Bgrx8, R8, Rg8, A8 };

Kind KindOf(uint32_t f)
{
    switch (f) {
    case 70:
    case 71:
    case 72: return Kind::Bc1;
    case 73:
    case 74:
    case 75: return Kind::Bc2;
    case 76:
    case 77:
    case 78: return Kind::Bc3;
    case 79:
    case 80: return Kind::Bc4;
    case 82:
    case 83: return Kind::Bc5;
    case 94:
    case 95: return Kind::Bc6;
    case 97:
    case 98:
    case 99: return Kind::Bc7;
    case 27:
    case 28:
    case 29: return Kind::Rgba8;
    case 87:
    case 90:
    case 91: return Kind::Bgra8;
    case 88:
    case 92:
    case 93: return Kind::Bgrx8;
    case 60:
    case 61: return Kind::R8;
    case 48:
    case 49: return Kind::Rg8;
    case 65: return Kind::A8;
    default: return Kind::None;
    }
}

bool Block(Kind k)
{
    return k >= Kind::Bc1 && k <= Kind::Bc7;
}

uint32_t BlockBytes(Kind k)
{
    return k == Kind::Bc1 || k == Kind::Bc4 ? 8u : 16u;
}

uint32_t PixelBytes(Kind k)
{
    switch (k) {
    case Kind::Rgba8:
    case Kind::Bgra8:
    case Kind::Bgrx8: return 4;
    case Kind::Rg8: return 2;
    case Kind::R8:
    case Kind::A8: return 1;
    default: return 0;
    }
}

using Texel = std::array<float, 4>;

float SrgbToLinear(float c)
{
    return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}

void DecodeBlock(Kind k, const uint8_t* b, std::array<Texel, 16>& out)
{
    if (k == Kind::Bc4 || k == Kind::Bc5) {
        uint8_t v[32] = {};
        if (k == Kind::Bc4)
            bcdec_bc4(b, v, 4);
        else
            bcdec_bc5(b, v, 8);
        for (int i = 0; i < 16; ++i)
            out[i] = k == Kind::Bc4 ? Texel{ v[i] / 255.f, 0.f, 0.f, 1.f }
                                    : Texel{ v[i * 2] / 255.f, v[i * 2 + 1] / 255.f, 0.f, 1.f };
        return;
    }
    if (k == Kind::Bc6) {
        float v[48] = {};
        bcdec_bc6h_float(b, v, 12, 0);
        for (int i = 0; i < 16; ++i) out[i] = Texel{ v[i * 3], v[i * 3 + 1], v[i * 3 + 2], 1.f };
        return;
    }
    uint8_t px[64] = {};
    if (k == Kind::Bc1)
        bcdec_bc1(b, px, 16);
    else if (k == Kind::Bc2)
        bcdec_bc2(b, px, 16);
    else if (k == Kind::Bc3)
        bcdec_bc3(b, px, 16);
    else
        bcdec_bc7(b, px, 16);
    for (int i = 0; i < 16; ++i)
        out[i] = Texel{ px[i * 4] / 255.f, px[i * 4 + 1] / 255.f, px[i * 4 + 2] / 255.f, px[i * 4 + 3] / 255.f };
}

Texel DecodePixel(Kind k, const uint8_t* p)
{
    switch (k) {
    case Kind::Rgba8: return Texel{ p[0] / 255.f, p[1] / 255.f, p[2] / 255.f, p[3] / 255.f };
    case Kind::Bgra8: return Texel{ p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, p[3] / 255.f };
    case Kind::Bgrx8: return Texel{ p[2] / 255.f, p[1] / 255.f, p[0] / 255.f, 1.f };
    case Kind::R8: return Texel{ p[0] / 255.f, 0.f, 0.f, 1.f };
    case Kind::Rg8: return Texel{ p[0] / 255.f, p[1] / 255.f, 0.f, 1.f };
    case Kind::A8: return Texel{ 0.f, 0.f, 0.f, p[0] / 255.f };
    default: return Texel{};
    }
}

class Sampler {
public:
    Sampler(const Mip& m, Kind k, bool srgb) : m_(m), k_(k), srgb_(srgb) {}

    bool Sample(float u, float v, Texel& out)
    {
        const float fx = u * (float)m_.w - 0.5f;
        const float fy = v * (float)m_.h - 0.5f;
        const float x0f = std::floor(fx);
        const float y0f = std::floor(fy);
        const float ax = Quantise(fx - x0f);
        const float ay = Quantise(fy - y0f);
        const int x0 = (int)x0f;
        const int y0 = (int)y0f;
        Texel t00;
        Texel t10;
        Texel t01;
        Texel t11;
        if (!Fetch(x0, y0, t00) || !Fetch(x0 + 1, y0, t10) || !Fetch(x0, y0 + 1, t01) || !Fetch(x0 + 1, y0 + 1, t11))
            return false;
        for (int c = 0; c < 4; ++c) {
            const float top = t00[c] + (t10[c] - t00[c]) * ax;
            const float bottom = t01[c] + (t11[c] - t01[c]) * ax;
            out[c] = top + (bottom - top) * ay;
        }
        return true;
    }

private:
    static float Quantise(float a) { return std::floor(a * 256.f + 0.5f) / 256.f; }

    bool Fetch(int x, int y, Texel& out)
    {
        const uint32_t cx = (uint32_t)std::clamp(x, 0, (int)m_.w - 1);
        const uint32_t cy = (uint32_t)std::clamp(y, 0, (int)m_.h - 1);
        if (Block(k_)) {
            const uint32_t bx = cx / 4;
            const uint32_t by = cy / 4;
            const uint64_t key = ((uint64_t)by << 32) | bx;
            auto it = blocks_.find(key);
            if (it == blocks_.end()) {
                const uint8_t* row = m_.row(by);
                if (!row) return false;
                std::array<Texel, 16> d{};
                DecodeBlock(k_, row + (size_t)bx * BlockBytes(k_), d);
                if (srgb_)
                    for (Texel& t : d)
                        for (int c = 0; c < 3; ++c) t[c] = SrgbToLinear(t[c]);
                it = blocks_.emplace(key, d).first;
            }
            out = it->second[(cy % 4) * 4 + (cx % 4)];
            return true;
        }
        const uint8_t* row = m_.row(cy);
        if (!row) return false;
        out = DecodePixel(k_, row + (size_t)cx * PixelBytes(k_));
        if (srgb_)
            for (int c = 0; c < 3; ++c) out[c] = SrgbToLinear(out[c]);
        return true;
    }

    const Mip& m_;
    Kind k_;
    bool srgb_;
    std::unordered_map<uint64_t, std::array<Texel, 16>> blocks_;
};

float Luma(const Texel& t)
{
    return t[0] * 0.25f + t[1] * 0.25f + t[2] * 0.25f + t[3] * 0.25f;
}

uint32_t Thermometer16(float c)
{
    const float s = std::clamp(c, 0.f, 1.f);
    const uint32_t q = (uint32_t)std::floor(s * 16.f + 0.5f);
    return (1u << q) - 1u;
}

} // namespace

bool Decodes(uint32_t fmt)
{
    return KindOf(fmt) != Kind::None;
}

bool HasSrgbView(uint32_t fmt)
{
    switch (KindOf(fmt)) {
    case Kind::Bc1:
    case Kind::Bc2:
    case Kind::Bc3:
    case Kind::Bc7:
    case Kind::Rgba8:
    case Kind::Bgra8:
    case Kind::Bgrx8: return true;
    default: return false;
    }
}

bool IsSrgb(uint32_t view_fmt)
{
    switch (view_fmt) {
    case 29:
    case 72:
    case 75:
    case 78:
    case 91:
    case 93:
    case 99: return true;
    default: return false;
    }
}

void RowsRead(uint32_t w, uint32_t h, uint32_t fmt, std::function<void(uint32_t row)> fn)
{
    const Kind k = KindOf(fmt);
    if (k == Kind::None || !w || !h) return;
    std::set<uint32_t> rows;
    auto add_y = [&](float v) {
        const float fy = v * (float)h - 0.5f;
        const int y0 = (int)std::floor(fy);
        for (int y = y0; y <= y0 + 1; ++y) {
            const uint32_t cy = (uint32_t)std::clamp(y, 0, (int)h - 1);
            rows.insert(Block(k) ? cy / 4 : cy);
        }
    };
    for (uint32_t y = 0; y < 8; ++y) add_y(((float)y + 0.5f) / 8.f);
    add_y(0.5f);
    for (uint32_t r : rows) fn(r);
}

bool Hash(const Mip& m, bool srgb, uint64_t* out)
{
    const Kind k = KindOf(m.fmt);
    if (k == Kind::None || !m.w || !m.h || !m.row || !out) return false;
    Sampler s(m, k, srgb && HasSrgbView(m.fmt));
    float value[72] = {};
    for (uint32_t y = 0; y < 8; ++y)
        for (uint32_t x = 0; x < 9; ++x) {
            Texel t{};
            if (!s.Sample(((float)x + 0.5f) / 9.f, ((float)y + 0.5f) / 8.f, t)) return false;
            value[y * 9 + x] = Luma(t);
        }
    uint64_t h = 0;
    for (uint32_t i = 0; i < 64; ++i) {
        const uint32_t at = i + (i >> 3);
        if (value[at] > value[at + 1]) h |= 1ull << i;
    }
    if (!h) {
        Texel c{};
        if (!s.Sample(0.5f, 0.5f, c)) return false;
        const uint64_t low = ((uint64_t)Thermometer16(c[2]) << 16) | Thermometer16(c[3]);
        const uint64_t high = ((uint64_t)Thermometer16(c[0]) << 16) | Thermometer16(c[1]);
        h = low | (high << 32);
    }
    *out = h;
    return true;
}

} // namespace texdhash
