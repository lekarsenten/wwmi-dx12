#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <cstdint>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

using CodeBlob = std::shared_ptr<const std::vector<uint8_t>>;

// One copy of each shader's bytecode, however many PSOs use it; it lives as long as a clone holds it.
class ShaderCodePool {
public:
    CodeBlob Pooled(const void* p, size_t n);
    void Count(size_t& blobs, size_t& bytes) const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, std::weak_ptr<const std::vector<uint8_t>>> pool_;
    size_t prune_at_ = 1024;
};
extern ShaderCodePool g_shader_code;

struct PsoClone {
    std::vector<reshade::api::pipeline_subobject> subs; // owns nothing directly; data() points into `store` / `code`
    std::vector<std::vector<uint8_t>> store; // backing storage for every element array / string
    std::vector<CodeBlob> code; // the shaders' bytecode, shared with every clone of the same shader
    reshade::api::pipeline_layout layout = { 0 };
    int blend_idx = -1; // index of the blend_state subobject in `subs`
};

struct PsoCloneStats {
    size_t clones;
    size_t bytes;
    size_t code_blobs;
    size_t code_bytes;
    unsigned forgotten;
    unsigned variants_released;
};

// A deep copy of every game PSO with a blend state, from on_init_pipeline until the PSO is destroyed: what the
// blend, input-layout and shader-patch variants are built from. Forgetting a PSO releases its variants too.
class PsoCloneTable {
public:
    void Store(uint64_t pso_handle, reshade::api::pipeline_layout layout, uint32_t subobject_count,
        const reshade::api::pipeline_subobject* subobjects);
    void Forget(reshade::api::device* dev, uint64_t pso_handle);
    PsoCloneStats Stats() const;
    unsigned DescribeDepthStencil(uint64_t pso_handle, char* out, size_t cap) const;
    unsigned WriteShaders(uint64_t pso_handle, const wchar_t* dir) const;
    // The clone's layout and a shallow copy of its subobjects (data pointers into the persistent store).
    bool Shallow(uint64_t src_pso, reshade::api::pipeline_layout& layout,
        std::vector<reshade::api::pipeline_subobject>& subs, int& blend_idx) const;
    bool DeepCopy(uint64_t src_pso, PsoClone& out) const;

private:
    static bool DeepCopySubobjects(uint32_t n, const reshade::api::pipeline_subobject* src, PsoClone& c);
    static size_t CloneBytes(const PsoClone& c);
    mutable std::mutex mutex_;
    std::unordered_map<uint64_t, PsoClone> clones_; // src PSO handle -> deep copy, until the PSO is destroyed
    size_t bytes_ = 0; // the shared code not included
    unsigned forgotten_ = 0;
    unsigned variants_released_ = 0;
};
extern PsoCloneTable g_pso_clones;
