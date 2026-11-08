#include "tex_replace.h"
#include "tex_defer.h"
#include "tex_encode.h"
#include "../addon_common.h"
#include "../mods/character_recognition.h"
#include "../migoto/dx12_wrap.h"
#include "../migoto/dx12_pipeline.h"
#include "mod_texture_store.h"
#include "../core/gpu_progress.h"
#include "../core/dxgi_bc.h"
#include "../core/log.h"
#include <d3d11.h>
#include <algorithm>
#include "../addon/addon.h"
#include "tex_place.h"

TexReplace g_tex_replace;

namespace {
constexpr uint64_t kPendingCap = 1024ull << 20; // bytes loaded and not yet swapped in before the loader waits
constexpr uint32_t kSrvQuarantineFrames = 8;
constexpr uint32_t kRetireReportFrames = 600;
constexpr auto kStressEvery = std::chrono::seconds(2);
constexpr double kEncodeReadPerFrame = 256e6; // source texels a frame's re-encodes may read

const char* PassName(TexReplace::Pass p)
{
    switch (p) {
    case TexReplace::Pass::Same: return "same size";
    case TexReplace::Pass::Half: return "half size";
    case TexReplace::Pass::Full: return "full size";
    case TexReplace::Pass::Sized: return "to the sizes asked";
    default: return "none";
    }
}

bool HalvesCleanly(const D3D12_RESOURCE_DESC& rd)
{
    if (rd.MipLevels < 2) return false;
    return !DxgiIsBc((uint32_t)rd.Format) || (((rd.Width / 2) % 4) == 0 && ((rd.Height / 2) % 4) == 0);
}

void Add(StampTally& a, const StampTally& b)
{
    a.moved_cpu_only += b.moved_cpu_only;
    a.moved_visible += b.moved_visible;
    a.restored += b.restored;
    a.nulled += b.nulled;
    a.raced += b.raced;
    a.backups += b.backups;
}

void Drop(PortApi::LoadedTexture& t)
{
    if (t.res) t.res->Release();
    t.res = nullptr;
}

PortApi::LoadedTexture Take(PortApi::LoadedTexture& t)
{
    PortApi::LoadedTexture r = std::move(t);
    t.res = nullptr;
    return r;
}
} // namespace

void TexReplace::OnRetire(ID3D12Resource* res)
{
    if (!res || res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return;
    const bool keyed = g_mod_textures.IndexOf(res) >= 0;
    std::lock_guard l(g_tex_replace.dying_mtx_);
    if (g_tex_replace.watched_.erase(res) || keyed) g_tex_replace.dying_.insert(res);
}

void TexReplace::Quarantine(D3D12_CPU_DESCRIPTOR_HANDLE srv, unsigned epoch)
{
    if (srv.ptr) quarantine_.push_back({ srv, epoch, g_frame_id });
}

bool TexReplace::Forget(ID3D12Resource* res)
{
    D3D12_CPU_DESCRIPTOR_HANDLE srv{};
    unsigned epoch = 0;
    if (!g_mod_textures.Forget(res, &srv, &epoch)) return false;
    Quarantine(srv, epoch);
    return true;
}

void TexReplace::ClearForParse()
{
    std::vector<ID3D12Resource*> held;
    for (const ModTextureStore::Resource& r : g_mod_textures.Resources())
        if (r.valid && r.res) held.push_back(r.res);
    unsigned taken_out = 0;
    for (ID3D12Resource* res : held) taken_out += Forget(res);
    g_mod_textures.ClearForParse();
    if (taken_out)
        logger::write("TEXSTORE: parse -- %u texture(s) taken out of the store, their SRV slots quarantined", taken_out);
}

void TexReplace::SweepDying()
{
    std::unordered_set<ID3D12Resource*> dying;
    {
        std::lock_guard l(dying_mtx_);
        dying.swap(dying_);
    }
    if (dying.empty()) return;
    unsigned forgotten = 0;
    for (ID3D12Resource* res : dying) {
        if (Forget(res)) ++forgotten;
        const auto it = replaced_.find(res);
        if (it == replaced_.end()) continue;
        ++watch_.retired;
        watch_.most_frames = (std::max)(watch_.most_frames, g_frame_id - it->second);
        replaced_.erase(it);
    }
    const StampTally t = g_stamps.UnstampDying(dying);
    if (watch_.replaced) Add(watch_.late, t);
    if (forgotten || t.restored || t.nulled || t.raced)
        logger::write(
            "TEXRETIRE: %zu texture(s) let go of, %u of them still in the texture store (taken out); slots still "
            "holding one: %u given the game's descriptor back, %u a null one, %u rewritten by the game meanwhile",
            dying.size(), forgotten, t.restored, t.nulled, t.raced);
}

void TexReplace::ReportRetired(bool timeout)
{
    logger::write(
        "TEXRETIRE-WATCH: %u of %u replaced texture(s) let go of, the slowest %u frames after its swap; slots "
        "still stamped with one then: %u given back, %u nulled, %u rewritten by the game meanwhile%s",
        watch_.retired, watch_.replaced, watch_.most_frames, watch_.late.restored, watch_.late.nulled,
        watch_.late.raced, timeout ? " -- the rest are still held 600 frames after the last swap" : "");
    watch_ = RetireWatch{};
    if (timeout) replaced_.clear();
}

void TexReplace::StartPass(Pass p)
{
    pass_ = PassTally{};
    pass_.pass = p;
    pass_.gen = PortApi::LoadGeneration();
    pass_.t0 = std::chrono::steady_clock::now();
    const std::vector<std::wstring> namespaces = NamespacesOnScreen();
    pass_.mods = (unsigned)namespaces.size();
    std::vector<Job> jobs;
    for (const std::wstring& ns : namespaces) {
        std::vector<PortApi::LoadedTexture> have;
        PortApi::LoadedTexturesOf(ns.c_str(), have);
        for (PortApi::LoadedTexture& t : have) {
            const D3D12_RESOURCE_DESC rd = t.dx12->GetDesc();
            const unsigned side = (unsigned)(std::max)(rd.Width, (UINT64)rd.Height);
            const bool half = halved_.count(t.load.fq_name) != 0;
            bool keep = !client_ || !client_->Owns(t.load.fq_name);
            switch (p) {
            case Pass::Same: t.load.maxsize = half ? side : 0; break;
            case Pass::Half:
                keep = keep && !half && HalvesCleanly(rd);
                t.load.maxsize = side / 2;
                break;
            case Pass::Full:
                keep = keep && half;
                t.load.maxsize = 0;
                break;
            default: keep = false;
            }
            if (!keep) {
                Drop(t);
                continue;
            }
            const D3D12_RESOURCE_ALLOCATION_INFO ai = g_addon.d3d12_device->GetResourceAllocationInfo(0, 1, &rd);
            jobs.push_back({ std::move(t), pass_.gen, ai.SizeInBytes });
        }
    }
    pass_.queued = (unsigned)jobs.size();
    logger::write("TEXREPL %s: %u texture(s) of %u mod(s) on screen to swap", PassName(p), pass_.queued, pass_.mods);
    if (jobs.empty()) {
        last_end_ = std::chrono::steady_clock::now();
        return;
    }
    busy_.store(true, std::memory_order_release);
    Queue(jobs);
}

void TexReplace::Queue(std::vector<Job>& jobs)
{
    if (jobs.empty()) return;
    {
        std::lock_guard l(mtx_);
        for (Job& j : jobs) jobs_.push_back(std::move(j));
        if (!thread_.joinable()) thread_ = std::thread(&TexReplace::Run, this);
    }
    cv_.notify_one();
}

bool TexReplace::Start(std::vector<Order>& orders)
{
    if (Busy()) return false;
    pass_ = PassTally{};
    pass_.pass = Pass::Sized;
    pass_.gen = PortApi::LoadGeneration();
    pass_.t0 = std::chrono::steady_clock::now();
    std::vector<Job> loads;
    size_t from_file = 0;
    for (Order& o : orders) {
        Job j{ Take(o.was), pass_.gen, o.bytes, o.shift, o.from_file };
        if (o.evict) {
            evicts_.push_back(std::move(j));
        } else if (j.shift && !j.from_file) {
            encodes_.push_back(std::move(j));
        } else {
            if (j.from_file) ++from_file;
            loads.push_back(std::move(j));
        }
    }
    orders.clear();
    pass_.queued = (unsigned)(loads.size() + encodes_.size() + evicts_.size());
    logger::write(
        "TEXREPL %s: %u texture(s) to swap, %zu of them re-encoded on the graphics card, %zu from their files a "
        "band at a time, %zu let go of",
        PassName(pass_.pass), pass_.queued, encodes_.size(), from_file, evicts_.size());
    if (!pass_.queued) {
        last_end_ = pass_.t0;
        return true;
    }
    busy_.store(true, std::memory_order_release);
    Queue(loads);
    return true;
}

void TexReplace::Evict()
{
    if (evicts_.empty()) return;
    std::vector<std::pair<std::wstring, ID3D11Resource*>> asked;
    asked.reserve(evicts_.size());
    for (const Job& j : evicts_) asked.push_back({ j.was.load.fq_name, j.was.res });
    const std::unordered_set<ID3D11Resource*> held = PortApi::HeldByRef(asked);
    std::unordered_map<ID3D11Resource*, ID3D11Resource*> gone;
    for (Job& j : evicts_) {
        ID3D12Resource* const res = j.was.dx12;
        if (held.count(j.was.res)) {
            logger::write(
                "TEXREPL: %ls not let go -- another [Resource] holds it by `= ref`, so it would stay in video "
                "memory; lowered only from now on",
                j.was.load.fq_name.c_str());
            if (client_) client_->Held(j.was.load.fq_name);
            Drop(j.was);
            continue;
        }
        {
            std::lock_guard l(dying_mtx_);
            watched_.insert(res);
        }
        if (!PortApi::EvictTexture(j.was.load.fq_name.c_str(), j.was.res)) {
            ++pass_.stale;
            std::lock_guard l(dying_mtx_);
            watched_.erase(res);
        } else {
            replaced_[res] = g_frame_id;
            ++watch_.replaced;
            watch_.last_swap_frame = g_frame_id;
            ++pass_.evicted;
            gone[j.was.res] = nullptr;
            if (client_) client_->Swapped(j.was.load.fq_name, nullptr);
        }
        Drop(j.was);
    }
    evicts_.clear();
    PortApi::ReleaseOldTextures(gone);
}

void TexReplace::SubmitEncodes(ID3D12CommandQueue* queue)
{
    if (encodes_.empty() || !queue || !g_tex_encoder.Ready()) return;
    std::vector<TexEncoder::Shrink> work;
    std::vector<Loaded> sent;
    double read = 0.0;
    size_t i = 0;
    for (; i < encodes_.size() && (work.empty() || read < kEncodeReadPerFrame); ++i) {
        Job& j = encodes_[i];
        const D3D12_RESOURCE_DESC rd = j.was.dx12->GetDesc();
        const MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(j.was.res);
        const UINT w = (UINT)(rd.Width >> j.shift);
        const UINT h = rd.Height >> j.shift;
        if (!wr || wr->state != D3D12_RESOURCE_STATE_COMMON) {
            ++pass_.in_use;
            if (client_) client_->Kept(j.was.load.fq_name);
            Drop(j.was);
            continue;
        }
        ID3D11Resource* fresh =
            w && h && !(w % 4) && !(h % 4) ? PortApi::CreateEmptyTexture(j.was.load, w, h, rd.Format) : nullptr;
        MigotoPort::WrapResource* nw = fresh ? MigotoPort::AsWrapResource(fresh) : nullptr;
        if (!nw || !nw->dx12) {
            ++pass_.failed;
            if (fresh) fresh->Release();
            Drop(j.was);
            continue;
        }
        work.push_back({ j.was.dx12, nw->dx12, j.shift });
        read += (double)rd.Width * rd.Height;
        sent.push_back({ std::move(j), fresh, 0 });
    }
    encodes_.erase(encodes_.begin(), encodes_.begin() + (ptrdiff_t)i);
    if (work.empty()) return;
    const uint64_t enc = g_tex_encoder.Submit(g_addon.d3d12_device, queue, work);
    for (size_t k = 0; k < sent.size(); ++k) {
        Loaded& x = sent[k];
        if (!enc || !work[k].recorded) {
            x.fresh->Release();
            x.fresh = nullptr;
        }
        x.enc = enc;
        encoding_.push_back(std::move(x));
    }
}

void TexReplace::StartStream(Loaded& x, std::vector<Loaded>& ready)
{
    const D3D12_RESOURCE_DESC rd = x.job.was.dx12->GetDesc();
    ID3D11Resource* fresh = PortApi::CreateEmptyTexture(
        x.job.was.load, x.stream->width >> x.job.shift, x.stream->height >> x.job.shift, rd.Format);
    MigotoPort::WrapResource* nw = fresh ? MigotoPort::AsWrapResource(fresh) : nullptr;
    if (!nw || !nw->dx12) {
        if (fresh) fresh->Release();
        x.stream->dropped.store(true, std::memory_order_release);
        ready.push_back(std::move(x));
        return;
    }
    x.fresh = fresh;
    x.stream->dst = nw->dx12;
    streaming_.push_back(std::move(x));
}

void TexReplace::EndStreams(unsigned gen)
{
    for (size_t i = 0; i < streaming_.size();) {
        Loaded& x = streaming_[i];
        TexEncoder::Stream& s = *x.stream;
        const bool failed = s.failed.load(std::memory_order_acquire);
        if (!s.done && !failed && x.job.gen == gen) {
            ++i;
            continue;
        }
        if (!s.done) {
            s.dropped.store(true, std::memory_order_release);
            x.why = failed ? "a band was not read or not re-encoded" : "the inis were reloaded";
            if (failed && x.job.gen == gen) {
                logger::write("TEXREPL: %ls not re-encoded from %ls, %s -- left at its size",
                    x.job.was.load.fq_name.c_str(), x.job.was.load.filename.c_str(), x.why);
                if (client_ && Busy()) client_->Kept(x.job.was.load.fq_name);
            }
        }
        x.enc = s.value;
        encoding_.push_back(std::move(x));
        streaming_.erase(streaming_.begin() + (ptrdiff_t)i);
    }
}

void TexReplace::Run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock l(mtx_);
            cv_.wait(l, [this] { return stop_ || (!jobs_.empty() && pending_bytes_ < kPendingCap); });
            if (stop_) return;
            job = std::move(jobs_.front());
            jobs_.pop_front();
            ++working_;
        }
        const bool current = PortApi::LoadGeneration() == job.gen;
        std::shared_ptr<TexEncoder::Stream> stream;
        const char* why = nullptr;
        ID3D11Resource* fresh = nullptr;
        if (!current) {
            why = "the inis were reloaded";
        } else if (job.from_file) {
            stream = std::make_shared<TexEncoder::Stream>();
            if (!TexEncoder::Open(job.was.load.filename, job.shift, stream.get(), &why)) stream.reset();
        } else {
            fresh = PortApi::LoadDeferredTexture(job.was.load);
        }
        {
            std::lock_guard l(mtx_);
            --working_;
            if (fresh || stream) pending_bytes_ += job.bytes;
            loaded_.push_back({ std::move(job), fresh, 0, 0, stream, why });
        }
        if (!stream) continue;
        for (UINT b = 0; b < stream->bands && g_tex_encoder.ReadBand(stream, b); ++b) {
        }
        CloseHandle(stream->file);
        stream->file = INVALID_HANDLE_VALUE;
    }
}

void TexReplace::SwapReady(std::vector<Loaded>& ready)
{
    std::unordered_map<ID3D12Resource*, StampMove> moves;
    std::unordered_map<int, int> index_to;
    std::unordered_map<ID3D11Resource*, ID3D11Resource*> old_to_new;
    for (Loaded& x : ready) {
        PortApi::LoadedTexture& was = x.job.was;
        if (!x.fresh) {
            ++pass_.failed;
            Drop(was);
            continue;
        }
        MigotoPort::WrapResource* nw = MigotoPort::AsWrapResource(x.fresh);
        ID3D12Resource* to = nw ? nw->dx12 : nullptr;
        const int to_index = to ? g_mod_textures.Describe(to, was.load.filename.c_str()) : -1;
        ModTextureStore::Entry te{};
        if (to_index < 0 || !g_mod_textures.EntryAt(to_index, &te)) {
            ++pass_.failed;
            x.fresh->Release();
            Drop(was);
            continue;
        }
        const int from_index = g_mod_textures.IndexOf(was.dx12);
        ModTextureStore::Entry fe{};
        const bool from_known = from_index >= 0 && g_mod_textures.EntryAt(from_index, &fe);
        {
            std::lock_guard l(dying_mtx_);
            watched_.insert(was.dx12);
        }
        if (!PortApi::SwapTextureResource(was.load.fq_name.c_str(), was.res, x.fresh)) {
            ++pass_.stale;
            {
                std::lock_guard l(dying_mtx_);
                watched_.erase(was.dx12);
            }
            Forget(to);
            x.fresh->Release();
            Drop(was);
            continue;
        }
        moves[was.dx12] = StampMove{ to, te.srv, from_known ? fe.srv : D3D12_CPU_DESCRIPTOR_HANDLE{ 0 } };
        if (from_known) index_to[from_index] = to_index;
        replaced_[was.dx12] = g_frame_id;
        ++watch_.replaced;
        watch_.last_swap_frame = g_frame_id;
        if (pass_.pass == Pass::Half)
            halved_.insert(was.load.fq_name);
        else if (pass_.pass == Pass::Full)
            halved_.erase(was.load.fq_name);
        if (pass_.pass == Pass::Sized && client_) client_->Swapped(was.load.fq_name, to);
        if (x.enc) ++pass_.encoded;
        if (x.job.from_file) ++pass_.banded;
        ++pass_.swapped;
        pass_.bytes += x.job.bytes;
        old_to_new[was.res] = x.fresh;
        Drop(was); // ours; the [Resource]'s own went with the swap
    }
    halved_any_.store(!halved_.empty(), std::memory_order_release);
    PortApi::ReleaseOldTextures(old_to_new);
    if (moves.empty()) return;
    g_tex_place.SnapshotsMove(index_to);
    for (const auto& [from, m] : moves) Forget(from);
    Add(pass_.slots, g_stamps.Move(moves));
    g_tex_place.RetryMissed("mod textures replaced", true);
}

void TexReplace::EndPass(const char* why)
{
    const auto now = std::chrono::steady_clock::now();
    const double ms = std::chrono::duration<double, std::milli>(now - pass_.t0).count();
    logger::write(
        "TEXREPL %s: %u of %u texture(s) swapped (%.0f MB) in %.0f ms%s%s; %u did not load, %u no longer held "
        "what was loaded; slots moved: %u CPU-only, %u shader-visible, %u rewritten by the game meanwhile; "
        "%u unstamp backup(s) moved",
        PassName(pass_.pass), pass_.swapped, pass_.queued, (double)pass_.bytes / 1048576.0, ms, why ? " -- " : "",
        why ? why : "", pass_.failed, pass_.stale, pass_.slots.moved_cpu_only, pass_.slots.moved_visible,
        pass_.slots.raced, pass_.slots.backups);
    if (pass_.pass == Pass::Sized)
        logger::write(
            "TEXREPL %s: %u of them re-encoded on the graphics card, %u of those from their files a band at a "
            "time; %u not re-encoded, the game had them in use; %u let go of",
            PassName(pass_.pass), pass_.encoded, pass_.banded, pass_.in_use, pass_.evicted);
    last_end_ = now;
    busy_.store(false, std::memory_order_release);
    if (pass_.pass == Pass::Sized && client_) client_->Ended();
}

void TexReplace::Tick(ID3D12CommandQueue* queue)
{
    if (!observing_) {
        MigotoPort::SetRetireObserver(&TexReplace::OnRetire);
        observing_ = true;
    }
    SweepDying();
    const unsigned gen = PortApi::LoadGeneration();
    if (gen != seen_gen_) { // a reload loads every texture at its own size
        seen_gen_ = gen;
        halved_.clear();
        halved_any_.store(false, std::memory_order_release);
    }
    for (size_t i = 0; i < quarantine_.size();) {
        if (g_frame_id - quarantine_[i].frame < kSrvQuarantineFrames) {
            ++i;
            continue;
        }
        g_mod_textures.SrvFree(quarantine_[i].srv, quarantine_[i].epoch);
        quarantine_[i] = quarantine_.back();
        quarantine_.pop_back();
    }
    if (watch_.replaced && !Busy()) {
        if (replaced_.empty())
            ReportRetired(false);
        else if (g_frame_id - watch_.last_swap_frame > kRetireReportFrames)
            ReportRetired(true);
    }

    std::vector<Loaded> got;
    bool idle = false;
    {
        std::lock_guard l(mtx_);
        got.swap(loaded_);
        if (Busy() && gen != pass_.gen) {
            for (Job& j : jobs_) Drop(j.was);
            jobs_.clear();
        }
    }
    if (gen != pass_.gen) {
        for (Job& j : encodes_) Drop(j.was);
        encodes_.clear();
        for (Job& j : evicts_) Drop(j.was);
        evicts_.clear();
    }
    std::vector<Loaded> ready;
    std::vector<Loaded> wait;
    uint64_t freed = 0;
    for (size_t i = 0; i < encoding_.size();) {
        Loaded& x = encoding_[i];
        if (x.fresh && !g_tex_encoder.Passed(x.enc)) {
            ++i;
            continue;
        }
        if (x.why && x.fresh) {
            x.fresh->Release();
            x.fresh = nullptr;
        }
        if (x.job.gen == gen) {
            ready.push_back(std::move(x));
        } else {
            if (x.fresh) x.fresh->Release();
            Drop(x.job.was);
        }
        encoding_.erase(encoding_.begin() + (ptrdiff_t)i);
    }
    for (Loaded& x : got) {
        if (x.job.from_file) {
            if (x.stream) freed += x.job.bytes;
            if (x.job.gen != gen) {
                if (x.stream) x.stream->dropped.store(true, std::memory_order_release);
                Drop(x.job.was);
            } else if (x.stream) {
                StartStream(x, ready);
            } else {
                logger::write("TEXREPL: %ls not read from %ls, %s -- left at its size", x.job.was.load.fq_name.c_str(),
                    x.job.was.load.filename.c_str(), x.why ? x.why : "?");
                if (client_ && Busy()) client_->Kept(x.job.was.load.fq_name);
                ready.push_back(std::move(x));
            }
            continue;
        }
        bool take = !x.fresh || x.job.gen != gen;
        if (x.fresh && !take) {
            if (!x.stamp) {
                if (!MigotoPort::UploadPendingFor(x.fresh)) x.stamp = GpuStamp();
            } else {
                take = GpuPassed(x.stamp);
            }
        }
        if (!take) {
            wait.push_back(std::move(x));
            continue;
        }
        if (x.fresh) freed += x.job.bytes;
        if (x.job.gen == gen) {
            ready.push_back(std::move(x));
            continue;
        }
        if (x.fresh) x.fresh->Release();
        Drop(x.job.was);
    }
    g_tex_encoder.SubmitBands(g_addon.d3d12_device, queue);
    EndStreams(gen);
    {
        std::lock_guard l(mtx_);
        for (Loaded& x : wait) loaded_.push_back(std::move(x));
        pending_bytes_ -= (std::min)(pending_bytes_, freed);
        idle = jobs_.empty() && loaded_.empty() && !working_;
    }
    cv_.notify_one();
    if (!Busy()) {
        for (Loaded& x : ready) {
            if (x.fresh) x.fresh->Release();
            Drop(x.job.was);
        }
    } else if (gen != pass_.gen) {
        EndPass("the inis were reloaded meanwhile");
        return;
    } else {
        if (!ready.empty()) SwapReady(ready);
        Evict();
        SubmitEncodes(queue);
        if (idle && encodes_.empty() && streaming_.empty() && encoding_.empty()) EndPass(nullptr);
        return;
    }
    Pass p = request_.exchange(Pass::None, std::memory_order_acq_rel);
    if (p == Pass::None && Stress() && std::chrono::steady_clock::now() - last_end_ >= kStressEvery) p = Pass::Same;
    if (p != Pass::None) StartPass(p);
}

void TexReplace::Stop()
{
    MigotoPort::SetRetireObserver(nullptr);
    {
        std::lock_guard l(mtx_);
        stop_ = true;
        jobs_.clear();
    }
    cv_.notify_all();
    g_tex_encoder.StopReading();
    if (thread_.joinable()) thread_.join();
}
