// The look index: references, verdicts and places, persisted in tex_look.bin. See tex_look.h.
#include "tex_look.h"
#include "tex_lod.h"
#include "../core/log.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <mutex>
#include <share.h>

using namespace texlook;

TexLook g_tex_look;

namespace {

constexpr uint32_t kAlgo = 1; // what a signature and a verdict mean; another value discards the file
constexpr uint32_t kMagic = 0x4b4c5854; // "TXLK"
constexpr uint32_t kMagicPlaces = 0x324c5854; // "TXL2": the same, then the places
constexpr uint32_t kMagicMesh = 0x334c5854; // "TXL3": the same, then the mesh places
constexpr uint32_t kMixed = 0xffffffffu; // a place that showed two targets

template <typename T> bool put(FILE* f, const T& v)
{
    return fwrite(&v, sizeof v, 1, f) == 1;
}
template <typename T> bool get(FILE* f, T& v)
{
    return fread(&v, sizeof v, 1, f) == 1;
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
        else {
            auto v = verdicts_.find(hash);
            want = v == verdicts_.end() || v->second.kind == Kind::Ref ||
                (v->second.kind == Kind::Match ? !g_tex_lod.Linked(hash) : v->second.gen != gen_);
        }
    }
    if (!want) skipped_.fetch_add(1, std::memory_order_relaxed);
    return want;
}

bool TexLook::KeepRef(uint32_t target, Ref&& r)
{ // caller holds mtx_
    auto it = refs_.find(target);
    if (it != refs_.end() && it->second.sig.pw * it->second.sig.ph >= r.sig.pw * r.sig.ph) return false;
    refs_[target] = std::move(r);
    ++gen_;
    MarkDirty();
    return true;
}

bool TexLook::Offer(uint32_t source, uint32_t target, LookSig&& sig)
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
    return kept;
}

void TexLook::Match(uint32_t hash, const LookSig& sig)
{
    const LookView cv = ViewOf(sig, sig.pw, sig.ph);
    std::vector<std::pair<uint32_t, Score>> hits;
    uint32_t gen = 0;
    uint32_t closest_t = 0;
    Score closest;
    {
        std::shared_lock l(mtx_);
        gen = gen_;
        for (const auto& kv : refs_) {
            if (kv.first == hash) continue;
            const Score s = Compare(sig, cv, kv.second);
            if (s.match())
                hits.emplace_back(kv.first, s);
            else if (s.hist < closest.hist || (s.hist == closest.hist && s.ssim > closest.ssim)) {
                closest = s;
                closest_t = kv.first;
            }
        }
    }
    {
        std::unique_lock l(mtx_);
        verdicts_[hash] = Verdict{ gen, hits.empty() ? Kind::NoMatch : Kind::Match };
        MarkDirty();
    }
    for (const auto& h : hits) {
        if (g_tex_lod.LearnVisual(hash, h.first, h.second.ssim) && g_debug)
            logger::trace("TexLook: %08x %ux%u looks like %08x (hist %.3f, ssim-min %.3f)", hash, sig.w, sig.h, h.first,
                h.second.hist, h.second.ssim);
    }
    if (hits.size() > 1)
        logger::info("TexLook: %08x %ux%u looks like %zu targets, e.g. %08x (ssim-min %.3f) and %08x (%.3f)", hash,
            sig.w, sig.h, hits.size(), hits[0].first, hits[0].second.ssim, hits[1].first, hits[1].second.ssim);
    if (!hits.empty())
        matched_.fetch_add(1, std::memory_order_relaxed);
    else if (closest_t && DiagOnce("TEXLOOK-NOMATCH", (int)hash))
        logger::trace(
            "TexLook: NO-MATCH %08x %ux%u | closest %08x hist %.3f ssim-min %.3f (need hist <= %.2f, ssim >= %.2f)",
            hash, sig.w, sig.h, closest_t, closest.hist, closest.ssim, kHistMax, kSsimMin);
}

void TexLook::OnCut(uint32_t hash, const TexLookCut& cut, bool is_target)
{
    LookSig sig;
    if (!g_settings.texlod_cache || !SigOf(cut, sig)) return;
    decoded_.fetch_add(1, std::memory_order_relaxed);
    uint32_t high = 0;
    {
        std::shared_lock l(mtx_);
        high = TargetOf(hash, is_target);
    }
    if (high)
        Offer(hash, high, std::move(sig));
    else
        Match(hash, sig);
}

bool TexLook::TakeRef(uint32_t hash, uint32_t target, const TexLookCut& cut)
{
    LookSig sig;
    if (!g_settings.texlod_cache || !hash || !target || g_tex_lod.Degenerate(hash) || !SigOf(cut, sig)) return false;
    decoded_.fetch_add(1, std::memory_order_relaxed);
    return Offer(hash, target, std::move(sig));
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
    bool ok = true;
    for (uint32_t i = 0; ok && i < nrefs; ++i) {
        uint32_t target = 0;
        Ref r;
        ok = get(f, target) && get(f, r.sig.codec) && get(f, r.sig.nch) && get(f, r.sig.w) && get(f, r.sig.h) &&
            get(f, r.sig.pw) && get(f, r.sig.ph);
        ok =
            ok && r.sig.nch && r.sig.nch <= 4 && r.sig.pw && r.sig.pw <= kMaxCells && r.sig.ph && r.sig.ph <= kMaxCells;
        if (!ok) break;
        r.sig.px.resize((size_t)kPatches * r.sig.pw * r.sig.ph * r.sig.nch);
        ok = fread(r.sig.px.data(), 1, r.sig.px.size(), f) == r.sig.px.size();
        if (!ok) break;
        r.st = ViewOf(r.sig, r.sig.pw, r.sig.ph).st;
        if (seed)
            taken += KeepRef(target, std::move(r));
        else {
            refs_[target] = std::move(r);
            ++taken;
        }
    }
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
    bool ok = put(f, kMagicMesh) && put(f, kAlgo) && put(f, gen_) && put(f, (uint32_t)refs_.size()) &&
        put(f, (uint32_t)verdicts_.size());
    for (const auto& kv : refs_) {
        const LookSig& s = kv.second.sig;
        ok = ok && put(f, kv.first) && put(f, s.codec) && put(f, s.nch) && put(f, s.w) && put(f, s.h) && put(f, s.pw) &&
            put(f, s.ph) && fwrite(s.px.data(), 1, s.px.size(), f) == s.px.size();
    }
    for (const auto& kv : verdicts_)
        ok = ok && put(f, kv.first) && put(f, kv.second.gen) && put(f, (uint8_t)kv.second.kind);
    ok = ok && put(f, (uint32_t)places_.size());
    for (const auto& kv : places_) ok = ok && put(f, kv.first) && put(f, kv.second);
    ok = ok && put(f, (uint32_t)mesh_places_.size());
    for (const auto& kv : mesh_places_) ok = ok && put(f, kv.first) && put(f, kv.second);
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
