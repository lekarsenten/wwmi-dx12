#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

// The shader-visible descriptor heap a RabbitFX draw binds through: the game's tables mirrored into it with the
// mod's maps in their slots. A reservation is a window of slots whose last GPU use has passed; the heap grows
// (the old one retired until the GPU is done with it) when every slot is still in flight.
class RfxShadowHeap {
public:
    struct Lease { // the heap lock is held for the lease's lifetime: the draw's mirror, copies and bind
        std::unique_lock<std::mutex> lock;
        uint32_t start = 0;
        SIZE_T cpu0 = 0;
        UINT64 gpu0 = 0;
        uint32_t inc = 0;
        ID3D12DescriptorHeap* heap = nullptr;
        explicit operator bool() const { return heap != nullptr; }
    };
    Lease Reserve(uint32_t n);
    uint32_t Inc();

private:
    bool Ensure(ID3D12Device* dev);
    bool ReserveLocked(uint32_t n);
    std::mutex mutex_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap_;
    uint32_t cap_ = 0;
    uint32_t cur_ = 0;
    uint32_t inc_ = 0;
    std::vector<uint64_t> tag_; // per block: GpuStamp() at its last write
    std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>, uint64_t>> retired_;
};
extern RfxShadowHeap g_rfx_shadow;
