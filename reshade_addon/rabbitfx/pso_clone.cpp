#include "pso_clone.h"
#include "pso_variant.h"
#include "rfx_patch.h"
#include "rfx_patched_ps.h"
#include "../core/fnv.h"
#include <cstdio>
#include <cstring>

using namespace reshade::api;

ShaderCodePool g_shader_code;
PsoCloneTable g_pso_clones;

CodeBlob ShaderCodePool::Pooled(const void* p, size_t n)
{
    const uint64_t key = Fnv1a64(p, n) ^ (uint64_t)n;
    std::lock_guard l(mutex_);
    auto& slot = pool_[key];
    if (CodeBlob b = slot.lock(); b && b->size() == n && memcmp(b->data(), p, n) == 0) return b;
    CodeBlob b = std::make_shared<const std::vector<uint8_t>>((const uint8_t*)p, (const uint8_t*)p + n);
    slot = b;
    if (pool_.size() >= prune_at_) {
        for (auto it = pool_.begin(); it != pool_.end();) it = it->second.expired() ? pool_.erase(it) : std::next(it);
        prune_at_ = pool_.size() * 2 + 1024;
    }
    return b;
}

void ShaderCodePool::Count(size_t& blobs, size_t& bytes) const
{
    blobs = 0;
    bytes = 0;
    std::lock_guard l(mutex_);
    for (const auto& kv : pool_)
        if (CodeBlob b = kv.second.lock()) {
            ++blobs;
            bytes += b->size();
        }
}

size_t PsoCloneTable::CloneBytes(const PsoClone& c)
{
    size_t n = 0;
    for (const auto& v : c.store) n += v.size();
    return n;
}

namespace {

size_t SubobjElemSize(pipeline_subobject_type t)
{
    using T = pipeline_subobject_type;
    switch (t) {
    case T::vertex_shader:
    case T::hull_shader:
    case T::domain_shader:
    case T::geometry_shader:
    case T::pixel_shader:
    case T::compute_shader:
    case T::amplification_shader:
    case T::mesh_shader:
    case T::raygen_shader:
    case T::any_hit_shader:
    case T::closest_hit_shader:
    case T::miss_shader:
    case T::intersection_shader:
    case T::callable_shader: return sizeof(shader_desc);
    case T::input_layout: return sizeof(input_element);
    case T::stream_output_state: return sizeof(stream_output_desc);
    case T::blend_state: return sizeof(blend_desc);
    case T::rasterizer_state: return sizeof(rasterizer_desc);
    case T::depth_stencil_state: return sizeof(depth_stencil_desc);
    case T::primitive_topology: return sizeof(primitive_topology);
    case T::depth_stencil_format: return sizeof(format);
    case T::render_target_formats: return sizeof(format);
    case T::dynamic_pipeline_states: return sizeof(dynamic_state);
    case T::sample_mask:
    case T::sample_count:
    case T::viewport_count:
    case T::max_vertex_count: return sizeof(uint32_t);
    default: return 0;
    }
}

const char* CmpName(compare_op c)
{
    switch (c) {
    case compare_op::never: return "never";
    case compare_op::less: return "less";
    case compare_op::equal: return "equal";
    case compare_op::less_equal: return "less_equal";
    case compare_op::greater: return "greater";
    case compare_op::not_equal: return "not_equal";
    case compare_op::greater_equal: return "greater_equal";
    case compare_op::always: return "always";
    }
    return "?";
}

const char* StOpName(stencil_op o)
{
    switch (o) {
    case stencil_op::keep: return "keep";
    case stencil_op::zero: return "zero";
    case stencil_op::replace: return "replace";
    case stencil_op::increment_saturate: return "incr_sat";
    case stencil_op::decrement_saturate: return "decr_sat";
    case stencil_op::invert: return "invert";
    case stencil_op::increment: return "incr";
    case stencil_op::decrement: return "decr";
    }
    return "?";
}

} // namespace

// Deep-copy a subobject array into a self-contained PsoClone (pointers fixed up to `store`).
bool PsoCloneTable::DeepCopySubobjects(uint32_t n, const pipeline_subobject* src, PsoClone& c)
{
    c.subs.resize(n);
    c.store.reserve(n * 4 + 8); // element arrays + per-shader code/entry + semantic strings
    auto blob = [&](const void* p, size_t bytes) -> void* {
        c.store.emplace_back(
            (const uint8_t*)p, (const uint8_t*)p + bytes); // moving the outer vec keeps inner data() valid
        return c.store.back().data();
    };
    for (uint32_t i = 0; i < n; ++i) {
        pipeline_subobject so = src[i];
        size_t esz = SubobjElemSize(so.type);
        if (esz == 0 || so.data == nullptr || so.count == 0) {
            c.subs[i] = so;
            continue;
        }
        void* base = blob(so.data, esz * so.count);
        so.data = base;
        switch (so.type) {
        case pipeline_subobject_type::vertex_shader:
        case pipeline_subobject_type::hull_shader:
        case pipeline_subobject_type::domain_shader:
        case pipeline_subobject_type::geometry_shader:
        case pipeline_subobject_type::pixel_shader:
        case pipeline_subobject_type::compute_shader:
        case pipeline_subobject_type::amplification_shader:
        case pipeline_subobject_type::mesh_shader: {
            auto* sd = static_cast<shader_desc*>(base);
            for (uint32_t e = 0; e < so.count; ++e) {
                if (sd[e].code && sd[e].code_size) {
                    c.code.push_back(g_shader_code.Pooled(sd[e].code, sd[e].code_size));
                    sd[e].code = c.code.back()->data();
                }
                if (sd[e].entry_point)
                    sd[e].entry_point = (const char*)blob(sd[e].entry_point, strlen(sd[e].entry_point) + 1);
                if (sd[e].spec_constants && sd[e].spec_constant_ids)
                    sd[e].spec_constant_ids =
                        (const uint32_t*)blob(sd[e].spec_constant_ids, sd[e].spec_constants * sizeof(uint32_t));
                if (sd[e].spec_constants && sd[e].spec_constant_values)
                    sd[e].spec_constant_values =
                        (const uint32_t*)blob(sd[e].spec_constant_values, sd[e].spec_constants * sizeof(uint32_t));
            }
            break;
        }
        case pipeline_subobject_type::input_layout: {
            auto* el = static_cast<input_element*>(base);
            for (uint32_t e = 0; e < so.count; ++e)
                if (el[e].semantic) el[e].semantic = (const char*)blob(el[e].semantic, strlen(el[e].semantic) + 1);
            break;
        }
        case pipeline_subobject_type::blend_state: c.blend_idx = (int)i; break;
        default: break;
        }
        c.subs[i] = so;
    }
    return true;
}

bool PsoCloneTable::Shallow(
    uint64_t src_pso, pipeline_layout& layout, std::vector<pipeline_subobject>& subs, int& blend_idx) const
{
    std::lock_guard l(mutex_);
    auto it = clones_.find(src_pso);
    if (it == clones_.end()) return false;
    layout = it->second.layout;
    subs = it->second.subs; // shallow copy: data ptrs still point into the persistent `store`
    blend_idx = it->second.blend_idx;
    return true;
}

bool PsoCloneTable::DeepCopy(uint64_t src_pso, PsoClone& out) const
{
    std::lock_guard l(mutex_);
    auto it = clones_.find(src_pso);
    if (it == clones_.end()) return false;
    out.layout = it->second.layout;
    DeepCopySubobjects((uint32_t)it->second.subs.size(), it->second.subs.data(), out);
    return true;
}

unsigned PsoCloneTable::WriteShaders(uint64_t pso_handle, const wchar_t* dir) const
{
    if (!dir || !dir[0]) return 0;
    std::vector<uint8_t> blob[2];
    static const char* const kStage[2] = { "vs", "ps" };
    {
        std::lock_guard l(mutex_);
        auto it = clones_.find(pso_handle);
        if (it == clones_.end()) return 0;
        for (const auto& so : it->second.subs) {
            const int k = so.type == pipeline_subobject_type::vertex_shader ? 0
                : so.type == pipeline_subobject_type::pixel_shader          ? 1
                                                                            : -1;
            if (k < 0 || so.count < 1 || !so.data) continue;
            const auto* sd = static_cast<const shader_desc*>(so.data);
            if (sd->code && sd->code_size)
                blob[k].assign((const uint8_t*)sd->code, (const uint8_t*)sd->code + sd->code_size);
        }
    }
    unsigned wrote = 0;
    for (int k = 0; k < 2; ++k) {
        if (blob[k].empty()) continue;
        wchar_t path[MAX_PATH];
        swprintf_s(path, L"%s\\%016llx.%S.cso", dir, (unsigned long long)HashShader(blob[k].data(), blob[k].size()),
            kStage[k]);
        if (GetFileAttributesW(path) != INVALID_FILE_ATTRIBUTES) continue;
        FILE* f = nullptr;
        if (_wfopen_s(&f, path, L"wb") != 0 || !f) continue;
        fwrite(blob[k].data(), 1, blob[k].size(), f);
        fclose(f);
        ++wrote;
    }
    return wrote;
}

unsigned PsoCloneTable::DescribeDepthStencil(uint64_t pso_handle, char* out, size_t cap) const
{
    if (!out || !cap) return 0;
    depth_stencil_desc ds{};
    bool have = false;
    {
        std::lock_guard l(mutex_);
        auto it = clones_.find(pso_handle);
        if (it != clones_.end())
            for (const auto& so : it->second.subs)
                if (so.type == pipeline_subobject_type::depth_stencil_state && so.count >= 1) {
                    ds = *static_cast<const depth_stencil_desc*>(so.data);
                    have = true;
                    break;
                }
    }
    if (!have) return 0;
    const int n = _snprintf_s(out, cap, _TRUNCATE,
        "depth %s write=%d func=%s | stencil %s read=%02x write=%02x ref=%u "
        "front func=%s pass=%s fail=%s zfail=%s back func=%s pass=%s fail=%s zfail=%s",
        ds.depth_enable ? "on" : "off", (int)ds.depth_write_mask, CmpName(ds.depth_func),
        ds.stencil_enable ? "on" : "off", (unsigned)ds.front_stencil_read_mask, (unsigned)ds.front_stencil_write_mask,
        (unsigned)ds.front_stencil_reference_value, CmpName(ds.front_stencil_func), StOpName(ds.front_stencil_pass_op),
        StOpName(ds.front_stencil_fail_op), StOpName(ds.front_stencil_depth_fail_op), CmpName(ds.back_stencil_func),
        StOpName(ds.back_stencil_pass_op), StOpName(ds.back_stencil_fail_op), StOpName(ds.back_stencil_depth_fail_op));
    return n > 0 ? (unsigned)n : 0u;
}

// Deep-copy a PSO's subobjects (called from on_init_pipeline); kept until the PSO is destroyed.
void PsoCloneTable::Store(
    uint64_t pso_handle, pipeline_layout layout, uint32_t subobject_count, const pipeline_subobject* subobjects)
{
    PsoClone c;
    c.layout = layout;
    if (!DeepCopySubobjects(subobject_count, subobjects, c) || c.blend_idx < 0) return;
    g_patched_ps.Forget(pso_handle);
    std::lock_guard l(mutex_);
    // A handle already here is a recycled one whose destroy we did not see: the new PSO replaces it.
    if (auto old = clones_.find(pso_handle); old != clones_.end()) {
        bytes_ -= CloneBytes(old->second);
        clones_.erase(old);
    }
    bytes_ += CloneBytes(c);
    clones_.emplace(pso_handle, std::move(c));
}

void PsoCloneTable::Forget(reshade::api::device* dev, uint64_t pso_handle)
{
    {
        std::lock_guard l(mutex_);
        if (auto e = clones_.find(pso_handle); e != clones_.end()) {
            bytes_ -= CloneBytes(e->second);
            clones_.erase(e);
            ++forgotten_;
        }
    }
    std::vector<ID3D12PipelineState*> dead;
    g_pso_variants.ForgetSource(pso_handle, dead);
    g_rfx_patch.ForgetSource(pso_handle, dead);
    for (ID3D12PipelineState* v : dead) {
        if (dev)
            dev->destroy_pipeline(reshade::api::pipeline{ reinterpret_cast<uint64_t>(v) });
        else
            v->Release();
    }
    if (!dead.empty()) {
        std::lock_guard l(mutex_);
        variants_released_ += (unsigned)dead.size();
    }
}

PsoCloneStats PsoCloneTable::Stats() const
{
    size_t code_blobs = 0;
    size_t code_bytes = 0;
    g_shader_code.Count(code_blobs, code_bytes);
    std::lock_guard l(mutex_);
    return PsoCloneStats{ clones_.size(), bytes_, code_blobs, code_bytes, forgotten_, variants_released_ };
}
