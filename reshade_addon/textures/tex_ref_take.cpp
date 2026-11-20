// The reference take. See tex_ref_take.h.
#include "tex_ref_take.h"
#include "tex_crawl.h"
#include "tex_identity.h"
#include "tex_lod.h"
#include "tex_look.h"
#include "tex_look_sig.h"
#include "tex_upload_identity.h"
#include "../migoto/dx12_bridge.h" // DumpDir: texsave writes under it
#include "../migoto/dx12_dump_readback.h"
#include "../migoto/port_api.h"
#include "../core/log.h"
#include <cstdio>
#include <unordered_set>

TexRefTake g_tex_ref_take;

namespace {

enum class Got : uint8_t {
    Kept,
    AsDense,
    Flat,
    Unread,
    SizeSkipped,
    NotCopied,
    OtherContent,
    Count,
};

const char* NameOf(Got g)
{
    switch (g) {
    case Got::Kept:
        return "kept";
    case Got::AsDense:
        return "as dense already";
    case Got::Flat:
        return "too flat";
    case Got::Unread:
        return "not read";
    case Got::SizeSkipped:
        return "size skipped";
    case Got::NotCopied:
        return "not copied";
    default:
        return "its copy hashes otherwise";
    }
}

struct Tally {
    unsigned n[(int)Got::Count] = {};
    void Add(Got g) { ++n[(int)g]; }
    unsigned operator[](Got g) const { return n[(int)g]; }
};

// A texture's mip0 from the pre-drain batch it was flushed in, and the hash that content gives.
struct Mip0 {
    D3D12_RESOURCE_DESC d = {};
    TexAssetLayout L;
    std::vector<uint8_t> tight;
    uint32_t hash = 0;
};
bool Mip0Of(ID3D12Resource* res, Mip0& m)
{
    unsigned bytes = 0;
    unsigned pitch = 0;
    const auto* data = static_cast<const uint8_t*>(MigotoPort::PreDrainLookup(res, &bytes, &pitch));
    m.d = res->GetDesc();
    if (!data || !TexAssetLayoutOf(m.d, 0, m.L) || !m.L.rows || pitch < m.L.tight_row ||
        bytes < (size_t)pitch * (m.L.rows - 1) + m.L.tight_row)
        return false;
    m.tight = TightFromUpload(data, pitch, m.L);
    m.hash = TexIdentityHash(PortApi::Crc32c(0, m.tight.data(), m.tight.size()), m.L.w, m.L.h,
        m.d.MipLevels ? m.d.MipLevels : 1, m.d.DepthOrArraySize ? m.d.DepthOrArraySize : 1, (uint32_t)m.d.Format);
    return true;
}

// One texture's read-back copy kept as the reference of `target`; `d` is its desc.
Got TakeOne(ID3D12Resource* res, uint32_t hash, uint32_t target, D3D12_RESOURCE_DESC& d)
{
    Mip0 m;
    const bool copied = Mip0Of(res, m);
    d = m.d;
    if (!copied) return Got::NotCopied;
    const std::vector<uint8_t>& tight = m.tight;
    const TexAssetLayout& L = m.L;
    if (m.hash != hash) return Got::OtherContent;
    const TexLookCut cut = TexLookWholeOf(tight.data(), tight.size(), L.w, L.h, (uint32_t)d.Format);
    if (cut.blocks.empty()) return Got::SizeSkipped;
    switch (g_tex_look.TakeRef(hash, target, cut)) {
    case TexLook::RefTaken::Kept:
        return Got::Kept;
    case TexLook::RefTaken::AsDense:
        return Got::AsDense;
    case TexLook::RefTaken::Flat:
        return Got::Flat;
    default:
        return Got::Unread;
    }
}

} // namespace

void TexRefTake::Arm()
{
    std::lock_guard l(mutex_);
    if (collecting_.load(std::memory_order_relaxed)) return;
    until_ = g_frame_id + 2;
    last_ = "collecting the textures character draws read and the drawn textures a section names";
    detail_.clear();
    collecting_.store(true, std::memory_order_relaxed);
    logger::write("TEXREFS: collecting the textures game character draws read and the drawn textures a section names, "
                  "frames %u to %u",
        g_frame_id, until_);
}

bool TexRefTake::Watching() const
{
    return collecting_.load(std::memory_order_relaxed) || g_tex_crawl.On();
}

// The crawl's first sight of a texture on this thread, per kind of draw: one with a reference verdict already is left
// alone.
bool TexRefTake::FirstSight(uint32_t hash, bool named)
{
    thread_local std::unordered_set<uint32_t> t_character;
    thread_local std::unordered_set<uint32_t> t_named;
    return (named ? t_named : t_character).insert(hash).second && !g_tex_look.Taken(hash);
}

void TexRefTake::Keep(Seen& take, Seen& crawl, ID3D12Resource* res, uint32_t hash)
{
    std::lock_guard l(mutex_);
    if (collecting_.load(std::memory_order_relaxed)) {
        if (take.emplace(res, hash).second) res->AddRef();
        return;
    }
    if (!g_tex_crawl.On() || !attempted_.insert(hash).second) return;
    if (crawl.emplace(res, hash).second) res->AddRef();
}

void TexRefTake::Note(ID3D12Resource* res)
{
    const uint32_t hash = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(res));
    if (!hash || (!collecting_.load(std::memory_order_relaxed) && !FirstSight(hash, false))) return;
    Keep(seen_, crawl_seen_, res, hash);
}

void TexRefTake::NoteNamed(ID3D12Resource* res)
{
    const uint32_t hash = g_tex_identity.HashOf(reinterpret_cast<uint64_t>(res));
    if (!hash || (!collecting_.load(std::memory_order_relaxed) && !FirstSight(hash, true))) return;
    if (!PortApi::TextureHashHasSections(hash)) return;
    Keep(named_, crawl_named_, res, hash);
}

void TexRefTake::Tick()
{
    Seen seen;
    Seen named;
    bool crawl = false;
    {
        std::lock_guard l(mutex_);
        if (collecting_.load(std::memory_order_relaxed)) {
            if (g_frame_id <= until_) return;
            collecting_.store(false, std::memory_order_relaxed);
            seen.swap(seen_);
            named.swap(named_);
        } else if (!crawl_seen_.empty() || !crawl_named_.empty()) {
            seen.swap(crawl_seen_);
            named.swap(crawl_named_);
            crawl = true;
        } else
            return;
    }
    Take(seen, named, crawl);
    for (const auto& kv : seen) kv.first->Release();
    for (const auto& kv : named) kv.first->Release();
}

void TexRefTake::Take(const Seen& characters, const Seen& named, bool crawl)
{
    MigotoPort::PreDrainBegin();
    for (const auto& kv : characters) MigotoPort::PreDrainAdd(kv.first);
    for (const auto& kv : named)
        if (!characters.count(kv.first)) MigotoPort::PreDrainAdd(kv.first);
    MigotoPort::PreDrainFlush();
    Tally ct;
    for (const auto& [res, hash] : characters) {
        const uint32_t known = g_tex_lod.Known(hash);
        D3D12_RESOURCE_DESC d;
        ct.Add(TakeOne(res, hash, known ? known : hash, d));
    }
    Tally nt;
    std::string flat;
    std::string skipped;
    size_t sections = 0;
    for (const auto& [res, hash] : named) {
        wchar_t section[256] = L"";
        PortApi::TextureOverrideSection(hash, section, _countof(section));
        if (!*section) continue;
        ++sections;
        D3D12_RESOURCE_DESC d;
        const Got g = TakeOne(res, hash, hash, d);
        nt.Add(g);
        logger::report("TEXREFS-UI %08x %ux%u fmt=%u %ls: %s", hash, (unsigned)d.Width, (unsigned)d.Height,
            (unsigned)d.Format, section, NameOf(g));
        if (g != Got::Flat && g != Got::SizeSkipped) continue;
        char b[48];
        std::snprintf(b, sizeof b, " %08x %ux%u", hash, (unsigned)d.Width, (unsigned)d.Height);
        (g == Got::Flat ? flat : skipped) += b;
    }
    MigotoPort::PreDrainBegin();
    char b[768];
    std::snprintf(b, sizeof b,
        "%zu texture(s) read by character draws: %u new or denser reference(s); %u already had one as dense or are "
        "too flat; %u of a size or format signatures skip; %u not copied; %u whose copy hashes otherwise. %zu drawn "
        "texture(s) a section names: %u new or denser reference(s); %u as dense already; %u too flat; %u of a size or "
        "format signatures skip; %u not read; %u not copied; %u whose copy hashes otherwise",
        characters.size(), ct[Got::Kept], ct[Got::AsDense] + ct[Got::Flat] + ct[Got::Unread], ct[Got::SizeSkipped],
        ct[Got::NotCopied], ct[Got::OtherContent], sections, nt[Got::Kept], nt[Got::AsDense], nt[Got::Flat],
        nt[Got::SizeSkipped], nt[Got::Unread], nt[Got::NotCopied], nt[Got::OtherContent]);
    logger::write("TEXREFS%s: %s", crawl ? " (crawl, each texture's first sight)" : "", b);
    std::string detail;
    if (!flat.empty()) detail += "too flat:" + flat + "\n";
    if (!skipped.empty()) detail += "size skipped:" + skipped + "\n";
    std::lock_guard l(mutex_);
    last_ = b;
    detail_ = detail;
}

std::string TexRefTake::Save(uint32_t hash)
{
    std::vector<ID3D12Resource*> live;
    g_tex_identity.ForEachHash([&](uint64_t key, uint32_t h) {
        if (h != hash) return;
        auto* res = reinterpret_cast<ID3D12Resource*>(key);
        res->AddRef();
        live.push_back(res);
    });
    char b[MAX_PATH + 128];
    if (live.empty()) {
        std::snprintf(b, sizeof b, "no loaded texture has hash %08x: open where it is drawn and try again", hash);
        return b;
    }
    MigotoPort::PreDrainBegin();
    for (ID3D12Resource* res : live) MigotoPort::PreDrainAdd(res);
    MigotoPort::PreDrainFlush();
    std::string out;
    size_t other = 0;
    for (ID3D12Resource* res : live) {
        Mip0 m;
        if (!Mip0Of(res, m)) continue;
        if (m.hash != hash) {
            ++other;
            continue;
        }
        const std::wstring dir = MigotoPort::DumpDir() + L"\\pipe";
        CreateDirectoryW(MigotoPort::DumpDir().c_str(), nullptr);
        CreateDirectoryW(dir.c_str(), nullptr);
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\tex_%08x_%ux%u.dds", dir.c_str(), hash, m.L.w, m.L.h);
        if (!TexWriteDds(path, m.tight, m.L, (uint32_t)m.d.Format)) {
            std::snprintf(b, sizeof b, "ERR cannot write %S", path);
            out = b;
            break;
        }
        logger::write("PIPE: texsave %08x %ux%u fmt=%u -> %S", hash, m.L.w, m.L.h, (unsigned)m.d.Format, path);
        std::snprintf(b, sizeof b, "%S (%ux%u fmt=%u, mip 0)", path, m.L.w, m.L.h, (unsigned)m.d.Format);
        out = b;
        break;
    }
    MigotoPort::PreDrainBegin();
    for (ID3D12Resource* res : live) res->Release();
    if (out.empty()) {
        std::snprintf(b, sizeof b,
            "ERR %zu loaded texture(s) carry hash %08x, none could be read back with that content (%zu hold another "
            "now)",
            live.size(), hash, other);
        out = b;
    }
    return out;
}

std::string TexRefTake::Last() const
{
    std::lock_guard l(mutex_);
    return last_;
}

std::string TexRefTake::Command(const std::vector<std::string>& args)
{
    if (args.size() == 1 && args[0] == "take") {
        Arm();
        return "collecting for two frames; texrefs shows the result";
    }
    if (!args.empty()) return "ERR texrefs take, or texrefs for the last result";
    std::lock_guard l(mutex_);
    return last_.empty() ? "no take yet" : last_ + "\n" + detail_;
}
