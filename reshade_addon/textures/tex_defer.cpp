#include "tex_defer.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include "../mods/character_recognition.h"
#include "../core/log.h"
#include "../core/frame_clock.h"
#include <windows.h>
#include <d3d11.h>
#include <algorithm>
#include <iterator>
#include <string>
#include "../mods/mod_set.h"

TexDefer g_tex_defer;

namespace {
int ModOfNamespace(const std::wstring& ns)
{
    ModSetRef set_ref;
    for (int md = 0; md < (int)Mods().Count(); ++md)
        if (_wcsicmp(Mods().At(md).wcl_ns.c_str(), ns.c_str()) == 0) return md;
    return -1;
}
} // namespace

std::atomic<uint32_t>& TexDefer::Slot(int mod)
{
    Table* t = table_.load(std::memory_order_relaxed);
    if (!t || mod >= t->n) {
        int n = t ? t->n : 64;
        while (n <= mod) n *= 2;
        auto grown = std::make_unique<Table>();
        grown->v = std::make_unique<std::atomic<uint32_t>[]>(n);
        grown->n = n;
        for (int i = 0; i < n; ++i)
            grown->v[i].store(t && i < t->n ? t->v[i].load(std::memory_order_relaxed) : 0, std::memory_order_relaxed);
        t = grown.get();
        tables_.push_back(std::move(grown));
        table_.store(t, std::memory_order_release);
    }
    return t->v[mod];
}

std::vector<std::wstring> NamespacesOnScreen()
{
    std::vector<std::wstring> namespaces;
    ModSetRef set_ref;
    for (int md = 0; md < (int)Mods().Count(); ++md) {
        if (!Mods().OnScreen(md) || g_tex_defer.Pending(md)) continue;
        const std::wstring& ns = Mods().At(md).wcl_ns;
        if (!ns.empty() && std::find(namespaces.begin(), namespaces.end(), ns) == namespaces.end())
            namespaces.push_back(ns);
    }
    return namespaces;
}

void TexDefer::Start()
{
    PortApi::SetTextureDeferral(&TexDefer::Offer, this);
}

bool TexDefer::Offer(const PortApi::DeferredTexture& t, void* user)
{
    if (!g_settings.tex_deferred) return false;
    TexDefer* self = static_cast<TexDefer*>(user);
    const unsigned gen = PortApi::LoadGeneration();
    const int mod = ModOfNamespace(PortApi::NamespaceOfResource(t.fq_name));
    {
        std::lock_guard l(self->mtx_);
        if (self->stop_) return false;
        if (gen != self->gen_.load(std::memory_order_relaxed)) {
            self->gen_.store(gen, std::memory_order_relaxed);
            if (Table* tb = self->table_.load(std::memory_order_relaxed))
                for (int i = 0; i < tb->n; ++i) tb->v[i].store(0, std::memory_order_relaxed);
            for (auto r = self->returns_.begin(); r != self->returns_.end();)
                r = r->second.gen != gen ? self->returns_.erase(r) : std::next(r);
        }
        // Counted here, on the drawing thread, so the draw that asked already sees its mod as loading.
        if (mod >= 0) self->Slot(mod).fetch_add(1, std::memory_order_acq_rel);
        self->in_flight_.fetch_add(1, std::memory_order_acq_rel);
        Job job{ t, mod, gen };
        const auto back = self->returns_.find(t.fq_name);
        if (back != self->returns_.end()) {
            if (back->second.gen == gen) job.tex.maxsize = back->second.maxsize;
            self->returns_.erase(back);
        }
        self->queue_.push_back(std::move(job));
        if (self->workers_.empty())
            for (unsigned i = 0; i < kWorkers; ++i) self->workers_.emplace_back(&TexDefer::Run, self);
    }
    self->cv_.notify_one();
    return true;
}

void TexDefer::Run()
{
    LARGE_INTEGER frequency;
    QueryPerformanceFrequency(&frequency);
    for (;;) {
        Job job;
        {
            std::unique_lock l(mtx_);
            cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            if (!burst_.working && !burst_.jobs) {
                LARGE_INTEGER now;
                QueryPerformanceCounter(&now);
                burst_.start = now.QuadPart;
            }
            ++burst_.working;
        }
        bool made = false;
        bool stale = false;
        LARGE_INTEGER a{};
        LARGE_INTEGER b{};
        LARGE_INTEGER c{};
        if (PortApi::LoadGeneration() != job.gen) {
            stale = true;
        } else {
            QueryPerformanceCounter(&a);
            ID3D11Resource* res = PortApi::LoadDeferredTexture(job.tex);
            QueryPerformanceCounter(&b);
            if (res) {
                if (PortApi::PublishDeferredTexture(job.tex.fq_name.c_str(), res))
                    made = true;
                else
                    res->Release();
            }
            QueryPerformanceCounter(&c);
        }
        if (made) loaded_.fetch_add(1, std::memory_order_acq_rel);

        Burst done;
        bool burst_over = false;
        {
            std::lock_guard l(mtx_);
            if (job.mod >= 0 && job.gen == gen_.load(std::memory_order_relaxed))
                Slot(job.mod).fetch_sub(1, std::memory_order_acq_rel);
            in_flight_.fetch_sub(1, std::memory_order_acq_rel);
            ++burst_.jobs;
            if (made) ++burst_.made;
            if (made && job.tex.maxsize) ++burst_.smaller;
            if (stale) ++burst_.stale;
            burst_.locked += c.QuadPart - b.QuadPart;
            burst_.load_sum += b.QuadPart - a.QuadPart;
            if (b.QuadPart - a.QuadPart > burst_.longest) burst_.longest = b.QuadPart - a.QuadPart;
            std::wstring ns = PortApi::NamespaceOfResource(job.tex.fq_name);
            if (burst_.first_namespace.empty())
                burst_.first_namespace = std::move(ns);
            else if (ns != burst_.first_namespace)
                burst_.several_namespaces = true;
            --burst_.working;
            if (queue_.empty() && !burst_.working) {
                done = std::move(burst_);
                burst_ = Burst{};
                burst_over = true;
            }
        }
        if (!burst_over) continue;
        LARGE_INTEGER end;
        QueryPerformanceCounter(&end);
        const double ms = 1000.0 / (double)frequency.QuadPart;
        logger::write(
            "TEXDEFER '%ls'%s: loaded %u of %u texture(s) on %u threads, %u of them asked for at the size they were "
            "let go at, in %.0f ms (loads added up %.0f ms), %.0f ms of it under the engine lock (longest texture "
            "%.0f ms)%s",
            done.first_namespace.c_str(), done.several_namespaces ? " and others" : "", done.made, done.jobs, kWorkers,
            done.smaller, (double)(end.QuadPart - done.start) * ms, (double)done.load_sum * ms,
            (double)done.locked * ms, (double)done.longest * ms,
            done.stale ? " -- some were asked for before the inis were reloaded, not loaded" : "");
    }
}

/**
 * Notes that a mod is being drawn this frame and starts a new appearance when it had left the screen.
 *
 * @param mod The mod's registry id.
 * @return The mod's appearance, or null when hiding is off or the mod has none.
 */
TexDefer::Appearance* TexDefer::TouchAppearance(int mod)
{
    if (!g_settings.tex_deferred || !g_settings.tex_hide_loading) return nullptr;
    if (mod < 0 || mod >= kAppearances) return nullptr;
    Appearance& appearance = appearances_[mod];
    const uint32_t now = g_frame_id;
    const uint32_t last_draw = appearance.last_draw.exchange(now, std::memory_order_acq_rel);
    if (now - last_draw > kGoneFrames) {
        appearance.first_draw.store(now, std::memory_order_release);
        appearance.last_pending.store(0, std::memory_order_release);
        appearance.hidden_since.store(0, std::memory_order_release);
        appearance.shown.store(false, std::memory_order_release);
    }
    return &appearance;
}

bool TexDefer::HidesGameDraw(int mod)
{
    Appearance* const found = TouchAppearance(mod);
    if (!found || found->shown.load(std::memory_order_acquire)) return false;
    const uint32_t now = g_frame_id;
    if (now - found->first_draw.load(std::memory_order_acquire) < kSettleFrames) return true;
    if (Pending(mod)) found->last_pending.store(now, std::memory_order_release);
    const uint32_t last_pending = found->last_pending.load(std::memory_order_acquire);
    return last_pending != 0 && now - last_pending <= kSettleFrames;
}

bool TexDefer::HidesDraw(int mod)
{
    Appearance* const found = TouchAppearance(mod);
    if (!found) return false;
    Appearance& appearance = *found;
    const uint32_t now = g_frame_id;
    if (appearance.shown.load(std::memory_order_acquire)) return false;

    if (Pending(mod)) appearance.last_pending.store(now, std::memory_order_release);
    const uint32_t last_pending = appearance.last_pending.load(std::memory_order_acquire);
    const bool loading = last_pending != 0 && now - last_pending <= kSettleFrames;
    if (loading) {
        uint32_t not_hidden = 0;
        if (appearance.hidden_since.compare_exchange_strong(not_hidden, now) && g_debug)
            logger::write("TEXHIDE mod=%d: not drawn while its textures load (frame %u)", mod, now);
        return true;
    }
    if (now - appearance.first_draw.load(std::memory_order_acquire) < kSettleFrames) return false;
    if (!appearance.shown.exchange(true, std::memory_order_acq_rel)) {
        const uint32_t hidden_since = appearance.hidden_since.load(std::memory_order_acquire);
        if (hidden_since && g_debug)
            logger::write("TEXHIDE mod=%d: its textures are loaded, drawn again after %u frame(s) (frame %u)", mod,
                now - hidden_since, now);
    }
    return false;
}

void TexDefer::ReturnAt(const std::wstring& fq_name, unsigned maxsize)
{
    std::lock_guard l(mtx_);
    returns_[fq_name] = { maxsize, PortApi::LoadGeneration() };
}

void TexDefer::Stop()
{
    PortApi::SetTextureDeferral(nullptr, nullptr);
    {
        std::lock_guard l(mtx_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    for (std::thread& worker : workers_)
        if (worker.joinable()) worker.join();
}
