#include "rfx_patch.h"
#include "rfx_patched_ps.h"
#include "../core/log.h"
#include "../core/fnv.h"
#include "../addon/addon.h"
#include "../hooks/pipeline_registry.h" // PipelineRegistry::SetDecensorVariant
#include "../migoto/dx12_bridge.h" // MigotoPort::DumpDir -- where the cutout's shader corpus goes
#include "../mods/wwmi_runtime.h" // WwmiReadFile
#include "../build_stamp.h" // WWMI_RFX_SRC_HASH: a cached patch is reused only by the patcher that wrote it
#include <cstring>
#include <string>

using namespace reshade::api;

RfxPatchWorker g_rfx_patch;

struct RfxPatchedPs {
    std::vector<uint8_t> code;
    unsigned max_reg = 0;
    bool has_glow = false;
    bool cached = false;
}; // empty code = a patch that failed: said once, never retried

namespace {

struct CacheHdr {
    uint32_t magic;
    uint32_t version;
    uint64_t src_hash;
    uint32_t src_size;
    uint32_t code_size;
    uint32_t max_reg;
    uint32_t has_glow;
};
constexpr uint32_t kCacheMagic = 0x43584652; // "RFXC"

std::wstring CachePath(uint64_t ps_key)
{
    wchar_t leaf[40];
    swprintf_s(leaf, L"\\%016llx.rfx", (unsigned long long)ps_key);
    return g_addon.shader_cache_dir + leaf;
}

// True for a file this build wrote for this shader -- with code, or empty for a patch that did not apply.
bool CacheLoad(uint64_t ps_key, uint64_t src_hash, size_t src_size, RfxPatchedPs& out)
{
    if (g_addon.shader_cache_dir.empty()) return false;
    const std::vector<uint8_t> f = WwmiReadFile(CachePath(ps_key).c_str());
    CacheHdr h;
    if (f.size() < sizeof h) return false;
    memcpy(&h, f.data(), sizeof h);
    if (h.magic != kCacheMagic || h.version != WWMI_RFX_SRC_HASH || h.src_hash != src_hash ||
        h.src_size != (uint32_t)src_size || f.size() != sizeof h + (size_t)h.code_size)
        return false;
    out.code.assign(f.begin() + sizeof h, f.end());
    out.max_reg = h.max_reg;
    out.has_glow = h.has_glow != 0;
    return true;
}

void CacheStore(uint64_t ps_key, uint64_t src_hash, size_t src_size, const RfxPatchedPs& p)
{
    if (g_addon.shader_cache_dir.empty()) return;
    CreateDirectoryW(g_addon.shader_cache_dir.c_str(), nullptr);
    const std::wstring path = CachePath(ps_key);
    const std::wstring tmp = path + L".tmp";
    const HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    const CacheHdr h{ kCacheMagic, WWMI_RFX_SRC_HASH, src_hash, (uint32_t)src_size, (uint32_t)p.code.size(), p.max_reg,
        p.has_glow ? 1u : 0u };
    DWORD wr = 0;
    bool ok = WriteFile(f, &h, sizeof h, &wr, nullptr) && wr == sizeof h;
    if (ok && !p.code.empty())
        ok = WriteFile(f, p.code.data(), (DWORD)p.code.size(), &wr, nullptr) && wr == p.code.size();
    CloseHandle(f);
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

#ifdef WWMI_DEBUG
void PatchDump(uint64_t ps_hash, const char* suffix, const void* p, size_t n)
{
    if (!p || !n) return;
    const std::wstring dir = MigotoPort::DumpDir() + L"\\RfxDxil";
    CreateDirectoryW(MigotoPort::DumpDir().c_str(), nullptr);
    CreateDirectoryW(dir.c_str(), nullptr);
    wchar_t path[MAX_PATH];
    swprintf_s(path, L"%s\\%016llx%S", dir.c_str(), (unsigned long long)ps_hash, suffix);
    const HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    DWORD wr = 0;
    WriteFile(f, p, (DWORD)n, &wr, nullptr);
    CloseHandle(f);
}
#endif

} // namespace

void RfxPatchWorker::Build(Job& job)
{
    reshade::api::device* const dev = job.dev;
    const uint64_t src_pso0 = job.src_pso0;
    const uint64_t key = job.key;
    const bool decensor = job.decensor;
    const bool inject = job.inject;
    const bool cut = !job.cuts.empty();
    const char* const label = cut ? "UIDCUT" : (inject ? "RFXPATCH" : "RFXDECENSOR");
    unsigned max_reg = 0;
    bool glow = false;
    CodeBlob dps;
    CodeBlob pps;
    auto settle = [&](ID3D12PipelineState* v) {
        bool orphan = false;
        {
            std::lock_guard l(mutex_);
            if (cut) {
                if (!uid_pending_.erase({ src_pso0, job.target }))
                    orphan = true;
                else {
                    if (v) {
                        uid_variants_.emplace(src_pso0, v);
                        ++variants_made_;
                    }
                    g_pipelines.SetUidVariant(src_pso0, job.target, reinterpret_cast<uint64_t>(v));
                }
            } else if (!pending_.erase(key))
                orphan = true;
            else {
                variants_[key] = Made{ v, max_reg, v && glow };
                if (v) ++variants_made_;
                if (!inject) g_pipelines.SetDecensorVariant(src_pso0, reinterpret_cast<uint64_t>(v));
                if (!inject && v && dps) g_patched_ps.SetDecensored(src_pso0, dps);
                if (inject && v && pps) g_patched_ps.SetRfx(key, pps);
            }
        }
        if (orphan && v) {
            logger::write(
                "%s variant src=%llx decensor=%d: the source PSO was destroyed while its patch was in flight -- variant released",
                label, src_pso0, (int)decensor);
            dev->destroy_pipeline(reshade::api::pipeline{ reinterpret_cast<uint64_t>(v) });
        }
    };
    std::vector<pipeline_subobject>& subs = job.clone.subs;
    // A GBuffer pass only: the DX11 regex matches the component's two 7-RT passes and neither depth pass.
    if (inject)
        for (const auto& so : subs)
            if (so.type == pipeline_subobject_type::render_target_formats) {
                const auto* fm = static_cast<const format*>(so.data);
                if (so.count < 4 || fm[3] == format::unknown) return settle(nullptr);
                break;
            }
    pipeline_subobject* ps_so = nullptr;
    for (auto& so : subs)
        if (so.type == pipeline_subobject_type::pixel_shader && so.count >= 1 && so.data) {
            ps_so = &so;
            break;
        }
    if (!ps_so) return settle(nullptr);
    const shader_desc src_ps = *static_cast<const shader_desc*>(ps_so->data);
    if (!src_ps.code || !src_ps.code_size) return settle(nullptr);
    const uint64_t ps_hash = Fnv1a64(src_ps.code, src_ps.code_size);
    const uint64_t ps_key = ps_hash ^ (decensor ? 0x9E3779B97F4A7C15ull : 0) ^ (inject ? 0 : 0xC2B2AE3D27D4EB4Full) ^
        (cut ? Fnv1a64(job.cuts.data(), job.cuts.size() * sizeof(RfxDxilCut))
             : 0); // the store holds every patch of a PS

    std::shared_ptr<RfxPatchedPs> patched;
    if (auto it = ps_.find(ps_key); it != ps_.end())
        patched = it->second;
    else {
        patched = std::make_shared<RfxPatchedPs>();
        patched->cached = CacheLoad(ps_key, ps_hash, src_ps.code_size, *patched);
        bool ok = patched->cached && !patched->code.empty();
        if (patched->cached)
            logger::trace("RFXCACHE ps=%016llx decensor=%d inject=%d: %zu bytes from the disk cache",
                (unsigned long long)ps_hash, (int)decensor, (int)inject, patched->code.size());
        if (!patched->cached) {
            std::string why;
            const ULONGLONG t0 = GetTickCount64();
            RfxDxilResult r;
            const bool init_ok = RfxDxilInit((g_addon.cso_dir + L"\\dxc").c_str(), why);
            RfxDxilSpec spec{ kRfxGlowReg, kRfxCutoutReg, kRfxParamsReg, decensor, inject };
            spec.cuts = job.cuts;
            ok = init_ok && RfxDxilPatch(src_ps.code, src_ps.code_size, spec, r);
            if (ok) {
                patched->code = std::move(r.code);
                patched->max_reg = r.srv_max_reg;
                patched->has_glow = r.has_glow;
            } else if (!r.why.empty())
                why = r.why;
            if (cut)
                logger::write("UIDCUT ps=%016llx (%zu bytes) for %ux%u: %s in %llu ms -- %s",
                    (unsigned long long)ps_hash, (size_t)src_ps.code_size, job.target >> 16, job.target & 0xFFFFu,
                    ok ? "PATCHED" : "NOT patched", (unsigned long long)(GetTickCount64() - t0),
                    ok ? r.why.c_str() : why.c_str());
            else if (!inject) {
                if (ok)
                    logger::write("RFXDECENSOR ps=%016llx (%zu bytes): PATCHED in %llu ms -- %s",
                        (unsigned long long)ps_hash, (size_t)src_ps.code_size,
                        (unsigned long long)(GetTickCount64() - t0), r.why.c_str());
                else
                    logger::trace(
                        "RFXDECENSOR ps=%016llx: NOT patched -- %s", (unsigned long long)ps_hash, why.c_str());
            } else {
                logger::write("RFXPATCH ps=%016llx (%zu bytes): %s in %llu ms -- %s", (unsigned long long)ps_hash,
                    (size_t)src_ps.code_size, ok ? "PATCHED" : "NOT patched",
                    (unsigned long long)(GetTickCount64() - t0), ok ? r.why.c_str() : why.c_str());
#ifdef WWMI_DEBUG
                if (logger::unlimited()) {
                    PatchDump(ps_hash, ".ps.cso", src_ps.code, src_ps.code_size);
                    PatchDump(ps_hash, ".before.ll", r.ll_before.data(), r.ll_before.size());
                    PatchDump(ps_hash, ".after.ll", r.ll_after.data(), r.ll_after.size());
                    PatchDump(ps_hash, ".patched.cso", patched->code.data(), patched->code.size());
                }
#endif
            }
            if (r.settled) CacheStore(ps_key, ps_hash, src_ps.code_size, *patched);
        }
        ps_.emplace(ps_key, patched);
        std::lock_guard l(mutex_);
        if (ok)
            ++ps_patched_;
        else
            ++ps_failed_;
    }
    if (patched->code.empty()) return settle(nullptr);
    max_reg = patched->max_reg;
    glow = patched->has_glow;
    if (!cut) {
        if (!inject)
            dps = g_shader_code.Pooled(patched->code.data(), patched->code.size());
        else
            pps = g_shader_code.Pooled(patched->code.data(), patched->code.size());
    }

    shader_desc ps = src_ps;
    ps.code = patched->code.data();
    ps.code_size = patched->code.size();
    ps_so->data = &ps;
    ps_so->count = 1;
    reshade::api::pipeline out = { 0 };
    const bool ok = dev->create_pipeline(job.clone.layout, (uint32_t)subs.size(), subs.data(), &out);
    ID3D12PipelineState* variant = ok ? reinterpret_cast<ID3D12PipelineState*>(out.handle) : nullptr;
    logger::write("%s variant src=%llx ps=%016llx decensor=%d -> %s", label, src_pso0, (unsigned long long)ps_hash,
        (int)decensor, variant ? "OK" : "FAILED(create)");
    if (!variant && patched->cached) {
        DeleteFileW(CachePath(ps_key).c_str());
        logger::write(
            "RFXCACHE ps=%016llx: its cached patch built no pipeline -- the cache file is removed, patched afresh next launch",
            (unsigned long long)ps_hash);
    }
    return settle(variant);
}

void RfxPatchWorker::Run()
{
    for (;;) {
        Job job;
        {
            std::unique_lock l(mutex_);
            cv_.wait(l, [this] { return stop_.load(std::memory_order_acquire) || !queue_.empty(); });
            if (stop_.load(std::memory_order_acquire)) return;
            job = std::move(queue_.front());
            queue_.pop_front();
        }
        Build(job);
    }
}

void RfxPatchWorker::Stop()
{
    {
        std::lock_guard l(mutex_);
        stop_.store(true, std::memory_order_release);
        queue_.clear();
        pending_.clear();
    } // an in-flight build then settles as an orphan
    cv_.notify_all();
    std::lock_guard t(thread_mutex_);
    if (worker_.t.joinable()) {
        worker_.t.join();
        logger::write("RFXPATCH worker stopped (device destroyed / addon unloading)");
    }
}

void RfxPatchWorker::Wake()
{
    {
        std::lock_guard t(thread_mutex_);
        if (!worker_.t.joinable() && !stop_.load(std::memory_order_acquire)) {
            worker_.t = std::thread([this] { Run(); });
            logger::write("RFXPATCH worker started");
        }
    }
    cv_.notify_one();
}

ID3D12PipelineState* RfxPatchWorker::Variant(
    reshade::api::device* dev, uint64_t src_pso0, unsigned* ps_max_srv_reg, bool* has_glow, bool decensor)
{
    if (ps_max_srv_reg) *ps_max_srv_reg = 0;
    if (has_glow) *has_glow = false;
    if (!dev || !src_pso0) return nullptr;
    const uint64_t src_pso = src_pso0 | ((uint64_t)decensor << 60);
    {
        std::lock_guard l(mutex_);
        auto it = variants_.find(src_pso);
        if (it != variants_.end()) {
            if (ps_max_srv_reg) *ps_max_srv_reg = it->second.max_reg;
            if (has_glow) *has_glow = it->second.has_glow;
            return it->second.pso;
        }
        if (pending_.count(src_pso)) return nullptr;
    } // in flight: this draw goes unpatched

    Job job{ dev, src_pso0, src_pso, decensor, {} };
    if (!g_pso_clones.DeepCopy(src_pso0, job.clone)) {
        // As CUSTOMBLEND-MISS: said once, not cached, retried per draw.
        std::lock_guard lv(mutex_);
        if (miss_logged_.insert(src_pso0).second)
            logger::write(
                "RFXPATCH variant src=%llx -> none: no clone of this PSO (created before the addon)", src_pso0);
        return nullptr;
    }
    {
        std::lock_guard l(mutex_);
        if (stop_.load(std::memory_order_acquire)) return nullptr;
        if (!pending_.insert(src_pso).second) return nullptr; // another draw thread queued it first
        queue_.push_back(std::move(job));
        logger::trace("RFXPATCH queued src=%llx decensor=%d (drawn unpatched until built; %zu queued)", src_pso0,
            (int)decensor, queue_.size());
    }
    Wake();
    return nullptr;
}

void RfxPatchWorker::QueueDecensor(reshade::api::device* dev, uint64_t src_pso0)
{
    if (!dev || !src_pso0) return;
    const uint64_t key = src_pso0 | (2ull << 60);
    Job job{ dev, src_pso0, key, true, {}, false };
    if (!g_pso_clones.DeepCopy(src_pso0, job.clone)) {
        logger::trace("RFXDECENSOR src=%llx -> none: no clone of this PSO", src_pso0);
        return;
    }
    {
        std::lock_guard l(mutex_);
        if (stop_.load(std::memory_order_acquire)) return;
        if (!pending_.insert(key).second) return;
        queue_.push_back(std::move(job));
    }
    Wake();
}

void RfxPatchWorker::QueueUid(
    reshade::api::device* dev, uint64_t src_pso0, uint32_t target, std::vector<RfxDxilCut> cuts)
{
    if (!dev || !src_pso0 || cuts.empty()) return;
    Job job{ dev, src_pso0, src_pso0 | (3ull << 60), false, {}, false, std::move(cuts), target };
    if (!g_pso_clones.DeepCopy(src_pso0, job.clone)) {
        logger::trace("UIDCUT src=%llx -> none: no clone of this PSO", src_pso0);
        return;
    }
    {
        std::lock_guard l(mutex_);
        if (stop_.load(std::memory_order_acquire)) return;
        if (!uid_pending_.insert({ src_pso0, target }).second) return;
        queue_.push_back(std::move(job));
    }
    Wake();
}

RfxPatchStats RfxPatchWorker::Stats()
{
    std::lock_guard l(mutex_);
    return RfxPatchStats{ ps_patched_, ps_failed_, variants_made_ };
}

void RfxPatchWorker::ForgetSource(uint64_t src_pso, std::vector<ID3D12PipelineState*>& dead)
{
    std::lock_guard l(mutex_);
    miss_logged_.erase(src_pso);
    g_patched_ps.Forget(src_pso);
    constexpr uint64_t kSrcMask = ~(7ull << 60);
    for (unsigned d = 0; d < 8; ++d) {
        const uint64_t key = src_pso | ((uint64_t)d << 60);
        if (auto it = variants_.find(key); it != variants_.end()) {
            if (it->second.pso) dead.push_back(it->second.pso);
            variants_.erase(it);
        }
        pending_.erase(key);
    }
    for (auto it = queue_.begin(); it != queue_.end();)
        if ((it->key & kSrcMask) == src_pso)
            it = queue_.erase(it);
        else
            ++it;
    for (auto it = uid_pending_.lower_bound({ src_pso, 0 }); it != uid_pending_.end() && it->first == src_pso;)
        it = uid_pending_.erase(it);
    const auto made = uid_variants_.equal_range(src_pso);
    for (auto it = made.first; it != made.second; ++it) dead.push_back(it->second);
    uid_variants_.erase(made.first, made.second);
}
