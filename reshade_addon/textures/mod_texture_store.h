#pragma once
#include <d3d12.h>
#include <reshade.hpp>
#include <wrl.h>
#include <atomic>
#include <shared_mutex>
#include <string>
#include <vector>

// The mod textures the host has loaded: an SRV for each in our own heap, the resource, the file it came from, and
// a lock-free table from the resource to its index that the draw path reads: when it was last handed to a draw,
// and how far down the game drew its own texture there.
class ModTextureStore {
public:
    static constexpr unsigned kMips = 4; // how far down the game draws its own texture there: 0, 1, 2, 3 or more
    static constexpr unsigned kMipUnknown = ~0u;
    struct Resource {
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
        bool valid = false;
        ID3D12Resource* res = nullptr;
        std::wstring path;
    };
    struct Entry {
        D3D12_CPU_DESCRIPTOR_HANDLE srv{};
        ID3D12Resource* res = nullptr;
    };

    void Init(reshade::api::device* device);
    void Reload();
    void Tick(); // runs a pending Reload
    unsigned SrvHeapSize() const { return srv_heap_size_; }
    unsigned SrvHeapCap() const { return srv_heap_cap_ * (unsigned)(1 + srv_more_.size()); }
    // Ini parsing is owned by the ported engine; runs once it has loaded: the SRV heap, sized from what it declared.
    void LoadTextureOverrides();
    void ClearForParse();
    bool SubstantiateWclResources();
    const std::vector<Resource>& Resources() const { return resources_; }
    int IndexOf(ID3D12Resource* res) const;
    bool EntryAt(int index, Entry* out) const;
    std::wstring NameOf(int index) const; // its file below the Mods folder; any thread
    unsigned Gen() const { return gen_.load(std::memory_order_acquire); }
    ID3D12Resource* ResOfSrv(D3D12_CPU_DESCRIPTOR_HANDLE srv) const;
    void Touch(int index); // handed to a draw now
    void TouchRes(ID3D12Resource* res); // ... by its resource
    void SetTouch(bool on) { touch_.store(on, std::memory_order_relaxed); } // off: nothing is recorded or read
    uint64_t LastUse(int index) const; // GetTickCount64 when it last was; 0 = never
    void TouchAt(int index, unsigned mip); // handed to a draw where the game draws its own `mip` levels down
    void SetMips(bool on) { mips_.store(on, std::memory_order_relaxed); } // off: TouchAt records only what Touch does
    unsigned Sharpest(int index, uint64_t since) const; // the fewest levels down it was handed at since then
    uint64_t Bytes() const { return bytes_.load(std::memory_order_acquire); } // every texture, at allocation size
    // Present thread, as SubstantiateWclResources: a texture described before anything can resolve to it; its
    // entry taken out again (key gone, index dead, its SRV slot handed back for SrvFree once nothing can be copying it).
    int Describe(ID3D12Resource* res, const wchar_t* name);
    bool Forget(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE* srv, unsigned* epoch); // epoch: SrvEpoch() then
    unsigned SrvEpoch() const { return srv_epoch_; }
    void SrvFree(D3D12_CPU_DESCRIPTOR_HANDLE srv, unsigned epoch);

private:
    struct Table {
        static constexpr uint64_t kGone = 1; // a key taken out: lookups probe past it
        uint32_t cap = 0;
        uint32_t icap = 0;
        unsigned shift = 64;
        Entry* entries = nullptr;
        std::atomic<uint64_t>* used = nullptr; // per index: GetTickCount64 when last handed to a draw
        std::atomic<uint64_t>* used_at = nullptr; // per index x kMips: ... where the game drew its own that far down
        uint64_t* bytes = nullptr; // per index: its allocation size; present thread
        unsigned* srv_epoch = nullptr; // per index: srv_epoch_ when its SRV slot was handed out; present thread
        std::atomic<uint64_t>* key = nullptr;
        std::atomic<uint32_t>* val = nullptr;
        uint32_t slot(uint64_t k) const { return (uint32_t)((k * 0x9E3779B97F4A7C15ull) >> shift) & (icap - 1); }
        void put_key(uint64_t k, uint32_t index);
        void drop_key(uint64_t k);
    };
    static Table* MakeTable(uint32_t cap);
    void AllocTable(uint32_t cap);
    void ClearTable();
    void Publish(uint32_t index, const Entry& e, uint64_t bytes);
    D3D12_CPU_DESCRIPTOR_HANDLE NextSrv(); // {0} if one could not be made
    int DescribeTexture(ID3D12Device* dev, ID3D12Resource* dx12, const wchar_t* name); // its index, or -1
    void DoReload();

    reshade::api::device* device_ = nullptr;
    bool reload_pending_ = false;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv_heap_;
    std::vector<Microsoft::WRL::ComPtr<ID3D12DescriptorHeap>> srv_more_; // further blocks of srv_heap_cap_
    uint32_t srv_heap_size_ = 0;
    uint32_t srv_heap_cap_ = 0; // descriptors in srv_heap_ (and in each further block); 0 until it exists
    uint32_t srv_descriptor_size_ = 0;
    std::vector<D3D12_CPU_DESCRIPTOR_HANDLE> srv_free_; // slots given back by SrvFree
    unsigned srv_epoch_ = 0; // bumped when every slot is handed out again from 0
    std::vector<Resource> resources_;
    mutable std::shared_mutex names_mutex_;
    std::vector<std::wstring> names_; // resources_' paths, for readers on other threads
    std::atomic<Table*> table_{ nullptr };
    std::atomic<uint32_t> count_{ 0 };
    std::atomic<unsigned> gen_{ 1 };
    std::atomic<uint64_t> bytes_{ 0 };
    std::atomic<bool> touch_{ false };
    std::atomic<bool> mips_{ false };
    bool overrides_loaded_ = false;
    long born_ = -1;
    unsigned substantiate_calls_ = 0;
};
extern ModTextureStore g_mod_textures;
