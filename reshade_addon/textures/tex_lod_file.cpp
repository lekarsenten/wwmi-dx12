// The LOD cache's file: read at load, seeds taken, written when quiet. See tex_lod.h.
#include "tex_lod_internal.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <map>
#include <share.h>

namespace {

using Link = TexLodCache::Link;
using Rec = TexLodCache::Rec;
using Records = TexLodCache::Records;

const char* const kHeader =
    "; wwmi-dx12 LOD texture matches. One line per target:  <target> = <low> <flags> | <low> <flags> ...\n"
    "; A low is a lower-resolution texture the game streams in; it is swapped as its target's mod texture.\n"
    ";   mip=K         the low is the target's mip K (0: the level is not known)\n"
    ";   exact=1       proven from the game's own data: the target's own mip, or a stream copy\n"
    ";   exact=0       only looks alike (tex_look, its reference in tex_look.bin): with lod_resemblance=1 swapped like\n"
    ";                 an exact low; under several targets, as the one ahead by 0.03 ssim, else not at all\n"
    ";   chain=1       learned through a chain of stream copies; a look-alike learned so has the ssim of the one it came through\n"
    ";   ssim=S        a look-alike's lowest patch similarity to the target's reference (1: identical)\n"
    ";   place=1       a look-alike by where it is drawn: HD drew the target at that slot of a mod's component\n"
    "; degenerate = <low> <low> ...  identical to the mips of more than one target: never swapped, never learned again\n";

void WriteEntry(FILE* f, uint32_t low, const Link& k)
{
    fprintf(f, "%08x mip=%u exact=%d chain=%d", low, k.mip, (int)k.exact, (int)k.chain);
    if (!k.exact) fprintf(f, " ssim=%.3f", k.ssim);
    if (k.place) fputs(" place=1", f);
}

std::string Trim(const char* s, const char* e)
{
    while (s < e && (*s == ' ' || *s == '\t')) ++s;
    while (e > s && (e[-1] == ' ' || e[-1] == '\t' || e[-1] == '\r' || e[-1] == '\n')) --e;
    return std::string(s, e);
}

bool ParseLine(const char* line, Records& into, std::set<uint32_t>& degen)
{
    const char* d = line;
    while (*d == ' ' || *d == '\t') ++d;
    if (strncmp(d, "degenerate", 10) == 0) {
        const char* p = strchr(d, '=');
        if (!p) return false;
        for (++p;;) {
            char* end = nullptr;
            const unsigned long low = strtoul(p, &end, 16);
            if (end == p) break;
            if (low) degen.insert((uint32_t)low);
            p = end;
        }
        return true;
    }
    unsigned t = 0;
    int n = 0;
    if (sscanf_s(line, " %x =%n", &t, &n) != 1 || !t || !n || !strstr(line, "exact=")) return false;
    for (const char* p = line + n; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '|') ++p;
        char* end = nullptr;
        const unsigned long low = strtoul(p, &end, 16);
        if (end == p) break;
        p = end;
        Link k;
        k.high = t;
        bool flagged = false;
        for (;;) {
            while (*p == ' ' || *p == '\t') ++p;
            if (!*p || *p == '|' || *p == '\r' || *p == '\n') break;
            const char* s = p;
            while (*p && *p != ' ' && *p != '\t' && *p != '|' && *p != '\r' && *p != '\n') ++p;
            const std::string tok(s, p);
            const size_t eq = tok.find('=');
            if (eq == std::string::npos) continue;
            const std::string key = tok.substr(0, eq);
            const char* val = tok.c_str() + eq + 1;
            if (key == "mip")
                k.mip = (uint32_t)strtoul(val, nullptr, 10);
            else if (key == "exact")
                k.exact = atoi(val) != 0;
            else if (key == "chain")
                k.chain = atoi(val) != 0;
            else if (key == "degenerate")
                flagged = atoi(val) != 0;
            else if (key == "ssim")
                k.ssim = (float)atof(val);
            else if (key == "place")
                k.place = atoi(val) != 0;
        }
        if (flagged && k.exact)
            degen.insert((uint32_t)low);
        else
            TexLodMergeLink(into, (uint32_t)low, k);
        if (*p == '\r' || *p == '\n') break;
    }
    return true;
}

bool ParseLegacyLine(const char* line, Records& into, std::set<uint32_t>& degen)
{
    unsigned lo = 0;
    unsigned hi = 0;
    if (sscanf_s(line, " %x = %x", &lo, &hi) != 2 || !lo) return false;
    std::vector<uint32_t> rivals;
    std::string note;
    if (const char* c = strchr(line, ';')) {
        ++c;
        while (*c == ' ' || *c == '\t') ++c;
        if (strncmp(c, "ini=", 4) == 0 || strncmp(c, "vs=", 3) == 0) {
            const char* e = strstr(c, " ;");
            const char* stop = e ? e : c + strlen(c);
            if (c[0] == 'v')
                for (const char* p = c + 3; p < stop;) {
                    char* end = nullptr;
                    const unsigned long h = strtoul(p, &end, 16);
                    if (end == p) break;
                    if (h) rivals.push_back((uint32_t)h);
                    p = end;
                    if (*p != ',') break;
                    ++p;
                }
            c = e ? e + 2 : stop;
        }
        note = Trim(c, c + strlen(c));
    }
    auto starts = [&](const char* s) { return note.rfind(s, 0) == 0; };
    if (!hi) {
        const size_t p = note.find("exact mip of");
        if (!starts("degenerate") || p == std::string::npos) return false;
        into.erase(lo);
        degen.insert(lo);
        return true;
    }
    into.erase(lo);
    degen.erase(lo);
    Link k;
    k.high = hi;
    k.exact = starts("mip-exact") || starts("lineage");
    k.chain = starts("lineage-chain") || starts("content-chain");
    if (starts("mip-exact")) k.mip = (uint32_t)strtoul(note.c_str() + 9, nullptr, 10);
    if (k.exact || rivals.size() < 2) {
        TexLodMergeLink(into, lo, k);
        return true;
    }
    for (uint32_t h : rivals) {
        Link x = k;
        x.high = h;
        TexLodMergeLink(into, lo, x);
    }
    return true;
}

void LoadFile(const std::wstring& path, int& files, Records& into, std::set<uint32_t>& degen)
{
    if (path.empty()) return;
    FILE* f = _wfsopen(path.c_str(), L"r", _SH_DENYNO);
    if (!f) return;
    ++files;
    std::string line;
    char chunk[1024];
    while (fgets(chunk, sizeof chunk, f)) {
        line += chunk;
        if (line.back() != '\n' && !feof(f)) continue;
        if (!ParseLine(line.c_str(), into, degen)) ParseLegacyLine(line.c_str(), into, degen);
        line.clear();
    }
    fclose(f);
}

// Returns the look-alike links from before tex_look it dropped.
size_t Normalize(Records& rs, std::set<uint32_t>& degen)
{
    size_t old = 0;
    for (auto it = rs.begin(); it != rs.end();) {
        auto& links = it->second.links;
        const size_t was = links.size();
        links.erase(std::remove_if(links.begin(), links.end(), [](const Link& k) { return !k.exact && k.ssim <= 0; }),
            links.end());
        old += was - links.size();
        if (std::any_of(links.begin(), links.end(), [](const Link& k) { return k.exact; }))
            links.erase(
                std::remove_if(links.begin(), links.end(), [](const Link& k) { return !k.exact; }), links.end());
        if (TexLodExact(it->second) && links.size() > 1) degen.insert(it->first);
        if (links.empty() || degen.count(it->first))
            it = rs.erase(it);
        else
            ++it;
    }
    return old;
}

} // namespace

std::wstring TexLodCache::CachePath() const
{
    return cache_dir_.empty() ? std::wstring() : (cache_dir_ + L"\\lod_tex_cache.lodcache");
}

// The links of records_ `keep` accepts (all, without one), one line per target, then degenerate_, to `path` through
// a temp file. Caller holds mutex_.
bool TexLodCache::WriteRecords(const std::wstring& path, bool (*keep)(const Link& k, void* user), void* user)
{
    std::map<uint32_t, std::vector<std::pair<uint32_t, const Link*>>> by_target;
    for (auto& kv : records_)
        for (const Link& k : kv.second.links)
            if (!keep || keep(k, user)) by_target[k.high].emplace_back(kv.first, &k);
    const std::wstring tmp = path + L".tmp";
    FILE* f = _wfsopen(tmp.c_str(), L"w", _SH_DENYNO);
    if (!f) {
        logger::write("TexLod: cannot write %S", tmp.c_str());
        return false;
    }
    fputs(kHeader, f);
    for (auto& t : by_target) {
        std::sort(t.second.begin(), t.second.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        fprintf(f, "%08x = ", t.first);
        for (size_t i = 0; i < t.second.size(); ++i) {
            if (i) fputs(" | ", f);
            WriteEntry(f, t.second[i].first, *t.second[i].second);
        }
        fputc('\n', f);
    }
    if (!degenerate_.empty()) {
        fputs("degenerate =", f);
        for (uint32_t low : degenerate_) fprintf(f, " %08x", low);
        fputc('\n', f);
    }
    fclose(f);
    if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        logger::write("TexLod: cannot replace %S (err %lu)", path.c_str(), GetLastError());
        return false;
    }
    return true;
}

// The whole cache. Caller holds mutex_.
void TexLodCache::RewriteCache()
{
    const std::wstring path = CachePath();
    if (path.empty() || !g_settings.texlod_cache || !WriteRecords(path, nullptr, nullptr)) return;
    dirty_ = false;
    for (const auto& seed : imported_) {
        if (MoveFileW(seed.first.c_str(), seed.second.c_str()))
            logger::write("TexLod: seed %S is in the cache now -- renamed %S", seed.first.c_str(), seed.second.c_str());
        else
            logger::write(
                "TexLod: seed %S is in the cache now, but cannot be renamed (err %lu) -- it is taken again next "
                "start",
                seed.first.c_str(), GetLastError());
    }
    imported_.clear();
}

void TexLodCache::ImportSeed(const std::wstring& path, const wchar_t* name)
{
    Records seed;
    std::set<uint32_t> seed_degen;
    int files = 0;
    LoadFile(path, files, seed, seed_degen);
    Normalize(seed, seed_degen);
    size_t proven = 0;
    size_t added = 0;
    size_t proved_guess = 0;
    size_t gave_mip = 0;
    size_t raised = 0;
    size_t kept = 0;
    size_t listed = 0;
    size_t disagree = 0;
    for (uint32_t low : seed_degen)
        if (degenerate_.insert(low).second) {
            records_.erase(low);
            ++listed;
        }
    for (auto& kv : seed) {
        auto& links = kv.second.links;
        links.erase(std::remove_if(links.begin(), links.end(), [](const Link& k) { return !k.exact; }), links.end());
        if (links.empty()) continue;
        ++proven;
        const uint32_t low = kv.first;
        if (degenerate_.count(low)) {
            ++kept;
            continue;
        }
        auto it = records_.find(low);
        if (it == records_.end()) {
            records_.emplace(low, std::move(kv.second));
            ++added;
            continue;
        }
        if (!TexLodExact(it->second)) {
            it->second = std::move(kv.second);
            ++proved_guess;
            continue;
        }
        const Link& s = links.front();
        Link& e = it->second.links.front();
        const uint32_t one = e.high == s.high ? e.high : OneTexture(e.high, s.high);
        if (one == e.high) {
            if (e.high == s.high && !e.mip && s.mip) {
                e.mip = s.mip;
                ++gave_mip;
            } else {
                ++kept;
            }
            continue;
        }
        if (one == s.high) {
            e = s;
            ++raised;
            continue;
        }
        records_.erase(it);
        degenerate_.insert(low);
        ++disagree;
    }
    SYSTEMTIME st;
    GetLocalTime(&st);
    wchar_t when[32];
    swprintf_s(when, L"%04u%02u%02u-%02u%02u%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    imported_.erase(std::remove_if(imported_.begin(), imported_.end(), [&](const auto& f) { return f.first == path; }),
        imported_.end());
    imported_.emplace_back(path, cache_dir_ + L"\\DISABLED_" + when + L"_" + name);
    MarkDirty();
    logger::write("TexLod: seed %S -- %zu proven low(s): %zu new, %zu proved a look-alike, %zu gave a mip level, "
                  "%zu moved to the higher of one texture, %zu kept as we had them; degenerate: %zu from its list, "
                  "%zu where its proof and ours name different targets",
        path.c_str(), proven, added, proved_guess, gave_mip, raised, kept, listed, disagree);
}

void TexLodCache::LoadAll()
{
    if (dirty_) RewriteCache(); // a reload reads the file back: what was learned since the last write goes in first
    std::vector<uint32_t> held;
    held.reserve(records_.size());
    for (auto& kv : records_) held.push_back(kv.first);
    records_.clear();
    degenerate_.clear();
    if (!g_settings.texlod_cache) {
        feeders_.clear();
        PublishDegenerate();
        for (uint32_t low : held) Publish(low);
        logger::write("TexLod: texture match cache off (lod_cache=0) -- nothing read, learned or saved");
        return;
    }
    int files = 0;
    LoadFile(CachePath(), files, records_, degenerate_);
    const size_t old = Normalize(records_, degenerate_);
    if (!cache_dir_.empty()) {
        WIN32_FIND_DATAW sf;
        HANDLE hs = FindFirstFileW((cache_dir_ + L"\\*.lodcache").c_str(), &sf);
        if (hs != INVALID_HANDLE_VALUE) {
            do {
                if (sf.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
                const size_t n = wcslen(sf.cFileName);
                if (n < 9 || _wcsicmp(sf.cFileName + n - 9, L".lodcache") != 0) continue; // not a short-name match
                if (_wcsicmp(sf.cFileName, L"lod_tex_cache.lodcache") == 0 ||
                    _wcsnicmp(sf.cFileName, L"DISABLED_", 9) == 0)
                    continue;
                ImportSeed(cache_dir_ + L"\\" + sf.cFileName, sf.cFileName);
            } while (FindNextFileW(hs, &sf));
            FindClose(hs);
        }
    }
    PublishDegenerate();
    for (uint32_t low : held) Publish(low); // one no file holds now leaves the table
    size_t everywhere = 0;
    for (auto& kv : records_) {
        Publish(kv.first);
        everywhere += TexLodExact(kv.second);
    }
    logger::trace("TexLod: loaded %zu LOD lows from %S%s: %zu swapped everywhere, %zu look-alike (%s), "
                  "%zu degenerate (never swapped); %zu look-alike link(s) from before tex_look dropped; "
                  "checked at engine load",
        records_.size() + degenerate_.size(), CachePath().c_str(), files ? "" : " (none yet)", everywhere,
        records_.size() - everywhere, g_settings.texlod_resemblance ? "used" : "learned, not used", degenerate_.size(),
        old);
}

void TexLodCache::Init(const std::wstring& cache_dir)
{
    std::lock_guard l(mutex_);
    if (!cache_dir.empty()) cache_dir_ = cache_dir;
    LoadAll();
}

void TexLodCache::SetCache(bool on)
{
    std::lock_guard l(mutex_);
    if (g_settings.texlod_cache == on) return;
    if (!on && dirty_) RewriteCache(); // what was learned while it was on is kept for when it is on again
    g_settings.texlod_cache = on;
    LoadAll();
    if (on) logger::write("TexLod: texture match cache on -- read back; learning and saving again");
}

void TexLodCache::FlushIfQuiet(unsigned quiet_ms)
{
    std::lock_guard l(mutex_);
    if (dirty_ && GetTickCount64() - dirty_at_ >= quiet_ms) RewriteCache();
}

void TexLodCache::OnExit()
{
    std::lock_guard l(mutex_);
    if (!dirty_) return;
    RewriteCache();
    logger::write("TexLod: exit -- what was learned since the last write is in the cache");
}

bool TexLodCache::Export(const std::wstring& path, bool (*keep)(uint32_t high, void* user), void* user)
{
    struct Keep {
        bool (*keep)(uint32_t, void*);
        void* user;
    } k{ keep, user };
    std::lock_guard l(mutex_);
    return WriteRecords(
        path,
        [](const Link& link, void* u) {
            const Keep* k = static_cast<const Keep*>(u);
            return link.exact && k->keep(link.high, k->user);
        },
        &k);
}
