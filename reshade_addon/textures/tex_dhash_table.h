#pragma once
#include <d3d12.h>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

struct TexAssetLayout;

// A23: the dHash (tex_dhash.h) of each game texture at the mips a [Filter] section can be asked about: the mip XXMI
// picks (the smallest whose sides are both 256 or more, else mip 0) and every mip smaller than 256 a side, taken as
// the uploads arrive and carried along the game's copies, by mip size. Taken only while a [Filter] may ask
// (PortApi::TextureDHashesWanted); a mip written without a dHash taken of it loses the one it had.
class TexDHashTable {
public:
    struct Kept {
        uint32_t w = 0;
        uint32_t h = 0;
        uint64_t linear = 0;
        uint64_t srgb = 0;
    };
    std::vector<Kept> KeptFor(uint64_t res);
    // Bit m: mip m has a kept dHash.
    uint32_t KeptMips(ID3D12Resource* res);
    void FromUpload(ID3D12Resource* src, uint64_t offset, ID3D12Resource* dst, uint32_t sub, const TexAssetLayout& L);
    void FromData(ID3D12Resource* dst, uint32_t sub, const uint8_t* data, size_t row_pitch);
    void Copy(uint64_t src, uint32_t src_sub, uint64_t dst, uint32_t dst_sub);
    void CopyAll(uint64_t src, uint64_t dst);
    // A write that is no whole upload or copy of the mip (a region of it).
    void Unknown(uint64_t res, uint32_t sub);
    void Forget(uint64_t res);
    bool Lookup(ID3D12Resource* res, uint32_t mip, uint32_t view_fmt, uint64_t* out);

private:
    using Entry = Kept;
    static bool Wanted(const D3D12_RESOURCE_DESC& d, uint32_t sub);
    static bool HashUpload(ID3D12Resource* src, uint64_t offset, const D3D12_RESOURCE_DESC& d, uint32_t sub,
        const TexAssetLayout& L, Entry& e);
    void Store(uint64_t res, const Entry& e);
    void Drop(uint64_t res, uint32_t w, uint32_t h);

    std::mutex mutex_;
    std::unordered_map<uint64_t, std::vector<Entry>> by_res_;
};
extern TexDHashTable g_tex_dhash;

bool TexDHashLookup(ID3D12Resource* res, unsigned mip, unsigned view_format, uint64_t* out);
unsigned TexDHashMips(ID3D12Resource* res);
