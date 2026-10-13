#include "../reshade_addon/migoto/dx12_device.h"
#include "../reshade_addon/migoto/dx12_wrap.h"
#include "../reshade_addon/migoto/dx12_bridge.h"
#include "../reshade_addon/migoto/dx12_pipeline.h"
#include "../reshade_addon/migoto/dx12_dump_readback.h"
#include "../src/gpu_progress.h"
#include "../reshade_addon/instrumentation/perf_monitor.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <cstdio>
#include <stdexcept>
#include <thread>
#include <fstream>
#include <sstream>

using Microsoft::WRL::ComPtr;
using namespace MigotoPort;

static void Check(bool condition, const char* message)
{
    if (!condition) throw std::runtime_error(message);
}

static void Empty()
{
    for (const auto& row : ResourceMemory().Snapshot().rows)
        Check(row.count == 0 && row.bytes == 0 && row.unknown == 0, "resource accounting did not return to zero");
}

int main()
{
    try {
        ResourceMemoryCounters counters;
        auto unavailable = counters.Created(UINT64_MAX, true);
        Check(counters.Snapshot().rows[1].unknown == 1 && counters.Snapshot().rows[1].bytes == 0,
            "invalid allocation size must not inflate memory totals");
        counters.Retired(unavailable);
        counters.Released(unavailable);
        counters.Retired({}); // Untracked game resources and staging pools.
        counters.Released({});
        auto workload = [&] {
            for (unsigned i = 0; i < 10000; ++i) {
                auto skinning = counters.Created(65536, false);
                counters.Retired(skinning);
                counters.Released(skinning);
            }
        };
        std::thread render(workload), loader(workload);
        render.join(); loader.join();
        for (const auto& row : counters.Snapshot().rows)
            Check(row.count == 0 && row.bytes == 0 && row.unknown == 0, "concurrent lifecycle accounting");

        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        ComPtr<ID3D12Device> device;
        Check(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))), "DXGI factory");
        Check(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))), "WARP adapter");
        Check(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device))), "WARP device");
        Dx12Bridge bridge{};
        bridge.device = device.Get();
        SetDx12Bridge(bridge);
        Empty();

        D3D11_BUFFER_DESC skeleton{};
        skeleton.ByteWidth = 76800;
        skeleton.Usage = D3D11_USAGE_DEFAULT;
        skeleton.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Buffer> buffer;
        Check(SUCCEEDED(Device()->CreateBuffer(&skeleton, nullptr, &buffer)), "mod skeleton buffer creation");
        auto* wrapper = AsWrapResource(buffer.Get());
        auto desc = wrapper->dx12->GetDesc();
        const auto expected = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        Check(ResourceMemory().Snapshot().rows[0].bytes == expected, "buffer allocation padding");

        D3D11_TEXTURE2D_DESC diffuse{};
        diffuse.Width = 1024; diffuse.Height = 1024; diffuse.MipLevels = 0; diffuse.ArraySize = 2;
        diffuse.Format = DXGI_FORMAT_BC7_UNORM; diffuse.SampleDesc.Count = 1;
        diffuse.Usage = D3D11_USAGE_DEFAULT; diffuse.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture2D> texture;
        Check(SUCCEEDED(Device()->CreateTexture2D(&diffuse, nullptr, &texture)), "BC7 mipmapped texture array");
        desc = AsWrapResource(texture.Get())->dx12->GetDesc();
        const auto texture_bytes = device->GetResourceAllocationInfo(0, 1, &desc).SizeInBytes;
        Check(ResourceMemory().Snapshot().rows[1].bytes == texture_bytes, "texture mip/array allocation");
        D3D11_TEXTURE1D_DESC palette{};
        palette.Width = 256; palette.MipLevels = 1; palette.ArraySize = 1;
        palette.Format = DXGI_FORMAT_R8G8B8A8_UNORM; palette.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture1D> palette_texture;
        Check(SUCCEEDED(Device()->CreateTexture1D(&palette, nullptr, &palette_texture)), "palette texture");
        D3D11_TEXTURE3D_DESC volume{};
        volume.Width = 32; volume.Height = 32; volume.Depth = 32; volume.MipLevels = 1;
        volume.Format = DXGI_FORMAT_R8_UNORM; volume.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        ComPtr<ID3D11Texture3D> volume_texture;
        Check(SUCCEEDED(Device()->CreateTexture3D(&volume, nullptr, &volume_texture)), "volume texture");
        Check(ResourceMemory().Snapshot().rows[1].count == 3, "all texture dimensions counted");
        const auto all_texture_bytes = ResourceMemory().Snapshot().rows[1].bytes;
        auto peak = ResourceMemory().Snapshot();
        buffer.Reset(); texture.Reset(); palette_texture.Reset(); volume_texture.Reset();
        auto retired = ResourceMemory().Snapshot();
        Check(retired.rows[0].count == 0 && retired.rows[1].count == 0,
            "destroyed wrappers remain active");
        Check(retired.rows[2].bytes == expected && retired.rows[3].bytes == all_texture_bytes,
            "retirement must retain allocation bytes");
        peak.Max(retired);
        Check(peak.rows[0].bytes == expected && peak.rows[3].bytes == all_texture_bytes, "independent sampled peaks");
        UploadFrameTick();
        Check(ResourceMemory().Snapshot().rows[2].count == 1, "unsignalled retirement was released early");

        ComPtr<ID3D12CommandQueue> queue;
        D3D12_COMMAND_QUEUE_DESC qd{};
        Check(SUCCEEDED(device->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))), "WARP queue");
        ID3D12CommandQueue* queues[] = { queue.Get() };
        const auto deadline = GetTickCount64() + 5000;
        do {
            GpuProgressTick(device.Get(), queues, 1);
            UploadFrameTick();
            if (!ResourceMemory().Snapshot().rows[2].count) break;
            Sleep(1);
        } while (GetTickCount64() < deadline);
        Empty();

        diffuse.Usage = D3D11_USAGE_STAGING; diffuse.BindFlags = 0;
        diffuse.CPUAccessFlags = D3D11_CPU_ACCESS_READ; diffuse.MipLevels = 1; diffuse.ArraySize = 1;
        Check(SUCCEEDED(Device()->CreateTexture2D(&diffuse, nullptr, &texture)), "texture readback allocation");
        Check(ResourceMemory().Snapshot().rows[0].count == 1 && ResourceMemory().Snapshot().rows[1].count == 0,
            "staging texture must count as its backing DX12 buffer");
        texture.Reset();
        Empty();

        skeleton.Usage = D3D11_USAGE_STAGING; skeleton.BindFlags = 0;
        skeleton.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        Check(SUCCEEDED(Device()->CreateBuffer(&skeleton, nullptr, &buffer)), "readback buffer");
        buffer.Reset(); // The idle dump queue releases this buffer immediately.
        Empty();
        Check(DumpReadbackList() != nullptr, "open dump queue");
        Check(SUCCEEDED(Device()->CreateBuffer(&skeleton, nullptr, &buffer)), "deferred readback buffer");
        buffer.Reset();
        Check(ResourceMemory().Snapshot().rows[2].count == 1, "readback retirement missing");
        Check(DumpReadbackFlush(), "flush readback queue");
        Empty();

        // Start a report after sampling zero resources to check that it refreshes the baseline.
        PerfMonitor monitor;
        g_prof_enabled = true;
        monitor.FrameEnd();
        monitor.FrameEnd(); // Samples zero resources.
        Check(SUCCEEDED(Device()->CreateBuffer(&skeleton, nullptr, &buffer)), "report readback buffer");
        monitor.RequestReport();
        monitor.FrameEnd(); // Refresh the baseline before the next periodic sample.
        Check(monitor.ReportRunning(), "performance report did not start");
        buffer.Reset();
        const auto report_deadline = GetTickCount64() + 15000;
        while (monitor.ReportRunning() && GetTickCount64() < report_deadline) {
            Sleep(20);
            monitor.FrameEnd();
        }
        Check(!monitor.ReportRunning(), "performance report did not finish");
        std::ifstream report(monitor.LastReport());
        Check(report.good(), "performance report file missing");
        std::string line;
        bool allocation_section = false, buffer_row = false;
        while (std::getline(report, line)) {
            if (line.find("MOD RESOURCE ALLOCATIONS") != std::string::npos) allocation_section = true;
            if (allocation_section && line.find("Active buffers") != std::string::npos) {
                std::istringstream row(line);
                std::string active, buffers;
                char slash;
                double now_mb, start_mb, peak_mb;
                uint64_t now_count, start_count, peak_count;
                Check(bool(row >> active >> buffers >> now_mb >> slash >> now_count
                    >> start_mb >> slash >> start_count >> peak_mb >> slash >> peak_count), "report row format");
                Check(now_count == 0 && now_mb == 0 && start_count == 1 && peak_count == 1
                    && start_mb > 0 && start_mb == peak_mb, "report start/end/peak accounting");
                buffer_row = true;
            }
        }
        Check(buffer_row, "mod allocation report section missing");
        g_prof_enabled = false;
        SetDx12Bridge({});
        std::puts("PASS: concurrent counters, unknown sizes, WARP buffer/BC7 array allocations, GPU retirement, immediate/deferred readback, report start/end/peak");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
}
