// skeleton_dump.cpp -- see skeleton_dump.h.
#include "../addon_common.h"
#include "skeleton_dump.h"
#include "component_table.h"
#include "game_cb_reader.h"
#include "../core/d3d12_buffer.h"
#include "../core/d3d12_barrier.h"
#include "../instrumentation/diag_arms.h"
#include "../migoto/dx12_bridge.h"
#include "../migoto/port_api.h"

SkeletonDump g_skeleton_dump;

void SkeletonDump::NoteDrawCbs(
    ID3D12Resource* cb4_res, uint64_t cb4_va, int cb4_root, ID3D12Resource* cb3_res, uint64_t cb3_va, int cb3_root)
{
    cb4_res_ = cb4_res;
    cb4_va_ = cb4_va;
    cb3_res_ = cb3_res;
    cb3_va_ = cb3_va;
    cb4_root_ = cb4_root;
    cb3_root_ = cb3_root;
}

void SkeletonDump::ChainTick(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* q)
{
    // ---- phase 2 ----
    if (!chain_readbacks_.empty() && g_frame_id >= chain_ready_frame_) {
        logger::write("=== SKELETON CHAIN DUMP (frame %u) ===", g_frame_id);
        for (auto& e : chain_readbacks_) {
            void* map = nullptr;
            D3D12_RANGE rr{ 0, (SIZE_T)kBytes };
            if (SUCCEEDED(e.second->Map(0, &rr, &map)) && map) {
                const float* f = (const float*)map;
                int nz = 0;
                for (int k = 0; k < 64 * 4; ++k)
                    if (f[k] != 0.0f) ++nz;
                logger::write("  %-46ls nonzero=%3d/256", e.first.c_str(), nz);
                for (int row = 0; row < 3; ++row)
                    logger::write("      [%8.3f %8.3f %8.3f %8.3f]", f[row * 4 + 0], f[row * 4 + 1], f[row * 4 + 2],
                        f[row * 4 + 3]);
                D3D12_RANGE none{ 0, 0 };
                e.second->Unmap(0, &none);
            }
            e.second->Release();
        }
        chain_readbacks_.clear();

        // ---- the variables that steer all of it ----
        logger::write("  --- engine variables (skeleton chain) ---");
        PortApi::ForEachVariable(
            [](const wchar_t* fq, float v, bool persist, void*) {
                if (!fq) return;
                std::wstring n(fq);
                for (auto& c : n) c = (wchar_t)towlower(c);
                static const wchar_t* kWanted[] = { L"state_id", L"vg_offset", L"vg_count", L"mod_enabled",
                    L"object_detected", L"mod_id", L"custom_mesh_scale", L"merge_status", L"blend_remaps_initialized" };
                for (const wchar_t* w : kWanted)
                    if (n.find(w) != std::wstring::npos) {
                        logger::write("    %-58ls = %g%s", fq, v, persist ? "  (persist)" : "");
                        return;
                    }
            },
            nullptr);
        logger::write("=== END SKELETON CHAIN DUMP ===");
        return;
    }
    if (!chain_readbacks_.empty()) return; // recorded, waiting
    if (!g_diag_arms.skelchain_req.exchange(false) || !dev || !cl || !q) return;

    // ---- phase 1: collect every source, then record one copy each ----
    std::vector<ChainSrc> srcs;
    if (cb4_res_) srcs.push_back({ L"INPUT  game vs-cb4 (merger cs-cb8)", cb4_res_, cb4_va_ });
    if (cb3_res_) srcs.push_back({ L"INPUT  game vs-cb3", cb3_res_, cb3_va_ });
    struct Acc {
        std::vector<ChainSrc>* v;
    } acc{ &srcs };
    PortApi::ForEachCustomResource(
        [](const wchar_t* fq, const wchar_t*, int, bool, ID3D12Resource* dx12, void* u) {
            if (!fq || !dx12) return;
            std::wstring n(fq);
            for (auto& c : n) c = (wchar_t)towlower(c);
            if (n.find(L"skeleton") == std::wstring::npos && n.find(L"blendremap") == std::wstring::npos) return;
            ((Acc*)u)->v->push_back({ fq, dx12, 0 });
        },
        &acc);

    if (srcs.empty()) {
        logger::write("SKELCHAIN: nothing to dump (no bone CB seen, no skeleton resources)");
        return;
    }

    logger::write("SKELCHAIN: recording %zu source(s); cb4root=%d cb3root=%d; reading at frame %u", srcs.size(),
        cb4_root_, cb3_root_, g_frame_id + 4);
    for (ChainSrc& c : srcs) {
        bool is_game_cb = false;
        {
            const bool is_cb = (c.res == cb4_res_ || c.res == cb3_res_);
            if (is_cb) {
                D3D12_HEAP_PROPERTIES chp{};
                D3D12_HEAP_FLAGS chf{};
                if (SUCCEEDED(c.res->GetHeapProperties(&chp, &chf)) && chp.Type == D3D12_HEAP_TYPE_UPLOAD)
                    is_game_cb = true; // GENERIC_READ: legal copy source, no barrier
                else {
                    logger::write("  SKELCHAIN skip '%ls' -- game CB is not on an UPLOAD heap and has no "
                                  "tracked state; a guessed barrier can remove the device",
                        c.label.c_str());
                    continue;
                }
            }
        }
        D3D12_RESOURCE_STATES st{};
        if (!is_game_cb && !CopySourceState(MigotoPort::QueryResourceState(c.res), &st)) {
            logger::write("  SKELCHAIN skip '%ls' -- no tracked state (never guess a barrier)", c.label.c_str());
            continue;
        }
        const D3D12_RESOURCE_DESC rd = c.res->GetDesc();
        const UINT64 base = c.res->GetGPUVirtualAddress();
        UINT64 off = 0;
        if (c.off && c.off >= base && c.off < base + rd.Width) off = c.off - base; // VA -> byte offset
        if (off + kBytes > rd.Width) continue;

        ID3D12Resource* rb = nullptr;
        if (FAILED(CreateD3D12Buffer(dev, D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST, &rb)))
            continue;

        const auto copy = [&] { cl->CopyBufferRegion(rb, 0, c.res, off, kBytes); };
        if (is_game_cb)
            copy(); // UPLOAD heap, GENERIC_READ: a legal copy source as-is
        else
            CopyAsSource(cl, c.res, st, copy);
        chain_readbacks_.emplace_back(c.label, rb);
    }
    chain_ready_frame_ = g_frame_id + 4;
}

void SkeletonDump::MergedTick(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* q)
{
    // ---- phase 2: the copy has been submitted and retired; read it ----
    if (merged_readback_ && g_frame_id >= merged_ready_frame_) {
        void* map = nullptr;
        D3D12_RANGE rr{ 0, (SIZE_T)kBytes };
        if (SUCCEEDED(merged_readback_->Map(0, &rr, &map)) && map) {
            const float* f = (const float*)map;
            int nonzero = 0;
            for (int k = 0; k < 64 * 4; ++k)
                if (f[k] != 0.0f) ++nonzero;
            logger::write("SKELDUMP '%ls' -- first 6 bones (float3x4 rows):", merged_fq_.c_str());
            for (int bone = 0; bone < 6; ++bone) {
                const float* r = f + bone * 12;
                logger::write("  bone %2d: [%8.3f %8.3f %8.3f %8.3f] [%8.3f %8.3f %8.3f %8.3f] "
                              "[%8.3f %8.3f %8.3f %8.3f]",
                    bone, r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11]);
            }
            logger::write(
                "SKELDUMP: %d of %d floats non-zero%s", nonzero, 64 * 4, nonzero == 0 ? "  *** ALL ZERO ***" : "");
            D3D12_RANGE none{ 0, 0 };
            merged_readback_->Unmap(0, &none);
        }
        merged_readback_->Release();
        merged_readback_ = nullptr;
        return;
    }
    if (merged_readback_) return; // recorded, still waiting
    if (!g_diag_arms.skeldump_req.exchange(false) || !dev || !cl || !q) return;

    // ---- phase 1: find it and record the copy ----
    struct Ctx {
        std::wstring fq;
        ID3D12Resource* res;
    } ctx{ std::wstring(), nullptr };
    PortApi::ForEachCustomResource(
        [](const wchar_t* fq, const wchar_t*, int, bool, ID3D12Resource* dx12, void* u) {
            Ctx* c = (Ctx*)u;
            if (c->res || !dx12 || !fq) return;
            std::wstring n(fq);
            for (auto& ch : n) ch = (wchar_t)towlower(ch);
            if (n.find(L"mergedskeletonrw") != std::wstring::npos && n.find(L"extra") == std::wstring::npos &&
                n.find(L"remap") == std::wstring::npos) {
                c->fq = fq;
                c->res = dx12;
            }
        },
        &ctx);
    if (!ctx.res) {
        logger::write("SKELDUMP: no substantiated ...MergedSkeletonRW found");
        return;
    }

    if (FAILED(CreateD3D12Buffer(
            dev, D3D12_HEAP_TYPE_READBACK, kBytes, D3D12_RESOURCE_STATE_COPY_DEST, &merged_readback_))) {
        logger::write("SKELDUMP: readback alloc failed");
        merged_readback_ = nullptr;
        return;
    }

    D3D12_RESOURCE_STATES before{};
    if (!CopySourceState(MigotoPort::QueryResourceState(ctx.res), &before)) {
        logger::write("SKELDUMP: '%ls' has no tracked resource state -- skipping rather than guessing "
                      "(a wrong StateBefore removes the device)",
            ctx.fq.c_str());
        merged_readback_->Release();
        merged_readback_ = nullptr;
        return;
    }
    CopyAsSource(cl, ctx.res, before, [&] { cl->CopyBufferRegion(merged_readback_, 0, ctx.res, 0, kBytes); });
    merged_fq_ = ctx.fq;
    merged_ready_frame_ = g_frame_id + 4; // well past this frame's submit + any in-flight depth
    logger::write("SKELDUMP: copy recorded for '%ls' (tracked state 0x%x); reading at frame %u", merged_fq_.c_str(),
        (unsigned)before, merged_ready_frame_);
}
