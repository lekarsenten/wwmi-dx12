#include "tex_defer.h"
#include "../instrumentation/diag_gate.h"
#include "../addon/settings.h"
#include "../mods/character_recognition.h"
#include "../core/log.h"
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
        if (!self->thread_.joinable()) self->thread_ = std::thread(&TexDefer::Run, self);
    }
    self->cv_.notify_one();
    return true;
}

void TexDefer::Run()
{
    for (;;) {
        std::deque<Job> batch;
        {
            std::unique_lock l(mtx_);
            cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            batch.swap(queue_);
        }
        LARGE_INTEGER f;
        LARGE_INTEGER t0;
        LARGE_INTEGER t1;
        QueryPerformanceFrequency(&f);
        QueryPerformanceCounter(&t0);
        unsigned made = 0;
        unsigned stale = 0;
        unsigned smaller = 0;
        LONGLONG locked = 0;
        LONGLONG longest = 0;
        for (const Job& job : batch) {
            {
                std::lock_guard l(mtx_);
                if (stop_) return;
            }
            if (PortApi::LoadGeneration() != job.gen) {
                ++stale;
            } else {
                LARGE_INTEGER a;
                LARGE_INTEGER b;
                LARGE_INTEGER c;
                QueryPerformanceCounter(&a);
                ID3D11Resource* res = PortApi::LoadDeferredTexture(job.tex);
                QueryPerformanceCounter(&b);
                if (res) {
                    if (PortApi::PublishDeferredTexture(job.tex.fq_name.c_str(), res)) {
                        ++made;
                        if (job.tex.maxsize) ++smaller;
                    } else {
                        res->Release();
                    }
                }
                QueryPerformanceCounter(&c);
                locked += c.QuadPart - b.QuadPart;
                if (b.QuadPart - a.QuadPart > longest) longest = b.QuadPart - a.QuadPart;
            }
            std::lock_guard l(mtx_);
            if (job.mod >= 0 && job.gen == gen_.load(std::memory_order_relaxed))
                Slot(job.mod).fetch_sub(1, std::memory_order_acq_rel);
            in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        }
        loaded_.fetch_add(made, std::memory_order_acq_rel);
        QueryPerformanceCounter(&t1);
        std::vector<std::wstring> namespaces;
        for (const Job& job : batch) {
            std::wstring ns = PortApi::NamespaceOfResource(job.tex.fq_name);
            if (std::find(namespaces.begin(), namespaces.end(), ns) == namespaces.end())
                namespaces.push_back(std::move(ns));
        }
        const double ms = 1000.0 / (double)f.QuadPart;
        logger::write(
            "TEXDEFER '%ls'%s: loaded %u of %zu texture(s), %u of them asked for at the size they were let go at, "
            "in %.0f ms, %.0f ms of it under the engine lock (longest texture %.0f ms)%s",
            namespaces.front().c_str(), namespaces.size() > 1 ? " and others" : "", made, batch.size(), smaller,
            (double)(t1.QuadPart - t0.QuadPart) * ms, (double)locked * ms, (double)longest * ms,
            stale ? " -- some were asked for before the inis were reloaded, not loaded" : "");
    }
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
    if (thread_.joinable()) thread_.join();
}
