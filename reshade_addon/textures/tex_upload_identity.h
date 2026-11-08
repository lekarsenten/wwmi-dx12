#pragma once
#include <reshade.hpp>
#include <d3d12.h>
#include <atomic>
#include <cstdint>
#include <vector>

// A 2D texture's mip as an upload lays it out: rows of tight bytes at a 256-aligned pitch.
struct TexAssetLayout {
    uint32_t w = 0;
    uint32_t h = 0;
    uint32_t rows = 0;
    uint32_t tight_row = 0;
    uint32_t pitch = 0;
    bool floor = false; // at or above kTexHashFloorPx: an upload can name it
};
bool TexAssetLayoutOf(const D3D12_RESOURCE_DESC& d, uint32_t mip, TexAssetLayout& o);
std::vector<uint8_t> TightFromUpload(const uint8_t* src, size_t pitch, const TexAssetLayout& L);
// BC formats: collapse unorm/srgb to the TYPELESS base (the resource format 3dmigoto hashes).
uint32_t TexIdentityHash(uint32_t data_hash, uint32_t w, uint32_t h, uint32_t mips, uint32_t arr, uint32_t dxgi_fmt);
// D15: a degenerate LOD low written as a .dds under the dump folder, once per hash.
void DumpIfDegenerate(
    uint32_t hash, const std::vector<uint8_t>& tight, const TexAssetLayout& L, uint32_t fmt, uint32_t sub, uint32_t of);

// A texture's identity from its mip0 as it uploads: the 3dmigoto hash, into the identity table, with everything
// that follows from knowing it (lineage edges parked on it, its held mips, the look-alike search).
class TexUploadIdentity {
public:
    // Hash an uploaded texture's mip0 (3dmigoto identity) into the identity table; updates rolling cache.
    void HashFromUpload(ID3D12Resource* dest, const uint8_t* src, size_t src_row_pitch);
    // The processor reads the game's upload memory too slowly (Resizable BAR): GPU copies from now on.
    bool GpuPreferred() const { return gpu_.load(std::memory_order_relaxed); }
    bool ReadSlow(const uint8_t* p, uint64_t span);
    bool HashOnGpu(reshade::api::command_list* cl, ID3D12Resource* src, uint64_t off, uint64_t span,
        ID3D12Resource* dst, uint32_t sub, const uint8_t* compare_from, const TexAssetLayout& L);

private:
    static constexpr uint32_t kUpProbeBytes = 16u * 1024;
    static constexpr double kUpSlowMBps = 256.0;
    std::atomic<uint32_t> slow_run_{ 0 }; // consecutive upload reads slower than kUpSlowMBps
    std::atomic<bool> gpu_{ false };
    std::atomic<uint32_t> probe_sink_{ 0 };
};
extern TexUploadIdentity g_tex_upload;
