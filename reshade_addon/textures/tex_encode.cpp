#include "tex_encode.h"
#include "tex_defer.h"
#include "../addon_common.h"
#include "../mods/character_recognition.h"
#include "../migoto/dx12_wrap.h"
#include "../migoto/dx12_pipeline.h"
#include "../core/d3d12_buffer.h"
#include "../core/log.h"
#include <d3d11.h>
#include <algorithm>
#include <cmath>
#include <string>
#include "../addon/addon.h"
#include "../mods/wwmi_runtime.h"

TexEncoder g_tex_encoder;
TexEncodeTest g_tex_encode_test;

namespace {
constexpr UINT kErrBytes = 256;
constexpr UINT64 kSlotBytes = 4ull << 20; // an upload buffer: 64 block rows of the widest BC7 texture, 16384 texels
constexpr UINT kRowsStep = 16; // a band's block rows come in these: whole blocks four levels down
constexpr DWORD kDdsHead = 148; // magic, DDS_HEADER, DDS_HEADER_DXT10

double Psnr(uint64_t sse, double samples)
{
    if (samples <= 0) return 0.0;
    if (!sse) return 99.0;
    return 10.0 * std::log10(255.0 * 255.0 * samples / (double)sse);
}

Microsoft::WRL::ComPtr<ID3D12Resource> MakeBuffer(
    ID3D12Device* dev, D3D12_HEAP_TYPE heap, UINT64 bytes, D3D12_RESOURCE_STATES state, D3D12_RESOURCE_FLAGS flags)
{
    Microsoft::WRL::ComPtr<ID3D12Resource> r;
    if (FAILED(CreateD3D12Buffer(dev, heap, bytes, state, &r, flags))) r.Reset();
    return r;
}

D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}
} // namespace

UINT Bc7RowPitch(UINT blocks_x)
{
    return (blocks_x * 16 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
}

void TexEncoder::Prepare(ID3D12Device* dev)
{
    if (!dev) return;
    std::call_once(started_, [this, dev] {
        dev->AddRef();
        builder_ = std::thread([this, dev] {
            Build(dev);
            dev->Release();
        });
    });
}

void TexEncoder::Build(ID3D12Device* dev)
{
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<uint8_t> cso = WwmiReadFile((g_addon.cso_dir + L"\\Bc7Half.cso").c_str());

    D3D12_DESCRIPTOR_RANGE range{};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    D3D12_ROOT_PARAMETER rp[4] = {};
    rp[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    rp[0].Constants.Num32BitValues = 8;
    rp[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    rp[1].DescriptorTable.NumDescriptorRanges = 1;
    rp[1].DescriptorTable.pDescriptorRanges = &range;
    rp[2].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[3].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
    rp[3].Descriptor.ShaderRegister = 1;
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 4;
    rsd.pParameters = rp;

    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    HRESULT hr = cso.empty() ? E_FAIL : D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors);
    if (SUCCEEDED(hr))
        hr = dev->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_));
    if (SUCCEEDED(hr)) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd{};
        pd.pRootSignature = rs_.Get();
        pd.CS.pShaderBytecode = cso.data();
        pd.CS.BytecodeLength = cso.size();
        hr = dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&pso_));
    }
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (SUCCEEDED(hr)) {
        err_ = MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, kErrBytes, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (!err_) hr = E_OUTOFMEMORY;
    }
    if (FAILED(hr)) {
        logger::write("TEXENC: the encoder could not be built, hr=%08lX%s -- no texture is re-encoded",
            (unsigned long)hr, cso.empty() ? " (Bc7Half.cso is missing)" : "");
        failed_.store(true, std::memory_order_release);
        return;
    }
    logger::write("TEXENC: encoder ready; its pipeline took %.0f ms to build",
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
    ready_.store(true, std::memory_order_release);
}

bool TexEncoder::Begin(ID3D12Device* dev, InFlight& f)
{
    if (!spare_.empty()) {
        f = std::move(spare_.back());
        spare_.pop_back();
    }
    HRESULT hr = S_OK;
    if (!f.alloc) {
        hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc));
        if (SUCCEEDED(hr))
            hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, f.alloc.Get(), nullptr, IID_PPV_ARGS(&f.cl));
    } else {
        hr = f.alloc->Reset();
        if (SUCCEEDED(hr)) hr = f.cl->Reset(f.alloc.Get(), nullptr);
    }
    if (FAILED(hr)) {
        logger::write("TEXENC: no command list to record on, hr=%08lX", (unsigned long)hr);
        return false;
    }
    f.cl->SetComputeRootSignature(rs_.Get());
    f.cl->SetPipelineState(pso_.Get());
    return true;
}

uint64_t TexEncoder::Finish(ID3D12CommandQueue* queue, InFlight& f, bool any)
{
    if (FAILED(f.cl->Close()) || !any) {
        for (UINT s : f.srv_slots) MigotoPort::ReleaseStagingDescriptor('t', s);
        f.srv_slots.clear();
        f.blocks.clear();
        spare_.push_back(std::move(f));
        return 0;
    }
    ID3D12CommandList* lists[] = { f.cl.Get() };
    queue->ExecuteCommandLists(1, lists);
    f.value = ++fence_value_;
    queue->Signal(fence_.Get(), f.value);
    const uint64_t value = f.value;
    in_flight_.push_back(std::move(f));
    return value;
}

uint64_t TexEncoder::Submit(ID3D12Device* dev, ID3D12CommandQueue* queue, std::vector<Shrink>& work)
{
    for (Shrink& s : work) s.recorded = false;
    if (!Ready() || !dev || !queue || work.empty()) return 0;
    InFlight f;
    if (!Begin(dev, f)) return 0;

    std::vector<D3D12_RESOURCE_BARRIER> before_copy;
    std::vector<D3D12_RESOURCE_BARRIER> after_copy;
    std::vector<size_t> copy_of;
    for (size_t i = 0; i < work.size(); ++i) {
        const Shrink& s = work[i];
        const D3D12_RESOURCE_DESC sd = s.src->GetDesc();
        const D3D12_RESOURCE_DESC dd = s.dst->GetDesc();
        const UINT bx = (UINT)(dd.Width / 4);
        const UINT by = dd.Height / 4;
        const UINT pitch = Bc7RowPitch(bx);
        auto blocks = MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, (UINT64)pitch * by, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        UINT slot = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE cpu =
            blocks ? MigotoPort::AllocStagingDescriptor('t', &slot) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        if (!cpu.ptr) continue;
        f.srv_slots.push_back(slot);
        D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
        vd.Format = sd.Format;
        vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        vd.Texture2D.MipLevels = 1;
        dev->CreateShaderResourceView(s.src, &vd, cpu);
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        if (!MigotoPort::ShaderVisibleCopy(cpu, &gpu, nullptr, f.cl.Get())) continue;
        const uint32_t consts[8] = { bx, by, pitch, 0, sd.Format == DXGI_FORMAT_BC7_UNORM_SRGB ? 1u : 0u, s.shift, 0,
            0 };
        f.cl->SetComputeRoot32BitConstants(0, 8, consts, 0);
        f.cl->SetComputeRootDescriptorTable(1, gpu);
        f.cl->SetComputeRootUnorderedAccessView(2, blocks->GetGPUVirtualAddress());
        f.cl->SetComputeRootUnorderedAccessView(3, err_->GetGPUVirtualAddress());
        f.cl->Dispatch((bx + 7) / 8, (by + 7) / 8, 1);
        before_copy.push_back(
            Transition(blocks.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
        before_copy.push_back(Transition(s.dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
        after_copy.push_back(Transition(s.dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON));
        copy_of.push_back(i);
        f.blocks.push_back(std::move(blocks));
    }
    if (!copy_of.empty()) {
        f.cl->ResourceBarrier((UINT)before_copy.size(), before_copy.data());
        for (size_t c = 0; c < copy_of.size(); ++c) {
            const D3D12_RESOURCE_DESC dd = work[copy_of[c]].dst->GetDesc();
            D3D12_TEXTURE_COPY_LOCATION from{};
            from.pResource = f.blocks[c].Get();
            from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            from.PlacedFootprint.Footprint.Format = dd.Format;
            from.PlacedFootprint.Footprint.Width = (UINT)dd.Width;
            from.PlacedFootprint.Footprint.Height = dd.Height;
            from.PlacedFootprint.Footprint.Depth = 1;
            from.PlacedFootprint.Footprint.RowPitch = Bc7RowPitch((UINT)(dd.Width / 4));
            D3D12_TEXTURE_COPY_LOCATION to{};
            to.pResource = work[copy_of[c]].dst;
            to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            f.cl->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        }
        f.cl->ResourceBarrier((UINT)after_copy.size(), after_copy.data());
    }
    const uint64_t value = Finish(queue, f, !copy_of.empty());
    if (value)
        for (size_t i : copy_of) work[i].recorded = true;
    return value;
}

bool TexEncoder::Open(const std::wstring& file, UINT shift, Stream* s, const char** why)
{
    *why = nullptr;
    const HANDLE h = CreateFileW(
        file.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *why = "it did not open";
        return false;
    }
    auto fail = [&](const char* w) {
        CloseHandle(h);
        *why = w;
        return false;
    };
    uint8_t head[kDdsHead];
    DWORD got = 0;
    if (!ReadFile(h, head, kDdsHead, &got, nullptr) || got != kDdsHead) return fail("it is shorter than a DDS header");
    auto at = [&head](size_t o) {
        uint32_t v;
        memcpy(&v, head + o, 4);
        return v;
    };
    if (at(0) != 0x20534444u || at(84) != 0x30315844u) return fail("it has no DX10 header");
    const UINT height = at(12);
    const UINT width = at(16);
    if (at(128) < 97 || at(128) > 99) return fail("it is not BC7");
    if (at(28) > 1 || at(140) > 1) return fail("it has mips or slices");
    if (!width || !height || width % 4 || height % 4) return fail("its size is not whole blocks");
    const UINT scale = 1u << shift;
    const UINT bx = width / 4;
    const UINT by = height / 4;
    if (!shift || scale > kRowsStep || bx % scale || by % scale)
        return fail("it does not go that far down in whole blocks");
    const UINT rows = (UINT)(kSlotBytes / Bc7RowPitch(bx)) / kRowsStep * kRowsStep;
    if (!rows) return fail("it is wider than a band can hold");
    s->file = h;
    s->width = width;
    s->height = height;
    s->shift = shift;
    s->rows = (std::min)(rows, by);
    s->bands = (by + s->rows - 1) / s->rows;
    return true;
}

bool TexEncoder::ReadBand(const std::shared_ptr<Stream>& s, UINT band)
{
    if (!Ready()) return false;
    auto gone = [&] {
        return s->dropped.load(std::memory_order_acquire) || stop_reading_.load(std::memory_order_acquire);
    };
    unsigned k = 0;
    uint64_t value = 0;
    {
        std::unique_lock l(ring_mtx_);
        k = next_slot_;
        while (ring_[k].filled && !gone()) ring_cv_.wait_for(l, std::chrono::milliseconds(50));
        if (gone()) return false;
        next_slot_ = (k + 1) % kSlots;
        value = ring_[k].value;
    }
    while (value && !Passed(value)) {
        if (gone()) return false;
        if (!wake_) wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (wake_ && SUCCEEDED(fence_->SetEventOnCompletion(value, wake_)))
            WaitForSingleObject(wake_, 50);
        else
            Sleep(1);
    }
    Slot& slot = ring_[k];
    if (!slot.upload) {
        slot.upload = MakeBuffer(g_addon.d3d12_device, D3D12_HEAP_TYPE_UPLOAD, kSlotBytes,
            D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_FLAG_NONE);
        const D3D12_RANGE none{ 0, 0 };
        if (!slot.upload || FAILED(slot.upload->Map(0, &none, reinterpret_cast<void**>(&slot.p))) || !slot.p) {
            slot.upload.Reset();
            slot.p = nullptr;
            s->failed.store(true, std::memory_order_release);
            return false;
        }
    }
    const UINT bx = s->width / 4;
    const UINT row = bx * 16;
    const UINT pitch = Bc7RowPitch(bx);
    const UINT rows = (std::min)(s->rows, s->height / 4 - band * s->rows);
    DWORD got = 0;
    bool ok = true;
    if (row == pitch)
        ok = ReadFile(s->file, slot.p, row * rows, &got, nullptr) && got == row * rows;
    else
        for (UINT r = 0; ok && r < rows; ++r)
            ok = ReadFile(s->file, slot.p + (size_t)r * pitch, row, &got, nullptr) && got == row;
    if (!ok) {
        s->failed.store(true, std::memory_order_release);
        return false;
    }
    {
        std::lock_guard l(ring_mtx_);
        slot.filled = true;
        read_.push_back({ s, band, k });
    }
    return true;
}

uint64_t TexEncoder::SubmitBands(ID3D12Device* dev, ID3D12CommandQueue* queue)
{
    std::vector<Band> take;
    {
        std::lock_guard l(ring_mtx_);
        while (!read_.empty() && (read_.front().s->dst || read_.front().s->dropped.load(std::memory_order_acquire))) {
            take.push_back(std::move(read_.front()));
            read_.pop_front();
        }
    }
    if (take.empty()) return 0;
    InFlight f;
    const bool open = Ready() && dev && queue && Begin(dev, f);

    struct InList {
        Stream* s;
        D3D12_GPU_DESCRIPTOR_HANDLE srv;
    };
    std::vector<InList> in_list;
    std::vector<char> recorded(take.size());
    for (size_t i = 0; i < take.size(); ++i) {
        const Band& b = take[i];
        Stream& s = *b.s;
        if (s.dropped.load(std::memory_order_acquire) || s.failed.load(std::memory_order_acquire)) continue;
        if (!open) {
            s.failed.store(true, std::memory_order_release);
            continue;
        }
        const D3D12_RESOURCE_DESC dd = s.dst->GetDesc();
        const UINT scale = 1u << s.shift;
        const UINT bx_in = s.width / 4;
        const UINT bx = bx_in / scale;
        const UINT pitch_in = Bc7RowPitch(bx_in);
        const UINT pitch = Bc7RowPitch(bx);
        auto at = std::find_if(in_list.begin(), in_list.end(), [&s](const InList& e) { return e.s == &s; });
        const bool first_in_list = at == in_list.end();
        if (first_in_list) {
            if (!s.scratch) {
                D3D12_RESOURCE_DESC sd = dd;
                sd.Width = s.width;
                sd.Height = s.rows * 4;
                sd.Flags = D3D12_RESOURCE_FLAG_NONE;
                D3D12_HEAP_PROPERTIES hp{};
                hp.Type = D3D12_HEAP_TYPE_DEFAULT;
                if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &sd, D3D12_RESOURCE_STATE_COPY_DEST,
                        nullptr, IID_PPV_ARGS(&s.scratch))))
                    s.scratch.Reset();
                if (s.scratch)
                    s.blocks = MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, (UINT64)pitch * (s.rows / scale),
                        D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            }
            UINT srv_slot = 0;
            D3D12_CPU_DESCRIPTOR_HANDLE cpu =
                s.blocks ? MigotoPort::AllocStagingDescriptor('t', &srv_slot) : D3D12_CPU_DESCRIPTOR_HANDLE{};
            D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
            if (cpu.ptr) {
                f.srv_slots.push_back(srv_slot);
                D3D12_SHADER_RESOURCE_VIEW_DESC vd{};
                vd.Format = dd.Format;
                vd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                vd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                vd.Texture2D.MipLevels = 1;
                dev->CreateShaderResourceView(s.scratch.Get(), &vd, cpu);
            }
            if (!cpu.ptr || !MigotoPort::ShaderVisibleCopy(cpu, &gpu, nullptr, f.cl.Get())) {
                s.failed.store(true, std::memory_order_release);
                continue;
            }
            f.blocks.push_back(s.scratch);
            f.blocks.push_back(s.blocks);
            in_list.push_back({ &s, gpu });
            at = in_list.end() - 1;
        }
        if (b.band == 0) {
            const D3D12_RESOURCE_BARRIER open_dst =
                Transition(s.dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            f.cl->ResourceBarrier(1, &open_dst);
        }
        const UINT r0 = b.band * s.rows;
        const UINT rows_in = (std::min)(s.rows, s.height / 4 - r0);
        const UINT rows = rows_in / scale;
        D3D12_TEXTURE_COPY_LOCATION band{};
        band.pResource = ring_[b.slot].upload.Get();
        band.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        band.PlacedFootprint.Footprint.Format = dd.Format;
        band.PlacedFootprint.Footprint.Width = s.width;
        band.PlacedFootprint.Footprint.Height = rows_in * 4;
        band.PlacedFootprint.Footprint.Depth = 1;
        band.PlacedFootprint.Footprint.RowPitch = pitch_in;
        D3D12_TEXTURE_COPY_LOCATION into{};
        into.pResource = s.scratch.Get();
        into.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f.cl->CopyTextureRegion(&into, 0, 0, 0, &band, nullptr);
        const D3D12_RESOURCE_BARRIER pre[2] = { Transition(s.scratch.Get(), D3D12_RESOURCE_STATE_COPY_DEST,
                                                    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
            Transition(s.blocks.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS) };
        f.cl->ResourceBarrier(first_in_list ? 1 : 2, pre);
        const uint32_t consts[8] = { bx, rows, pitch, 0, dd.Format == DXGI_FORMAT_BC7_UNORM_SRGB ? 1u : 0u, s.shift, 0,
            0 };
        f.cl->SetComputeRoot32BitConstants(0, 8, consts, 0);
        f.cl->SetComputeRootDescriptorTable(1, at->srv);
        f.cl->SetComputeRootUnorderedAccessView(2, s.blocks->GetGPUVirtualAddress());
        f.cl->SetComputeRootUnorderedAccessView(3, err_->GetGPUVirtualAddress());
        f.cl->Dispatch((bx + 7) / 8, (rows + 7) / 8, 1);
        const D3D12_RESOURCE_BARRIER post[2] = {
            Transition(s.blocks.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE),
            Transition(s.scratch.Get(), D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST)
        };
        f.cl->ResourceBarrier(2, post);
        D3D12_TEXTURE_COPY_LOCATION out{};
        out.pResource = s.blocks.Get();
        out.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        out.PlacedFootprint.Footprint.Format = dd.Format;
        out.PlacedFootprint.Footprint.Width = bx * 4;
        out.PlacedFootprint.Footprint.Height = rows * 4;
        out.PlacedFootprint.Footprint.Depth = 1;
        out.PlacedFootprint.Footprint.RowPitch = pitch;
        D3D12_TEXTURE_COPY_LOCATION to{};
        to.pResource = s.dst;
        to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        f.cl->CopyTextureRegion(&to, 0, (r0 / scale) * 4, 0, &out, nullptr);
        if (b.band + 1 == s.bands) {
            const D3D12_RESOURCE_BARRIER close_dst =
                Transition(s.dst, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            f.cl->ResourceBarrier(1, &close_dst);
            s.done = true;
        }
        recorded[i] = 1;
    }
    uint64_t value = 0;
    if (open) value = Finish(queue, f, !in_list.empty());
    for (const InList& e : in_list) {
        if (value) {
            e.s->value = value;
        } else {
            e.s->done = false;
            e.s->failed.store(true, std::memory_order_release);
        }
    }
    {
        std::lock_guard l(ring_mtx_);
        for (size_t i = 0; i < take.size(); ++i) {
            Slot& slot = ring_[take[i].slot];
            if (recorded[i] && value) slot.value = value;
            slot.filled = false;
        }
    }
    ring_cv_.notify_all();
    return value;
}

void TexEncoder::StopReading()
{
    stop_reading_.store(true, std::memory_order_release);
    ring_cv_.notify_all();
}

void TexEncoder::Tick()
{
    for (size_t i = 0; i < in_flight_.size();) {
        if (!Passed(in_flight_[i].value)) {
            ++i;
            continue;
        }
        InFlight f = std::move(in_flight_[i]);
        in_flight_.erase(in_flight_.begin() + (ptrdiff_t)i);
        for (UINT s : f.srv_slots) MigotoPort::ReleaseStagingDescriptor('t', s);
        f.srv_slots.clear();
        f.blocks.clear();
        spare_.push_back(std::move(f));
    }
}

bool TexEncodeTest::Ensure(ID3D12Device* dev)
{
    if (cl_) return true;
    if (failed_ || !dev) return false;
    HRESULT hr = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc_));
    if (SUCCEEDED(hr))
        hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc_.Get(), nullptr, IID_PPV_ARGS(&cl_));
    if (SUCCEEDED(hr)) hr = cl_->Close();
    if (SUCCEEDED(hr)) hr = dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    if (FAILED(hr)) {
        logger::write("TEXENC: test setup FAILED hr=%08lX", (unsigned long)hr);
        failed_ = true;
        cl_.Reset();
        return false;
    }
    return true;
}

void TexEncodeTest::Start(ID3D12Device* dev, ID3D12CommandQueue* queue)
{
    skipped_ = Skipped{};
    items_.clear();
    const std::vector<std::wstring> namespaces = NamespacesOnScreen();
    mods_ = (unsigned)namespaces.size();

    UINT64 out_bytes = 0;
    for (const std::wstring& ns : namespaces) {
        std::vector<PortApi::LoadedTexture> have;
        PortApi::LoadedTexturesOf(ns.c_str(), have);
        for (PortApi::LoadedTexture& t : have) {
            const D3D12_RESOURCE_DESC rd = t.dx12->GetDesc();
            const MigotoPort::WrapResource* wr = MigotoPort::AsWrapResource(t.res);
            unsigned* skip = nullptr;
            if (!IsBc7(rd.Format))
                skip = &skipped_.format;
            else if (rd.MipLevels > 1)
                skip = &skipped_.mips;
            else if ((std::max)(rd.Width, (UINT64)rd.Height) < kTexMinSide)
                skip = &skipped_.too_small;
            else if ((rd.Width / 2) % 4 || (rd.Height / 2) % 4 || rd.DepthOrArraySize != 1)
                skip = &skipped_.unaligned;
            else if (!wr || wr->state != D3D12_RESOURCE_STATE_COMMON)
                skip = &skipped_.not_at_rest;
            if (skip) {
                ++*skip;
                if (t.res) t.res->Release();
                continue;
            }
            Item it{ std::move(t), (UINT)(rd.Width / 8), rd.Height / 8, 0, {}, false };
            it.srv = MigotoPort::AllocStagingDescriptor('t', &it.srv_slot);
            if (!it.srv.ptr) {
                if (it.tex.res) it.tex.res->Release();
                continue;
            }
            D3D12_SHADER_RESOURCE_VIEW_DESC sd{};
            sd.Format = rd.Format;
            sd.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            sd.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            sd.Texture2D.MipLevels = 1;
            dev->CreateShaderResourceView(it.tex.dx12, &sd, it.srv);
            out_bytes = (std::max)(out_bytes, (UINT64)Bc7RowPitch(it.blocks_x) * it.blocks_y);
            items_.push_back(std::move(it));
        }
    }
    if (items_.empty()) {
        logger::write(
            "TEXENC: nothing to encode in %u mod(s) on screen -- skipped %u not BC7, %u with mips, %u under %u px, "
            "%u not block-aligned at half size, %u not at rest",
            mods_, skipped_.format, skipped_.mips, skipped_.too_small, kTexMinSide, skipped_.unaligned,
            skipped_.not_at_rest);
        return;
    }

    const UINT n = (UINT)items_.size();
    const UINT64 err_bytes = 16ull * n;
    if (!out_ || out_->GetDesc().Width < out_bytes)
        out_ = MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, out_bytes, D3D12_RESOURCE_STATE_COMMON,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    err_ = MakeBuffer(dev, D3D12_HEAP_TYPE_DEFAULT, err_bytes, D3D12_RESOURCE_STATE_COMMON,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    readback_ = MakeBuffer(
        dev, D3D12_HEAP_TYPE_READBACK, err_bytes + 16ull * n, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_FLAG_NONE);
    D3D12_QUERY_HEAP_DESC qd{};
    qd.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;
    qd.Count = 2 * n;
    stamps_.Reset();
    if (!out_ || !err_ || !readback_ || FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&stamps_))) ||
        FAILED(alloc_->Reset()) || FAILED(cl_->Reset(alloc_.Get(), nullptr))) {
        logger::write("TEXENC: could not create its buffers (%llu KB of output) -- nothing encoded",
            (unsigned long long)(out_bytes / 1024));
        Release();
        return;
    }

    cl_->SetComputeRootSignature(g_tex_encoder.RootSignature());
    cl_->SetPipelineState(g_tex_encoder.Pipeline());
    for (UINT i = 0; i < n; ++i) {
        Item& it = items_[i];
        D3D12_GPU_DESCRIPTOR_HANDLE gpu{};
        if (!MigotoPort::ShaderVisibleCopy(it.srv, &gpu, nullptr, cl_.Get())) continue;
        const D3D12_RESOURCE_DESC rd = it.tex.dx12->GetDesc();
        const uint32_t consts[8] = { it.blocks_x, it.blocks_y, Bc7RowPitch(it.blocks_x), i,
            rd.Format == DXGI_FORMAT_BC7_UNORM_SRGB ? 1u : 0u, 1, 0, 0 };
        cl_->SetComputeRoot32BitConstants(0, 8, consts, 0);
        cl_->SetComputeRootDescriptorTable(1, gpu);
        cl_->SetComputeRootUnorderedAccessView(2, out_->GetGPUVirtualAddress());
        cl_->SetComputeRootUnorderedAccessView(3, err_->GetGPUVirtualAddress());
        cl_->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * i);
        cl_->Dispatch((it.blocks_x + 7) / 8, (it.blocks_y + 7) / 8, 1);
        D3D12_RESOURCE_BARRIER uav{};
        uav.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl_->ResourceBarrier(1, &uav);
        cl_->EndQuery(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 2 * i + 1);
        it.recorded = true;
    }
    const D3D12_RESOURCE_BARRIER to_copy =
        Transition(err_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cl_->ResourceBarrier(1, &to_copy);
    cl_->CopyBufferRegion(readback_.Get(), 0, err_.Get(), 0, err_bytes);
    cl_->ResolveQueryData(stamps_.Get(), D3D12_QUERY_TYPE_TIMESTAMP, 0, 2 * n, readback_.Get(), err_bytes);
    if (FAILED(cl_->Close())) {
        logger::write("TEXENC: its command list did not close -- nothing encoded");
        Release();
        return;
    }
    ID3D12CommandList* lists[] = { cl_.Get() };
    queue->ExecuteCommandLists(1, lists);
    queue->Signal(fence_.Get(), ++fence_value_);
    UINT64 freq = 0;
    ticks_per_ms_ = SUCCEEDED(queue->GetTimestampFrequency(&freq)) ? (double)freq / 1000.0 : 0.0;
    t0_ = std::chrono::steady_clock::now();
    busy_.store(true, std::memory_order_release);
}

void TexEncodeTest::Finish()
{
    const double wall_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0_).count();
    const UINT n = (UINT)items_.size();
    void* p = nullptr;
    D3D12_RANGE all{ 0, (SIZE_T)(32ull * n) };
    if (FAILED(readback_->Map(0, &all, &p)) || !p) {
        logger::write("TEXENC: its readback did not map");
        Release();
        return;
    }
    const uint64_t* sums = (const uint64_t*)p;
    const uint64_t* ts = sums + 2ull * n;
    double gpu_ms = 0.0;
    double mpx_in = 0.0;
    double px_out = 0.0;
    uint64_t sse_rgb = 0;
    uint64_t sse_a = 0;
    unsigned done = 0;
    for (UINT i = 0; i < n; ++i) {
        const Item& it = items_[i];
        if (!it.recorded) continue;
        const D3D12_RESOURCE_DESC rd = it.tex.dx12->GetDesc();
        const double ms = ticks_per_ms_ > 0 ? (double)(ts[2 * i + 1] - ts[2 * i]) / ticks_per_ms_ : 0.0;
        const double out = (double)it.blocks_x * it.blocks_y * 16.0;
        logger::write("TEXENC %llux%u %s -> %ux%u in %.2f ms on the graphics card; against a plain half-size average: "
                      "colour %.1f dB, alpha %.1f dB  [%ls]",
            (unsigned long long)rd.Width, rd.Height, rd.Format == DXGI_FORMAT_BC7_UNORM_SRGB ? "BC7 sRGB" : "BC7",
            it.blocks_x * 4, it.blocks_y * 4, ms, Psnr(sums[2 * i], out * 3.0), Psnr(sums[2 * i + 1], out),
            it.tex.load.fq_name.c_str());
        gpu_ms += ms;
        mpx_in += (double)rd.Width * rd.Height / 1e6;
        px_out += out;
        sse_rgb += sums[2 * i];
        sse_a += sums[2 * i + 1];
        ++done;
    }
    D3D12_RANGE none{ 0, 0 };
    readback_->Unmap(0, &none);
    logger::write(
        "TEXENC: %u texture(s) of %u mod(s) on screen: %.0f Mpx read, %.0f Mpx written as BC7 in %.1f ms on the "
        "graphics card (%.0f Mpx written a second), %.0f ms from the request to the result; colour %.1f dB, "
        "alpha %.1f dB over all; skipped %u not BC7, %u with mips, %u under %u px, %u not block-aligned at half "
        "size, %u not at rest",
        done, mods_, mpx_in, px_out / 1e6, gpu_ms, gpu_ms > 0 ? px_out / 1e6 / (gpu_ms / 1000.0) : 0.0, wall_ms,
        Psnr(sse_rgb, px_out * 3.0), Psnr(sse_a, px_out), skipped_.format, skipped_.mips, skipped_.too_small,
        kTexMinSide, skipped_.unaligned, skipped_.not_at_rest);
    Release();
}

void TexEncodeTest::Release()
{
    for (Item& it : items_) {
        if (it.srv.ptr) MigotoPort::ReleaseStagingDescriptor('t', it.srv_slot);
        if (it.tex.res) it.tex.res->Release();
    }
    items_.clear();
    out_.Reset();
    err_.Reset();
    readback_.Reset();
    stamps_.Reset();
    busy_.store(false, std::memory_order_release);
}

void TexEncodeTest::Tick(ID3D12CommandQueue* queue)
{
    if (busy_.load(std::memory_order_acquire)) {
        if (fence_->GetCompletedValue() >= fence_value_) Finish();
        return;
    }
    if (!request_.load(std::memory_order_acquire)) return;
    ID3D12Device* dev = g_addon.d3d12_device;
    if (!queue || !dev) return;
    if (!g_tex_encoder.Ready()) {
        g_tex_encoder.Prepare(dev);
        if (g_tex_encoder.Failed()) {
            request_.store(false, std::memory_order_release);
            logger::write("TEXENC: the encoder could not be built -- nothing encoded");
        }
        return;
    }
    request_.store(false, std::memory_order_release);
    if (!Ensure(dev)) return;
    if (queue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
        logger::write("TEXENC: the present queue is not a graphics queue -- nothing encoded");
        return;
    }
    Start(dev, queue);
}
