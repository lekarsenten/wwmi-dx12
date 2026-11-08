// tex_slot_log.cpp -- see tex_slot_log.h.
#include "tex_slot_log.h"
#include "tex_lod.h"
#include "mod_texture_store.h"
#include "../core/log.h"
#include "../core/frame_clock.h"
#include "../migoto/port_api.h"
#include <cstdio>
#include <cstring>
#include <mutex>
#include <algorithm>
#include <cstdarg>

TexSlotLog g_tex_slot_log;

std::string TexSlotLog::BoundHow(uint32_t bound, uint32_t rule)
{
    if (bound == rule) return "its own hash";
    char b[48];
    if (const unsigned k = g_tex_lod.MipOf(bound)) {
        sprintf_s(b, "mip %u of the target", k);
        return b;
    }
    return "a look-alike of the target";
}

void TexSlotLog::Note(
    const char* site, ID3D12Resource* game_res, uint32_t bound, uint32_t rule, ID3D12Resource* mod_res, bool snapshot)
{
    const bool at_draw = strcmp(site, "draw") == 0;
    const uint64_t key = reinterpret_cast<uint64_t>(game_res) | (at_draw ? 1u : 0u);
    const int entry = mod_res ? g_mod_textures.IndexOf(mod_res) : -1;
    const long long value = ((long long)entry << 1) | (snapshot ? 1 : 0);
    {
        std::shared_lock l(mutex_);
        const auto it = logged_.find(key);
        if (it != logged_.end() && (it->second.value == value || it->second.changes >= kChanges)) return;
    }
    unsigned changes = 0;
    {
        std::unique_lock l(mutex_);
        auto [it, fresh] = logged_.try_emplace(key, Logged{ value, 0 });
        if (!fresh) {
            if (it->second.value == value || it->second.changes >= kChanges) return;
            it->second.value = value;
            changes = ++it->second.changes;
        }
    }
    const std::wstring name =
        mod_res ? g_mod_textures.NameOf(entry) : L"the game's own texture (its section selects nothing)";
    logger::write("TEXSLOT f=%u %s res=%p bound=%08x (%s) rule=%08x -> %ls (entry %d)%s%s", g_frame_id, site,
        (void*)game_res, bound, BoundHow(bound, rule).c_str(), rule, name.c_str(), entry,
        snapshot ? " -- from the selection snapshot" : "",
        changes == kChanges ? " -- its 8th change, no more logged for this texture" : "");
}

void TexSlotLog::Forget(uint64_t game_res)
{
    {
        std::shared_lock l(mutex_);
        if (!logged_.count(game_res) && !logged_.count(game_res | 1u)) return;
    }
    std::unique_lock l(mutex_);
    logged_.erase(game_res);
    logged_.erase(game_res | 1u);
}

TexTrace g_tex_trace;

std::string TexTrace::Arm(uint32_t hash, uint32_t frames)
{
    struct Ctx {
        uint32_t hash = 0;
        std::vector<const float*> slots;
        std::vector<Var> named;
    } ctx;
    ctx.hash = hash;
    PortApi::ForEachTextureRuleVar(
        [](uint32_t h, const wchar_t*, const float* var, void* user) {
            auto* c = (Ctx*)user;
            if (h == c->hash && std::find(c->slots.begin(), c->slots.end(), var) == c->slots.end())
                c->slots.push_back(var);
        },
        &ctx);
    PortApi::ForEachVariableSlot(
        [](const wchar_t* name, const float* slot, void* user) {
            auto* c = (Ctx*)user;
            if (std::find(c->slots.begin(), c->slots.end(), slot) == c->slots.end()) return;
            const wchar_t* leaf = wcsrchr(name, L'\\');
            c->named.push_back(Var{ leaf ? leaf + 1 : name, slot });
        },
        &ctx);
    const unsigned gen = PortApi::LoadGeneration();
    {
        std::lock_guard l(mutex_);
        vars_ = std::move(ctx.named);
        gen_ = gen;
    }
    hash_.store(hash, std::memory_order_relaxed);
    left_.store(frames, std::memory_order_release);
    char b[160];
    sprintf_s(b, "TEXTRACE armed: texture %08x for %u frame(s), %zu variable(s) its sections read", hash, frames,
        ctx.slots.size());
    logger::report("%s", b);
    return b;
}

bool TexTrace::Armed() const
{
    return left_.load(std::memory_order_acquire) != 0;
}

bool TexTrace::On(uint32_t hash) const
{
    return hash && left_.load(std::memory_order_acquire) != 0 && hash_.load(std::memory_order_relaxed) == hash;
}

void TexTrace::Line(const char* fmt, ...)
{
    char body[640];
    va_list a;
    va_start(a, fmt);
    vsnprintf(body, sizeof body, fmt, a);
    va_end(a);
    const unsigned gen = PortApi::LoadGeneration();
    std::string set;
    {
        std::lock_guard l(mutex_);
        if (gen_ == gen)
            for (const Var& v : vars_) {
                const float x = *v.slot;
                if (x == 0.f) continue;
                char b[96];
                sprintf_s(b, " %ls=%g", v.name.c_str(), (double)x);
                set += b;
            }
    }
    logger::report("TEXTRACE f=%u t=%lu %s | set:%s", g_frame_id, GetCurrentThreadId(), body,
        set.empty() ? " none" : set.c_str());
}

std::wstring TexTrace::ModText(ID3D12Resource* mod_res)
{
    if (!mod_res) return L"no texture";
    const int entry = g_mod_textures.IndexOf(mod_res);
    const D3D12_RESOURCE_DESC d = mod_res->GetDesc();
    wchar_t b[120];
    swprintf_s(b, L" (entry %d, %llux%u, format %d, %u mip(s))", entry, (unsigned long long)d.Width, d.Height,
        (int)d.Format, (unsigned)d.MipLevels);
    return (entry >= 0 ? g_mod_textures.NameOf(entry) : std::wstring(L"a texture not in the mod table")) + b;
}

void TexTrace::FrameEnd()
{
    const uint32_t left = left_.load(std::memory_order_acquire);
    if (!left) return;
    left_.store(left - 1, std::memory_order_release);
    if (left == 1) logger::report("TEXTRACE done: texture %08x", hash_.load(std::memory_order_relaxed));
}
