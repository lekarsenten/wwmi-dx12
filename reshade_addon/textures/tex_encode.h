#pragma once
#include "../migoto/port_api.h" // LoadedTexture
#include <d3d12.h>
#include <wrl/client.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// The long side below which no texture is made smaller, by the budget or the C11 test.
constexpr UINT kTexMinSide = 1024;
inline bool IsBc7(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_BC7_UNORM || f == DXGI_FORMAT_BC7_UNORM_SRGB;
}

// A smaller BC7 copy of a BC7 texture, made on the graphics card from the texture in video memory or from its file.
class TexEncoder {
public:
    struct Shrink {
        ID3D12Resource* src; // at rest (COMMON), one level
        ID3D12Resource* dst; // empty, the source's format, one level, 2^shift times smaller on each side
        UINT shift;
        bool recorded = false; // set by Submit: its destination gets written
    };
    // A single-level BC7 file made smaller band by band: each band of block rows is read into one of a few upload
    // buffers the encoder reuses, so neither the file nor the texture at its own size is ever held whole.
    struct Stream {
        HANDLE file = INVALID_HANDLE_VALUE; // the reading thread's, past the header
        UINT width = 0; // the file's
        UINT height = 0;
        UINT shift = 0;
        UINT rows = 0; // block rows a band holds
        UINT bands = 0;
        std::atomic<bool> failed{ false }; // a band was not read or not recorded
        std::atomic<bool> dropped{ false }; // not wanted any more: read no further
        ID3D12Resource* dst = nullptr; // present thread from here down: set before its first band is recorded
        Microsoft::WRL::ComPtr<ID3D12Resource> scratch; // a band at the file's size
        Microsoft::WRL::ComPtr<ID3D12Resource> blocks; // a band at the destination's
        bool done = false; // every band recorded
        uint64_t value = 0; // the fence value of the last list that wrote dst
    };
    void Prepare(ID3D12Device* dev); // builds the pipeline on a worker thread, once
    bool Ready() const { return ready_.load(std::memory_order_acquire); }
    bool Failed() const { return failed_.load(std::memory_order_acquire); }
    // Records the shrinks on a list of its own and executes it on the queue. Returns the value its fence reaches once
    // every recorded destination is written and at rest again, or 0 when nothing was submitted.
    uint64_t Submit(ID3D12Device* dev, ID3D12CommandQueue* queue, std::vector<Shrink>& work);
    // Any thread: the file opened past its header; false, with why, when it is not a single-level BC7 .dds that goes
    // shift levels down in whole blocks.
    static bool Open(const std::wstring& file, UINT shift, Stream* s, const char** why);
    // The one reading thread, bands in order: waits for a free upload buffer and reads the band into it. False when
    // the band was not read (s->failed), the stream was dropped or reading stopped.
    bool ReadBand(const std::shared_ptr<Stream>& s, UINT band);
    uint64_t SubmitBands(
        ID3D12Device* dev, ID3D12CommandQueue* queue); // present thread, every frame: records the bands read
    void StopReading(); // for good: a waiting ReadBand returns false
    bool Passed(uint64_t value) const { return fence_ && fence_->GetCompletedValue() >= value; }
    void Tick(); // present thread: frees what the graphics card is done with
    ID3D12RootSignature* RootSignature() const { return rs_.Get(); }
    ID3D12PipelineState* Pipeline() const { return pso_.Get(); }
    ~TexEncoder()
    {
        if (builder_.joinable()) builder_.detach();
        if (wake_) CloseHandle(wake_);
    }

private:
    static constexpr unsigned kSlots = 4;
    struct Slot {
        Microsoft::WRL::ComPtr<ID3D12Resource> upload;
        uint8_t* p = nullptr; // mapped for good
        uint64_t value = 0; // the list that last read it
        bool filled = false; // read, not yet recorded
    };
    struct Band {
        std::shared_ptr<Stream> s;
        UINT band;
        unsigned slot;
    };
    struct InFlight {
        Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc;
        Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl;
        std::vector<Microsoft::WRL::ComPtr<ID3D12Resource>> blocks; // and every other resource the list uses
        std::vector<UINT> srv_slots;
        uint64_t value = 0;
    };
    bool Begin(ID3D12Device* dev, InFlight& f);
    uint64_t Finish(ID3D12CommandQueue* queue, InFlight& f, bool any);
    void Build(ID3D12Device* dev);

    std::atomic<bool> ready_{ false };
    std::atomic<bool> failed_{ false };
    std::once_flag started_;
    std::thread builder_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> rs_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    Microsoft::WRL::ComPtr<ID3D12Resource> err_; // the shader's error sums, never read here
    uint64_t fence_value_ = 0; // present thread from here down
    std::vector<InFlight> in_flight_;
    std::vector<InFlight> spare_;

    std::mutex ring_mtx_;
    std::condition_variable ring_cv_;
    Slot ring_[kSlots]; // under ring_mtx_: value and filled
    std::deque<Band> read_; // under ring_mtx_: read, in order, not yet recorded
    unsigned next_slot_ = 0; // under ring_mtx_
    std::atomic<bool> stop_reading_{ false };
    HANDLE wake_ = nullptr; // the reading thread's: the fence reached a buffer's value
};
extern TexEncoder g_tex_encoder;

// Bytes per row of a BC buffer that CopyTextureRegion can read: 16 per block, rows aligned.
UINT Bc7RowPitch(UINT blocks_x);

// A half-size BC7 copy of every single-mip BC7 texture of the mods on screen, made on the graphics card, measured and
// thrown away.
class TexEncodeTest {
public:
    void Request() { request_.store(true, std::memory_order_release); }
    bool Busy() const { return busy_.load(std::memory_order_acquire); }
    void Tick(ID3D12CommandQueue* queue); // present thread, after the replacer's

private:
    struct Item {
        PortApi::LoadedTexture tex; // its reference held until the graphics card is done
        UINT blocks_x;
        UINT blocks_y;
        UINT srv_slot;
        D3D12_CPU_DESCRIPTOR_HANDLE srv;
        bool recorded;
    };
    struct Skipped {
        unsigned format = 0;
        unsigned mips = 0;
        unsigned too_small = 0;
        unsigned unaligned = 0;
        unsigned not_at_rest = 0;
    };
    bool Ensure(ID3D12Device* dev);
    void Start(ID3D12Device* dev, ID3D12CommandQueue* queue);
    void Finish();
    void Release();

    std::atomic<bool> request_{ false };
    std::atomic<bool> busy_{ false };
    bool failed_ = false; // present thread from here down

    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> alloc_;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> cl_;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence_;
    uint64_t fence_value_ = 0;
    Microsoft::WRL::ComPtr<ID3D12QueryHeap> stamps_;
    Microsoft::WRL::ComPtr<ID3D12Resource> out_;
    Microsoft::WRL::ComPtr<ID3D12Resource> err_;
    Microsoft::WRL::ComPtr<ID3D12Resource> readback_;

    std::vector<Item> items_;
    Skipped skipped_;
    unsigned mods_ = 0;
    double ticks_per_ms_ = 0.0;
    std::chrono::steady_clock::time_point t0_;
};
extern TexEncodeTest g_tex_encode_test;
