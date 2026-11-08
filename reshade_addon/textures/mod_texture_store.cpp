// mod_texture_store.cpp -- see mod_texture_store.h.
#include "mod_texture_store.h"
#include "../core/log.h"
#include "tex_lod.h" // TexLodCache::Init: the LOD cache read again on F5
#include "tex_look.h" // TexLook::Init: a seed dropped in since taken on F5
#include "../migoto/port_api.h" // their [Resource*] registry
#include "../instrumentation/profiler.h" // ET:TexStore
#include <windows.h>
#include <algorithm>
#include <cwctype>
#include "../mods/mod_registry.h"

using namespace reshade::api;

ModTextureStore g_mod_textures;

void ModTextureStore::Table::put_key(uint64_t k, uint32_t index)
{
    for (uint32_t i = slot(k), n = 0; n < icap; ++n, i = (i + 1) & (icap - 1)) {
        const uint64_t was = key[i].load(std::memory_order_acquire);
        if (was == k || was == 0) {
            val[i].store(index, std::memory_order_release);
            key[i].store(k, std::memory_order_release); // publish the key LAST
            return;
        }
    }
}

void ModTextureStore::Table::drop_key(uint64_t k)
{
    for (uint32_t i = slot(k), n = 0; n < icap; ++n, i = (i + 1) & (icap - 1)) {
        const uint64_t was = key[i].load(std::memory_order_acquire);
        if (was == k) {
            key[i].store(kGone, std::memory_order_release);
            return;
        }
        if (was == 0) return;
    }
}

D3D12_CPU_DESCRIPTOR_HANDLE ModTextureStore::NextSrv()
{
    if (!srv_heap_ || !srv_heap_cap_) return { 0 };
    if (!srv_free_.empty()) {
        const D3D12_CPU_DESCRIPTOR_HANDLE c = srv_free_.back();
        srv_free_.pop_back();
        return c;
    }
    const uint32_t blk = srv_heap_size_ / srv_heap_cap_;
    const uint32_t off = srv_heap_size_ % srv_heap_cap_;
    ID3D12DescriptorHeap* h = srv_heap_.Get();
    if (blk) {
        while (srv_more_.size() < blk) {
            Microsoft::WRL::ComPtr<ID3D12Device> dev;
            Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> nb;
            const D3D12_DESCRIPTOR_HEAP_DESC d = srv_heap_->GetDesc();
            if (FAILED(srv_heap_->GetDevice(IID_PPV_ARGS(&dev))) ||
                FAILED(dev->CreateDescriptorHeap(&d, IID_PPV_ARGS(&nb))))
                return { 0 };
            srv_more_.push_back(nb);
            logger::write("ModLoader: mod SRV heap grown to %u descriptors (the library outgrew its first sizing)",
                srv_heap_cap_ * (unsigned)(1 + srv_more_.size()));
        }
        h = srv_more_[blk - 1].Get();
    }
    D3D12_CPU_DESCRIPTOR_HANDLE c = h->GetCPUDescriptorHandleForHeapStart();
    c.ptr += (SIZE_T)off * srv_descriptor_size_;
    ++srv_heap_size_;
    return c;
}

static uint32_t SrvHeapCapacity(unsigned* dds_out)
{
    unsigned dds = 0;
    PortApi::ForEachCustomResource(
        [](const wchar_t*, const wchar_t* filename, int, bool, ID3D12Resource*, void* user) {
            const size_t n = filename ? wcslen(filename) : 0;
            if (n >= 4 && _wcsicmp(filename + n - 4, L".dds") == 0) ++*static_cast<unsigned*>(user);
        },
        &dds);
    if (dds_out) *dds_out = dds;
    const uint32_t need = 2u * dds + 256u;
    const uint32_t cap = (need < 1024u) ? 1024u : ((need + 63u) & ~63u);
    return cap;
}

void ModTextureStore::Init(device* device)
{
    device_ = device;
}

void ModTextureStore::DoReload()
{
    if (!device_) return;
    if (device_->get_api() != reshade::api::device_api::d3d12) {
        logger::write("ModLoader: skipping reload -- device is not D3D12 (api=%u)", (unsigned)device_->get_api());
        return;
    }
    g_tex_lod.Init(); // the LOD cache read again, and any seed dropped in since taken
    g_tex_look.Init();
    srv_heap_size_ = 0;
    srv_free_.clear();
    ++srv_epoch_;
}

ModTextureStore::Table* ModTextureStore::MakeTable(uint32_t cap)
{
    auto* s = new Table();
    uint32_t icap = 4096u;
    unsigned bits = 12;
    while (icap < 4u * cap) {
        icap <<= 1;
        ++bits;
    }
    s->entries = new Entry[cap]();
    s->used = new std::atomic<uint64_t>[cap];
    s->bytes = new uint64_t[cap]();
    s->srv_epoch = new unsigned[cap]();
    s->used_at = new std::atomic<uint64_t>[(size_t)cap * kMips];
    for (uint32_t i = 0; i < cap; ++i) s->used[i].store(0);
    for (size_t i = 0; i < (size_t)cap * kMips; ++i) s->used_at[i].store(0);
    s->key = new std::atomic<uint64_t>[icap];
    s->val = new std::atomic<uint32_t>[icap];
    for (uint32_t i = 0; i < icap; ++i) {
        s->key[i].store(0);
        s->val[i].store(0);
    }
    s->shift = 64u - bits;
    s->icap = icap;
    s->cap = cap;
    return s;
}

void ModTextureStore::AllocTable(uint32_t cap)
{
    if (table_.load(std::memory_order_acquire) || !cap) return;
    table_.store(MakeTable(cap), std::memory_order_release);
}

void ModTextureStore::ClearTable()
{
    gen_.fetch_add(1, std::memory_order_acq_rel);
    count_.store(0, std::memory_order_release);
    bytes_.store(0, std::memory_order_release);
    if (Table* s = table_.load(std::memory_order_acquire)) {
        for (uint32_t i = 0; i < s->icap; ++i) s->key[i].store(0, std::memory_order_release);
        for (uint32_t i = 0; i < s->cap; ++i) {
            s->used[i].store(0, std::memory_order_relaxed);
            s->bytes[i] = 0;
        }
        for (size_t i = 0; i < (size_t)s->cap * kMips; ++i) s->used_at[i].store(0, std::memory_order_relaxed);
    }
}

void ModTextureStore::Publish(uint32_t index, const Entry& e, uint64_t bytes)
{
    Table* s = table_.load(std::memory_order_acquire);
    if (!s || !e.res) return;
    if (index >= s->cap) {
        uint32_t cap = s->cap * 2;
        while (cap <= index) cap *= 2;
        Table* g = MakeTable(cap);
        const uint32_t n = count_.load(std::memory_order_acquire);
        for (uint32_t i = 0; i < n && i < s->cap; ++i) {
            g->entries[i] = s->entries[i];
            g->used[i].store(s->used[i].load(std::memory_order_relaxed), std::memory_order_relaxed);
            for (unsigned k = 0; k < kMips; ++k)
                g->used_at[(size_t)i * kMips + k].store(
                    s->used_at[(size_t)i * kMips + k].load(std::memory_order_relaxed), std::memory_order_relaxed);
            g->bytes[i] = s->bytes[i];
            g->srv_epoch[i] = s->srv_epoch[i];
            if (s->entries[i].res) g->put_key((uint64_t)s->entries[i].res, i);
        }
        table_.store(g, std::memory_order_release);
        logger::write("ModLoader: texture store grown to %u entries", cap);
        s = g;
    }
    s->entries[index] = e;
    s->used[index].store(0, std::memory_order_relaxed);
    for (unsigned k = 0; k < kMips; ++k) s->used_at[(size_t)index * kMips + k].store(0, std::memory_order_relaxed);
    s->bytes[index] = bytes;
    s->srv_epoch[index] = srv_epoch_;
    bytes_.fetch_add(bytes, std::memory_order_acq_rel);
    s->put_key((uint64_t)e.res, index);
    count_.store(index + 1, std::memory_order_release);
    gen_.fetch_add(1, std::memory_order_acq_rel);
}

void ModTextureStore::Touch(int index)
{
    if (!touch_.load(std::memory_order_relaxed)) return;
    const Table* s = table_.load(std::memory_order_acquire);
    if (!s || index < 0 || (uint32_t)index >= s->cap) return;
    const uint64_t now = GetTickCount64();
    if (s->used[index].load(std::memory_order_relaxed) != now) s->used[index].store(now, std::memory_order_relaxed);
}

void ModTextureStore::TouchRes(ID3D12Resource* res)
{
    if (touch_.load(std::memory_order_relaxed)) Touch(IndexOf(res));
}

uint64_t ModTextureStore::LastUse(int index) const
{
    const Table* s = table_.load(std::memory_order_acquire);
    return s && index >= 0 && (uint32_t)index < s->cap ? s->used[index].load(std::memory_order_relaxed) : 0;
}

void ModTextureStore::TouchAt(int index, unsigned mip)
{
    Touch(index);
    if (mip == kMipUnknown || !mips_.load(std::memory_order_relaxed) || !touch_.load(std::memory_order_relaxed)) return;
    const Table* s = table_.load(std::memory_order_acquire);
    if (!s || index < 0 || (uint32_t)index >= s->cap) return;
    const uint64_t now = GetTickCount64();
    std::atomic<uint64_t>& at = s->used_at[(size_t)index * kMips + (mip < kMips ? mip : kMips - 1)];
    if (at.load(std::memory_order_relaxed) != now) at.store(now, std::memory_order_relaxed);
}

unsigned ModTextureStore::Sharpest(int index, uint64_t since) const
{
    const Table* s = table_.load(std::memory_order_acquire);
    if (!s || index < 0 || (uint32_t)index >= s->cap) return kMipUnknown;
    for (unsigned k = 0; k < kMips; ++k) {
        const uint64_t at = s->used_at[(size_t)index * kMips + k].load(std::memory_order_relaxed);
        if (at && at >= since) return k;
    }
    return kMipUnknown;
}

int ModTextureStore::IndexOf(ID3D12Resource* res) const
{
    const Table* s = table_.load(std::memory_order_acquire);
    if (!res || !s) return -1;
    const uint64_t key = (uint64_t)res;
    for (uint32_t i = s->slot(key), n = 0; n < s->icap; ++n, i = (i + 1) & (s->icap - 1)) {
        const uint64_t k = s->key[i].load(std::memory_order_acquire);
        if (k == key) {
            const uint32_t v = s->val[i].load(std::memory_order_acquire);
            return (v < count_.load(std::memory_order_acquire)) ? (int)v : -1;
        }
        if (k == 0) return -1;
    }
    return -1;
}

bool ModTextureStore::EntryAt(int index, Entry* out) const
{
    const Table* s = table_.load(std::memory_order_acquire);
    if (!out || !s || index < 0 || (uint32_t)index >= count_.load(std::memory_order_acquire) ||
        (uint32_t)index >= s->cap)
        return false;
    *out = s->entries[index];
    return out->res != nullptr && out->srv.ptr != 0;
}

std::wstring ModTextureStore::NameOf(int index) const
{
    std::wstring name;
    {
        std::shared_lock l(names_mutex_);
        if (index < 0 || (size_t)index >= names_.size()) return L"?";
        name = names_[index];
    }
    std::wstring lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    const size_t at = lower.find(L"\\mods\\");
    return at == std::wstring::npos ? name : name.substr(at + 6);
}

ID3D12Resource* ModTextureStore::ResOfSrv(D3D12_CPU_DESCRIPTOR_HANDLE srv) const
{
    const Table* s = table_.load(std::memory_order_acquire);
    if (!srv.ptr || !s) return nullptr;
    const uint32_t n = (std::min)(count_.load(std::memory_order_acquire), s->cap);
    for (uint32_t i = 0; i < n; ++i)
        if (s->entries[i].srv.ptr == srv.ptr) return s->entries[i].res;
    return nullptr;
}

bool ModTextureStore::Forget(ID3D12Resource* res, D3D12_CPU_DESCRIPTOR_HANDLE* srv, unsigned* epoch)
{
    Table* s = table_.load(std::memory_order_acquire);
    const int index = IndexOf(res);
    if (!s || index < 0) return false;
    Entry& e = s->entries[index];
    if (srv) *srv = e.srv;
    if (epoch) *epoch = s->srv_epoch[index];
    bytes_.fetch_sub((std::min)(s->bytes[index], bytes_.load(std::memory_order_acquire)), std::memory_order_acq_rel);
    s->bytes[index] = 0;
    s->drop_key((uint64_t)res);
    std::atomic_ref<ID3D12Resource*>(e.res).store(nullptr, std::memory_order_release);
    if ((size_t)index < resources_.size()) {
        resources_[index].valid = false;
        resources_[index].res = nullptr;
    }
    gen_.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void ModTextureStore::SrvFree(D3D12_CPU_DESCRIPTOR_HANDLE srv, unsigned epoch)
{
    if (srv.ptr && epoch == srv_epoch_) srv_free_.push_back(srv);
}

void ModTextureStore::LoadTextureOverrides()
{
    if (overrides_loaded_) return;
    ID3D12Device* dev = device_ ? reinterpret_cast<ID3D12Device*>(device_->get_native()) : nullptr;
    if (!dev) return;
    overrides_loaded_ = true;
    if (!srv_heap_) {
        unsigned n_dds = 0;
        const uint32_t cap = SrvHeapCapacity(&n_dds);
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, cap,
            D3D12_DESCRIPTOR_HEAP_FLAG_NONE };
        HRESULT hr = dev->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&srv_heap_));
        if (FAILED(hr)) {
            logger::write("ModLoader Error: Failed to create SRV heap hr=%08x", hr);
            return;
        }
        srv_descriptor_size_ = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        srv_heap_cap_ = cap;
        AllocTable(cap);
        logger::write("ModLoader: mod SRV heap sized %u for the library (%u .dds declared)", cap, n_dds);
    }
    logger::write("ModLoader: %zu mods (from the inis their engine parsed)", ModRegistry::Discover().size());
}

static DXGI_FORMAT SrvFormatFor(DXGI_FORMAT f)
{
    switch (f) {
    case DXGI_FORMAT_BC1_TYPELESS: return DXGI_FORMAT_BC1_UNORM;
    case DXGI_FORMAT_BC2_TYPELESS: return DXGI_FORMAT_BC2_UNORM;
    case DXGI_FORMAT_BC3_TYPELESS: return DXGI_FORMAT_BC3_UNORM;
    case DXGI_FORMAT_BC4_TYPELESS: return DXGI_FORMAT_BC4_UNORM;
    case DXGI_FORMAT_BC5_TYPELESS: return DXGI_FORMAT_BC5_UNORM;
    case DXGI_FORMAT_BC6H_TYPELESS: return DXGI_FORMAT_BC6H_UF16;
    case DXGI_FORMAT_BC7_TYPELESS: return DXGI_FORMAT_BC7_UNORM;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_B8G8R8X8_TYPELESS: return DXGI_FORMAT_B8G8R8X8_UNORM;
    case DXGI_FORMAT_R8G8_TYPELESS: return DXGI_FORMAT_R8G8_UNORM;
    case DXGI_FORMAT_R8_TYPELESS: return DXGI_FORMAT_R8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS: return DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS: return DXGI_FORMAT_R32G32B32A32_FLOAT;
    case DXGI_FORMAT_R32G32B32_TYPELESS: return DXGI_FORMAT_R32G32B32_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    default: return f; // already fully qualified
    }
}

int ModTextureStore::DescribeTexture(ID3D12Device* dev, ID3D12Resource* dx12, const wchar_t* name)
{
    const D3D12_RESOURCE_DESC rd = dx12->GetDesc();
    D3D12_SHADER_RESOURCE_VIEW_DESC srv{};
    srv.Format = SrvFormatFor(rd.Format);
    if (srv.Format != rd.Format)
        logger::write(
            "PortRes: typeless resource format %d described as %d for the SRV", (int)rd.Format, (int)srv.Format);
    if (srv.Format == DXGI_FORMAT_UNKNOWN) {
        logger::write("PortRes: SKIPPING SRV -- resource format %d has no view-legal form", (int)rd.Format);
        return -1;
    }
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = rd.MipLevels ? rd.MipLevels : 1;

    const D3D12_CPU_DESCRIPTOR_HANDLE h = NextSrv();
    if (!h.ptr) return -1;
    dev->CreateShaderResourceView(dx12, &srv, h);

    resources_.push_back({ h, true, dx12, name });
    const uint32_t index = (uint32_t)resources_.size() - 1;
    {
        std::unique_lock l(names_mutex_);
        if (names_.size() <= index) names_.resize(index + 1);
        names_[index] = name;
    }
    Publish(index, Entry{ h, dx12 }, dev->GetResourceAllocationInfo(0, 1, &rd).SizeInBytes);
    return (int)index;
}

int ModTextureStore::Describe(ID3D12Resource* res, const wchar_t* name)
{
    ID3D12Device* dev = device_ ? reinterpret_cast<ID3D12Device*>(device_->get_native()) : nullptr;
    if (!dev || !srv_heap_ || !res) return -1;
    if (res->GetDesc().Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return -1;
    const int known = IndexOf(res);
    return known >= 0 ? known : DescribeTexture(dev, res, name ? name : L"");
}

void ModTextureStore::ClearForParse()
{
    ClearTable(); // the lock-free read side first, so no reader resolves into the old table
    resources_.clear();
    {
        std::unique_lock l(names_mutex_);
        names_.clear();
    }
    born_ = -1;
}

bool ModTextureStore::SubstantiateWclResources()
{
    PROF(PROF_ET_TexStore);
    ID3D12Device* dev = device_ ? reinterpret_cast<ID3D12Device*>(device_->get_native()) : nullptr;
    if (!dev || !srv_heap_) return false;
    {
        const long born = PortApi::WrapResourcesCreated();
        if (born == born_ && (++substantiate_calls_ & 7u)) return true;
        born_ = born;
        substantiate_calls_ = 0;
    }

    int registered = 0;
    struct Ctx {
        int* registered;
        ID3D12Device* dev;
        ModTextureStore* store;
    } ctx{ &registered, dev, this };
    PortApi::ForEachCustomResource(
        [](const wchar_t* fq_name, const wchar_t* filename, int type, bool substantiated, ID3D12Resource* dx12,
            void* user) {
            Ctx* c = (Ctx*)user;
            (void)type;
            (void)substantiated;
            if (!dx12) return; // not substantiated yet, or nothing to create
            const D3D12_RESOURCE_DESC rd = dx12->GetDesc();
            if (rd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D) return; // buffers are not swapped

            if (c->store->IndexOf(dx12) >= 0) return;

            if (c->store->DescribeTexture(c->dev, dx12, filename && *filename ? filename : fq_name) >= 0)
                ++*c->registered;
        },
        &ctx, [](ID3D12Resource* dx12, void* user) { return dx12 && ((Ctx*)user)->store->IndexOf(dx12) < 0; });

    if (registered > 0)
        logger::write("PortRes: described %d newly substantiated texture(s); %zu total, %u SRV slots used", registered,
            resources_.size(), srv_heap_size_);
    return true;
}

void ModTextureStore::Tick()
{
    if (reload_pending_) {
        DoReload();
        reload_pending_ = false;
    }
}

void ModTextureStore::Reload()
{
    reload_pending_ = true;
}
