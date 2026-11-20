// outline_hide.cpp -- OURS. See outline_hide.h.
#include "outline_hide.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <set>
#include <utility>
#include "../addon/addon.h"
#include "../addon/settings.h"
#include "../core/fnv.h"
#include "../core/log.h"
#include "../hooks/dxil_psv.h"
#include "../hooks/pipeline_registry.h"
#include "../instrumentation/diag_gate.h"
#include "../rabbitfx/pso_clone.h"
#include "../rabbitfx/rfx_dxil.h"

namespace {
const char* const kGbufferTargets[] = { "SV_Target0", "SV_Target1", "SV_Target2", "SV_Target3", "SV_Target4",
    "SV_Target5", "SV_Target6" };

const std::set<std::pair<uint32_t, std::string>> kOutlineStores = {
    { 0u << 2 | 3, "0.000000e+00" },
    { 1u << 2 | 2, "1.000000e+00" },
    { 3u << 2 | 3, "0.000000e+00" },
    { 5u << 2 | 0, "0.000000e+00" },
    { 5u << 2 | 1, "0.000000e+00" },
    { 5u << 2 | 2, "0.000000e+00" },
    { 5u << 2 | 3, "0.000000e+00" },
    { 6u << 2 | 0, "0.000000e+00" },
    { 6u << 2 | 1, "0.000000e+00" },
    { 6u << 2 | 2, "0.000000e+00" },
    { 6u << 2 | 3, "0.000000e+00" },
};

bool Attribute(const reshade::api::input_element& e, uint32_t index, reshade::api::format format)
{
    return e.semantic && std::strcmp(e.semantic, "ATTRIBUTE") == 0 && e.semantic_index == index && e.format == format;
}

bool HasInput(const DxilShape& s, const char* name)
{
    return std::find(s.inputs.begin(), s.inputs.end(), name) != s.inputs.end();
}

// The shell's depth bias: enough to lose the ties where its back faces meet the front faces at a silhouette, far less
// than a centimetre of cloth, so a back face nothing covers still draws.
constexpr float kShellDepthBias = 16.f;
constexpr float kShellSlopeBias = 2.f;

// The sign of a depth bias that moves a surface away from the camera under this test; 0 when the test has no
// farther side (off, equal, always).
float AwayFromCamera(const reshade::api::depth_stencil_desc& ds)
{
    using reshade::api::compare_op;
    if (!ds.depth_enable) return 0.f;
    if (ds.depth_func == compare_op::greater || ds.depth_func == compare_op::greater_equal) return -1.f;
    if (ds.depth_func == compare_op::less || ds.depth_func == compare_op::less_equal) return 1.f;
    return 0.f;
}

const char* const kCompareNames[] = { "never", "less", "equal", "less_equal", "greater", "not_equal", "greater_equal",
    "always" };

std::vector<uint8_t> ClonedPixelShader(uint64_t pso, uint64_t ps_hash)
{
    PsoClone clone;
    if (!g_pso_clones.DeepCopy(pso, clone)) return {};
    for (const auto& so : clone.subs) {
        if (so.type != reshade::api::pipeline_subobject_type::pixel_shader || so.count < 1 || !so.data) continue;
        const auto* ps = static_cast<const reshade::api::shader_desc*>(so.data);
        if (!ps->code || !ps->code_size || HashShader(ps->code, ps->code_size) != ps_hash) return {};
        const auto* bytes = static_cast<const uint8_t*>(ps->code);
        return std::vector<uint8_t>(bytes, bytes + ps->code_size);
    }
    return {};
}
}

bool OutlineHide::Candidate(
    const reshade::api::input_element* layout, uint32_t layout_count, const void* ps, size_t ps_size)
{
    if (!layout || !ps) return false;
    bool skinned = false;
    for (uint32_t i = 0; i < layout_count && !skinned; ++i) {
        if (!Attribute(layout[i], 3, reshade::api::format::r8g8b8a8_uint)) continue;
        for (uint32_t k = 0; k < layout_count && !skinned; ++k)
            skinned = Attribute(layout[k], 4, reshade::api::format::r8g8b8a8_unorm) &&
                layout[k].buffer_binding == layout[i].buffer_binding;
    }
    if (!skinned) return false;
    DxilShape s;
    if (!DxilShaderShape(ps, ps_size, s) || s.other_resources != 0) return false;
    std::sort(s.outputs.begin(), s.outputs.end());
    if (!std::equal(s.outputs.begin(), s.outputs.end(), std::begin(kGbufferTargets), std::end(kGbufferTargets)))
        return false;
    return HasInput(s, "VELOCITY_PREV_POS0") && HasInput(s, "SV_IsFrontFace0");
}

void OutlineHide::StartLocked()
{
    if (!stop_ && !worker_.t.joinable()) worker_.t = std::thread(&OutlineHide::Run, this);
}

void OutlineHide::Queue(uint64_t pso, uint64_t ps_hash, const void* ps, size_t ps_size)
{
    const bool read_now = g_settings.hide_char_outlines && !g_addon.disabled;
    bool queued = false;
    bool outline = false;
    {
        std::lock_guard l(mutex_);
        const auto v = verdicts_.find(ps_hash);
        if (v != verdicts_.end()) {
            outline = v->second;
        } else if (!read_now) {
            noted_.push_back(Noted{ pso, ps_hash });
            noted_any_.store(true, std::memory_order_release);
        } else if (!stop_) {
            const auto* bytes = static_cast<const uint8_t*>(ps);
            queue_.push_back(Job{ pso, ps_hash, std::vector<uint8_t>(bytes, bytes + ps_size) });
            StartLocked();
            queued = true;
        }
    }
    if (queued)
        cv_.notify_one();
    else if (outline)
        g_pipelines.SetOutline(pso, ps_hash);
}

void OutlineHide::QueueNoted()
{
    {
        std::lock_guard l(mutex_);
        noted_any_.store(false, std::memory_order_release);
        if (stop_ || noted_.empty()) return;
        for (const Noted& n : noted_) {
            Job job;
            job.pso = n.pso;
            job.ps_hash = n.ps_hash;
            queue_.push_back(std::move(job));
        }
        noted_.clear();
        StartLocked();
    }
    cv_.notify_one();
}

void OutlineHide::Stop()
{
    std::thread worker;
    {
        std::lock_guard l(mutex_);
        stop_ = true;
        queue_.clear();
        pending_.clear();
        worker = std::move(worker_.t);
    }
    cv_.notify_all();
    if (!worker.joinable()) return;
    worker.join();
    logger::write("OUTLINE worker stopped (device destroyed / addon unloading)");
}

void OutlineHide::QueueStandIn(reshade::api::device* dev, uint64_t pso)
{
    {
        std::lock_guard l(mutex_);
        if (!dev || stop_ || !pending_.insert(pso).second) return;
        Job job;
        job.pso = pso;
        job.dev = dev;
        queue_.push_back(std::move(job));
        StartLocked();
    }
    cv_.notify_one();
}

std::string OutlineHide::Report()
{
    std::lock_guard l(mutex_);
    size_t marked = 0;
    for (const auto& v : verdicts_) marked += v.second ? 1 : 0;
    size_t vs_ok = 0;
    std::string failed;
    for (const auto& p : patched_) {
        if (!p.second->code.empty()) {
            ++vs_ok;
            continue;
        }
        char b[48];
        std::snprintf(b, sizeof b, "  vs %016llx: ", (unsigned long long)p.first);
        failed += b + p.second->why + "\n";
    }
    char b[512];
    std::snprintf(b, sizeof b,
        "pixel shaders read %zu, outline %zu; pipelines noted while A6 was off, not read yet %zu\n"
        "vertex shaders patched %zu, not patched %zu\n"
        "stand-ins alive %zu, failed %u, queued or in flight %zu\n",
        verdicts_.size(), marked, noted_.size(), vs_ok, patched_.size() - vs_ok, stand_ins_.size(), stand_ins_failed_,
        pending_.size());
    std::string out = b;
    out += "stand-in depth tests:";
    for (const auto& f : depth_funcs_)
        out += std::string(" ") + (f.first >= 0 && f.first < 8 ? kCompareNames[f.first] : "off") + " " +
            std::to_string(f.second);
    out += " (greater, less and their _equal get the shell's depth bias)\n";
#ifdef WWMI_DEBUG
    std::snprintf(b, sizeof b, "binds with the stand-in %llu, without %llu; game draws dropped %llu\n",
        (unsigned long long)bound_stand_in.load(), (unsigned long long)bound_source.load(),
        (unsigned long long)dropped.load());
    out += b;
#endif
    return out + failed;
}

void OutlineHide::BuildStandIn(const Job& job)
{
    using namespace reshade::api;
    PsoClone clone;
    pipeline_subobject* vs_so = nullptr;
    if (g_pso_clones.DeepCopy(job.pso, clone))
        for (auto& so : clone.subs)
            if (so.type == pipeline_subobject_type::vertex_shader && so.count >= 1 && so.data) {
                vs_so = &so;
                break;
            }
    const shader_desc* src = vs_so ? static_cast<const shader_desc*>(vs_so->data) : nullptr;
    const uint64_t vs_hash = src && src->code && src->code_size ? HashShader(src->code, src->code_size) : 0;
    std::shared_ptr<const Patched> patched;
    if (vs_hash) {
        {
            std::lock_guard l(mutex_);
            if (const auto it = patched_.find(vs_hash); it != patched_.end()) patched = it->second;
        }
        if (!patched) {
            auto p = std::make_shared<Patched>();
            RfxDxilResult r;
            if (RfxDxilZeroOffset(src->code, src->code_size, r)) p->code = std::move(r.code);
            p->why = r.why;
            logger::report("OUTLINE vs=%016llx: %s -- %s", (unsigned long long)vs_hash,
                p->code.empty() ? "NOT patched, its game draws are dropped instead" : "patched", p->why.c_str());
            patched = p;
            std::lock_guard l(mutex_);
            patched_[vs_hash] = patched;
        }
    }
    uint64_t made = 0;
    if (patched && !patched->code.empty()) {
        shader_desc vs = *src;
        vs.code = patched->code.data();
        vs.code_size = patched->code.size();
        vs_so->data = &vs;
        vs_so->count = 1;
        int func = -1;
        float away = 0.f;
        for (const auto& so : clone.subs)
            if (so.type == pipeline_subobject_type::depth_stencil_state && so.count >= 1 && so.data) {
                const auto& ds = *static_cast<const depth_stencil_desc*>(so.data);
                func = ds.depth_enable ? (int)ds.depth_func : -1;
                away = AwayFromCamera(ds);
            }
        rasterizer_desc rs;
        for (auto& so : clone.subs)
            if (so.type == pipeline_subobject_type::rasterizer_state && so.count >= 1 && so.data && away != 0.f) {
                rs = *static_cast<const rasterizer_desc*>(so.data);
                rs.depth_bias += away * kShellDepthBias;
                rs.slope_scaled_depth_bias += away * kShellSlopeBias;
                so.data = &rs;
                so.count = 1;
            }
        {
            std::lock_guard l(mutex_);
            ++depth_funcs_[func];
        }
        pipeline out = { 0 };
        if (job.dev->create_pipeline(clone.layout, (uint32_t)clone.subs.size(), clone.subs.data(), &out)) {
            g_pipelines.Register(job.dev, clone.layout, (uint32_t)clone.subs.size(), clone.subs.data(), out, job.pso);
            made = out.handle;
        }
    }
    {
        std::lock_guard l(mutex_);
        pending_.erase(job.pso);
        if (made)
            stand_ins_[job.pso] = made;
        else
            ++stand_ins_failed_;
        g_pipelines.SetOutlineStandIn(job.pso, made);
    }
    if (!vs_hash)
        logger::write(
            "OUTLINE src=%llx: no clone of it to build a stand-in from -- its game draws are dropped", job.pso);
    else if (!made && !patched->code.empty())
        logger::write(
            "OUTLINE src=%llx vs=%016llx: the stand-in pipeline was not created -- its game draws are dropped", job.pso,
            (unsigned long long)vs_hash);
}

void OutlineHide::Run()
{
    std::string why;
    const bool ready = RfxDxilInit((g_addon.cso_dir + L"\\dxc").c_str(), why);
    if (!ready) logger::write("OUTLINE: the shader reader did not start (%s) -- A6 hides nothing", why.c_str());
    for (;;) {
        Job job;
        bool known = false;
        bool outline = false;
        {
            std::unique_lock l(mutex_);
            cv_.wait(l, [this] { return stop_ || !queue_.empty(); });
            if (stop_) return;
            job = std::move(queue_.front());
            queue_.pop_front();
            const auto v = verdicts_.find(job.ps_hash);
            known = v != verdicts_.end();
            if (known) outline = v->second;
        }
        if (job.dev) {
            BuildStandIn(job);
            continue;
        }
        if (!known) {
            if (job.code.empty()) job.code = ClonedPixelShader(job.pso, job.ps_hash);
            if (job.code.empty()) continue;
            std::set<std::pair<uint32_t, std::string>> stores;
            outline = ready && RfxDxilLiteralStores(job.code.data(), job.code.size(), stores, why) &&
                stores == kOutlineStores;
            {
                std::lock_guard l(mutex_);
                verdicts_[job.ps_hash] = outline;
            }
            if (outline && g_debug)
                logger::report(
                    "OUTLINE ps=%016llx: stores the outline's G-buffer constants -- A6 lays its shell on the mesh",
                    (unsigned long long)job.ps_hash);
        }
        if (outline) g_pipelines.SetOutline(job.pso, job.ps_hash);
    }
}
