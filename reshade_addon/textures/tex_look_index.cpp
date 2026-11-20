// The look index: references, verdicts and places, persisted in tex_look.bin. See tex_look.h.
#include "tex_look.h"
#include "shipped_matches.h"
#include "tex_lod.h"
#include "../core/log.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <deque>
#include <mutex>
#include <share.h>

using namespace texlook;

TexLook g_tex_look;

namespace {

constexpr uint32_t kAlgo = 1; // what a signature and a verdict mean; another value discards the file
constexpr uint32_t kMagic = 0x4b4c5854; // "TXLK"
constexpr uint32_t kMagicPlaces = 0x324c5854; // "TXL2": the same, then the places
constexpr uint32_t kMagicMesh = 0x334c5854; // "TXL3": the same, then the mesh places
constexpr uint32_t kTagFree = 0x464c5854; // "TXLF": after the mesh places, the references not a power of two a side
constexpr uint32_t kTagPlaced = 0x504c5854; // "TXLP": after those, the ones at spots of their own, with the spots
constexpr uint32_t kMixed = 0xffffffffu; // a place that showed two targets

template <typename T> bool put(FILE* f, const T& v)
{
    return fwrite(&v, sizeof v, 1, f) == 1;
}
template <typename T> bool get(FILE* f, T& v)
{
    return fread(&v, sizeof v, 1, f) == 1;
}

// "<what> live N: x,y std S ..." -- each patch's highest channel std.
std::string SpotsLine(const char* what, const LookSig& s)
{
    const LookStats st = ViewOf(s, s.pw, s.ph).st;
    char b[64];
    std::snprintf(b, sizeof b, "%s live %d:", what, LivePatches(st, s.nch));
    std::string out = b;
    for (int k = 0; k < kPatches; ++k) {
        float top = 0;
        for (uint32_t c = 0; c < s.nch; ++c) top = (std::max)(top, st.patch_std[k][c]);
        std::snprintf(b, sizeof b, " %u,%u std %.1f", s.at[k].x, s.at[k].y, top);
        out += b;
    }
    return out;
}

} // namespace

void TexLook::MarkDirty()
{ // caller holds mtx_
    dirty_ = true;
    dirty_at_ = GetTickCount64();
}

bool TexLook::KeepPlace(PlaceMap& m, uint64_t place, uint32_t target)
{ // caller holds mtx_
    auto it = m.find(place);
    if (it == m.end()) {
        m.emplace(place, target);
    } else {
        if (it->second == target || it->second == kMixed) return false;
        it->second = kMixed;
    }
    if (&m == &mesh_places_) mesh_gen_.fetch_add(1, std::memory_order_release);
    MarkDirty();
    return true;
}

// `target`: what `hash` is to the mods (a target, or exactly one), 0 for a texture they do not know. A known one
// records its place; an unknown one at a place recorded with one target becomes that target's look-alike.
void TexLook::AtPlaceIn(PlaceMap& m, uint64_t place, uint32_t hash, uint32_t target)
{
    if (!g_settings.texlod_cache || !hash) return;
    if (target) {
        {
            std::shared_lock l(mtx_);
            auto it = m.find(place);
            if (it != m.end() && (it->second == target || it->second == kMixed)) return;
        }
        std::unique_lock l(mtx_);
        if (KeepPlace(m, place, target)) places_new_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (g_tex_lod.Degenerate(hash)) return;
    uint32_t to = 0;
    {
        std::shared_lock l(mtx_);
        auto it = m.find(place);
        if (it == m.end() || it->second == kMixed) return;
        to = it->second;
        if (placed_.count(((uint64_t)hash << 32) | to)) return;
    }
    {
        std::unique_lock l(mtx_);
        if (!placed_.insert(((uint64_t)hash << 32) | to).second) return;
    }
    if (!g_tex_lod.LearnPlace(hash, to)) return;
    by_place_.fetch_add(1, std::memory_order_relaxed);
    logger::info("TexLook: %08x is drawn where HD draws %08x -- its look-alike by place", hash, to);
}

void TexLook::AtPlace(uint64_t pass, uint32_t slot, uint32_t hash, uint32_t target)
{
    AtPlaceIn(places_, TexLookSlotOf(pass, slot), hash, target);
}

void TexLook::AtMeshPlace(uint64_t place, uint32_t hash, uint32_t target)
{
    AtPlaceIn(mesh_places_, place, hash, target);
}

void TexLook::AddMeshPlace(uint64_t place, uint32_t target)
{
    std::unique_lock l(mtx_);
    if (KeepPlace(mesh_places_, place, target)) places_new_.fetch_add(1, std::memory_order_relaxed);
    has_mesh_.store(true, std::memory_order_relaxed);
}

// Remembered per thread and pass until mesh_places_ changes: a character draw asks every frame.
uint32_t TexLook::MeshMask(uint64_t pass, unsigned regs) const
{
    struct Memo {
        uint32_t mask = 0;
        uint32_t gen = 0;
    };
    thread_local std::unordered_map<uint64_t, Memo> t_memo;
    const uint32_t gen = mesh_gen_.load(std::memory_order_acquire);
    auto it = t_memo.find(pass);
    if (it != t_memo.end() && it->second.gen == gen) return it->second.mask;
    uint32_t mask = 0;
    {
        std::shared_lock l(mtx_);
        for (unsigned r = 0; r < regs && r < 32; ++r) {
            auto p = mesh_places_.find(TexLookSlotOf(pass, r));
            if (p != mesh_places_.end() && p->second != kMixed) mask |= 1u << r;
        }
    }
    t_memo[pass] = Memo{ mask, gen };
    return mask;
}

bool TexLook::Weaker(uint32_t target, uint32_t w, uint32_t h) const
{ // caller holds mtx_
    auto it = refs_.find(target);
    return it == refs_.end() || it->second.sig.pw * it->second.sig.ph < CellsOf(w, h);
}

uint32_t TexLook::TargetOf(uint32_t hash, bool is_target) const
{ // caller holds mtx_
    return is_target || refs_.count(hash) ? hash : g_tex_lod.Known(hash);
}

bool TexLook::Offered(uint32_t hash) const
{ // caller holds mtx_
    auto it = verdicts_.find(hash);
    return it != verdicts_.end() && it->second.kind == Kind::Ref;
}

bool TexLook::Wants(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target)
{
    if (!g_settings.texlod_cache || !hash || !Eligible(w, h, fmt)) return false;
    bool want = false;
    if (!g_tex_lod.Degenerate(hash)) {
        std::shared_lock l(mtx_);
        const uint32_t high = TargetOf(hash, is_target);
        if (high)
            want = !Offered(hash) && Weaker(high, w, h);
        else if (!PowerOfTwo(w, h) && !free_shapes_.count(ShapeOf(CodecOf(fmt), w, h)))
            return false;
        else {
            auto v = verdicts_.find(hash);
            want = v == verdicts_.end() || v->second.kind == Kind::Ref ||
                (v->second.kind == Kind::Match ? !g_tex_lod.Linked(hash) : v->second.gen != gen_);
            if (!want && g_debug && v->second.kind == Kind::Match) {
                const uint32_t to = g_tex_lod.Lookup(hash);
                auto r = refs_.find(to);
                if (r != refs_.end()) NoteSize(hash, w, h, to, r->second.sig.w, r->second.sig.h);
            }
        }
    }
    if (!want) skipped_.fetch_add(1, std::memory_order_relaxed);
    return want;
}

std::vector<Spot> TexLook::SpotsFor(uint32_t hash, uint32_t w, uint32_t h, uint32_t fmt, bool is_target) const
{
    std::vector<Spot> out;
    if (PowerOfTwo(w, h)) return out;
    std::shared_lock l(mtx_);
    if (TargetOf(hash, is_target)) return out;
    auto it = free_shapes_.find(ShapeOf(CodecOf(fmt), w, h));
    if (it == free_shapes_.end()) return out;
    for (const ShapeSpots& set : it->second)
        for (const Spot& p : set.at)
            if (std::find(out.begin(), out.end(), p) == out.end()) out.push_back(p);
    return out;
}

// Once per texture, debug builds: a look-alike at its reference's size is counted, one at another size named.
void TexLook::NoteSize(uint32_t hash, uint32_t w, uint32_t h, uint32_t target, uint32_t ref_w, uint32_t ref_h)
{
    {
        std::lock_guard l(sized_mtx_);
        if (!sized_.insert(hash).second) return;
    }
    if (w == ref_w && h == ref_h) {
        sized_same_.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    sized_other_.fetch_add(1, std::memory_order_relaxed);
    logger::report(
        "TexLook: %08x %ux%u looks like %08x, whose reference is %ux%u -- another size", hash, w, h, target, ref_w, ref_h);
}

void TexLook::PutRef(uint32_t target, Ref&& r)
{ // caller holds mtx_
    Ref& slot = refs_[target];
    if (!slot.sig.px.empty() && !PowerOfTwo(slot.sig.w, slot.sig.h)) {
        auto it = free_shapes_.find(ShapeOf(slot.sig.codec, slot.sig.w, slot.sig.h));
        if (it != free_shapes_.end()) {
            std::vector<ShapeSpots>& sets = it->second;
            auto s = std::find_if(sets.begin(), sets.end(), [&](const ShapeSpots& e) { return e.at == slot.sig.at; });
            if (s != sets.end() && !--s->refs) sets.erase(s);
            if (sets.empty()) free_shapes_.erase(it);
        }
    }
    if (!PowerOfTwo(r.sig.w, r.sig.h)) {
        std::vector<ShapeSpots>& sets = free_shapes_[ShapeOf(r.sig.codec, r.sig.w, r.sig.h)];
        auto s = std::find_if(sets.begin(), sets.end(), [&](const ShapeSpots& e) { return e.at == r.sig.at; });
        if (s == sets.end())
            sets.push_back(ShapeSpots{ r.sig.at, 1 });
        else
            ++s->refs;
    }
    slot = std::move(r);
}

bool TexLook::KeepRef(uint32_t target, Ref&& r)
{ // caller holds mtx_
    auto it = refs_.find(target);
    if (it != refs_.end() && it->second.sig.pw * it->second.sig.ph >= r.sig.pw * r.sig.ph) return false;
    PutRef(target, std::move(r));
    ++gen_;
    MarkDirty();
    return true;
}

TexLook::RefTaken TexLook::Offer(uint32_t source, uint32_t target, LookSig&& sig)
{
    Ref r;
    r.sig = std::move(sig);
    r.st = ViewOf(r.sig, r.sig.pw, r.sig.ph).st;
    const bool flat = LivePatches(r.st, r.sig.nch) < kLiveMin;
    const uint32_t w = r.sig.w;
    const uint32_t h = r.sig.h;
    bool kept = false;
    {
        std::unique_lock l(mtx_);
        verdicts_[source] = Verdict{ gen_, Kind::Ref };
        MarkDirty();
        if (!flat) kept = KeepRef(target, std::move(r));
    }
    if (kept) refs_new_.fetch_add(1, std::memory_order_relaxed);
    if (g_debug && flat)
        logger::trace("TexLook: %08x %ux%u too flat to be the reference of %08x", source, w, h, target);
    else if (g_debug && kept)
        logger::trace("TexLook: %08x %ux%u is now the reference of %08x%s", source, w, h, target,
            source == target ? " (itself)" : ", a proven lower mip of it");
    return flat ? RefTaken::Flat : kept ? RefTaken::Kept : RefTaken::AsDense;
}

// Each reference compares at its own spots: a texture not a power of two a side is decoded once per spot set the
// references of its size use, and one whose spots the cut lacks (added after the upload) decides it again next time.
void TexLook::Match(uint32_t hash, const TexLookCut& cut)
{
    struct Cand {
        LookSig sig;
        LookView cv;
    };
    std::deque<Cand> cands; // references to them stay valid as more are added
    const auto cand_at = [&](const Spots& at) -> const Cand* {
        for (const Cand& c : cands)
            if (c.sig.at == at) return &c;
        Cand c;
        if (!SigOf(cut, at, c.sig)) return nullptr;
        c.cv = ViewOf(c.sig, c.sig.pw, c.sig.ph);
        cands.push_back(std::move(c));
        return &cands.back();
    };
    const bool own_size = !PowerOfTwo(cut.w, cut.h);
    const uint64_t shape = ShapeOf(CodecOf(cut.fmt), cut.w, cut.h);
    const Cand* def = own_size ? nullptr : cand_at(DefaultSpots(cut.w, cut.h));
    if (!own_size && !def) return;
    struct Hit {
        uint32_t target = 0;
        Score s;
        uint32_t w = 0; // its reference's
        uint32_t h = 0;
    };
    std::vector<Hit> hits;
    uint32_t gen = 0;
    bool missed = false;
    uint32_t closest_t = 0;
    Score closest;
    {
        std::shared_lock l(mtx_);
        gen = gen_;
        for (const auto& kv : refs_) {
            if (kv.first == hash) continue;
            const LookSig& rs = kv.second.sig;
            const Cand* c = def;
            if (own_size) {
                if (ShapeOf(rs.codec, rs.w, rs.h) != shape) continue;
                c = cand_at(rs.at);
                if (!c) {
                    missed = true;
                    continue;
                }
            }
            const Score s = Compare(c->sig, c->cv, kv.second);
            if (s.match())
                hits.push_back(Hit{ kv.first, s, rs.w, rs.h });
            else if (s.hist < closest.hist || (s.hist == closest.hist && s.ssim > closest.ssim)) {
                closest = s;
                closest_t = kv.first;
            }
        }
    }
    if (cands.empty()) return;
    decoded_.fetch_add(1, std::memory_order_relaxed);
    {
        std::unique_lock l(mtx_);
        verdicts_[hash] = Verdict{ missed ? 0u : gen, hits.empty() ? Kind::NoMatch : Kind::Match };
        MarkDirty();
    }
    for (const Hit& h : hits) {
        if (g_tex_lod.LearnVisual(hash, h.target, h.s.ssim) && g_debug)
            logger::trace("TexLook: %08x %ux%u looks like %08x (hist %.3f, ssim-min %.3f)", hash, cut.w, cut.h, h.target,
                h.s.hist, h.s.ssim);
    }
    if (g_debug && !hits.empty()) NoteSize(hash, cut.w, cut.h, hits[0].target, hits[0].w, hits[0].h);
    if (hits.size() > 1)
        logger::info("TexLook: %08x %ux%u looks like %zu targets, e.g. %08x (ssim-min %.3f) and %08x (%.3f)", hash,
            cut.w, cut.h, hits.size(), hits[0].target, hits[0].s.ssim, hits[1].target, hits[1].s.ssim);
    if (!hits.empty())
        matched_.fetch_add(1, std::memory_order_relaxed);
    else if (closest_t && DiagOnce("TEXLOOK-NOMATCH", (int)hash))
        logger::trace(
            "TexLook: NO-MATCH %08x %ux%u | closest %08x hist %.3f ssim-min %.3f (need hist <= %.2f, ssim >= %.2f)",
            hash, cut.w, cut.h, closest_t, closest.hist, closest.ssim, kHistMax, kSsimMin);
}

void TexLook::OnCut(uint32_t hash, const TexLookCut& cut, bool is_target)
{
    if (!g_settings.texlod_cache) return;
    uint32_t high = 0;
    {
        std::shared_lock l(mtx_);
        high = TargetOf(hash, is_target);
    }
    if (!high) {
        Match(hash, cut);
        return;
    }
    LookSig sig;
    if (!SigOf(cut, DefaultSpots(cut.w, cut.h), sig)) return;
    decoded_.fetch_add(1, std::memory_order_relaxed);
    Offer(hash, high, std::move(sig));
}

TexLook::RefTaken TexLook::TakeRef(uint32_t hash, uint32_t target, const TexLookCut& cut)
{
    LookSig sig;
    if (!g_settings.texlod_cache || !hash || !target || g_tex_lod.Degenerate(hash) ||
        !SigOf(cut, DefaultSpots(cut.w, cut.h), sig))
        return RefTaken::Unread;
    decoded_.fetch_add(1, std::memory_order_relaxed);
    if (!PowerOfTwo(cut.w, cut.h)) {
        std::string line = SpotsLine("default spots", sig);
        Spots at;
        LookSig busy;
        if (LivePatches(ViewOf(sig, sig.pw, sig.ph).st, sig.nch) < kLiveMin && BusiestSpots(cut, at) &&
            SigOf(cut, at, busy)) {
            line += " | " + SpotsLine("busiest", busy);
            sig = std::move(busy);
        }
        logger::report("TexLook: take %08x %ux%u, %s", hash, cut.w, cut.h, line.c_str());
    }
    return Offer(hash, target, std::move(sig));
}

bool TexLook::Taken(uint32_t hash) const
{
    std::shared_lock l(mtx_);
    return Offered(hash);
}

TexLook::RefGroup TexLook::GroupOf(const LookSig& s)
{
    if (PowerOfTwo(s.w, s.h)) return RefGroup::PowerOfTwo;
    return s.at == DefaultSpots(s.w, s.h) ? RefGroup::FreeDefault : RefGroup::FreePlaced;
}

bool TexLook::ReadRefs(FILE* f, uint32_t n, bool seed, bool with_spots, size_t* taken)
{ // caller holds mtx_
    for (uint32_t i = 0; i < n; ++i) {
        uint32_t target = 0;
        Ref r;
        bool ok = get(f, target) && get(f, r.sig.codec) && get(f, r.sig.nch) && get(f, r.sig.w) && get(f, r.sig.h) &&
            get(f, r.sig.pw) && get(f, r.sig.ph);
        ok =
            ok && r.sig.nch && r.sig.nch <= 4 && r.sig.pw && r.sig.pw <= kMaxCells && r.sig.ph && r.sig.ph <= kMaxCells;
        if (!ok) return false;
        r.sig.px.resize((size_t)kPatches * r.sig.pw * r.sig.ph * r.sig.nch);
        if (fread(r.sig.px.data(), 1, r.sig.px.size(), f) != r.sig.px.size()) return false;
        if (with_spots) {
            for (Spot& p : r.sig.at)
                if (!get(f, p.x) || !get(f, p.y)) return false;
            if (PowerOfTwo(r.sig.w, r.sig.h) || !SpotsFit(r.sig.w, r.sig.h, r.sig.at)) return false;
        } else
            r.sig.at = DefaultSpots(r.sig.w, r.sig.h);
        r.st = ViewOf(r.sig, r.sig.pw, r.sig.ph).st;
        if (seed)
            *taken += KeepRef(target, std::move(r));
        else {
            PutRef(target, std::move(r));
            ++*taken;
        }
    }
    return true;
}

bool TexLook::WriteRefs(FILE* f, RefGroup group) const
{ // caller holds mtx_
    bool ok = true;
    for (const auto& kv : refs_) {
        const LookSig& s = kv.second.sig;
        if (GroupOf(s) != group) continue;
        ok = ok && put(f, kv.first) && put(f, s.codec) && put(f, s.nch) && put(f, s.w) && put(f, s.h) && put(f, s.pw) &&
            put(f, s.ph) && fwrite(s.px.data(), 1, s.px.size(), f) == s.px.size();
        if (group == RefGroup::FreePlaced)
            for (const Spot& p : s.at) ok = ok && put(f, p.x) && put(f, p.y);
    }
    return ok;
}

// The references (all a seed gives) and, from our own file, the verdicts. Caller holds mtx_.
size_t TexLook::Load(const std::wstring& file, bool seed)
{
    FILE* f = _wfsopen(file.c_str(), L"rb", _SH_DENYNO);
    if (!f) return 0;
    uint32_t magic = 0;
    uint32_t algo = 0;
    uint32_t gen = 0;
    uint32_t nrefs = 0;
    uint32_t nverdicts = 0;
    if (!get(f, magic) || !get(f, algo) || !get(f, gen) || !get(f, nrefs) || !get(f, nverdicts) ||
        (magic != kMagic && magic != kMagicPlaces && magic != kMagicMesh) || algo != kAlgo) {
        fclose(f);
        logger::write("TexLook: %S is not algorithm v%u (it says v%u) -- %s", file.c_str(), kAlgo, algo,
            seed ? "seed not taken" : "discarded, every texture is looked at again");
        return 0;
    }
    size_t taken = 0;
    bool ok = ReadRefs(f, nrefs, seed, false, &taken);
    for (uint32_t i = 0; ok && i < nverdicts; ++i) {
        uint32_t hash = 0;
        Verdict v;
        uint8_t kind = 0;
        ok = get(f, hash) && get(f, v.gen) && get(f, kind) && kind <= (uint8_t)Kind::Ref;
        v.kind = (Kind)kind;
        if (ok && !seed) verdicts_[hash] = v;
    }
    uint32_t nplaces = 0;
    if (ok && magic != kMagic) ok = get(f, nplaces);
    for (uint32_t i = 0; ok && i < nplaces; ++i) {
        uint64_t place = 0;
        uint32_t target = 0;
        ok = get(f, place) && get(f, target);
        if (ok && target) KeepPlace(places_, place, target);
    }
    uint32_t nmesh = 0;
    if (ok && magic == kMagicMesh) ok = get(f, nmesh);
    for (uint32_t i = 0; ok && i < nmesh; ++i) {
        uint64_t place = 0;
        uint32_t target = 0;
        ok = get(f, place) && get(f, target);
        if (ok && target) KeepPlace(mesh_places_, place, target);
    }
    uint32_t tag = 0;
    uint32_t nfree = 0;
    uint32_t nplaced = 0;
    if (ok && magic == kMagicMesh && get(f, tag) && tag == kTagFree) {
        ok = get(f, nfree) && ReadRefs(f, nfree, seed, false, &taken);
        if (ok && get(f, tag) && tag == kTagPlaced) ok = get(f, nplaced) && ReadRefs(f, nplaced, seed, true, &taken);
    }
    if (!mesh_places_.empty()) has_mesh_.store(true, std::memory_order_relaxed);
    fclose(f);
    if (!seed) gen_ = (std::max)(gen_, gen);
    if (!ok) logger::write("TexLook: %S ends early -- what came before is kept", file.c_str());
    return taken;
}

void TexLook::Write()
{ // caller holds mtx_
    const std::wstring p = Path();
    if (p.empty()) return;
    const std::wstring tmp = p + L".tmp";
    FILE* f = _wfsopen(tmp.c_str(), L"wb", _SH_DENYNO);
    if (!f) {
        logger::write("TexLook: cannot write %S", tmp.c_str());
        return;
    }
    uint32_t nfree = 0;
    uint32_t nplaced = 0;
    for (const auto& kv : refs_) {
        const RefGroup g = GroupOf(kv.second.sig);
        nfree += g == RefGroup::FreeDefault;
        nplaced += g == RefGroup::FreePlaced;
    }
    bool ok = put(f, kMagicMesh) && put(f, kAlgo) && put(f, gen_) &&
        put(f, (uint32_t)refs_.size() - nfree - nplaced) && put(f, (uint32_t)verdicts_.size()) &&
        WriteRefs(f, RefGroup::PowerOfTwo);
    for (const auto& kv : verdicts_)
        ok = ok && put(f, kv.first) && put(f, kv.second.gen) && put(f, (uint8_t)kv.second.kind);
    ok = ok && put(f, (uint32_t)places_.size());
    for (const auto& kv : places_) ok = ok && put(f, kv.first) && put(f, kv.second);
    ok = ok && put(f, (uint32_t)mesh_places_.size());
    for (const auto& kv : mesh_places_) ok = ok && put(f, kv.first) && put(f, kv.second);
    ok = ok && put(f, kTagFree) && put(f, nfree) && WriteRefs(f, RefGroup::FreeDefault);
    ok = ok && put(f, kTagPlaced) && put(f, nplaced) && WriteRefs(f, RefGroup::FreePlaced);
    ok = fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), p.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logger::write("TexLook: cannot replace %S (err %lu)", p.c_str(), GetLastError());
        return;
    }
    dirty_ = false;
    logger::trace(
        "TexLook: saved %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s); this session %u "
        "decoded, %u skipped by their hash, %u matched, %u new reference(s), %u new place(s), %u look-alike(s) by "
        "place",
        refs_.size(), verdicts_.size(), places_.size(), mesh_places_.size(), decoded_.load(), skipped_.load(),
        matched_.load(), refs_new_.load(), places_new_.load(), by_place_.load());
    for (const auto& seed : imported_) {
        if (MoveFileW(seed.first.c_str(), seed.second.c_str()))
            logger::write(
                "TexLook: seed %S is in tex_look.bin now -- renamed %S", seed.first.c_str(), seed.second.c_str());
        else
            logger::write(
                "TexLook: seed %S is in tex_look.bin now, but cannot be renamed (err %lu) -- it is taken again "
                "next start",
                seed.first.c_str(), GetLastError());
    }
    imported_.clear();
    if (!shipped_taken_.empty()) {
        if (g_shipped_matches.MarkTaken(dir_, shipped_taken_))
            logger::write("TexLook: %S is in tex_look.bin now", shipped_taken_.c_str());
        else
            logger::write("TexLook: %S is in tex_look.bin now, but cannot be marked so -- it is taken again next start",
                shipped_taken_.c_str());
        shipped_taken_.clear();
    }
}

void TexLook::Init(const std::wstring& cache_dir)
{
    std::unique_lock l(mtx_);
    if (!cache_dir.empty()) dir_ = cache_dir;
    if (dir_.empty()) return;
    if (!loaded_) {
        loaded_ = true;
        const size_t n = Load(Path(), false);
        logger::write(
            "TexLook: %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s) from %S (algorithm v%u)", n,
            verdicts_.size(), places_.size(), mesh_places_.size(), Path().c_str(), kAlgo);
    }
    if (shipped_taken_.empty()) {
        const std::wstring shipped = g_shipped_matches.ToTake(dir_, L"looks.texlook", Path());
        if (!shipped.empty()) {
            const size_t taken = Load(shipped, true);
            shipped_taken_ = shipped;
            MarkDirty();
            logger::write("TexLook: %S -- %zu reference(s) new or denser than ours", shipped.c_str(), taken);
        }
    }
    WIN32_FIND_DATAW sf;
    HANDLE hs = FindFirstFileW((dir_ + L"\\*.texlook").c_str(), &sf);
    if (hs == INVALID_HANDLE_VALUE) return;
    do {
        if (sf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        const size_t n = wcslen(sf.cFileName);
        if (n < 8 || _wcsicmp(sf.cFileName + n - 8, L".texlook") != 0 || _wcsnicmp(sf.cFileName, L"DISABLED_", 9) == 0)
            continue;
        const std::wstring from = dir_ + L"\\" + sf.cFileName;
        if (std::any_of(imported_.begin(), imported_.end(), [&](const auto& s) { return s.first == from; })) continue;
        const size_t taken = Load(from, true);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t when[32];
        swprintf_s(when, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        imported_.emplace_back(from, dir_ + L"\\DISABLED_" + when + L"_" + sf.cFileName);
        MarkDirty();
        logger::write("TexLook: seed %S -- %zu reference(s) new or denser than ours", from.c_str(), taken);
    } while (FindNextFileW(hs, &sf));
    FindClose(hs);
}

void TexLook::FlushIfQuiet(unsigned quiet_ms)
{
    std::unique_lock l(mtx_);
    if (dirty_ && GetTickCount64() - dirty_at_ >= quiet_ms) Write();
}

void TexLook::OnExit()
{
    std::unique_lock l(mtx_);
    if (dirty_) Write();
    logger::write(
        "TexLook: exit -- %zu reference(s), %zu verdict(s), %zu place(s), %zu mesh place(s); this session %u "
        "decoded, %u skipped by their hash, %u matched, %u new reference(s), %u new place(s), %u look-alike(s) by "
        "place",
        refs_.size(), verdicts_.size(), places_.size(), mesh_places_.size(), decoded_.load(), skipped_.load(),
        matched_.load(), refs_new_.load(), places_new_.load(), by_place_.load());
    if (g_debug)
        logger::write("TexLook: exit -- %u look-alike(s) seen at their reference's size, %u at another (named above)",
            sized_same_.load(), sized_other_.load());
}

uint64_t TexLookPlaceOf(const wchar_t* ns, int comp, uint64_t ps)
{
    uint64_t h = 0xcbf29ce484222325ull;
    for (const wchar_t* c = ns; c && *c; ++c) h = (h ^ (uint64_t)towlower(*c)) * 0x100000001b3ull;
    h = (h ^ (uint64_t)(uint32_t)comp) * 0x100000001b3ull;
    return (h ^ ps) * 0x100000001b3ull;
}

uint64_t TexLookMeshPassOf(uint32_t ib_hash, uint32_t first_index, uint32_t index_count, uint64_t ps)
{
    uint64_t h = 0x84222325cbf29ce4ull;
    for (const uint32_t v : { ib_hash, first_index, index_count }) h = (h ^ v) * 0x100000001b3ull;
    return (h ^ ps) * 0x100000001b3ull;
}

uint64_t TexLookSlotOf(uint64_t pass, uint32_t slot)
{
    return (pass ^ slot) * 0x100000001b3ull;
}
