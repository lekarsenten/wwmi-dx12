// The crawl. See tex_crawl.h.
#include "tex_crawl.h"
#include "tex_lod.h"
#include "tex_look.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/addon.h"
#include "../core/log.h"
#include <windows.h>
#include <algorithm>
#include <filesystem>
#include <unordered_set>

TexCrawl g_tex_crawl;

void TexCrawl::Save()
{
    std::unordered_map<uint64_t, std::vector<uint32_t>> seen;
    {
        std::lock_guard l(mutex_);
        seen = seen_;
    }
    std::unordered_set<uint32_t> textures;
    unsigned placed = 0;
    unsigned split = 0;
    for (const auto& kv : seen) {
        uint32_t one = 0;
        bool two = false;
        for (const uint32_t h : kv.second) {
            const uint32_t known = g_tex_lod.Known(h);
            const uint32_t top = known ? known : h;
            textures.insert(top);
            two = two || (one && one != top);
            if (!one) one = top;
        }
        if (two) {
            ++split;
            continue;
        }
        g_tex_look.AddMeshPlace(kv.first, one);
        ++placed;
    }
    if (g_addon.data_dir.empty()) {
        logger::write("TexCrawl: %u mesh place(s) kept; no data folder, so no proofs written", placed);
        return;
    }
    if (file_.empty()) {
        const std::filesystem::path dir = std::filesystem::path(g_addon.data_dir) / L"crawl";
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        SYSTEMTIME st;
        GetLocalTime(&st);
        wchar_t name[48];
        swprintf_s(name, L"crawl_%04u%02u%02u_%02u%02u%02u.lodcache", st.wYear, st.wMonth, st.wDay, st.wHour,
            st.wMinute, st.wSecond);
        file_ = (dir / name).wstring();
    }
    const std::wstring& path = file_;
    const bool ok = g_tex_lod.Export(
        path, [](uint32_t high, void* u) { return static_cast<std::unordered_set<uint32_t>*>(u)->count(high) != 0; },
        &textures);
    logger::write("TexCrawl: %u of %zu mesh place(s) kept, %u drawn with two textures; %zu character texture(s), their "
                  "proofs %s %S",
        placed, seen.size(), split, textures.size(), ok ? "written to" : "NOT written to", path.c_str());
}

void TexCrawl::Set(bool on)
{
    on_.store(on, std::memory_order_relaxed);
    logger::write(
        "TexCrawl: %s", on ? "on -- every texture's mips proven, every character draw's slots recorded" : "off");
    if (!on) FlushIfQuiet(0);
}

void TexCrawl::Note(uint64_t place, uint32_t hash)
{
    std::lock_guard l(mutex_);
    std::vector<uint32_t>& v = seen_[place];
    if (std::find(v.begin(), v.end(), hash) != v.end()) return;
    v.push_back(hash);
    dirty_ = true;
    new_at_ = GetTickCount64();
}

void TexCrawl::FlushIfQuiet(unsigned quiet_ms)
{
    {
        std::lock_guard l(mutex_);
        const uint64_t now = GetTickCount64();
        if (!dirty_ || (now - new_at_ < quiet_ms && now - saved_at_ < 60000)) return;
        dirty_ = false;
        saved_at_ = now;
    }
    Save();
}
