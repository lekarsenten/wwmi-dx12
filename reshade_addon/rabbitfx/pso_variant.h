#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace MigotoPort {
struct ModPipeline;
}

// A variant remembers its SOURCE so PsoCloneTable::Forget can find it under the composite key.
struct PsoVariant {
    uint64_t src;
    ID3D12PipelineState* pso;
};

// The game PSOs rebuilt the way a section asks: a [CustomShader]'s blend state, or an input layout override,
// each with the source's pixel shader or its patched one. Built on the drawing thread, kept per source.
class PsoVariantCache {
public:
    ID3D12PipelineState* Blend(reshade::api::device* dev, uint64_t src_pso, const std::string& blend_mode,
        const std::string& alpha_mode, const float bf[4], bool decensor);
    ID3D12PipelineState* BlendFromDesc(reshade::api::device* dev, uint64_t src_pso,
        const struct D3D11_BLEND_DESC* bdesc, const float bf[4], bool decensor);
    ID3D12PipelineState* InputLayout(reshade::api::device* dev, uint64_t src_pso,
        const struct D3D11_INPUT_ELEMENT_DESC* elements, uint32_t count, bool decensor, bool rfx);
    // The source with a list's own shaders and root signature in place of its own, and the blend, rasterizer, depth,
    // input layout and targets a [CustomShader] set in place of the source's (MigotoPort::GameBindings::pipeline).
    ID3D12PipelineState* ModShaders(reshade::api::device* dev, uint64_t src_pso, const MigotoPort::ModPipeline& mp);
    // The source's variants leave the tables; the caller destroys them.
    void ForgetSource(uint64_t src_pso, std::vector<ID3D12PipelineState*>& dead);

private:
    std::mutex blend_mutex_;
    std::unordered_map<uint64_t, PsoVariant> blend_; // (src^blendkey) -> variant
    // Sources whose clone was missing when a variant was asked for, logged once each (under the blend mutex).
    std::unordered_set<uint64_t> blend_miss_logged_;
    std::mutex layout_mutex_;
    std::unordered_map<uint64_t, PsoVariant> layout_; // (src^layoutkey) -> variant; pso nullptr = could not be made
    std::mutex mod_mutex_;
    std::unordered_map<uint64_t, PsoVariant> mod_; // (src^ModPipeline::key) -> variant; pso nullptr = could not be made
};
extern PsoVariantCache g_pso_variants;
