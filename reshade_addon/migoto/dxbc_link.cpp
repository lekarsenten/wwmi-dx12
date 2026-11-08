#include "dxbc_link.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <map>

namespace MigotoPort {
namespace {

constexpr uint32_t FourCC(const char (&s)[5])
{
    return (uint32_t)(uint8_t)s[0] | ((uint32_t)(uint8_t)s[1] << 8) | ((uint32_t)(uint8_t)s[2] << 16) |
        ((uint32_t)(uint8_t)s[3] << 24);
}

uint32_t Rd(const uint8_t* p)
{
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

void Wr(uint8_t* p, uint32_t v)
{
    memcpy(p, &v, 4);
}

uint32_t Rotl(uint32_t x, uint32_t c)
{
    return (x << c) | (x >> (32 - c));
}

void Md5Block(uint32_t h[4], const uint8_t* block)
{
    static const uint32_t k[64] = { 0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613,
        0xfd469501, 0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6,
        0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681,
        0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70, 0x289b7ec6, 0xeaa127fa, 0xd4ef3085,
        0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039,
        0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82,
        0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint32_t r[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
    uint32_t m[16];
    memcpy(m, block, sizeof m);
    uint32_t a = h[0];
    uint32_t b = h[1];
    uint32_t c = h[2];
    uint32_t d = h[3];
    for (uint32_t i = 0; i < 64; ++i) {
        uint32_t f;
        uint32_t g;
        if (i < 16) {
            f = (b & c) | (~b & d);
            g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c);
            g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d;
            g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d);
            g = (7 * i) % 16;
        }
        const uint32_t t = d;
        d = c;
        c = b;
        b += Rotl(a + f + k[i] + m[g], r[(i / 16) * 4 + i % 4]);
        a = t;
    }
    h[0] += a;
    h[1] += b;
    h[2] += c;
    h[3] += d;
}

// The checksum a DXBC container carries after its magic: MD5 over the rest, padded its own way.
void DxbcChecksum(const uint8_t* dxbc, size_t size, uint32_t out[4])
{
    const uint8_t* data = dxbc + 20;
    const uint32_t n = (uint32_t)(size - 20);
    uint32_t h[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 };
    const uint32_t full = n & ~63u;
    for (uint32_t i = 0; i < full; i += 64) Md5Block(h, data + i);
    const uint32_t left = n - full;
    uint8_t block[64] = {};
    if (left >= 56) {
        memcpy(block, data + full, left);
        block[left] = 0x80;
        Md5Block(h, block);
        memset(block, 0, sizeof block);
    } else {
        memcpy(block + 4, data + full, left);
        block[4 + left] = 0x80;
    }
    Wr(block, n << 3);
    Wr(block + 60, (n * 2) | 1);
    Md5Block(h, block);
    memcpy(out, h, sizeof h);
}

struct Chunk {
    uint32_t fourcc;
    uint32_t off; // of its header
    uint32_t size;
};

bool Chunks(const uint8_t* b, size_t n, std::vector<Chunk>& out)
{
    if (!b || n < 32 || Rd(b) != FourCC("DXBC") || Rd(b + 24) > n) return false;
    n = Rd(b + 24);
    const uint32_t count = Rd(b + 28);
    if (32 + (uint64_t)count * 4 > n) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint32_t off = Rd(b + 32 + 4 * i);
        if ((uint64_t)off + 8 > n) return false;
        const uint32_t size = Rd(b + off + 4);
        if ((uint64_t)off + 8 + size > n) return false;
        out.push_back({ Rd(b + off), off, size });
    }
    return true;
}

const Chunk* Find(const std::vector<Chunk>& cs, uint32_t fourcc)
{
    for (const Chunk& c : cs)
        if (c.fourcc == fourcc) return &c;
    return nullptr;
}

enum class SigFormat { kBasic, kStream, kPrecision }; // ISGN/OSGN, OSG5, ISG1/OSG1

size_t ElemSize(SigFormat f)
{
    return f == SigFormat::kBasic ? 24 : f == SigFormat::kStream ? 28 : 32;
}

struct SigElem {
    uint32_t stream = 0;
    std::string name;
    uint32_t index = 0;
    uint32_t sv = 0;
    uint32_t ctype = 0;
    uint32_t reg = 0;
    uint8_t mask = 0;
    uint8_t rw = 0;
    uint32_t precision = 0;
};

constexpr uint32_t kSvPosition = 1;

bool ParseSig(const uint8_t* d, uint32_t size, SigFormat f, std::vector<SigElem>& out)
{
    if (size < 8) return false;
    const uint32_t count = Rd(d);
    const uint32_t first = Rd(d + 4);
    const size_t es = ElemSize(f);
    if ((uint64_t)first + (uint64_t)count * es > size) return false;
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* e = d + first + i * es;
        const size_t p = f == SigFormat::kBasic ? 0 : 4;
        SigElem x;
        if (p) x.stream = Rd(e);
        const uint32_t name = Rd(e + p);
        x.index = Rd(e + p + 4);
        x.sv = Rd(e + p + 8);
        x.ctype = Rd(e + p + 12);
        x.reg = Rd(e + p + 16);
        x.mask = e[p + 20];
        x.rw = e[p + 21];
        if (f == SigFormat::kPrecision) x.precision = Rd(e + p + 24);
        if (name >= size) return false;
        const void* z = memchr(d + name, 0, size - name);
        if (!z) return false;
        x.name.assign((const char*)d + name, (const char*)z);
        out.push_back(std::move(x));
    }
    return true;
}

std::vector<uint8_t> WriteSig(const std::vector<SigElem>& es, SigFormat f)
{
    const size_t esz = ElemSize(f);
    std::vector<uint8_t> d(8 + es.size() * esz);
    Wr(&d[0], (uint32_t)es.size());
    Wr(&d[4], 8);
    std::map<std::string, uint32_t> names;
    for (const SigElem& e : es) {
        if (!names.emplace(e.name, (uint32_t)d.size()).second) continue;
        d.insert(d.end(), e.name.begin(), e.name.end());
        d.push_back(0);
    }
    while (d.size() % 4) d.push_back(0xAB);
    for (size_t i = 0; i < es.size(); ++i) {
        const SigElem& e = es[i];
        uint8_t* x = &d[8 + i * esz];
        const size_t p = f == SigFormat::kBasic ? 0 : 4;
        if (p) Wr(x, e.stream);
        Wr(x + p, names[e.name]);
        Wr(x + p + 4, e.index);
        Wr(x + p + 8, e.sv);
        Wr(x + p + 12, e.ctype);
        Wr(x + p + 16, e.reg);
        x[p + 20] = e.mask;
        x[p + 21] = e.rw;
        if (f == SigFormat::kPrecision) Wr(x + p + 24, e.precision);
    }
    return d;
}

bool SameName(const std::string& a, const std::string& b)
{
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return std::tolower((unsigned char)x) == std::tolower((unsigned char)y);
    });
}

bool SameElem(const SigElem& a, const SigElem& b)
{
    return a.index == b.index && SameName(a.name, b.name);
}

std::string Text(const SigElem& e)
{
    std::string s = e.name + std::to_string(e.index) + " v" + std::to_string(e.reg) + ".";
    for (int c = 0; c < 4; ++c)
        if (e.mask & (1 << c)) s += "xyzw"[c];
    return s;
}

// The rasterized outputs of the stage before the pixel shader.
bool ReadOutputs(const uint8_t* b, const std::vector<Chunk>& cs, std::vector<SigElem>& out)
{
    const Chunk* c = Find(cs, FourCC("OSG5"));
    SigFormat f = SigFormat::kStream;
    if (!c) {
        c = Find(cs, FourCC("OSG1"));
        f = SigFormat::kPrecision;
    }
    if (!c) {
        c = Find(cs, FourCC("OSGN"));
        f = SigFormat::kBasic;
    }
    std::vector<SigElem> all;
    if (!c || !ParseSig(b + c->off + 8, c->size, f, all)) return false;
    for (SigElem& e : all)
        if (e.stream == 0) out.push_back(std::move(e));
    return true;
}

// Whether D3D12 takes the pair as it is: each input the rasterizer does not make sits where its output does.
bool Links(const std::vector<SigElem>& outs, const std::vector<SigElem>& ins)
{
    for (const SigElem& e : ins) {
        const SigElem* u = nullptr;
        for (const SigElem& o : outs)
            if (SameElem(o, e)) u = &o;
        if (!u) {
            if (e.sv != 0 && e.sv != kSvPosition) continue;
            return false;
        }
        if (u->reg != e.reg || (e.mask & ~u->mask)) return false;
    }
    return true;
}

struct LinkPlan {
    std::vector<SigElem> inputs; // the new input signature
    std::map<uint32_t, uint32_t> moves; // input register -> the one it is read from now
};

bool Plan(const std::vector<SigElem>& outs, const std::vector<SigElem>& ins, LinkPlan& plan, std::string& note)
{
    for (const SigElem& e : ins) {
        if (e.sv == kSvPosition) {
            const SigElem* u = nullptr;
            for (const SigElem& o : outs)
                if (o.sv == kSvPosition) u = &o;
            if (!u) {
                note = "the stage before writes no SV_Position";
                return false;
            }
            SigElem n = e;
            n.reg = u->reg;
            if (n.reg != e.reg) {
                plan.moves[e.reg] = n.reg;
                note += " " + Text(e) + " -> v" + std::to_string(n.reg) + ";";
            }
            plan.inputs.push_back(std::move(n));
            continue;
        }
        if (e.sv != 0) {
            plan.inputs.push_back(e);
            continue;
        }
        uint8_t covered = 0;
        for (const SigElem& o : outs) {
            if (o.reg != e.reg || !(o.mask & e.mask)) continue;
            if (o.sv != 0) {
                note = Text(e) + " would read " + o.name + ", a system value";
                return false;
            }
            SigElem n = e;
            n.name = o.name;
            n.index = o.index;
            n.ctype = o.ctype;
            n.mask = e.mask & o.mask;
            n.rw = e.rw & o.mask;
            covered |= o.mask;
            if (!SameElem(n, e)) note += " " + Text(e) + " -> " + o.name + std::to_string(o.index) + ";";
            plan.inputs.push_back(std::move(n));
        }
        if (e.rw & ~covered) {
            note = Text(e) + " reads what the stage before does not write there";
            return false;
        }
    }
    std::vector<SigElem> merged;
    for (SigElem& e : plan.inputs) {
        auto it = std::find_if(merged.begin(), merged.end(),
            [&](const SigElem& m) { return m.reg == e.reg && SameElem(m, e) && m.sv == e.sv; });
        if (it == merged.end()) {
            merged.push_back(std::move(e));
        } else {
            it->mask |= e.mask;
            it->rw |= e.rw;
        }
    }
    auto low = [](int m) { return m ? (m & -m) : 16; };
    std::stable_sort(merged.begin(), merged.end(),
        [&](const SigElem& a, const SigElem& b) { return a.reg != b.reg ? a.reg < b.reg : low(a.mask) < low(b.mask); });
    for (size_t i = 0; i < merged.size(); ++i)
        for (size_t j = i + 1; j < merged.size(); ++j)
            if (merged[i].reg == merged[j].reg && (merged[i].mask & merged[j].mask)) {
                note = Text(merged[i]) + " and " + Text(merged[j]) + " would overlap";
                return false;
            }
    plan.inputs = std::move(merged);
    return true;
}

constexpr uint32_t kOpCustomData = 53;
constexpr uint32_t kOpDclIndexRange = 91;
constexpr uint32_t kOpDclInput = 95;
constexpr uint32_t kOpDclInputPsSiv = 100;
constexpr uint32_t kOpInterfaceCall = 120;
constexpr uint32_t kOperandInput = 1;
constexpr uint32_t kOperandImm32 = 4;
constexpr uint32_t kOperandImm64 = 5;

bool IsDeclaration(uint32_t op)
{
    return (op >= 88 && op <= 106) || (op >= 143 && op <= 162) || op == 206;
}

bool IsInputDeclaration(uint32_t op)
{
    return op == kOpDclIndexRange || (op >= kOpDclInput && op <= kOpDclInputPsSiv);
}

struct OperandAt {
    size_t token;
    size_t index; // of its first index's immediate; SIZE_MAX when it has none
    uint32_t type;
    uint32_t dim;
    bool relative;
};

// The operand at t[p] and the operands its relative indices hold, into `seen`; p ends past it.
bool ParseOperand(const uint32_t* t, size_t end, size_t& p, std::vector<OperandAt>& seen)
{
    if (p >= end) return false;
    const size_t at = p;
    const uint32_t tok = t[p++];
    const uint32_t comps = tok & 3;
    const uint32_t type = (tok >> 12) & 0xFF;
    const uint32_t dim = (tok >> 20) & 3;
    bool more = (tok >> 31) != 0;
    while (more) {
        if (p >= end) return false;
        more = (t[p++] >> 31) != 0;
    }
    if (type == kOperandImm32 || type == kOperandImm64) {
        const size_t n = comps == 1 ? 1 : comps == 2 ? 4 : 0;
        if (!n) return false;
        p += n * (type == kOperandImm64 ? 2 : 1);
        return p <= end;
    }
    OperandAt o{ at, SIZE_MAX, type, dim, false };
    for (uint32_t i = 0; i < dim; ++i) {
        const uint32_t rep = (tok >> (22 + 3 * i)) & 7;
        if (rep > 4) return false;
        if (rep == 0 || rep == 3) {
            if (i == 0) o.index = p;
            p += 1;
        } else if (rep == 1 || rep == 4) {
            p += 2;
        }
        if (p > end) return false;
        if (rep >= 2) {
            o.relative = true;
            if (!ParseOperand(t, end, p, seen)) return false;
        }
    }
    seen.push_back(o);
    return true;
}

// Each instruction of a program (its version and length tokens first) parsed, `f` given its opcode, its extent and
// its operands; a declaration other than an input's is handed over unparsed.
template <class F> bool ForInstructions(const std::vector<uint32_t>& t, F&& f)
{
    if (t.size() < 2 || t[1] != t.size()) return false;
    std::vector<OperandAt> seen;
    for (size_t p = 2; p < t.size();) {
        const uint32_t op = t[p] & 0x7FF;
        const size_t len = op == kOpCustomData ? (p + 1 < t.size() ? t[p + 1] : 0) : (t[p] >> 24) & 0x7F;
        if (len == 0 || p + len > t.size()) return false;
        const size_t end = p + len;
        seen.clear();
        if (op == kOpInterfaceCall) return false;
        const bool input_dcl = IsInputDeclaration(op);
        if (input_dcl || (op != kOpCustomData && !IsDeclaration(op))) {
            size_t q = p + 1;
            bool more = (t[p] >> 31) != 0;
            while (more) {
                if (q >= end) return false;
                more = (t[q++] >> 31) != 0;
            }
            if (input_dcl) {
                if (!ParseOperand(t.data(), end, q, seen)) return false;
            } else {
                while (q < end)
                    if (!ParseOperand(t.data(), end, q, seen)) return false;
                if (q != end) return false;
            }
        }
        if (!f(op, p, end, seen)) return false;
        p = end;
    }
    return true;
}

bool Relink(const std::vector<uint32_t>& in, const LinkPlan& plan, std::vector<uint32_t>& out)
{
    out.assign(in.begin(), in.begin() + 2);
    bool input_range = false;
    bool rewrote = !plan.moves.empty();
    const bool ok = ForInstructions(in, [&](uint32_t op, size_t p, size_t end, const std::vector<OperandAt>& seen) {
        std::vector<uint32_t> ins(in.begin() + p, in.begin() + end);
        for (const OperandAt& o : seen) {
            if (o.type != kOperandInput) continue;
            if (op == kOpDclIndexRange) {
                input_range = true;
                continue;
            }
            if (o.relative || o.dim != 1 || o.index == SIZE_MAX) {
                if (plan.moves.empty()) continue;
                return false;
            }
            const auto m = plan.moves.find(in[o.index]);
            if (m != plan.moves.end()) ins[o.index - p] = m->second;
        }
        const OperandAt* d = (op >= kOpDclInput && op <= kOpDclInputPsSiv && seen.size() == 1 &&
                                 seen[0].type == kOperandInput && seen[0].index != SIZE_MAX)
            ? &seen[0]
            : nullptr;
        const uint32_t tok = d ? in[d->token] : 0;
        if (!d || (tok & 3) != 2 || ((tok >> 2) & 3) != 0) {
            out.insert(out.end(), ins.begin(), ins.end());
            return true;
        }
        const uint32_t reg = ins[d->index - p];
        const uint8_t mask = (uint8_t)((tok >> 4) & 0xF);
        std::vector<uint8_t> parts;
        for (const SigElem& e : plan.inputs)
            if (e.reg == reg && (e.mask & mask)) parts.push_back(e.mask & mask);
        if (parts.size() < 2) {
            out.insert(out.end(), ins.begin(), ins.end());
            return true;
        }
        rewrote = true;
        for (const uint8_t m : parts) {
            ins[d->token - p] = (tok & ~(0xFu << 4)) | ((uint32_t)m << 4);
            out.insert(out.end(), ins.begin(), ins.end());
        }
        return true;
    });
    if (!ok || (input_range && rewrote)) return false;
    out[1] = (uint32_t)out.size();
    return true;
}

bool Program(const uint8_t* b, const std::vector<Chunk>& cs, const Chunk*& c, std::vector<uint32_t>& t)
{
    c = Find(cs, FourCC("SHEX"));
    if (!c) c = Find(cs, FourCC("SHDR"));
    if (!c || c->size < 8) return false;
    const uint32_t len = Rd(b + c->off + 12);
    if (len < 2 || (uint64_t)len * 4 > c->size) return false;
    t.resize(len);
    memcpy(t.data(), b + c->off + 8, (size_t)len * 4);
    return true;
}

std::vector<uint8_t> Rebuild(const uint8_t* b, const std::vector<Chunk>& cs, const Chunk* sig_chunk,
    const std::vector<uint8_t>& sig, const Chunk* prog_chunk, const std::vector<uint32_t>& prog)
{
    std::vector<uint8_t> out(32 + 4 * cs.size());
    memcpy(out.data(), b, 32);
    for (size_t i = 0; i < cs.size(); ++i) {
        const Chunk& c = cs[i];
        Wr(&out[32 + 4 * i], (uint32_t)out.size());
        const uint8_t* data = b + c.off + 8;
        size_t n = c.size;
        if (&c == sig_chunk) {
            data = sig.data();
            n = sig.size();
        } else if (&c == prog_chunk) {
            data = (const uint8_t*)prog.data();
            n = prog.size() * 4;
        }
        const size_t at = out.size();
        out.resize(at + 8 + n);
        Wr(&out[at], c.fourcc);
        Wr(&out[at + 4], (uint32_t)n);
        memcpy(&out[at + 8], data, n);
    }
    Wr(&out[24], (uint32_t)out.size());
    uint32_t h[4];
    DxbcChecksum(out.data(), out.size(), h);
    memcpy(&out[4], h, sizeof h);
    return out;
}

} // namespace

PixelLink LinkPixelInputs(const uint8_t* upstream, size_t upstream_size, const uint8_t* ps, size_t ps_size,
    std::vector<uint8_t>& out, std::string& note)
{
    note.clear();
    std::vector<Chunk> uc;
    std::vector<Chunk> pc;
    std::vector<SigElem> outs;
    if (!Chunks(upstream, upstream_size, uc) || !Chunks(ps, ps_size, pc) || !ReadOutputs(upstream, uc, outs))
        return PixelLink::kLinks;
    const Chunk* in_chunk = Find(pc, FourCC("ISG1"));
    SigFormat in_format = SigFormat::kPrecision;
    if (!in_chunk) {
        in_chunk = Find(pc, FourCC("ISGN"));
        in_format = SigFormat::kBasic;
    }
    std::vector<SigElem> ins;
    if (!in_chunk || !ParseSig(ps + in_chunk->off + 8, in_chunk->size, in_format, ins)) return PixelLink::kLinks;
    if (Links(outs, ins)) return PixelLink::kLinks;
    LinkPlan plan;
    if (!Plan(outs, ins, plan, note)) return PixelLink::kImpossible;
    const Chunk* prog_chunk = nullptr;
    std::vector<uint32_t> prog;
    std::vector<uint32_t> relinked;
    if (!Program(ps, pc, prog_chunk, prog) || !Relink(prog, plan, relinked)) {
        note = "its program could not be rewritten:" + note;
        return PixelLink::kImpossible;
    }
    if (!Links(outs, plan.inputs)) {
        note = "the rewrite would still not link:" + note;
        return PixelLink::kImpossible;
    }
    out = Rebuild(ps, pc, in_chunk, WriteSig(plan.inputs, in_format), prog_chunk, relinked);
    return PixelLink::kRewritten;
}

} // namespace MigotoPort
