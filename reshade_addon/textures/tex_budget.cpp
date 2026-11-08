#include "tex_budget.h"
#include "tex_defer.h"
#include "tex_encode.h"
#include "tex_gc.h"
#include "../addon_common.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include "tex_toggles.h"
#include "../core/dxgi_bc.h"
#include "../core/log.h"
#include "mod_texture_store.h"
#include <d3d11.h>
#include <algorithm>
#include <cwctype>
#include <vector>
#include "../addon/addon.h"
#include "../mods/mod_registry.h"

TexBudget g_tex_budget;

namespace {
constexpr unsigned kMaxDown = 4;
constexpr auto kSettle = std::chrono::seconds(1);
constexpr auto kRecheck = std::chrono::seconds(30);

uint64_t TextureBytes(UINT64 w, UINT h, unsigned mips, DXGI_FORMAT f)
{
    const uint32_t block = DxgiBcBlockBytes((uint32_t)f);
    const uint32_t px = DxgiPlainBytesPerPixel((uint32_t)f);
    uint64_t sum = 0;
    for (unsigned m = 0; m < mips; ++m) {
        sum += block ? ((w + 3) / 4) * ((h + 3) / 4) * block : w * h * (px ? px : 4);
        w = (std::max)(w / 2, (UINT64)1);
        h = (std::max)(h / 2, 1u);
    }
    return (sum + 0xFFFF) & ~0xFFFFull;
}

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s) c = towlower(c);
    return s;
}

// Every loaded ini's namespace (lower case) -> its mod, as the mod count names it (ModRegistry::Discover).
std::unordered_map<std::wstring, std::wstring> ModsOfNamespaces()
{
    std::unordered_map<std::wstring, std::wstring> mods;
    for (const DiscoveredMod& m : ModRegistry::Discover())
        for (const ModIni& i : m.inis) mods[Lower(i.ns)] = m.name;
    return mods;
}
} // namespace

bool TexBudget::Owns(const std::wstring& fq_name) const
{
    return lowered_.count(fq_name) || planned_.count(fq_name);
}

void TexBudget::Swapped(const std::wstring& fq_name, ID3D12Resource* now)
{
    g_tex_gc.Swapped(fq_name, now);
    const auto it = planned_.find(fq_name);
    if (it == planned_.end()) return;
    const Lowered& p = it->second;
    if (now && p.level)
        lowered_[fq_name] = Lowered{ p.stored, p.level, now };
    else
        lowered_.erase(fq_name);
    if (!now && p.level && p.stored.mips > p.level) {
        returning_[fq_name] = p;
        g_tex_defer.ReturnAt(fq_name, (unsigned)((std::max)(p.stored.width, (UINT64)p.stored.height) >> p.level));
    }
    planned_.erase(it);
}

void TexBudget::Kept(const std::wstring& fq_name)
{
    kept_.insert(fq_name);
}

void TexBudget::Held(const std::wstring& fq_name)
{
    g_tex_gc.Held(fq_name);
}

void TexBudget::Ended()
{
    planned_.clear();
}

bool TexBudget::Due(std::chrono::steady_clock::time_point now)
{
    const int step = g_settings.tex_mod_budget;
    if (step != step_) {
        step_ = step;
        step_moved_ = now;
        wanted_ = true;
    }
    const bool limited = TexBudgetMb(step) != 0;
    const bool collects = TexCollects(step);
    const unsigned loads = g_tex_defer.Loaded();
    if (loads != loads_seen_) {
        loads_seen_ = loads;
        loads_moved_ = now;
        wanted_ = wanted_ || limited;
    }
    if (limited && now - last_fit_ >= kRecheck) wanted_ = true;
    if (now - step_moved_ < kSettle) return false;
    const bool collect = collects && g_tex_gc.Due();
    if (!collect && (!wanted_ || now - loads_moved_ < kSettle)) return false;
    if (collects && !g_tex_encoder.Ready() && !g_tex_encoder.Failed()) {
        g_tex_encoder.Prepare(g_addon.d3d12_device);
        return false;
    }
    wanted_ = false;
    return true;
}

void TexBudget::Fit()
{
    const int step = g_settings.tex_mod_budget;
    const uint64_t budget = (uint64_t)TexBudgetMb(step) << 20;
    last_fit_ = std::chrono::steady_clock::now();

    struct Tex {
        PortApi::LoadedTexture t;
        D3D12_RESOURCE_DESC rd;
        Stored stored;
        unsigned now = 0;
        unsigned want = 0;
        unsigned most = 0; // the furthest down it may go
        bool fixed = false;
        bool evict = false;
    };
    std::vector<PortApi::LoadedTexture> all;
    PortApi::LoadedTexturesAll(all);
    std::vector<Tex> texs;
    texs.reserve(all.size());
    const bool can_encode = !g_tex_encoder.Failed();
    for (PortApi::LoadedTexture& t : all) {
        Tex x{ std::move(t), {}, {} };
        x.rd = x.t.dx12->GetDesc();
        auto lo = lowered_.find(x.t.load.fq_name);
        if (lo != lowered_.end() && lo->second.now != x.t.dx12) {
            lowered_.erase(lo);
            lo = lowered_.end();
        }
        if (lo != lowered_.end()) {
            x.stored = lo->second.stored;
            x.now = lo->second.level;
        } else {
            x.stored = { x.rd.Width, x.rd.Height, x.rd.MipLevels };
            const auto back = returning_.find(x.t.load.fq_name);
            if (back != returning_.end()) {
                const Lowered& r = back->second;
                if (x.rd.Width == r.stored.width >> r.level && x.rd.Height == r.stored.height >> r.level) {
                    x.stored = r.stored;
                    x.now = r.level;
                    lowered_[x.t.load.fq_name] = Lowered{ r.stored, r.level, x.t.dx12 };
                }
                returning_.erase(back);
            }
        }
        x.fixed =
            g_tex_replace.HalfSize(x.t.load.fq_name) || x.rd.DepthOrArraySize != 1 || kept_.count(x.t.load.fq_name);
        const bool bc = DxgiIsBc((uint32_t)x.rd.Format);
        const bool encodable = can_encode && x.stored.mips == 1 && IsBc7(x.rd.Format);
        for (unsigned k = 1; !x.fixed && k <= kMaxDown; ++k) {
            const UINT64 w = x.stored.width >> k;
            const UINT h = x.stored.height >> k;
            if ((std::max)(w, (UINT64)h) < kTexMinSide || (bc && (w % 4 || h % 4))) break;
            if (x.stored.mips <= k && !encodable) break;
            x.most = k;
        }
        x.want = x.now;
        texs.push_back(std::move(x));
    }

    if (mods_of_ns_.empty()) mods_of_ns_ = ModsOfNamespaces();
    std::vector<std::wstring> mods;
    std::unordered_map<std::wstring, size_t> mod_index;
    std::vector<std::vector<size_t>> members;
    std::vector<size_t> mod_of(texs.size());
    for (size_t i = 0; i < texs.size(); ++i) {
        const std::wstring ns = PortApi::NamespaceOfResource(texs[i].t.load.fq_name);
        const auto it = mods_of_ns_.find(Lower(ns));
        const std::wstring& mod = it != mods_of_ns_.end() ? it->second : ns;
        const auto at_mod = mod_index.emplace(Lower(mod), mods.size());
        if (at_mod.second) {
            mods.push_back(mod);
            members.emplace_back();
        }
        members[at_mod.first->second].push_back(i);
        mod_of[i] = at_mod.first->second;
    }

    auto at = [](const Tex& x, unsigned k) {
        if (x.fixed) return TextureBytes(x.rd.Width, x.rd.Height, x.rd.MipLevels, x.rd.Format) * x.rd.DepthOrArraySize;
        return TextureBytes(
            x.stored.width >> k, x.stored.height >> k, x.stored.mips > k ? x.stored.mips - k : 1u, x.rd.Format);
    };
    std::vector<uint64_t> before(mods.size());
    std::vector<char> over(mods.size());
    unsigned mods_over = 0;
    for (size_t m = 0; m < mods.size(); ++m) {
        uint64_t after = 0;
        for (size_t i : members[m]) {
            Tex& x = texs[i];
            before[m] += at(x, x.now);
            if (!x.fixed) x.want = 0;
            after += at(x, x.want);
        }
        while (budget && after > budget) {
            Tex* pick = nullptr;
            for (size_t i : members[m]) {
                Tex& x = texs[i];
                if (x.fixed || x.want >= x.most) continue;
                if (!pick || at(x, x.want) > at(*pick, pick->want) ||
                    (at(x, x.want) == at(*pick, pick->want) && x.t.load.fq_name < pick->t.load.fq_name))
                    pick = &x;
            }
            if (!pick) break;
            after -= at(*pick, pick->want) - at(*pick, pick->want + 1);
            ++pick->want;
        }
        if (budget && after > budget) {
            over[m] = 1;
            ++mods_over;
        }
    }

    if (TexCollects(step)) {
        std::vector<TexGc::Item> items;
        items.reserve(texs.size());
        for (size_t i = 0; i < texs.size(); ++i) {
            const Tex& x = texs[i];
            TexGc::Item it{ &x.t.load.fq_name, &mods[mod_of[i]], x.t.dx12, x.now, x.want, x.most, x.fixed, {} };
            for (unsigned k = 0; k < TexGc::kLevels; ++k) it.bytes[k] = at(x, k);
            items.push_back(it);
        }
        g_tex_gc.Decide(items);
        for (size_t i = 0; i < texs.size(); ++i) {
            texs[i].want = items[i].want;
            texs[i].evict = items[i].evict;
        }
    }

    std::vector<TexReplace::Order> orders;
    unsigned mods_changed = 0;
    for (size_t m = 0; m < mods.size(); ++m) {
        uint64_t after = 0;
        unsigned from_mips = 0;
        unsigned encoded = 0;
        unsigned raised = 0;
        unsigned evicted = 0;
        for (size_t i : members[m]) {
            Tex& x = texs[i];
            if (x.evict) {
                ++evicted;
                planned_[x.t.load.fq_name] = Lowered{ x.stored, x.want, nullptr };
                orders.push_back({ std::move(x.t), 0, 0, true });
                x.t.res = nullptr;
                continue;
            }
            after += at(x, x.want);
            if (x.fixed || x.want == x.now) continue;
            const bool mipped = x.stored.mips > 1;
            const bool from_file = !mipped && x.want < x.now && x.want > 0;
            const UINT shift = !mipped && x.want > x.now ? x.want - x.now : from_file ? x.want : 0;
            if (!mipped && x.want > x.now)
                ++encoded;
            else if (x.want < x.now)
                ++raised;
            else
                ++from_mips;
            if (mipped)
                x.t.load.maxsize =
                    x.want ? (unsigned)((std::max)(x.stored.width, (UINT64)x.stored.height) >> x.want) : 0;
            planned_[x.t.load.fq_name] = Lowered{ x.stored, x.want, nullptr };
            orders.push_back({ std::move(x.t), shift, at(x, x.want), false, from_file });
            x.t.res = nullptr;
        }
        if (!from_mips && !encoded && !raised && !evicted) continue;
        ++mods_changed;
        logger::write("TEXFIT %ls: %.0f -> %.0f MB of textures (budget %s): %u lowered from their mips, %u re-encoded, "
                      "%u raised, %u let go%s",
            mods[m].c_str(), (double)before[m] / 1048576.0, (double)after / 1048576.0, TexBudgetName(step), from_mips,
            encoded, raised, evicted, over[m] ? " -- still over: nothing left to lower" : "");
    }
    for (Tex& x : texs)
        if (x.t.res) x.t.res->Release();
    if (orders.empty()) return;
    logger::write(
        "TEXFIT (budget %s): %zu texture(s) of %u mod(s) to swap, of %zu loaded in %zu mod(s); %u mod(s) stay over",
        TexBudgetName(step), orders.size(), mods_changed, texs.size(), mods.size(), mods_over);
    if (g_tex_replace.Start(orders)) return;
    for (TexReplace::Order& o : orders)
        if (o.was.res) o.was.res->Release();
    planned_.clear();
    wanted_ = true;
}

void TexBudget::Tick()
{
    g_tex_replace.SetClient(this);
    g_tex_gc.Tick();
    const unsigned gen = PortApi::LoadGeneration();
    if (gen != seen_gen_) { // a reload loads every texture at its stored size
        seen_gen_ = gen;
        lowered_.clear();
        returning_.clear();
        mods_of_ns_.clear();
        kept_.clear();
    }
    if (!g_tex_replace.Busy() && Due(std::chrono::steady_clock::now())) Fit();
}
