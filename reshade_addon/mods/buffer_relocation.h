#pragma once
#include <cstdint>
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <reshade.hpp>
#include <d3d12.h>

// d3dx.ini `allow_buffer_resize = 1` lets a [TextureOverride] grow the buffer the GAME allocated, to
// override_vertex_count * override_byte_stride: the game sizes its morph buffer for the vanilla mesh and a WWMI
// mod has more vertices. Read per hash through PortApi::ForEachBufferResize, keyed by the hash the override matches.
struct BufResize {
    uint32_t stride = 0;
    uint32_t vertex_count = 0;
    uint64_t bytes() const { return (uint64_t)stride * (uint64_t)vertex_count; }
};

struct RelocBuf {
    ID3D12Resource* res = nullptr;
    uint64_t va = 0;
    uint64_t bytes = 0;
};

class BufferRelocator {
public:
    bool Resolve(ID3D12Resource* orig, RelocBuf* out);
    void RememberOriginal(ID3D12Resource* relocated, unsigned original_bytes);
    void ForgetResource(uint64_t handle);
    // Was this buffer's size changed by us, and what was it born as?
    bool BirthBytes(uint64_t handle, unsigned* out);
    unsigned IdentityBytes(ID3D12Resource* res, unsigned current_bytes);
    bool Ensure(reshade::api::command_list* cmd_list, ID3D12Resource* orig, unsigned orig_bytes, unsigned hash,
        const BufResize& want, ID3D12Resource** out_res, uint64_t* out_va);
    // Destroys retired replacements the GPU has passed. Once per present.
    void Sweep(reshade::api::device* dev);
    // The resizes the inis declare, by the hash their [TextureOverride] matches; refilled at every load.
    void ClearDeclared();
    void Declare(uint32_t hash, const BufResize& want);
    BufResize Declared(uint32_t hash); // {} when none
    size_t DeclaredCount();

private:
    void RetireLocked(const RelocBuf& b);
    std::unordered_map<uint64_t, RelocBuf> reloc_; // original resource ptr -> its replacement
    std::unordered_map<uint64_t, unsigned> orig_bytes_; // relocated resource -> ORIGINAL bytes
    std::vector<std::pair<RelocBuf, uint64_t>> grave_; // retired replacement, GpuStamp() at its retirement
    std::mutex mtx_; // reloc_, orig_bytes_ AND grave_
    std::unordered_map<unsigned, unsigned> reloc_count_; // relocations per hash, for RELOC-STORM
    std::mutex count_mtx_;
    std::unordered_map<uint32_t, BufResize> declared_;
    std::mutex declared_mtx_;
};
extern BufferRelocator g_buffer_reloc;
