#include "pso_variant.h"
#include "pso_clone.h"
#include "rfx_patched_ps.h"
#include "rfx_toggles.h"
#include "../core/log.h"
#include "../core/fnv.h"
#include "../migoto/dx12_bridge.h" // ModPipeline
#include <d3d11.h> // D3D11_BLEND_DESC: the state-only [CustomShader] seam hands over the PARSED desc
#include <chrono>
#include <cstring>
#include <memory>
#include <sstream>

using namespace reshade::api;

PsoVariantCache g_pso_variants;

namespace {

void SwapPixelShader(std::vector<pipeline_subobject>& subs, const CodeBlob& ps, shader_desc& storage)
{
    for (auto& so : subs)
        if (so.type == pipeline_subobject_type::pixel_shader && so.count == 1 && so.data) {
            storage = *static_cast<const shader_desc*>(so.data);
            storage.code = ps->data();
            storage.code_size = ps->size();
            so.data = &storage;
            return;
        }
}

blend_op BlendOpFromTok(const std::string& t)
{
    if (t == "SUBTRACT") return blend_op::subtract;
    if (t == "REV_SUBTRACT") return blend_op::reverse_subtract;
    if (t == "MIN") return blend_op::min;
    if (t == "MAX") return blend_op::max;
    return blend_op::add;
}

blend_factor BlendFacFromTok(const std::string& t, bool alpha)
{
    if (t == "ZERO") return blend_factor::zero;
    if (t == "ONE") return blend_factor::one;
    if (t == "SRC_COLOR") return blend_factor::source_color;
    if (t == "INV_SRC_COLOR") return blend_factor::one_minus_source_color;
    if (t == "SRC_ALPHA") return blend_factor::source_alpha;
    if (t == "INV_SRC_ALPHA") return blend_factor::one_minus_source_alpha;
    if (t == "DEST_ALPHA") return blend_factor::dest_alpha;
    if (t == "INV_DEST_ALPHA") return blend_factor::one_minus_dest_alpha;
    if (t == "DEST_COLOR") return blend_factor::dest_color;
    if (t == "INV_DEST_COLOR") return blend_factor::one_minus_dest_color;
    if (t == "SRC_ALPHA_SAT") return blend_factor::source_alpha_saturate;
    if (t == "BLEND_FACTOR") return alpha ? blend_factor::constant_alpha : blend_factor::constant_color;
    if (t == "INV_BLEND_FACTOR")
        return alpha ? blend_factor::one_minus_constant_alpha : blend_factor::one_minus_constant_color;
    if (t == "SRC1_COLOR") return blend_factor::source1_color;
    if (t == "INV_SRC1_COLOR") return blend_factor::one_minus_source1_color;
    if (t == "SRC1_ALPHA") return blend_factor::source1_alpha;
    if (t == "INV_SRC1_ALPHA") return blend_factor::one_minus_source1_alpha;
    return blend_factor::one;
}

const char* TokFromD3D11Fac(D3D11_BLEND f)
{
    switch (f) {
    case D3D11_BLEND_ZERO: return "ZERO";
    case D3D11_BLEND_ONE: return "ONE";
    case D3D11_BLEND_SRC_COLOR: return "SRC_COLOR";
    case D3D11_BLEND_INV_SRC_COLOR: return "INV_SRC_COLOR";
    case D3D11_BLEND_SRC_ALPHA: return "SRC_ALPHA";
    case D3D11_BLEND_INV_SRC_ALPHA: return "INV_SRC_ALPHA";
    case D3D11_BLEND_DEST_ALPHA: return "DEST_ALPHA";
    case D3D11_BLEND_INV_DEST_ALPHA: return "INV_DEST_ALPHA";
    case D3D11_BLEND_DEST_COLOR: return "DEST_COLOR";
    case D3D11_BLEND_INV_DEST_COLOR: return "INV_DEST_COLOR";
    case D3D11_BLEND_SRC_ALPHA_SAT: return "SRC_ALPHA_SAT";
    case D3D11_BLEND_BLEND_FACTOR: return "BLEND_FACTOR";
    case D3D11_BLEND_INV_BLEND_FACTOR: return "INV_BLEND_FACTOR";
    case D3D11_BLEND_SRC1_COLOR: return "SRC1_COLOR";
    case D3D11_BLEND_INV_SRC1_COLOR: return "INV_SRC1_COLOR";
    case D3D11_BLEND_SRC1_ALPHA: return "SRC1_ALPHA";
    case D3D11_BLEND_INV_SRC1_ALPHA: return "INV_SRC1_ALPHA";
    default: return nullptr; // unmapped: say so, never silently blend wrong
    }
}

const char* TokFromD3D11Op(D3D11_BLEND_OP o)
{
    switch (o) {
    case D3D11_BLEND_OP_ADD: return "ADD";
    case D3D11_BLEND_OP_SUBTRACT: return "SUBTRACT";
    case D3D11_BLEND_OP_REV_SUBTRACT: return "REV_SUBTRACT";
    case D3D11_BLEND_OP_MIN: return "MIN";
    case D3D11_BLEND_OP_MAX: return "MAX";
    default: return nullptr;
    }
}

// A [CustomShader]'s D3D11 blend over the source's: per target, or target 0's for all without independent blend.
bool OverBlend(const D3D11_BLEND_DESC& d, blend_desc& out)
{
    out.alpha_to_coverage_enable = d.AlphaToCoverageEnable != FALSE;
    for (int rt = 0; rt < 8; ++rt) {
        const D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[d.IndependentBlendEnable ? rt : 0];
        out.blend_enable[rt] = r.BlendEnable != FALSE;
        out.render_target_write_mask[rt] = r.RenderTargetWriteMask;
        if (!r.BlendEnable) continue;
        const char* cop = TokFromD3D11Op(r.BlendOp);
        const char* csrc = TokFromD3D11Fac(r.SrcBlend);
        const char* cdst = TokFromD3D11Fac(r.DestBlend);
        const char* aop = TokFromD3D11Op(r.BlendOpAlpha);
        const char* asrc = TokFromD3D11Fac(r.SrcBlendAlpha);
        const char* adst = TokFromD3D11Fac(r.DestBlendAlpha);
        if (!cop || !csrc || !cdst || !aop || !asrc || !adst) return false;
        out.color_blend_op[rt] = BlendOpFromTok(cop);
        out.source_color_blend_factor[rt] = BlendFacFromTok(csrc, false);
        out.dest_color_blend_factor[rt] = BlendFacFromTok(cdst, false);
        out.alpha_blend_op[rt] = BlendOpFromTok(aop);
        out.source_alpha_blend_factor[rt] = BlendFacFromTok(asrc, true);
        out.dest_alpha_blend_factor[rt] = BlendFacFromTok(adst, true);
    }
    return true;
}

int FindSubobject(const std::vector<pipeline_subobject>& subs, pipeline_subobject_type t)
{
    for (size_t s = 0; s < subs.size(); ++s)
        if (subs[s].type == t) return (int)s;
    return -1;
}

} // namespace

ID3D12PipelineState* PsoVariantCache::BlendFromDesc(
    reshade::api::device* dev, uint64_t src_pso, const D3D11_BLEND_DESC* bdesc, const float bf[4], bool decensor)
{
    if (!dev || !src_pso || !bdesc) return nullptr;
    const D3D11_RENDER_TARGET_BLEND_DESC& r = bdesc->RenderTarget[0];
    if (!r.BlendEnable) return nullptr; // `blend = disable`: no variant wanted, draw with the game's PSO

    const char* cop = TokFromD3D11Op(r.BlendOp);
    const char* csrc = TokFromD3D11Fac(r.SrcBlend);
    const char* cdst = TokFromD3D11Fac(r.DestBlend);
    const char* aop = TokFromD3D11Op(r.BlendOpAlpha);
    const char* asrc = TokFromD3D11Fac(r.SrcBlendAlpha);
    const char* adst = TokFromD3D11Fac(r.DestBlendAlpha);
    if (!cop || !csrc || !cdst || !aop || !asrc || !adst) {
        static bool once = false;
        if (!once) {
            once = true;
            logger::write("PSOBLEND: unmapped D3D11 blend enum in a state-only [CustomShader] "
                          "(op=%d src=%d dst=%d) -- drawing OPAQUE rather than guessing",
                (int)r.BlendOp, (int)r.SrcBlend, (int)r.DestBlend);
        }
        return nullptr;
    }
    const std::string cmode = std::string(cop) + " " + csrc + " " + cdst;
    const std::string amode = std::string(aop) + " " + asrc + " " + adst;
    return Blend(dev, src_pso, cmode, amode, bf, decensor);
}

ID3D12PipelineState* PsoVariantCache::Blend(reshade::api::device* dev, uint64_t src_pso, const std::string& blend_mode,
    const std::string& alpha_mode, const float bf[4], bool decensor)
{
    if (!dev || !src_pso || blend_mode.empty()) return nullptr;
    const bool dbg = g_rfx_toggles.blend_debug_zero;
    uint64_t bkey = Fnv1a64(blend_mode.data(), blend_mode.size(), dbg ? 0xD000000000000001ULL : kFnv64Basis);
    bkey = (bkey ^ 0x5bd1e995u) * kFnv64Prime;
    bkey = Fnv1a64(alpha_mode.data(), alpha_mode.size(), bkey);
    const CodeBlob dps = decensor ? g_patched_ps.Decensored(src_pso) : nullptr;
    uint64_t key = src_pso ^ (bkey * 2654435761ULL) ^ (dps ? 0x9E3779B97F4A7C15ULL : 0);
    {
        std::lock_guard l(blend_mutex_);
        auto it = blend_.find(key);
        if (it != blend_.end()) return it->second.pso;
    }

    std::string ctoks[3];
    {
        std::istringstream is(blend_mode);
        is >> ctoks[0] >> ctoks[1] >> ctoks[2];
    }
    std::string atoks[3];
    if (!alpha_mode.empty()) {
        std::istringstream is(alpha_mode);
        is >> atoks[0] >> atoks[1] >> atoks[2];
    } else {
        atoks[0] = ctoks[0];
        atoks[1] = ctoks[1];
        atoks[2] = ctoks[2];
    }

    pipeline_layout layout;
    std::vector<pipeline_subobject> subs;
    int bidx = -1;
    const bool have = g_pso_clones.Shallow(src_pso, layout, subs, bidx);
    if (!have || bidx < 0) {
        std::lock_guard lv(blend_mutex_);
        if (blend_miss_logged_.insert(src_pso).second)
            logger::write("CUSTOMBLEND-MISS src=%llx: %s -- the piece draws OPAQUE", src_pso,
                !have ? "no clone of this PSO (created before the addon)" : "the clone has no blend_state subobject");
        return nullptr;
    }
    auto bd = std::make_unique<blend_desc>(*static_cast<const blend_desc*>(subs[bidx].data));
    for (int rt = 0; rt < 8; ++rt) {
        bd->blend_enable[rt] = true;
        if (dbg) { // SHOW BACKGROUND: result = 0*src + 1*dst = whatever is behind the piece
            bd->color_blend_op[rt] = bd->alpha_blend_op[rt] = blend_op::add;
            bd->source_color_blend_factor[rt] = bd->source_alpha_blend_factor[rt] = blend_factor::zero;
            bd->dest_color_blend_factor[rt] = bd->dest_alpha_blend_factor[rt] = blend_factor::one;
        } else {
            bd->color_blend_op[rt] = BlendOpFromTok(ctoks[0]);
            bd->source_color_blend_factor[rt] = BlendFacFromTok(ctoks[1], false);
            bd->dest_color_blend_factor[rt] = BlendFacFromTok(ctoks[2], false);
            bd->alpha_blend_op[rt] = BlendOpFromTok(atoks[0]);
            bd->source_alpha_blend_factor[rt] = BlendFacFromTok(atoks[1], true);
            bd->dest_alpha_blend_factor[rt] = BlendFacFromTok(atoks[2], true);
        }
    }
    // blend_constant here is informational only -- D3D12 takes the factor from OMSetBlendFactor, not the PSO.
    bd->blend_constant[0] = bf[0];
    bd->blend_constant[1] = bf[1];
    bd->blend_constant[2] = bf[2];
    bd->blend_constant[3] = bf[3];
    subs[bidx].data = bd.get();
    shader_desc dps_desc{};
    if (dps) SwapPixelShader(subs, dps, dps_desc);

    reshade::api::pipeline out = { 0 };
    const auto t0 = std::chrono::steady_clock::now();
    bool ok = dev->create_pipeline(layout, (uint32_t)subs.size(), subs.data(), &out);
    const double ms = logger::ms_since(t0);
    ID3D12PipelineState* variant = ok ? reinterpret_cast<ID3D12PipelineState*>(out.handle) : nullptr;
    {
        std::lock_guard lv(blend_mutex_);
        auto it = blend_.find(key);
        if (it != blend_.end()) {
            if (variant) dev->destroy_pipeline(out);
            return it->second.pso;
        }
        blend_[key] = PsoVariant{ src_pso, variant };
    }
    logger::write(
        "CUSTOMBLEND variant src=%llx blend='%s' alpha='%s' iniFactor=%.2f,%.2f,%.2f,%.2f (factor dynamic) decensored=%d -> %s, "
        "built on the drawing thread in %.1f ms",
        src_pso, blend_mode.c_str(), alpha_mode.empty() ? "(=color)" : alpha_mode.c_str(), bf[0], bf[1], bf[2], bf[3],
        (int)(dps != nullptr), variant ? "OK" : "FAILED(create)", ms);
    return variant;
}

ID3D12PipelineState* PsoVariantCache::InputLayout(reshade::api::device* dev, uint64_t src_pso,
    const D3D11_INPUT_ELEMENT_DESC* elements, uint32_t count, bool decensor, bool rfx)
{
    if (!dev || !src_pso || !elements || !count) return nullptr;
    uint64_t lkey = kFnv64Basis;
    for (uint32_t i = 0; i < count; ++i) {
        const D3D11_INPUT_ELEMENT_DESC& e = elements[i];
        if (e.SemanticName) lkey = Fnv1a64(e.SemanticName, strlen(e.SemanticName), lkey);
        const uint32_t v[3] = { e.SemanticIndex, (uint32_t)e.Format, e.AlignedByteOffset };
        lkey = Fnv1a64(v, sizeof v, lkey);
    }
    const CodeBlob dps = rfx ? g_patched_ps.Rfx(src_pso, decensor)
        : decensor           ? g_patched_ps.Decensored(src_pso)
                             : nullptr;
    if (rfx && !dps) return nullptr;
    const uint64_t salt = !dps ? 0
        : !rfx                 ? 0x9E3779B97F4A7C15ULL
        : decensor             ? 0xD6E8FEB86659FD93ULL
                               : 0xC2B2AE3D27D4EB4FULL;
    const uint64_t key = src_pso ^ (lkey * 2654435761ULL) ^ salt;
    {
        std::lock_guard l(layout_mutex_);
        if (auto it = layout_.find(key); it != layout_.end()) return it->second.pso;
    }

    pipeline_layout layout = {};
    std::vector<pipeline_subobject> subs;
    const char* why = nullptr;
    int bidx = -1;
    if (!g_pso_clones.Shallow(src_pso, layout, subs, bidx)) why = "no clone of this PSO (created before the addon)";
    int il = -1;
    for (size_t s = 0; !why && s < subs.size(); ++s)
        if (subs[s].type == pipeline_subobject_type::input_layout) il = (int)s;
    if (!why && (il < 0 || subs[il].count != count || !subs[il].data))
        why = "the PSO's input layout is not the one the override was made from";
    std::vector<input_element> ie;
    ID3D12PipelineState* variant = nullptr;
    double ms = 0.0;
    if (!why) {
        const auto* src = static_cast<const input_element*>(subs[il].data);
        ie.assign(src, src + count);
        for (uint32_t i = 0; i < count; ++i) {
            ie[i].semantic = elements[i].SemanticName;
            ie[i].semantic_index = elements[i].SemanticIndex;
            ie[i].format = (format)elements[i].Format;
            ie[i].offset = elements[i].AlignedByteOffset;
        }
        subs[il].data = ie.data();
        shader_desc dps_desc{};
        if (dps) SwapPixelShader(subs, dps, dps_desc);
        reshade::api::pipeline out = { 0 };
        const auto t0 = std::chrono::steady_clock::now();
        if (dev->create_pipeline(layout, (uint32_t)subs.size(), subs.data(), &out))
            variant = reinterpret_cast<ID3D12PipelineState*>(out.handle);
        else
            why = "create_pipeline failed";
        ms = logger::ms_since(t0);
    }
    {
        std::lock_guard l(layout_mutex_);
        if (auto it = layout_.find(key); it != layout_.end()) {
            if (variant) dev->destroy_pipeline(reshade::api::pipeline{ reinterpret_cast<uint64_t>(variant) });
            return it->second.pso;
        }
        layout_[key] = PsoVariant{ src_pso, variant };
    }
    char desc[512];
    size_t n = 0;
    desc[0] = 0;
    for (uint32_t i = 0; i < count && n + 48 < sizeof desc; ++i) {
        const int w = _snprintf_s(desc + n, sizeof desc - n, _TRUNCATE, " %s%u+%u:f%u",
            elements[i].SemanticName ? elements[i].SemanticName : "?", elements[i].SemanticIndex,
            elements[i].AlignedByteOffset, (unsigned)elements[i].Format);
        if (w < 0) break;
        n += (size_t)w;
    }
    logger::write("ILAYOUT variant src=%llx elems=%u ps=%s ->%s%s, built on the drawing thread in %.1f ms:%s",
        (unsigned long long)src_pso, count,
        !dps      ? "game"
            : rfx ? (decensor ? "rabbitfx+decensored" : "rabbitfx")
                  : "decensored",
        variant ? " OK" : " NONE, drawn with the game's layout -- ", variant ? "" : why, ms, desc);
    return variant;
}

ID3D12PipelineState* PsoVariantCache::ModShaders(device* dev, uint64_t src_pso, const MigotoPort::ModPipeline& mp)
{
    if (!dev || !src_pso || !mp.root_signature || !mp.code[0]) return nullptr;
    const uint64_t key = src_pso ^ (mp.key * 0x9E3779B97F4A7C15ULL);
    {
        std::lock_guard l(mod_mutex_);
        if (auto it = mod_.find(key); it != mod_.end()) return it->second.pso;
    }

    pipeline_layout layout = {};
    std::vector<pipeline_subobject> subs;
    int bidx = -1;
    const char* why = nullptr;
    if (!g_pso_clones.Shallow(src_pso, layout, subs, bidx)) why = "no clone of this PSO (created before the addon)";
    static const pipeline_subobject_type kStage[5] = { pipeline_subobject_type::vertex_shader,
        pipeline_subobject_type::hull_shader, pipeline_subobject_type::domain_shader,
        pipeline_subobject_type::geometry_shader, pipeline_subobject_type::pixel_shader };
    shader_desc sd[5] = {};
    for (int i = 0; !why && i < 5; ++i) {
        const int at = FindSubobject(subs, kStage[i]);
        const bool game_has = at >= 0 && subs[at].data && static_cast<const shader_desc*>(subs[at].data)->code_size;
        if (mp.code[i]) {
            sd[i].code = mp.code[i];
            sd[i].code_size = mp.code_size[i];
            if (at >= 0)
                subs[at] = { kStage[i], 1, &sd[i] };
            else
                subs.push_back({ kStage[i], 1, &sd[i] });
        } else if (game_has && i == 4) {
            subs.erase(subs.begin() + at);
        } else if (game_has) {
            why = "the game's draw has a hull, domain or geometry shader the list does not replace";
        }
    }
    blend_desc bd;
    if (!why && mp.blend) {
        const int at = FindSubobject(subs, pipeline_subobject_type::blend_state);
        if (at >= 0 && subs[at].data) bd = *static_cast<const blend_desc*>(subs[at].data);
        if (!OverBlend(*mp.blend, bd))
            why = "a blend operation or factor with no DX12 counterpart";
        else if (at >= 0)
            subs[at] = { pipeline_subobject_type::blend_state, 1, &bd };
        else
            subs.push_back({ pipeline_subobject_type::blend_state, 1, &bd });
    }
    rasterizer_desc rd;
    if (!why && mp.raster) {
        const int at = FindSubobject(subs, pipeline_subobject_type::rasterizer_state);
        if (at >= 0 && subs[at].data) rd = *static_cast<const rasterizer_desc*>(subs[at].data);
        rd.cull_mode = mp.raster->CullMode == D3D11_CULL_NONE ? cull_mode::none
            : mp.raster->CullMode == D3D11_CULL_FRONT         ? cull_mode::front
                                                              : cull_mode::back;
        rd.fill_mode = mp.raster->FillMode == D3D11_FILL_WIREFRAME ? fill_mode::wireframe : fill_mode::solid;
        if (at >= 0)
            subs[at] = { pipeline_subobject_type::rasterizer_state, 1, &rd };
        else
            subs.push_back({ pipeline_subobject_type::rasterizer_state, 1, &rd });
    }
    depth_stencil_desc dd;
    const int dat = FindSubobject(subs, pipeline_subobject_type::depth_stencil_state);
    if (dat >= 0 && subs[dat].data) dd = *static_cast<const depth_stencil_desc*>(subs[dat].data);
    bool depth_changed = false;
    if (!why && mp.depth) {
        dd.depth_enable = mp.depth->DepthEnable != FALSE;
        dd.depth_write_mask = mp.depth->DepthWriteMask == D3D11_DEPTH_WRITE_MASK_ALL;
        dd.depth_func = (compare_op)((uint32_t)mp.depth->DepthFunc - 1u);
        depth_changed = true;
    }
    std::vector<input_element> ie;
    if (!why && mp.layout) {
        ie.resize(mp.layout_count);
        for (unsigned i = 0; i < mp.layout_count; ++i) {
            const D3D12_INPUT_ELEMENT_DESC& e = mp.layout[i];
            ie[i].location = i;
            ie[i].semantic = e.SemanticName;
            ie[i].semantic_index = e.SemanticIndex;
            ie[i].format = (format)e.Format;
            ie[i].buffer_binding = e.InputSlot;
            ie[i].offset = e.AlignedByteOffset;
            ie[i].instance_step_rate =
                e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA ? e.InstanceDataStepRate : 0;
        }
        const int at = FindSubobject(subs, pipeline_subobject_type::input_layout);
        if (at >= 0)
            subs[at] = { pipeline_subobject_type::input_layout, mp.layout_count, ie.data() };
        else
            subs.push_back({ pipeline_subobject_type::input_layout, mp.layout_count, ie.data() });
    }
    format rtf[8] = {};
    format dsf = format::unknown;
    if (!why && mp.own_targets) {
        for (unsigned i = 0; i < mp.num_rt && i < 8; ++i) rtf[i] = (format)mp.rt_format[i];
        dsf = (format)mp.ds_format;
        const int rat = FindSubobject(subs, pipeline_subobject_type::render_target_formats);
        if (rat >= 0)
            subs[rat] = { pipeline_subobject_type::render_target_formats, mp.num_rt, rtf };
        else
            subs.push_back({ pipeline_subobject_type::render_target_formats, mp.num_rt, rtf });
        const int fat = FindSubobject(subs, pipeline_subobject_type::depth_stencil_format);
        if (fat >= 0)
            subs[fat] = { pipeline_subobject_type::depth_stencil_format, 1, &dsf };
        else
            subs.push_back({ pipeline_subobject_type::depth_stencil_format, 1, &dsf });
        if (mp.ds_format == DXGI_FORMAT_UNKNOWN) {
            dd.depth_enable = false;
            dd.stencil_enable = false;
            depth_changed = true;
        }
    }
    if (!why && depth_changed) {
        if (dat >= 0)
            subs[dat] = { pipeline_subobject_type::depth_stencil_state, 1, &dd };
        else
            subs.push_back({ pipeline_subobject_type::depth_stencil_state, 1, &dd });
    }

    ID3D12PipelineState* variant = nullptr;
    double ms = 0.0;
    if (!why) {
        pipeline out = { 0 };
        const auto t0 = std::chrono::steady_clock::now();
        if (dev->create_pipeline(pipeline_layout{ reinterpret_cast<uint64_t>(mp.root_signature) },
                (uint32_t)subs.size(), subs.data(), &out))
            variant = reinterpret_cast<ID3D12PipelineState*>(out.handle);
        else
            why = "create_pipeline failed";
        ms = logger::ms_since(t0);
    }
    {
        std::lock_guard l(mod_mutex_);
        if (auto it = mod_.find(key); it != mod_.end()) {
            if (variant) dev->destroy_pipeline(pipeline{ reinterpret_cast<uint64_t>(variant) });
            return it->second.pso;
        }
        mod_[key] = PsoVariant{ src_pso, variant };
    }
    logger::write(
        "MODPSO variant src=%llx vs=%zu ps=%zu gs=%zu blend=%s raster=%s depth=%s layout=%u targets=%s -> %s%s, "
        "built on the drawing thread in %.1f ms",
        (unsigned long long)src_pso, mp.code_size[0], mp.code_size[4], mp.code_size[3], mp.blend ? "list" : "game",
        mp.raster ? "list" : "game", mp.depth ? "list" : "game", mp.layout_count, mp.own_targets ? "list" : "game",
        variant ? "OK" : "NONE: ", variant ? "" : why, ms);
    return variant;
}

void PsoVariantCache::ForgetSource(uint64_t src_pso, std::vector<ID3D12PipelineState*>& dead)
{
    {
        std::lock_guard l(blend_mutex_);
        blend_miss_logged_.erase(src_pso);
        for (auto it = blend_.begin(); it != blend_.end();) {
            if (it->second.src == src_pso) {
                if (it->second.pso) dead.push_back(it->second.pso);
                it = blend_.erase(it);
            } else
                ++it;
        }
    }
    {
        std::lock_guard l(layout_mutex_);
        for (auto it = layout_.begin(); it != layout_.end();) {
            if (it->second.src == src_pso) {
                if (it->second.pso) dead.push_back(it->second.pso);
                it = layout_.erase(it);
            } else
                ++it;
        }
    }
    std::lock_guard l(mod_mutex_);
    for (auto it = mod_.begin(); it != mod_.end();) {
        if (it->second.src == src_pso) {
            if (it->second.pso) dead.push_back(it->second.pso);
            it = mod_.erase(it);
        } else
            ++it;
    }
}
