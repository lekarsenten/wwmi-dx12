// The look-alike worker. See tex_look_worker.h.
#include "tex_look_worker.h"
#include "tex_identity.h"
#include "tex_lod.h"
#include "tex_crawl.h"
#include "../hooks/stamp_ledger.h"
#include "../mods/mod_set.h"
#include "../instrumentation/profiler.h"
#include <chrono>

TexLookWorker g_tex_look_worker;

void TexLookWorker::Run()
{
    for (;;) {
        Job j;
        {
            std::unique_lock l(mtx_);
            if (!cv_.wait_for(l, std::chrono::seconds(1), [this] { return stop_ || !queue_.empty(); })) {
                l.unlock();
                g_tex_lod.FlushIfQuiet(2000);
                g_tex_crawl.FlushIfQuiet(10000);
                g_tex_look.FlushIfQuiet(10000);
                continue;
            }
            if (stop_) return;
            j = std::move(queue_.front());
            queue_.pop_front();
        }
        const bool known_before = g_tex_lod.Known(j.crc) || g_tex_lod.Lookup(j.crc);
        {
            PROF(PROF_CB_TexLod);
            g_tex_look.OnCut(j.crc, j.cut, j.is_target);
        }
        if (known_before || !(g_tex_lod.Known(j.crc) || g_tex_lod.Lookup(j.crc))) continue;
        const uint64_t key = reinterpret_cast<uint64_t>(j.dest);
        if (g_tex_identity.NoteModTexIfHash(key, j.crc)) {
            ModSetRef set_ref;
            g_stamps.Restamp(key);
        }
    }
}

void TexLookWorker::Stop()
{
    {
        std::lock_guard l(mtx_);
        stop_ = true;
        queue_.clear();
    }
    cv_.notify_all();
    if (thread_.joinable()) thread_.join();
}

void TexLookWorker::Queue(ID3D12Resource* dest, uint32_t crc, uint32_t w, uint32_t h, uint32_t fmt, bool is_target,
    const std::vector<uint8_t>& bc)
{
    if (!g_tex_look.Wants(crc, w, h, fmt, is_target)) return; // decided for this content already
    {
        std::lock_guard l(mtx_);
        if (stop_) return;
    }
    TexLookCut cut = TexLookCutOf(bc.data(), bc.size(), w, h, fmt); // the patches' blocks queued, not the whole mip
    if (cut.blocks.empty()) return;
    {
        std::lock_guard l(mtx_);
        if (stop_) return;
        queue_.push_back(Job{ dest, crc, is_target, std::move(cut) });
        if (!thread_.joinable()) thread_ = std::thread(&TexLookWorker::Run, this);
    }
    cv_.notify_one();
}
