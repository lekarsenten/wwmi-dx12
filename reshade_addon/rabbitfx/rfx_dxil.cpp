// rfx_dxil.cpp -- OURS. See rfx_dxil.h.
#include <windows.h>
#include <dxcapi.h>
#include <mutex>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "rfx_dxil.h"

namespace {

HMODULE g_dxc = nullptr;
HMODULE g_dxil = nullptr;
DxcCreateInstanceProc g_dxc_create = nullptr;
DxcCreateInstanceProc g_dxil_create = nullptr;
bool g_init_tried = false;
bool g_init_ok = false;
std::string g_init_why;
std::mutex g_mtx; // init, and one patch at a time: a handful of shaders per session

template <class T> struct Com {
    T* p = nullptr;
    ~Com()
    {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

std::string BlobText(IDxcBlob* b)
{
    if (!b || !b->GetBufferSize()) return {};
    std::string s((const char*)b->GetBufferPointer(), b->GetBufferSize());
    while (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}
std::string ErrText(IDxcOperationResult* r)
{
    Com<IDxcBlobEncoding> e;
    if (r && SUCCEEDED(r->GetErrorBuffer(&e)) && e) return BlobText(e.p);
    return {};
}

struct Module {
    std::string t;
    int max_node = -1;

    void Scan()
    {
        max_node = -1;
        for (size_t i = 0; (i = t.find("\n!", i)) != std::string::npos; ++i) {
            size_t j = i + 2;
            int n = 0;
            bool any = false;
            while (j < t.size() && t[j] >= '0' && t[j] <= '9') {
                n = n * 10 + (t[j] - '0');
                ++j;
                any = true;
            }
            if (any && t.compare(j, 3, " = ") == 0 && n > max_node) max_node = n;
        }
    }
    // [begin, end) of the line that defines `!n`; npos when there is none.
    bool NodeSpan(int n, size_t& b, size_t& e) const
    {
        const std::string key = "\n!" + std::to_string(n) + " = ";
        const size_t i = t.find(key);
        if (i == std::string::npos) return false;
        b = i + 1;
        e = t.find('\n', b);
        if (e == std::string::npos) e = t.size();
        return true;
    }
    std::string Node(int n) const
    {
        size_t b;
        size_t e;
        return NodeSpan(n, b, e) ? t.substr(b, e - b) : std::string();
    }
    void SetNode(int n, const std::string& line)
    {
        size_t b;
        size_t e;
        if (NodeSpan(n, b, e)) t.replace(b, e - b, line);
    }
    int AddNode(const std::string& body)
    { // body = `!{...}`
        const int n = ++max_node;
        if (!t.empty() && t.back() != '\n') t += '\n';
        t += "!" + std::to_string(n) + " = " + body + "\n";
        return n;
    }
};

// The operands of `!N = !{a, b, c}` split at top-level commas (quotes, <>, [], {} and () nest).
std::vector<std::string> Operands(const std::string& node)
{
    std::vector<std::string> out;
    const size_t open = node.find("!{");
    const size_t close = node.rfind('}');
    if (open == std::string::npos || close == std::string::npos || close < open + 2) return out;
    int depth = 0;
    bool quote = false;
    std::string cur;
    for (size_t i = open + 2; i < close; ++i) {
        const char c = node[i];
        if (quote) {
            cur += c;
            if (c == '"') quote = false;
            continue;
        }
        if (c == '"') {
            quote = true;
            cur += c;
            continue;
        }
        if (c == '<' || c == '[' || c == '{' || c == '(') ++depth;
        if (c == '>' || c == ']' || c == '}' || c == ')') --depth;
        if (c == ',' && depth == 0) {
            out.push_back(cur);
            cur.clear();
            continue;
        }
        cur += c;
    }
    out.push_back(cur);
    for (auto& s : out) {
        size_t a = s.find_first_not_of(' ');
        size_t b = s.find_last_not_of(' ');
        s = (a == std::string::npos) ? std::string() : s.substr(a, b - a + 1);
    }
    return out;
}
// `!12` -> 12, `null` -> -1, anything else -> -2.
int NodeRef(const std::string& op)
{
    if (op == "null") return -1;
    if (op.size() < 2 || op[0] != '!') return -2;
    int n = 0;
    for (size_t i = 1; i < op.size(); ++i) {
        if (op[i] < '0' || op[i] > '9') return -2;
        n = n * 10 + (op[i] - '0');
    }
    return n;
}
// `i32 51` / `i8 3` -> the integer; false when the operand is not an integer constant.
bool IntOf(const std::string& op, long long& v)
{
    const size_t sp = op.find(' ');
    if (sp == std::string::npos || op[0] != 'i') return false;
    char* end = nullptr;
    v = strtoll(op.c_str() + sp + 1, &end, 10);
    return end && *end == '\0';
}
std::string Join(const std::vector<std::string>& ops)
{
    std::string s = "!{";
    for (size_t i = 0; i < ops.size(); ++i) {
        if (i) s += ", ";
        s += ops[i];
    }
    return s + "}";
}

const char kTexType[] = "%\"class.Texture2D<vector<float, 4> >\"";
const char kMipsType[] = "%\"class.Texture2D<vector<float, 4> >::mips_type\"";
const char kBufType[] = "%\"class.Buffer<vector<float, 4> >\"";

#include "rfx_glow_ir.inc"

void ReplaceAll(std::string& s, const std::string& from, const std::string& to)
{
    for (size_t i = 0; (i = s.find(from, i)) != std::string::npos; i += to.size()) s.replace(i, from.size(), to);
}

// The censor's discard chains, in the disassembly text: the opacity mask, the dither gate and the vertex mask.
std::string Decensor(Module& m, int in_list_node, RfxDxilResult& out)
{
    out.decensored = 0;
    out.degated = 0;
    out.unmasked = 0;
    auto def_of = [&](const std::string& name) -> std::string { // the text of `  <name> = ...` (no newline)
        const std::string key = "\n  " + name + " = ";
        const size_t p = m.t.find(key);
        if (p == std::string::npos) return std::string();
        const size_t e = m.t.find('\n', p + 1);
        return m.t.substr(p + key.size(), e - (p + key.size()));
    };
#ifdef WWMI_DEBUG
    auto cb_ref = [&](const std::string& v) -> RfxCbRef {
        RfxCbRef r;
        static const char kEv[] = "extractvalue %dx.types.CBufRet.f32 %";
        static const char kHandle[] = "%dx.types.Handle %";
        static const char kCreate[] = "call %dx.types.Handle @dx.op.createHandle(i32 57, i8 2, i32 ";
        const std::string d = def_of(v);
        if (d.rfind(kEv, 0) != 0) return r;
        const size_t comma = d.rfind(',');
        const std::string ld = def_of(d.substr(sizeof(kEv) - 2, comma - (sizeof(kEv) - 2)));
        const size_t h = ld.find(kHandle);
        if (ld.find("@dx.op.cbufferLoadLegacy.f32(") == std::string::npos || h == std::string::npos) return r;
        const size_t h0 = h + sizeof(kHandle) - 2;
        const size_t h1 = ld.find(',', h0);
        const std::string hd = def_of(ld.substr(h0, h1 - h0));
        const size_t i0 = hd.rfind(kCreate, 0) == 0 ? hd.find(", i32 ", sizeof(kCreate) - 1) : std::string::npos;
        if (i0 == std::string::npos || !isdigit((unsigned char)hd[i0 + 6]) || !isdigit((unsigned char)ld[h1 + 6]))
            return r;
        r.reg = (uint8_t)std::strtoul(hd.c_str() + i0 + 6, nullptr, 10);
        r.row = (uint16_t)std::strtoul(ld.c_str() + h1 + 6, nullptr, 10);
        r.comp = (uint8_t)std::strtoul(d.c_str() + comma + 2, nullptr, 10);
        return r;
    };
    out.opacity.clear();
#endif
    auto cb_of = [&](const std::string& v) -> std::string {
        static const char kEv[] = "extractvalue %dx.types.CBufRet.f32 %";
        const std::string d = def_of(v);
        if (d.rfind(kEv, 0) != 0) return std::string();
        const std::string ld = def_of(d.substr(sizeof(kEv) - 2, d.rfind(',') - (sizeof(kEv) - 2)));
        const size_t h = ld.find("%dx.types.Handle %");
        if (ld.find("@dx.op.cbufferLoadLegacy.f32(") == std::string::npos || h == std::string::npos)
            return std::string();
        return ld.substr(h, ld.find(',', h) - h);
    };
    out.unveiled = 0;
    static const char kUlt[] = " = fcmp fast ult float %";
    static const char kHalf[] = ", 5.000000e-01";
    static const char kLabel[] = ", label %";
    static const char kMin[] = " = call float @dx.op.binary.f32(i32 36, float %";
    for (size_t p = 0; (p = m.t.find(kUlt, p)) != std::string::npos; p += sizeof(kUlt) - 1) {
        const size_t ls = m.t.rfind('\n', p) + 1;
        const size_t le = m.t.find('\n', p);
        const std::string line = m.t.substr(ls, le - ls);
        const size_t vh = line.size() - (sizeof(kHalf) - 1);
        if (line.rfind("  %", 0) != 0 || line.size() <= sizeof(kHalf) ||
            line.compare(vh, sizeof(kHalf) - 1, kHalf) != 0)
            continue;
        const std::string c = line.substr(2, line.find(' ', 2) - 2);
        const size_t v0 = line.find('%', 3);
        const std::string sw = line.substr(v0, vh - v0);
        if (cb_of(sw).empty()) continue;
        const std::string br = "\n  br i1 " + c + kLabel;
        const size_t bp = m.t.find(br);
        if (bp == std::string::npos) continue;
        const size_t s0 = bp + br.size();
        const size_t s1 = m.t.find_first_of(",\n", s0);
        if (s1 == std::string::npos) continue;
        const std::string own_label = "\n; <label>:" + m.t.substr(s0, s1 - s0);
        size_t bs = m.t.find(own_label + " ");
        if (bs == std::string::npos) bs = m.t.find(own_label + "\n");
        if (bs == std::string::npos) continue;
        bs += own_label.size();
        const size_t be = (std::min)(m.t.find("\n; <label>:", bs), m.t.find("\n}\n", bs));
        for (size_t q = m.t.find(kMin, bs); q != std::string::npos && q < be; q = m.t.find(kMin, q + 1)) {
            const size_t a0 = q + sizeof(kMin) - 2;
            const size_t a1 = m.t.find(',', a0);
            const size_t b0 = m.t.find('%', a1);
            const size_t b1 = m.t.find(')', b0);
            const std::string a = m.t.substr(a0, a1 - a0);
            const std::string b = m.t.substr(b0, b1 - b0);
            const std::string cb = cb_of(a);
            if (cb.empty() || cb != cb_of(b)) continue;
#ifdef WWMI_DEBUG
            out.opacity.push_back({ cb_ref(sw), cb_ref(a), cb_ref(b) });
#endif
            m.t.replace(a0, a.size(), b);
            ++out.unveiled;
            break;
        }
    }
    const std::string dcall = "call void @dx.op.discard(i32 82, i1 ";
    for (size_t p = 0; !out.unveiled && (p = m.t.find(dcall, p)) != std::string::npos; p += dcall.size()) {
        const size_t ce = m.t.find(')', p + dcall.size());
        const std::string cond = m.t.substr(p + dcall.size(), ce - (p + dcall.size()));
        if (cond.empty() || cond[0] != '%') continue;
        const std::string cd = def_of(cond);
        if (cd.rfind("fcmp fast olt float %", 0) != 0 || cd.find(", 0.000000e+00") == std::string::npos) continue;
        const std::string y = cd.substr(cd.find('%'), cd.find(',') - cd.find('%'));
        const std::string yd = def_of(y);
        if (yd.rfind("fadd fast float %", 0) != 0 || yd.find(", 0xBFD5") == std::string::npos) continue;
        const std::string x = yd.substr(yd.find('%'), yd.find(',') - yd.find('%'));
        const std::string xd = def_of(x);
        if (xd.rfind("call float @dx.op.binary.f32(i32 36, ", 0) != 0) continue; // FMin
        const std::string key = "\n  " + y + " = " + yd;
        const size_t yp = m.t.find(key);
        if (yp == std::string::npos) continue;
        const size_t cpos = yp + key.size() - 18; // the 18-char constant ends the line
        if (m.t.compare(cpos, 6, "0xBFD5") != 0) continue; // trailing metadata would shift it
        m.t.replace(cpos, 18, "1.000000e+00");
        ++out.unmasked;
    }
    auto users = [&](const std::string& name) {
        std::vector<std::pair<std::string, std::string>> r;
        for (size_t p = 0; (p = m.t.find(name, p)) != std::string::npos; p += name.size()) {
            const char nx = p + name.size() < m.t.size() ? m.t[p + name.size()] : '\0';
            if (nx >= '0' && nx <= '9') continue;
            const size_t ls = m.t.rfind('\n', p) + 1;
            const std::string line = m.t.substr(ls, m.t.find('\n', p) - ls);
            const size_t eq = line.find(" = ");
            if (line.rfind("  %", 0) != 0 || eq == std::string::npos || ls + eq + 3 > p) continue;
            r.emplace_back(line.substr(2, eq - 2), line.substr(eq + 3));
        }
        return r;
    };
    auto value_of = [](const std::string& op, double& v) {
        if (op.size() == 18 && op.rfind("0x", 0) == 0) {
            const unsigned long long bits = strtoull(op.c_str() + 2, nullptr, 16);
            memcpy(&v, &bits, sizeof v);
            return true;
        }
        char* end = nullptr;
        v = strtod(op.c_str(), &end);
        return end != op.c_str() && *end == '\0';
    };
    static const char kThirdOff[] = ", 0xBFD5";
    for (size_t p = 0; !out.unveiled && (p = m.t.find(kThirdOff, p)) != std::string::npos;
         p += sizeof(kThirdOff) - 1) {
        const size_t ls = m.t.rfind('\n', p) + 1;
        const size_t le = m.t.find('\n', p);
        const std::string line = m.t.substr(ls, le - ls);
        const size_t eq = line.find(" = fadd fast float %");
        if (line.rfind("  %", 0) != 0 || eq == std::string::npos || le != p + 20) continue;
        const std::string y = line.substr(2, eq - 2);
        const size_t x0 = eq + 19;
        if (def_of(line.substr(x0, line.find(',', x0) - x0)).rfind("call float @dx.op.binary.f32(i32 36, ", 0) != 0)
            continue;
        std::string v = y;
        auto us = users(v);
        double k = 0;
        if (us.size() == 1 && us[0].second.rfind("fmul fast float " + v + ", ", 0) == 0 &&
            value_of(us[0].second.substr(16 + v.size() + 2), k) && k > 0) {
            v = us[0].first;
            us = users(v);
        }
        double t = -1;
        if (us.size() != 1 || us[0].second.rfind("fcmp fast olt float " + v + ", ", 0) != 0 ||
            !value_of(us[0].second.substr(20 + v.size() + 2), t) || t < 0 || t > 0.01)
            continue;
        const std::string c = us[0].first;
        const auto cus = users(c);
        if (cus.empty() || !std::all_of(cus.begin(), cus.end(), [&](const std::pair<std::string, std::string>& u) {
                return u.second == "select i1 " + c + ", float 0.000000e+00, float 1.000000e+00";
            }))
            continue;
        m.t.replace(p + 2, 18, "1.000000e+00");
        ++out.unmasked;
    }
    static const char kIgn[] = "0x404A7DD040000000";
    std::vector<std::string> noise;
    for (size_t p = 0; (p = m.t.find(kIgn, p)) != std::string::npos; p += sizeof(kIgn) - 1) {
        const size_t ls = m.t.rfind('\n', p) + 1;
        const std::string line = m.t.substr(ls, m.t.find('\n', p) - ls);
        if (line.find("= fmul fast float ") == std::string::npos) continue;
        const size_t mp = line.find('%');
        const std::string mulname = line.substr(mp, line.find(' ', mp) - mp);
        const std::string frc = "= call float @dx.op.unary.f32(i32 22, float " + mulname + ")";
        const size_t fp = m.t.find(frc);
        if (fp == std::string::npos) continue;
        const size_t fls = m.t.rfind('\n', fp) + 1;
        const size_t np = m.t.find('%', fls);
        noise.push_back(m.t.substr(np, m.t.find(' ', np) - np));
    }
    std::vector<std::string> cw;
    for (const auto& el_ref : Operands(m.Node(in_list_node))) {
        const auto f = Operands(m.Node(NodeRef(el_ref)));
        if (f.size() < 11 || f[1] != "!\"COLOR\"") continue;
        const auto idx = Operands(m.Node(NodeRef(f[4])));
        long long csig;
        long long first;
        if (idx.empty() || !IntOf(idx[0], first) || first != 0 || !IntOf(f[0], csig)) continue;
        const std::string ld =
            "= call float @dx.op.loadInput.f32(i32 4, i32 " + std::to_string(csig) + ", i32 0, i8 3, i32 undef)";
        for (size_t p = 0; (p = m.t.find(ld, p)) != std::string::npos; p += ld.size()) {
            const size_t ls = m.t.rfind('\n', p) + 1;
            const size_t pct = m.t.find('%', ls);
            if (pct != std::string::npos && pct < p) cw.push_back(m.t.substr(pct, m.t.find(' ', pct) - pct));
        }
        break;
    }
    auto set_def = [&](const std::string& name, const std::string& was, const std::string& now) {
        const std::string key = "\n  " + name + " = " + was + "\n";
        const size_t p = m.t.find(key);
        if (p != std::string::npos) m.t.replace(p, key.size(), "\n  " + name + " = " + now + "\n");
        return p != std::string::npos;
    };
    auto has = [](const std::vector<std::string>& v, const std::string& s) {
        return std::find(v.begin(), v.end(), s) != v.end();
    };
    static const char kThird[] = "0xBFD554C980000000";
    static const char kNew[] = "0xBFC3333340000000";
    static const char kFMin[] = "call float @dx.op.binary.f32(i32 36, float ";
    for (size_t p = 0; (p = m.t.find(dcall, p)) != std::string::npos; p += dcall.size()) {
        const size_t ce = m.t.find(')', p + dcall.size());
        const std::string cond = m.t.substr(p + dcall.size(), ce - (p + dcall.size()));
        if (cond.empty() || cond[0] != '%') continue;
        const std::string cd = def_of(cond);
        if (cd.rfind("fcmp fast olt float %", 0) != 0 || cd.find(", 0.000000e+00") == std::string::npos) continue;
        const std::string yd = def_of(cd.substr(cd.find('%'), cd.find(',') - cd.find('%')));
        if (yd.rfind(kFMin, 0) != 0) continue;
        const size_t a0 = sizeof(kFMin) - 1;
        const size_t a1 = yd.find(',', a0);
        const size_t b0 = yd.find('%', a1);
        const size_t b1 = yd.find(')', b0);
        if (a1 == std::string::npos || b0 == std::string::npos || b1 == std::string::npos) continue;
        for (const std::string& op : { yd.substr(a0, a1 - a0), yd.substr(b0, b1 - b0) }) {
            if (op.empty() || op[0] != '%') continue;
            const std::string od = def_of(op);
            std::string s = op;
            std::string sd = od;
            if (od.rfind("select i1 %", 0) == 0 && od.size() > 20 &&
                od.compare(od.size() - 20, 20, ", float 1.000000e+00") == 0) {
                const size_t f0 = od.find(", float %");
                s = od.substr(f0 + 8, od.find(',', f0 + 8) - (f0 + 8));
                sd = def_of(s);
            }
            if (sd.rfind("fsub fast float %", 0) == 0) {
                const size_t c = sd.rfind(", ");
                if (has(noise, sd.substr(c + 2)) && set_def(s, sd, sd.substr(0, c) + ", -1.000000e+00")) ++out.degated;
            }
            for (const auto& w : cw)
                if (od == "fadd fast float " + w + ", " + kThird &&
                    set_def(op, od, "fadd fast float " + w + ", " + kNew)) {
                    ++out.decensored;
                    break;
                }
        }
    }
    return "opacity mask: " +
        (out.unmasked ? std::to_string(out.unmasked) + " FMin - 1/3 threshold(s) -> +1" : std::string("none")) +
        ", dither gate: " +
        (out.degated ? std::to_string(out.degated) + " subtraction(s) -> -1" : std::string("none")) +
        ", vertex mask: " +
        (out.decensored ? std::to_string(out.decensored) + " COLOR0.w - 1/3 -> -0.15" : std::string("none")) +
        ", fixed opacity: " +
        (out.unveiled ? std::to_string(out.unveiled) + " min(first, second) -> second" : std::string("none"));
}

// A float as LLVM IR spells it exactly: the hex of its double.
std::string FloatLit(float f)
{
    const double d = f;
    uint64_t u = 0;
    memcpy(&u, &d, sizeof u);
    char s[24];
    snprintf(s, sizeof s, "0x%016llX", (unsigned long long)u);
    return s;
}

// The input signature's element `name` index 0, a float of at least `cols_min` columns whose used mask covers
// `need`; -1 when there is none.
long long InputElement(
    Module& m, int in_list_node, const char* name, long long cols_min, long long need, std::string& why)
{
    for (const auto& el_ref : Operands(m.Node(in_list_node))) {
        const auto f = Operands(m.Node(NodeRef(el_ref)));
        if (f.size() < 11 || f[1] != std::string("!\"") + name + "\"") continue;
        const auto idx = Operands(m.Node(NodeRef(f[4])));
        long long sig;
        long long comp;
        long long cols;
        long long first;
        if (idx.empty() || !IntOf(idx[0], first) || first != 0) continue;
        if (!IntOf(f[0], sig) || !IntOf(f[2], comp) || !IntOf(f[7], cols)) continue;
        if (comp != 9 || cols < cols_min) {
            why = std::string(name) + " is not a float" + std::to_string(cols_min) + "+";
            return -1;
        }
        long long used = 0;
        const auto tags = Operands(m.Node(NodeRef(f[10])));
        for (size_t i = 0; i + 1 < tags.size(); i += 2) {
            long long tag;
            long long mask;
            if (IntOf(tags[i], tag) && tag == 3 && IntOf(tags[i + 1], mask)) used = mask;
        }
        if ((used & need) != need) {
            why = std::string(name) + " is read in columns " + std::to_string(used) + ", the cut needs " +
                std::to_string(need);
            return -1;
        }
        return sig;
    }
    why = std::string("the input signature has no ") + name + "0";
    return -1;
}

// The cuts: discard where SV_Position.xy lies inside a box and COLOR0.rgb inside that box's colour range.
bool Cut(Module& m, const std::string& fn, int sig_node, const std::vector<RfxDxilCut>& cuts, RfxDxilResult& out)
{
    std::string& why = out.why;
    const auto sops = Operands(m.Node(sig_node));
    const int in_list_node = sops.empty() ? -1 : NodeRef(sops[0]);
    if (in_list_node < 0) {
        why = "no input signature";
        return false;
    }
    const long long pos = InputElement(m, in_list_node, "SV_Position", 2, 3, why);
    if (pos < 0) return false;
    const long long col = InputElement(m, in_list_node, "COLOR", 3, 7, why);
    if (col < 0) return false;

    auto load = [&](const char* name, long long sig, int c) {
        return std::string("  %uidc_") + name + " = call float @dx.op.loadInput.f32(i32 4, i32 " + std::to_string(sig) +
            ", i32 0, i8 " + std::to_string(c) + ", i32 undef)\n";
    };
    std::string b = load("x", pos, 0) + load("y", pos, 1) + load("r", col, 0) + load("g", col, 1) + load("b", col, 2);
    std::string any;
    for (size_t k = 0; k < cuts.size(); ++k) {
        const RfxDxilCut& c = cuts[k];
        const std::string p = "%uidc" + std::to_string(k) + "_";
        std::string all;
        auto test = [&](const char* name, const char* op, const char* v, float lit) {
            b += "  " + p + name + " = fcmp fast " + op + " float %uidc_" + v + ", " + FloatLit(lit) + "\n";
            if (all.empty())
                all = p + name;
            else {
                b += "  " + p + name + "_and = and i1 " + all + ", " + p + name + "\n";
                all = p + name + "_and";
            }
        };
        test("x0", "oge", "x", c.x0);
        test("x1", "olt", "x", c.x1);
        test("y0", "oge", "y", c.y0);
        test("y1", "olt", "y", c.y1);
        test("r0", "oge", "r", c.lo[0]);
        test("r1", "ole", "r", c.hi[0]);
        test("g0", "oge", "g", c.lo[1]);
        test("g1", "ole", "g", c.hi[1]);
        test("b0", "oge", "b", c.lo[2]);
        test("b1", "ole", "b", c.hi[2]);
        if (any.empty())
            any = all;
        else {
            b += "  " + p + "or = or i1 " + any + ", " + all + "\n";
            any = p + "or";
        }
    }
    b += "  call void @dx.op.discard(i32 82, i1 " + any + ")\n";

    std::string pre;
    auto need_decl = [&](const char* name, const char* decl) {
        if (m.t.find(std::string("@dx.op.") + name + "(i32,") == std::string::npos) pre += std::string(decl) + "\n";
    };
    need_decl("loadInput.f32", "declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) nounwind readnone");
    need_decl("discard", "declare void @dx.op.discard(i32, i1) nounwind");
    const size_t def = m.t.find("\ndefine void " + fn + "(");
    if (def == std::string::npos) {
        why = "no `define void " + fn + "(`";
        return false;
    }
    const size_t brace = m.t.find("{\n", def + 1);
    if (brace == std::string::npos) {
        why = "the function has no body";
        return false;
    }
    m.t.insert(brace + 2, b);
    m.t.insert(def + 1, pre);
    why = "cut: SV_Position = input " + std::to_string(pos) + ", COLOR0 = input " + std::to_string(col) + ", " +
        std::to_string(cuts.size()) + " box(es)";
    return true;
}

bool PatchText(Module& m, const RfxDxilSpec& spec, RfxDxilResult& out)
{
    std::string& why = out.why;
    m.Scan();
    out.srv_max_reg = 0;
    out.has_glow = false;
    out.glow_why.clear();
    // ---- entry point: function, signatures, resources -------------------------------------------
    const size_t ep = m.t.find("\n!dx.entryPoints = !{!");
    if (ep == std::string::npos) {
        why = "no !dx.entryPoints";
        return false;
    }
    const int ep_node = atoi(m.t.c_str() + ep + 22);
    const auto eops = Operands(m.Node(ep_node));
    if (eops.size() < 5) {
        why = "entry point node has " + std::to_string(eops.size()) + " operands";
        return false;
    }
    const size_t at = eops[0].find('@');
    if (at == std::string::npos) {
        why = "entry point names no function";
        return false;
    }
    const std::string fn = eops[0].substr(at); // @main, or a quoted mangled name
    const int sig_node = NodeRef(eops[2]);
    const int res_node = NodeRef(eops[3]);
    if (sig_node < 0) {
        why = "entry point has no signatures";
        return false;
    }
    if (!spec.cuts.empty()) return Cut(m, fn, sig_node, spec.cuts, out);
    if (!spec.inject) {
        const auto s0 = Operands(m.Node(sig_node));
        const int il = s0.empty() ? -1 : NodeRef(s0[0]);
        if (il < 0) {
            why = "no input signature";
            return false;
        }
        const std::string dc = Decensor(m, il, out);
        if (!out.decensored && !out.degated && !out.unmasked && !out.unveiled) {
            why = "no censor chain";
            return false;
        }
        why = "decensor only: " + dc;
        return true;
    }
    if (res_node < 0) {
        why = "entry point has no resources (a pixel shader with no texture is not the body PS)";
        return false;
    }
    if (m.t.find("@dx.op.createHandle(") == std::string::npos) {
        why = "the shader does not use dx.op.createHandle (SM 6.6 createHandleFromBinding form is not handled)";
        return false;
    }

    // ---- resources: free SRV ids, and nothing already declared over the registers we add --------
    auto rops = Operands(m.Node(res_node));
    if (rops.size() != 4) {
        why = "resource node has " + std::to_string(rops.size()) + " operands";
        return false;
    }
    std::vector<std::string> srv_list;
    const int srv_list_node = NodeRef(rops[0]);
    if (srv_list_node >= 0) srv_list = Operands(m.Node(srv_list_node));
    long long new_id = 0;
    bool glow_reg_taken = false;
    for (const auto& rec_ref : srv_list) {
        const auto f = Operands(m.Node(NodeRef(rec_ref)));
        long long id;
        long long space;
        long long lower;
        long long range;
        if (f.size() < 7 || !IntOf(f[0], id) || !IntOf(f[3], space) || !IntOf(f[4], lower) || !IntOf(f[5], range)) {
            why = "SRV record " + rec_ref + " did not parse";
            return false;
        }
        if (id >= new_id) new_id = id + 1;
        const bool unbounded = (range < 0 || range >= 0xFFFFFFFFll);
        if (space != 0 || unbounded) {
            why = "the shader reads an SRV outside the fixed space-0 registers (space " + std::to_string(space) +
                ", t" + std::to_string(lower) + (unbounded ? ", unbounded" : "") +
                "): a one-table shadow bind would leave it dangling";
            return false;
        }
        auto over = [&](unsigned reg) { return lower <= (long long)reg && lower + range > (long long)reg; };
        if (over(spec.fx_reg)) {
            why = "the shader already declares t" + std::to_string(lower) + " x" + std::to_string(range) + " over t" +
                std::to_string(spec.fx_reg);
            return false;
        }
        if (over(spec.glow_reg) || over(spec.params_reg)) glow_reg_taken = true;
        if ((unsigned)(lower + range - 1) > out.srv_max_reg) out.srv_max_reg = (unsigned)(lower + range - 1);
    }
    long long smp_id = -1;
    long long smp_reg = 0;
    if (const int smp_list_node = NodeRef(rops[3]); smp_list_node >= 0)
        for (const auto& rec_ref : Operands(m.Node(smp_list_node))) {
            const auto f = Operands(m.Node(NodeRef(rec_ref)));
            long long id;
            long long space;
            long long lower;
            long long range;
            long long kind;
            if (f.size() < 7 || !IntOf(f[0], id) || !IntOf(f[3], space) || !IntOf(f[4], lower) || !IntOf(f[5], range) ||
                !IntOf(f[6], kind))
                continue;
            if (kind != 0 || range == 0) continue;
            if (space == 0 && lower == 0) {
                smp_id = id;
                smp_reg = 0;
                break;
            }
            if (smp_id < 0) {
                smp_id = id;
                smp_reg = lower;
            }
        }
    // ---- input signature: TEXCOORD0, float, at least two columns --------------------------------
    const auto sops = Operands(m.Node(sig_node));
    const int in_list_node = sops.empty() ? -1 : NodeRef(sops[0]);
    if (in_list_node < 0) {
        why = "no input signature";
        return false;
    }
    long long uv_sig = -1;
    int uv_elem_node = -1;
    std::vector<std::string> uv_elem;
    for (const auto& el_ref : Operands(m.Node(in_list_node))) {
        const auto f = Operands(m.Node(NodeRef(el_ref)));
        if (f.size() < 11 || f[1] != "!\"TEXCOORD\"") continue;
        const auto idx = Operands(m.Node(NodeRef(f[4])));
        long long sig;
        long long comp;
        long long cols;
        long long first;
        if (idx.empty() || !IntOf(idx[0], first) || first != 0) continue;
        if (!IntOf(f[0], sig) || !IntOf(f[2], comp) || !IntOf(f[7], cols)) continue;
        if (comp != 9 || cols < 2) {
            why = "TEXCOORD0 is not a float2+ (type " + std::to_string(comp) + ", cols " + std::to_string(cols) + ")";
            return false;
        }
        uv_sig = sig;
        uv_elem_node = NodeRef(el_ref);
        uv_elem = f;
        break;
    }
    if (uv_sig < 0) {
        why = "the input signature has no TEXCOORD0";
        return false;
    }
    // ---- glow: SV_Target0 and SV_Target3, float x3+, each column stored somewhere ---------------
    long long o_sig[2] = { -1, -1 }; // [0] = SV_Target0 (scene colour), [1] = SV_Target3 (base colour)
    if (const int out_list_node = sops.size() > 1 ? NodeRef(sops[1]) : -1; out_list_node >= 0)
        for (const auto& el_ref : Operands(m.Node(out_list_node))) {
            const auto f = Operands(m.Node(NodeRef(el_ref)));
            if (f.size() < 11 || f[1] != "!\"SV_Target\"") continue;
            const auto idx = Operands(m.Node(NodeRef(f[4])));
            long long sig;
            long long comp;
            long long cols;
            long long first;
            if (idx.empty() || !IntOf(idx[0], first) || !IntOf(f[0], sig) || !IntOf(f[2], comp) || !IntOf(f[7], cols))
                continue;
            if (comp != 9 || cols < 3) continue;
            if (first == 0) o_sig[0] = sig;
            if (first == 3) o_sig[1] = sig;
        }
    auto store_prefix = [](long long sig, int col) {
        return "call void @dx.op.storeOutput.f32(i32 5, i32 " + std::to_string(sig) + ", i32 0, i8 " +
            std::to_string(col) + ", float ";
    };
    if (glow_reg_taken)
        out.glow_why =
            "t" + std::to_string(spec.glow_reg) + " / t" + std::to_string(spec.params_reg) + " already declared";
    else if (smp_id < 0)
        out.glow_why = "no plain sampler";
    else if (o_sig[0] < 0 || o_sig[1] < 0)
        out.glow_why = "no float SV_Target0 + SV_Target3";
    else {
        for (int o = 0; o < 2 && out.glow_why.empty(); ++o)
            for (int c = 0; c < 3; ++c)
                if (m.t.find(store_prefix(o_sig[o], c)) == std::string::npos) {
                    out.glow_why =
                        "SV_Target" + std::to_string(o ? 3 : 0) + " column " + std::to_string(c) + " is never stored";
                    break;
                }
    }
    out.has_glow = out.glow_why.empty();

    // The uv element's used mask (tag 3) must cover x and y. The tag node may be shared: never edit it.
    {
        std::vector<std::string> tags;
        const int tag_node = NodeRef(uv_elem[10]);
        if (tag_node >= 0) tags = Operands(m.Node(tag_node));
        bool have = false;
        bool changed = false;
        for (size_t i = 0; i + 1 < tags.size(); i += 2) {
            long long tag;
            long long mask;
            if (IntOf(tags[i], tag) && tag == 3 && IntOf(tags[i + 1], mask)) {
                have = true;
                if ((mask & 3) != 3) {
                    tags[i + 1] = "i32 " + std::to_string(mask | 3);
                    changed = true;
                }
            }
        }
        if (!have) {
            tags.push_back("i32 3");
            tags.push_back("i32 3");
            changed = true;
        }
        if (changed) {
            uv_elem[10] = "!" + std::to_string(m.AddNode(Join(tags)));
            m.SetNode(uv_elem_node, "!" + std::to_string(uv_elem_node) + " = " + Join(uv_elem));
        }
    }
    // ---- the new SRV records --------------------------------------------------------------------
    const long long fx_id = new_id;
    const long long glow_id = new_id + 1;
    const long long prm_id = new_id + 2;
    {
        const int tag = m.AddNode("!{i32 0, i32 9}"); // element type: float
        auto add_srv = [&](long long id, const char* type, unsigned reg, int shape) {
            const int rec = m.AddNode("!{i32 " + std::to_string(id) + ", " + type + "* undef, !\"\", i32 0, i32 " +
                std::to_string(reg) + ", i32 1, i32 " + std::to_string(shape) + ", i32 0, !" + std::to_string(tag) +
                "}");
            srv_list.push_back("!" + std::to_string(rec));
        };
        add_srv(fx_id, kTexType, spec.fx_reg, 2); // shape 2 = Texture2D
        if (out.has_glow) {
            add_srv(glow_id, kTexType, spec.glow_reg, 2);
            add_srv(prm_id, kBufType, spec.params_reg, 10);
        } // shape 10 = typed Buffer
        if (srv_list_node >= 0)
            m.SetNode(srv_list_node, "!" + std::to_string(srv_list_node) + " = " + Join(srv_list));
        else {
            rops[0] = "!" + std::to_string(m.AddNode(Join(srv_list)));
            m.SetNode(res_node, "!" + std::to_string(res_node) + " = " + Join(rops));
        }
    }
    // ---- types and declarations the injection needs, placed before the function -----------------
    std::string pre;
    auto need_type = [&](const std::string& name, const std::string& body) {
        if (m.t.find("\n" + name + " = type") == std::string::npos) pre += name + " = type " + body + "\n";
    };
    need_type("%dx.types.Handle", "{ i8* }");
    need_type("%dx.types.ResRet.f32", "{ float, float, float, float, i32 }");
    need_type("%dx.types.Dimensions", "{ i32, i32, i32, i32 }");
    need_type(kTexType, std::string("{ <4 x float>, ") + kMipsType + " }");
    need_type(kMipsType, "{ i32 }");
    if (out.has_glow) need_type(kBufType, "{ <4 x float> }");
    // A declaration's parameter list opens `(i32,` (the opcode's TYPE); a call's opens `(i32 <opcode>,`.
    auto need_decl = [&](const char* name, const char* decl) {
        if (m.t.find(std::string("@dx.op.") + name + "(i32,") == std::string::npos) pre += std::string(decl) + "\n";
    };
    need_decl("loadInput.f32", "declare float @dx.op.loadInput.f32(i32, i32, i32, i8, i32) nounwind readnone");
    need_decl("getDimensions",
        "declare %dx.types.Dimensions @dx.op.getDimensions(i32, %dx.types.Handle, i32) nounwind readonly");
    need_decl("textureLoad.f32",
        "declare %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32) nounwind readonly");
    need_decl("discard", "declare void @dx.op.discard(i32, i1) nounwind");
    if (smp_id >= 0)
        need_decl("sample.f32",
            "declare %dx.types.ResRet.f32 @dx.op.sample.f32(i32, %dx.types.Handle, %dx.types.Handle, float, float, float, float, i32, i32, i32, float) nounwind readonly");
    if (out.has_glow) {
        need_decl("bufferLoad.f32",
            "declare %dx.types.ResRet.f32 @dx.op.bufferLoad.f32(i32, %dx.types.Handle, i32, i32) nounwind readonly");
        need_decl("unary.f32", "declare float @dx.op.unary.f32(i32, float) nounwind readnone");
        need_decl("binary.f32", "declare float @dx.op.binary.f32(i32, float, float) nounwind readnone");
    }

    if (out.has_glow) {
        int k = 0;
        for (int o = 0; o < 2; ++o)
            for (int c = 0; c < 3; ++c) {
                const std::string pfx = store_prefix(o_sig[o], c);
                for (size_t i = 0; (i = m.t.find(pfx, i)) != std::string::npos;) {
                    const size_t v0 = i + pfx.size();
                    const size_t v1 = m.t.find(')', v0);
                    const size_t ls = m.t.rfind('\n', i) + 1;
                    if (v1 == std::string::npos) break;
                    const std::string val = m.t.substr(v0, v1 - v0);
                    const std::string sel = "%rfxg_sel" + std::to_string(k++);
                    m.t.replace(v0, v1 - v0, sel);
                    const std::string line = "  " + sel + " = select i1 %rfxg_onb, float " +
                        (o ? kRfxGlowO3[c] : kRfxGlowO0[c]) + ", float " + val + "\n";
                    m.t.insert(ls, line);
                    i = v0 + line.size() + sel.size();
                }
            }
    }

    // ---- the injection, at the function's entry -------------------------------------------------
    const std::string sig = std::to_string(uv_sig);
    const std::string id = std::to_string(fx_id);
    const std::string reg = std::to_string(spec.fx_reg);
    std::string b;
    b += "  %rfx_t = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 0, i32 " + id + ", i32 " + reg +
        ", i1 false)\n";
    b += "  %rfx_u = call float @dx.op.loadInput.f32(i32 4, i32 " + sig + ", i32 0, i8 0, i32 undef)\n";
    b += "  %rfx_v = call float @dx.op.loadInput.f32(i32 4, i32 " + sig + ", i32 0, i8 1, i32 undef)\n";
    b += "  %rfx_dim = call %dx.types.Dimensions @dx.op.getDimensions(i32 72, %dx.types.Handle %rfx_t, i32 0)\n";
    b += "  %rfx_wi = extractvalue %dx.types.Dimensions %rfx_dim, 0\n";
    b += "  %rfx_hi = extractvalue %dx.types.Dimensions %rfx_dim, 1\n";
    b += "  %rfx_sum = add i32 %rfx_wi, %rfx_hi\n";
    b += "  %rfx_bound = icmp ne i32 %rfx_sum, 0\n";
    b += "  %rfx_wf = uitofp i32 %rfx_wi to float\n";
    b += "  %rfx_hf = uitofp i32 %rfx_hi to float\n";
    b += "  %rfx_xf = fmul fast float %rfx_wf, %rfx_u\n";
    b += "  %rfx_yf = fmul fast float %rfx_hf, %rfx_v\n";
    b += "  %rfx_x = fptosi float %rfx_xf to i32\n";
    b += "  %rfx_y = fptosi float %rfx_yf to i32\n";
    b +=
        "  %rfx_ld = call %dx.types.ResRet.f32 @dx.op.textureLoad.f32(i32 66, %dx.types.Handle %rfx_t, i32 0, i32 %rfx_x, i32 %rfx_y, i32 undef, i32 1, i32 1, i32 undef)\n";
    b += "  %rfx_la = extractvalue %dx.types.ResRet.f32 %rfx_ld, 3\n";
    if (smp_id >= 0) {
        b += "  %rfx_s = call %dx.types.Handle @dx.op.createHandle(i32 57, i8 3, i32 " + std::to_string(smp_id) +
            ", i32 " + std::to_string(smp_reg) + ", i1 false)\n";
        b +=
            "  %rfx_sm = call %dx.types.ResRet.f32 @dx.op.sample.f32(i32 60, %dx.types.Handle %rfx_t, %dx.types.Handle %rfx_s, float %rfx_u, float %rfx_v, float undef, float undef, i32 0, i32 0, i32 undef, float undef)\n";
        b += "  %rfx_sa = extractvalue %dx.types.ResRet.f32 %rfx_sm, 3\n";
        b += "  %rfx_z = fcmp fast oeq float %rfx_la, 0.000000e+00\n";
        b += "  %rfx_a = select i1 %rfx_z, float %rfx_sa, float %rfx_la\n";
    } else {
        b += "  %rfx_a = fadd fast float %rfx_la, 0.000000e+00\n";
    }
    b += "  %rfx_lt = fcmp fast olt float %rfx_a, 1.000000e+00\n";
    b += "  %rfx_cut = and i1 %rfx_lt, %rfx_bound\n";
    b += "  call void @dx.op.discard(i32 82, i1 %rfx_cut)\n";
    if (out.has_glow) {
        std::string g;
        for (const char* l : kRfxGlowIr) g += l;
        ReplaceAll(g, "@T50@", std::to_string(glow_id));
        ReplaceAll(g, "@T52@", std::to_string(prm_id));
        ReplaceAll(g, "@SMPID@", std::to_string(smp_id));
        ReplaceAll(g, "@SMPREG@", std::to_string(smp_reg));
        ReplaceAll(g, "@UVSIG@", sig);
        // The generated block names its registers 50 / 52; the spec's go in their place.
        ReplaceAll(g, ", i32 50, i1 false)", ", i32 " + std::to_string(spec.glow_reg) + ", i1 false)");
        ReplaceAll(g, ", i32 52, i1 false)", ", i32 " + std::to_string(spec.params_reg) + ", i1 false)");
        b += g;
        b += std::string("  %rfxg_onb = fcmp fast une float ") + kRfxGlowOn + ", 0.000000e+00\n";
    }

    const size_t def = m.t.find("\ndefine void " + fn + "(");
    if (def == std::string::npos) {
        why = "no `define void " + fn + "(`";
        return false;
    }
    const size_t brace = m.t.find("{\n", def + 1);
    if (brace == std::string::npos) {
        why = "the function has no body";
        return false;
    }
    m.t.insert(brace + 2, b);
    m.t.insert(def + 1, pre);
    const std::string dc_why = spec.decensor ? Decensor(m, in_list_node, out) : std::string("off");
    why = "uv=input " + sig + " (TEXCOORD0), its own SRVs reach t" + std::to_string(out.srv_max_reg) +
        "; cutout: SRV id " + id + " at t" + reg +
        (smp_id >= 0 ? ", fallback sample through s" + std::to_string(smp_reg) : ", no plain sampler: load only") +
        "; glow: " +
        (out.has_glow ? "SRV ids " + std::to_string(glow_id) + " / " + std::to_string(prm_id) + " at t" +
                    std::to_string(spec.glow_reg) + " / t" + std::to_string(spec.params_reg) + ", o0 = output " +
                    std::to_string(o_sig[0]) + ", o3 = output " + std::to_string(o_sig[1])
                      : "NOT patched (" + out.glow_why + ")") +
        "; decensor: " + dc_why;
    return true;
}

} // namespace

bool RfxDxilInit(const wchar_t* dll_dir, std::string& why)
{
    std::lock_guard<std::mutex> l(g_mtx);
    if (g_init_tried) {
        why = g_init_why;
        return g_init_ok;
    }
    g_init_tried = true;
    const std::wstring dir = dll_dir ? dll_dir : L".";
    g_dxc = LoadLibraryExW((dir + L"\\dxcompiler.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    g_dxil = LoadLibraryExW((dir + L"\\dxil.dll").c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!g_dxc || !g_dxil) {
        g_init_why = std::string(!g_dxc ? "dxcompiler.dll" : "dxil.dll") + " did not load (error " +
            std::to_string(GetLastError()) + ")";
        why = g_init_why;
        return false;
    }
    g_dxc_create = (DxcCreateInstanceProc)GetProcAddress(g_dxc, "DxcCreateInstance");
    g_dxil_create = (DxcCreateInstanceProc)GetProcAddress(g_dxil, "DxcCreateInstance");
    if (!g_dxc_create || !g_dxil_create) {
        g_init_why = "DxcCreateInstance export missing";
        why = g_init_why;
        return false;
    }
    g_init_ok = true;
    g_init_why = "ok";
    why = g_init_why;
    return true;
}

bool RfxDxilPatch(const void* ps, size_t ps_size, const RfxDxilSpec& spec, RfxDxilResult& out)
{
    std::lock_guard<std::mutex> l(g_mtx);
    if (!g_init_ok) {
        out.why = "not initialised: " + g_init_why;
        return false;
    }
    if (!ps || ps_size < 32) {
        out.why = "no shader bytes";
        return false;
    }
    Com<IDxcLibrary> lib;
    Com<IDxcCompiler> comp;
    Com<IDxcAssembler> as;
    Com<IDxcValidator> val;
    if (FAILED(g_dxc_create(CLSID_DxcLibrary, __uuidof(IDxcLibrary), (void**)&lib)) ||
        FAILED(g_dxc_create(CLSID_DxcCompiler, __uuidof(IDxcCompiler), (void**)&comp)) ||
        FAILED(g_dxc_create(CLSID_DxcAssembler, __uuidof(IDxcAssembler), (void**)&as)) ||
        FAILED(g_dxil_create(CLSID_DxcValidator, __uuidof(IDxcValidator), (void**)&val))) {
        out.why = "DxcCreateInstance failed";
        return false;
    }

    Com<IDxcBlobEncoding> src;
    if (FAILED(lib->CreateBlobWithEncodingOnHeapCopy(ps, (UINT32)ps_size, 0, &src))) {
        out.why = "blob";
        return false;
    }
    Com<IDxcBlobEncoding> dis;
    if (FAILED(comp->Disassemble(src.p, &dis)) || !dis) {
        out.why = "disassemble failed (not a DXIL container?)";
        return false;
    }
    Module m;
    m.t = "\n" + BlobText(dis.p); // leading newline: every search is for "\n<start of line>"
    out.ll_before = m.t;
    if (!PatchText(m, spec, out)) {
        out.why = "patch: " + out.why;
        out.settled = true;
        return false;
    }
    const std::string resolved = out.why;
    out.ll_after = m.t;

    Com<IDxcBlobEncoding> txt;
    if (FAILED(lib->CreateBlobWithEncodingOnHeapCopy(m.t.data(), (UINT32)m.t.size(), CP_UTF8, &txt))) {
        out.why = "blob";
        return false;
    }
    Com<IDxcOperationResult> ar;
    HRESULT st = E_FAIL;
    if (FAILED(as->AssembleToContainer(txt.p, &ar)) || !ar || FAILED(ar->GetStatus(&st)) || FAILED(st)) {
        out.why = "assemble: " + ErrText(ar.p);
        return false;
    }
    Com<IDxcBlob> cont;
    ar->GetResult(&cont);
    if (!cont) {
        out.why = "assemble: no container";
        return false;
    }
    Com<IDxcOperationResult> vr;
    HRESULT vst = E_FAIL;
    if (FAILED(val->Validate(cont.p, DxcValidatorFlags_InPlaceEdit, &vr)) || !vr || FAILED(vr->GetStatus(&vst)) ||
        FAILED(vst)) {
        out.why = "validate: " + ErrText(vr.p);
        return false;
    }
    out.code.assign(
        (const uint8_t*)cont->GetBufferPointer(), (const uint8_t*)cont->GetBufferPointer() + cont->GetBufferSize());
    out.why = resolved;
    out.settled = true;
    return true;
}
