#include "tex_gc.h"
#include "tex_defer.h"
#include "tex_replace.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include "tex_toggles.h"
#include "../instrumentation/vram_meter.h"
#include "../migoto/dx12_pipeline.h"
#include "mod_texture_store.h"
#include "../core/log.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <map>

TexGc g_tex_gc;

namespace {
constexpr uint64_t kMb = 1ull << 20;
constexpr uint64_t kSampleMs = 250;
constexpr uint64_t kGameWindowMs = 10000;
constexpr uint64_t kRunGapMs = 250; // on screen: drawn this recently; a run of draws survives a gap this long
constexpr uint64_t kSettleMs = 1000;
constexpr uint64_t kQuietMs = 5000; // after a collection that could not act on tight or a raise, before they ask again
constexpr uint64_t kIdleMs[3] = { 5000, 10000, 60000 }; // unseen this long: 0 and 1 give way first, 2 goes down to 1
constexpr uint64_t kOnScreenMs = 3000; // tight this long before what is on screen is lowered
constexpr auto kAfterPass = std::chrono::seconds(1); // a pass's old copies die after it, still in the usage meanwhile
constexpr uint64_t kFarMs = 5000; // A20: drawn smaller through this long before it goes down to match
constexpr unsigned kMaxLower = 64;
constexpr unsigned kMaxEvict = 32;
constexpr uint64_t kMaxEvictBytes = 512 * kMb;
constexpr unsigned kMaxLowerDrawn = 32;
constexpr uint64_t kMaxRaiseBytes = 256 * kMb;

double Mb(uint64_t b)
{
    return (double)b / (double)kMb;
}
unsigned DrawnDown(
    int index, uint64_t since) // A20: how many mips down the game drew its own at its sharpest since then
{
    if (!g_settings.tex_distance) return 0;
    const unsigned k = g_mod_textures.Sharpest(index, since);
    return k == ModTextureStore::kMipUnknown ? 0 : k;
}
} // namespace

void TexGc::OnUse(ID3D12Resource* res)
{
    g_mod_textures.TouchRes(res);
}

unsigned TexGc::Far(const Tracked& t, int index, uint64_t now)
{
    return t.run_ms && t.run_ms + kFarMs <= now ? (std::min)(DrawnDown(index, now - kFarMs), t.most) : 0;
}

unsigned TexGc::Floor(const Tracked& t, int index, uint64_t now)
{
    return (std::max)(t.ceiling, (std::min)(DrawnDown(index, now - kFarMs), t.most));
}

void TexGc::Tick()
{
    const bool collects = TexCollects(g_settings.tex_mod_budget);
    if (collects != observing_) {
        MigotoPort::SetTextureUseObserver(collects ? &TexGc::OnUse : nullptr);
        g_mod_textures.SetTouch(collects);
        observing_ = collects;
    }
    const bool mips = collects && g_settings.tex_distance;
    if (mips != mips_) {
        g_mod_textures.SetMips(mips);
        mips_ = mips;
        decided_loads_ = ~0u;
        census_.clear();
    }
    const uint64_t now = GetTickCount64();
    const unsigned gen = PortApi::LoadGeneration();
    if (gen != seen_gen_ || !collects) {
        if (!tracked_.empty())
            logger::report("TEXGEN: every generation cleared (%zu texture(s)) -- %s", tracked_.size(),
                gen != seen_gen_ ? "the inis were reloaded" : "A19 is at Original");
        seen_gen_ = gen;
        tracked_.clear();
        let_go_.clear();
    }
    const bool first_loads = g_tex_defer.Loading();
    if (first_loads) loading_ms_ = now;
    const bool loading =
        first_loads || g_tex_replace.Busy() || std::chrono::steady_clock::now() - g_tex_replace.LastEnd() < kAfterPass;
    const bool changed = g_vram.BudgetChanged();
    if (changed || now >= next_sample_ms_) Sample(now, loading);
}

void TexGc::Stop()
{
    MigotoPort::SetTextureUseObserver(nullptr);
    g_mod_textures.SetTouch(false);
    g_mod_textures.SetMips(false);
    observing_ = false;
    mips_ = false;
}

void TexGc::Sample(uint64_t now, bool loading)
{
    next_sample_ms_ = now + kSampleMs;
    const uint64_t mods = g_mod_textures.Bytes();
    const bool first = !reading_.valid;
    VramReading v;
    reading_.mods = mods;
    reading_.valid = g_vram.Read(&v);
    if (reading_.valid) {
        uint64_t budget = v.budget;
        if (g_settings.tex_vram_cap_mb) budget = (std::min)(budget, (uint64_t)g_settings.tex_vram_cap_mb * kMb);
        reading_.budget = budget;
        reading_.used = v.used;
        if (!loading || game_.empty()) {
            reading_.game_now = v.used > mods ? v.used - mods : 0;
            game_.push_back({ now, reading_.game_now });
        }
        while (game_.size() > 1 && game_.front().ms + kGameWindowMs < now) game_.pop_front();
        uint64_t game = 0;
        for (const GameSample& s : game_) game = (std::max)(game, s.game);
        reading_.game = game;
        const uint64_t margin = budget / 20;
        const uint64_t band = budget / 40;
        reading_.available = budget > reading_.game_now + margin ? budget - reading_.game_now - margin : 0;
        reading_.room = reading_.available > band ? reading_.available - band : 0;
        const uint64_t calm = budget > game + margin ? budget - game - margin : 0;
        reading_.raise_room = calm > band ? calm - band : 0;
    }
    const bool was = tight_;
    tight_ = reading_.valid && (mods > reading_.available || (tight_ && mods > reading_.room));
    if (tight_ && !was) tight_ms_ = now;
    if (reading_.valid && (first || tight_ != was))
        logger::write(
            "TEXGC: video memory %s -- budget %.0f MB, in use %.0f MB, the game's %.0f MB now and %.0f MB at most "
            "over the last 10 s, mod textures %.0f of %.0f MB allowed (short above %.0f MB)",
            first        ? "at start"
                : tight_ ? "short"
                         : "enough again",
            Mb(reading_.budget), Mb(reading_.used), Mb(reading_.game_now), Mb(reading_.game), Mb(mods),
            Mb(reading_.room), Mb(reading_.available));

    raise_ready_ = false;
    far_due_ = g_settings.tex_distance && decided_loads_ != g_tex_defer.Loaded() && now >= collect_ms_ + kFarMs;
    const bool may_raise = !tight_ && now >= raise_quiet_ms_ + kQuietMs;
    const bool census = mips_ && now >= census_ms_ + kFarMs;
    std::map<std::wstring, std::array<unsigned, ModTextureStore::kMips + 1>> drawn;
    for (auto& kv : tracked_) {
        Tracked& t = kv.second;
        const int index = g_mod_textures.IndexOf(t.res);
        const uint64_t last = g_mod_textures.LastUse(index);
        if (last > t.last_ms) t.last_ms = last;
        if (census && last + kFarMs >= now) {
            const unsigned k = g_mod_textures.Sharpest(index, now - kFarMs);
            ++drawn[t.mod][k == ModTextureStore::kMipUnknown ? ModTextureStore::kMips : k];
        }
        if (t.last_ms && t.last_ms + kRunGapMs >= now) {
            if (!t.run_ms) t.run_ms = t.last_ms;
        } else {
            t.run_ms = 0;
        }
        if (t.level > Floor(t, index, now) && may_raise && Watched(t, now)) raise_ready_ = true;
        if (Far(t, index, now) > t.level) far_due_ = true;
    }
    if (census) {
        census_ms_ = now;
        std::wstring line;
        for (const auto& [mod, n] : drawn) {
            line += (line.empty() ? L"" : L", ") + mod + L" ";
            for (unsigned k = 0; k <= ModTextureStore::kMips; ++k) line += (k ? L"/" : L"") + std::to_wstring(n[k]);
        }
        if (line != census_) {
            logger::report(
                "TEXMIP (A20): mod textures drawn in the last 5 s, by how many mips below its full size the game "
                "drew its own there (0/1/2/3 or more/not known) -- %ls",
                line.empty() ? L"none" : line.c_str());
            census_.swap(line);
        }
    }
}

bool TexGc::Watched(const Tracked& t, uint64_t now) const
{
    return t.run_ms && (std::max)(t.run_ms, loading_ms_) + g_settings.tex_watch_ms <= now;
}

bool TexGc::Due() const
{
    const uint64_t now = GetTickCount64();
    if (now < collect_ms_ + kSettleMs || now < quiet_ms_ + kQuietMs) return false;
    return tight_ || raise_ready_ || far_due_;
}

void TexGc::Decide(std::vector<Item>& items)
{
    const uint64_t now = GetTickCount64();
    const uint64_t mods = reading_.mods;
    const bool sweep = tight_ || raise_ready_ || !far_due_;
    const uint64_t since = sweep_ms_;
    collect_ms_ = now;
    if (sweep) sweep_ms_ = now;
    decided_loads_ = g_tex_defer.Loaded();

    struct GenMoves {
        unsigned up[3] = {}; // promoted to 1, 2
        unsigned down = 0; // from 2 to 1
        unsigned out[3] = {}; // let go from 0, 1, 2
    };
    std::map<std::wstring, GenMoves> moves;

    std::unordered_map<std::wstring, Tracked> next;
    next.reserve(items.size());
    std::vector<Tracked*> tr;
    tr.reserve(items.size());
    for (Item& it : items) {
        it.evict = false;
        const auto old = tracked_.find(*it.fq_name);
        Tracked& t = next[*it.fq_name];
        if (old != tracked_.end()) {
            t = old->second;
            const uint64_t last = g_mod_textures.LastUse(g_mod_textures.IndexOf(it.dx12));
            if (last > t.last_ms) t.last_ms = last;
            if (sweep && t.last_ms > since && t.gen < 2) {
                ++t.gen;
                ++moves[*it.mod].up[t.gen];
            } else if (t.gen == 2 && t.last_ms + kIdleMs[2] <= now) {
                t.gen = 1;
                ++moves[*it.mod].down;
            }
        } else {
            t.level = it.now;
            t.last_ms = now;
            const auto back = let_go_.find(*it.fq_name);
            if (back != let_go_.end()) {
                t.level = (std::max)(it.now, back->second);
                t.returned = true;
                t.back_ms = now;
                let_go_.erase(back);
            }
        }
        t.res = it.dx12;
        t.mod = *it.mod;
        t.ceiling = it.ceiling;
        t.most = it.fixed ? 0 : it.most;
        tr.push_back(&t);
    }
    tracked_.swap(next);

    int64_t projected = (int64_t)mods;
    unsigned far_lowered = 0;
    std::vector<unsigned> raise_to(items.size());
    for (size_t i = 0; i < items.size(); ++i) {
        Item& it = items[i];
        Tracked& t = *tr[i];
        const int index = g_mod_textures.IndexOf(it.dx12);
        const unsigned down = Far(t, index, now);
        if (down > t.level) {
            t.level = down;
            if (down > it.now && down > it.ceiling) ++far_lowered;
        }
        raise_to[i] = Floor(t, index, now);
        it.want = it.fixed ? it.now : (std::max)(it.ceiling, (std::min)(t.level, it.most));
        projected += (int64_t)it.bytes[it.want] - (int64_t)it.bytes[it.now];
    }
    const int64_t room = reading_.valid ? (int64_t)(tight_ ? reading_.room : reading_.raise_room) : INT64_MAX;

    std::vector<size_t> up;
    if (!tight_ && now >= raise_quiet_ms_ + kQuietMs)
        for (size_t i = 0; i < items.size(); ++i)
            if (!items[i].fixed && items[i].want > raise_to[i] && tr[i]->last_ms + kRunGapMs >= now &&
                Watched(*tr[i], now))
                up.push_back(i);
    auto cost = [&](size_t i, unsigned to) { return items[i].bytes[to] - items[i].bytes[items[i].want]; };
    std::sort(up.begin(), up.end(), [&](size_t a, size_t b) { return cost(a, raise_to[a]) > cost(b, raise_to[b]); });
    uint64_t asked = 0;
    for (size_t i : up) asked += cost(i, raise_to[i]);
    asked = (std::min)(asked, kMaxRaiseBytes);
    const int64_t target = tight_ ? room : room - (int64_t)asked;

    unsigned lowered = 0;
    unsigned evicted = 0;
    unsigned lowered_drawn = 0;
    if ((tight_ || !up.empty()) && projected > target) {
        std::vector<size_t> tiers[2]; // idle; the rest off screen
        for (size_t i = 0; i < items.size(); ++i) {
            const Tracked& t = *tr[i];
            if (items[i].fixed || t.last_ms + kRunGapMs >= now) continue;
            const bool idle = t.last_ms + kIdleMs[t.gen] <= now;
            if (idle || tight_) tiers[idle ? 0 : 1].push_back(i);
        }
        uint64_t freed = 0;
        bool through = true; // every tier it reached was lowered as far as it goes and let go of
        for (std::vector<size_t>& unseen : tiers) {
            if (projected <= target) break;
            std::sort(unseen.begin(), unseen.end(), [&](size_t a, size_t b) {
                if (tr[a]->gen != tr[b]->gen) return tr[a]->gen < tr[b]->gen;
                if (tr[a]->last_ms != tr[b]->last_ms) return tr[a]->last_ms < tr[b]->last_ms;
                return *items[a].fq_name < *items[b].fq_name;
            });
            for (bool any = true; any && projected > target && lowered < kMaxLower;) {
                any = false;
                for (size_t i : unseen) {
                    if (projected <= target || lowered >= kMaxLower) break;
                    Item& it = items[i];
                    if (it.want >= it.most) continue;
                    projected -= (int64_t)(it.bytes[it.want] - it.bytes[it.want + 1]);
                    tr[i]->level = ++it.want;
                    ++lowered;
                    any = true;
                }
            }
            if (lowered >= kMaxLower && projected > target) {
                through = false;
                break;
            }
            size_t u = 0;
            for (; u < unseen.size() && projected > target && evicted < kMaxEvict && freed < kMaxEvictBytes; ++u) {
                Item& it = items[unseen[u]];
                Tracked& t = *tr[unseen[u]];
                if (t.held) continue;
                if (t.returned && t.last_ms <= t.back_ms) {
                    if (!t.told)
                        logger::write(
                            "TEXGC: %ls came back after it was let go, and no draw has been seen using it since -- "
                            "in use where no draw stamps it; lowered only, not let go again",
                            it.fq_name->c_str());
                    t.told = true;
                    continue;
                }
                it.evict = true;
                ++moves[*it.mod].out[t.gen];
                projected -= (int64_t)it.bytes[it.want];
                freed += it.bytes[it.now];
                ++evicted;
            }
            if (u < unseen.size() && projected > target) {
                through = false;
                break;
            }
        }
        while (tight_ && through && now >= tight_ms_ + kOnScreenMs && projected > target &&
            lowered_drawn < kMaxLowerDrawn) {
            Item* pick = nullptr;
            for (size_t i = 0; i < items.size(); ++i) {
                Item& it = items[i];
                if (it.fixed || it.evict || it.want >= it.most) continue;
                if (!pick || it.bytes[it.want] > pick->bytes[pick->want]) pick = &it;
            }
            if (!pick) break;
            projected -= (int64_t)(pick->bytes[pick->want] - pick->bytes[pick->want + 1]);
            ++pick->want;
            tr[pick - items.data()]->level = pick->want;
            ++lowered_drawn;
        }
    }

    unsigned raised = 0;
    uint64_t raised_bytes = 0;
    for (size_t i : up) {
        Item& it = items[i];
        unsigned to = raise_to[i];
        for (; to < it.want; ++to) {
            const uint64_t c = cost(i, to);
            if ((!raised_bytes || raised_bytes + c <= kMaxRaiseBytes) && projected + (int64_t)c <= room) break;
        }
        if (to == it.want) continue;
        const uint64_t c = cost(i, to);
        it.want = to;
        tr[i]->level = to;
        projected += (int64_t)c;
        raised_bytes += c;
        ++raised;
    }

    std::wstring promoted;
    std::wstring demoted;
    std::wstring released;
    for (const auto& [mod, m] : moves) {
        for (unsigned g = 1; g < 3; ++g)
            if (m.up[g])
                promoted += (promoted.empty() ? L"" : L", ") + mod + L" " + std::to_wstring(m.up[g]) + L" to " +
                    std::to_wstring(g);
        if (m.down) demoted += (demoted.empty() ? L"" : L", ") + mod + L" " + std::to_wstring(m.down);
        for (unsigned g = 0; g < 3; ++g)
            if (m.out[g])
                released += (released.empty() ? L"" : L", ") + mod + L" " + std::to_wstring(m.out[g]) + L" from " +
                    std::to_wstring(g);
    }
    if (!promoted.empty() || !demoted.empty() || !released.empty())
        logger::report("TEXGEN: promoted %ls; 2 to 1, unseen for a minute: %ls; let go, out of the generations: %ls",
            promoted.empty() ? L"none" : promoted.c_str(), demoted.empty() ? L"none" : demoted.c_str(),
            released.empty() ? L"none" : released.c_str());

    const bool acted = lowered || evicted || lowered_drawn;
    if (tight_ && acted) raise_quiet_ms_ = now;
    const bool waits = tight_ && now < tight_ms_ + kOnScreenMs;
    if ((tight_ && !acted && !waits) || (!tight_ && raise_ready_ && !raised && !lowered && !evicted)) quiet_ms_ = now;
    if (!acted && !raised && !far_lowered) return;
    logger::write(
        "TEXGC (%s): of %zu mod texture(s), %u lowered as the game draws them smaller, %u lowered and %u let go "
        "of those not on screen (unseen longest for their generation first), %u lowered on screen, %u raised "
        "(%.0f MB) -- mod textures %.0f MB, about %.0f MB after, of %.0f MB allowed (%.0f MB for raises)",
        tight_           ? "video memory short"
            : up.empty() ? "the game draws them smaller"
                         : "room for what is on screen",
        items.size(), far_lowered, lowered, evicted, lowered_drawn, raised, Mb(raised_bytes), Mb(mods),
        Mb(projected > 0 ? (uint64_t)projected : 0), Mb(reading_.room), Mb(reading_.raise_room));
}

void TexGc::Held(const std::wstring& fq_name)
{
    const auto it = tracked_.find(fq_name);
    if (it != tracked_.end()) it->second.held = true;
}

void TexGc::Swapped(const std::wstring& fq_name, ID3D12Resource* now)
{
    const auto it = tracked_.find(fq_name);
    if (it == tracked_.end()) return;
    if (now) {
        it->second.res = now;
    } else {
        let_go_[fq_name] = it->second.level;
        tracked_.erase(it);
    }
}
