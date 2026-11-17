// rt/rt_vertex_fetch.cpp -- PTSKIN: the buffers the pass-through vertex shader loads by vertex id (tangents,
// UVs, color, previous position) are pointed at the mod's, with the LocalVF constants that index them.
#include "rt_internal.h"

using namespace rt_portable;

namespace {

/** Copies a layout's vertex-stage register maps out of the registry (LayoutRegistry::WithRegMap's callback). */
struct CopyVsRegMap {
    RegBind* srvs;
    RegBind* cbs;

    /**
     * @param map The layout's register map.
     */
    void operator()(const LayoutRegMap& map) const
    {
        memcpy(srvs, map.t_vs, sizeof(map.t_vs));
        memcpy(cbs, map.b_vs, sizeof(map.b_vs));
    }
};

/**
 * Tells whether 24 bytes read from a vertex-stage constant buffer are the game's LocalVF constants and not
 * another buffer bound at the same register (the shadow passes keep their pass buffer at vs-b2).
 *
 * @param values The first six 32-bit values of the buffer.
 * @return true when they read as { color index mask, UV set count, light map UV index, first vertex, ... }.
 */
bool LooksLikeLocalVf(const int32_t* values)
{
    const bool color_mask_fits = values[0] >= -1 && values[0] <= 255;
    const bool uv_set_count_fits = values[1] >= 1 && values[1] <= 8;
    const bool light_map_index_fits = values[2] >= -1 && values[2] <= 8;
    const bool first_vertex_fits = values[3] >= 0 && values[3] < 0x1000000;
    return color_mask_fits && uv_set_count_fits && light_map_index_fits && first_vertex_fits;
}

} // namespace

/**
 * Points the buffer SRVs the pass-through vertex shader loads by vertex id at the mod's buffers, and gives the draw
 * a copy of the game's LocalVF constants (vs-b2, vs-b3 in the shadow passes) that indexes them from vertex 0 with
 * four UV sets. Which register
 * holds what differs per shader, so each is told by the format of the view the game has bound there.
 *
 * @param cmd_list The game's command list (ReShade's).
 * @param cl       The same list.
 * @param fv       The mod's buffers.
 * @param dm       The mod, for the log.
 * @param comp     The component, for the log.
 * @return What to put back after the draw: the game's LocalVF and whether a shadow bind is needed.
 */
VfRestore RtDrawPass::StampVertexFetch(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl,
    const PtSkinPass::FetchViews& fv, int dm, int comp)
{
    VfRestore restore;
    if (g_settings.ptskin_test == 2 || !g_addon.d3d12_device) return restore;
    const CmdState& state = g_command_lists.Gfx(cmd_list);
    const uint64_t layout = g_command_lists.GfxLayout(cmd_list);
    RegBind srvs[RegisterFileWidth(TargetClass::ShaderResource)];
    RegBind cbs[RegisterFileWidth(TargetClass::ConstantBuffer)];
    const CopyVsRegMap copy{ srvs, cbs };
    if (!g_layouts.WithRegMap(layout, copy)) return restore;
    const DxilSrvRegs reads = g_pipelines.SrvRegs(state.gfx_pso, 'v');
    reshade::api::device* device = cmd_list->get_device();
    const int log_key = (int)(((state.gfx_pso ^ (state.gfx_pso >> 32)) * 31u) & 0x7fffffff);
    const bool log = LogOnce(15, log_key);
    std::string report;

    std::lock_guard lock(fetch_mutex_);
    if (!fetch_heap_) {
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc = {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        heap_desc.NumDescriptors = kFetchRing;
        if (FAILED(g_addon.d3d12_device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&fetch_heap_)))) return restore;
    }
    const UINT increment = g_addon.d3d12_device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    std::vector<ShadowSlotOvr> slots;
    const unsigned register_count = (unsigned)RegisterFileWidth(TargetClass::ShaderResource);
    for (unsigned reg = 0; reg < register_count && reg < 64; ++reg) {
        if (reads.parsed && !reads.has(reg)) continue;
        const RegBind& bind = srvs[reg];
        if (bind.param >= 64 || !bind.is_table) continue;
        const TableStart table = TableStartOf(device, state.root[bind.param].table);
        if (!table) continue;
        ResInfo info = {};
        if (!g_descriptors.Find({ table.heap, table.base + bind.offset }, info) || !info.is_buffer || !info.view_handle)
            continue;
        const reshade::api::resource_view_desc view = device->get_resource_view_desc({ info.view_handle });

        D3D12_SHADER_RESOURCE_VIEW_DESC desc = {};
        desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        ID3D12Resource* resource = nullptr;
        const char* role = nullptr;
        switch (view.format) {
        case reshade::api::format::r16g16b16a16_snorm: // two per vertex: TangentX, TangentZ
            resource = fv.tan;
            desc.Format = DXGI_FORMAT_R16G16B16A16_SNORM;
            desc.Buffer.NumElements = fv.vcount * 2;
            role = "tangents";
            break;
        case reshade::api::format::r16g16_float: // four UV sets per vertex
            resource = fv.uv;
            desc.Format = DXGI_FORMAT_R16G16_FLOAT;
            desc.Buffer.FirstElement = fv.uv_first;
            desc.Buffer.NumElements = fv.uv_elems;
            role = "UVs";
            break;
        case reshade::api::format::r32_float: // three per vertex: the previous position
            resource = fv.prev;
            desc.Format = DXGI_FORMAT_R32_FLOAT;
            desc.Buffer.NumElements = fv.vcount * 3;
            role = "previous positions";
            break;
        case reshade::api::format::r8g8b8a8_unorm: // the mod's colors are the game's bytes: read them the game's way
        case reshade::api::format::b8g8r8a8_unorm:
            resource = fv.color;
            desc.Format = (DXGI_FORMAT)view.format;
            desc.Buffer.FirstElement = fv.color_first;
            desc.Buffer.NumElements = fv.vcount;
            role = "colors";
            break;
        default: break;
        }
        if (log) {
            char part[96];
            snprintf(part, sizeof(part), " t%u=format %u (%s)", reg, (unsigned)view.format, role ? role : "left alone");
            report += part;
        }
        if (!resource) continue;
        if (fetch_next_ >= kFetchRing) fetch_next_ = 0;
        D3D12_CPU_DESCRIPTOR_HANDLE handle = fetch_heap_->GetCPUDescriptorHandleForHeapStart();
        handle.ptr += (SIZE_T)fetch_next_++ * increment;
        g_addon.d3d12_device->CreateShaderResourceView(resource, &desc, handle);
        slots.push_back(ShadowSlotOvr{ bind.param, bind.offset, handle });
    }
    if (!slots.empty()) {
        g_rfx_bind.AddExtraSlots(slots.data(), (unsigned)slots.size());
        restore.fetch = true;
    }

    // LocalVF: int4 { color index mask, UV set count, light map UV index, first vertex }. It is vs-b2 in the
    // color passes and vs-b3 in the shadow passes, where vs-b2 is the pass's own buffer and must stay.
    int32_t local_vf[8] = {};
    unsigned local_vf_register = 0;
    bool local_vf_found = false;
    for (unsigned candidate_register = 2; candidate_register <= 3 && !local_vf_found; ++candidate_register) {
        const RegBind& candidate = cbs[candidate_register];
        int32_t values[8] = {};
        if (candidate.param >= 64 || candidate.is_table) continue;
        if (!g_game_cbs.ReadRootCbv(cmd_list, candidate.param, values, 24)) continue;
        if (candidate_register == 2) memcpy(local_vf, values, sizeof(local_vf)); // logged when neither fits
        if (!LooksLikeLocalVf(values)) continue;
        memcpy(local_vf, values, sizeof(local_vf));
        local_vf_register = candidate_register;
        local_vf_found = true;
    }
    const RegBind& cb = cbs[local_vf_found ? local_vf_register : 2];
    bool cb_copied = false;
    if (!slots.empty() && local_vf_found) {
        int32_t ours[8];
        memcpy(ours, local_vf, sizeof(ours));
        ours[1] = 4; // the mod's vb2 holds four UV sets per vertex
        ours[3] = 0; // our buffers start at the mod's vertex 0
        if (lvf_ring_.Ensure(g_addon.d3d12_device, 256 * 1024)) {
            uint64_t va = 0;
            uint8_t* slot = lvf_ring_.Take(&va);
            if (slot) {
                memset(slot, 0, CbRing::kSlot);
                memcpy(slot, ours, 24);
                cl->SetGraphicsRootConstantBufferView(cb.param, va);
                restore.param = cb.param;
                restore.va = g_command_lists.Cbv(cmd_list).binds[cb.param].va;
                cb_copied = true;
            }
        }
    }
    if (log && g_debug) {
        // Debug (PTSKIN-MAT): the game's material constants of this draw (vs-b4), saved to study the vertex
        // offset the game's vertex shader takes from them.
        const RegBind& material = cbs[4];
        uint8_t bytes[3072] = {};
        RootCbvBind material_bind;
        if (material.param < 64 && !material.is_table) material_bind = g_command_lists.Cbv(cmd_list).binds[material.param];
        const uint32_t material_read = material_bind.res
            ? g_game_cbs.Snapshot(material_bind.res, material_bind.va, bytes, sizeof(bytes), true) : 0;
        const bool readable = material_read > 0;
        uint32_t non_zero = 0;
        for (size_t i = 0; i < sizeof(bytes); ++i) non_zero += bytes[i] != 0;
        if (readable) {
            const std::wstring folder = g_addon.data_dir + L"\\rt_dump\\";
            wchar_t name[80];
            swprintf_s(name, L"mat_mod%d_comp%d_pso%llx.bin", dm, comp, (unsigned long long)state.gfx_pso);
            const HANDLE file = CreateFileW((folder + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                FILE_ATTRIBUTE_NORMAL, nullptr);
            if (file != INVALID_HANDLE_VALUE) {
                DWORD written = 0;
                WriteFile(file, bytes, sizeof(bytes), &written, nullptr);
                CloseHandle(file);
            }
        }
        logger::write("PTSKIN-MAT mod=%d comp=%d pso=%llx: vs-b4 root %u%s va %llx -> %s, %u non-zero bytes", dm, comp,
            (unsigned long long)state.gfx_pso, (unsigned)material.param, material.is_table ? " (in a table)" : "",
            (unsigned long long)material_bind.va,
            readable ? "read and saved" : "not readable", non_zero);
    }
    if (log)
        logger::write("PTSKIN-FETCH mod=%d comp=%d pso=%llx:%s | LocalVF (%s, root %u%s) = %d %d %d %d %d %d -> %s", dm,
            comp, (unsigned long long)state.gfx_pso,
            report.empty() ? " no buffer SRV in the vertex stage" : report.c_str(),
            !local_vf_found ? "not found, vs-b2 shown" : local_vf_register == 2 ? "vs-b2" : "vs-b3", (unsigned)cb.param,
            cb.is_table ? ", in a table" : "", local_vf[0], local_vf[1], local_vf[2], local_vf[3], local_vf[4],
            local_vf[5], cb_copied ? "replaced with UV sets 4, first vertex 0" : "left as it is");
    return restore;
}

void RtDrawPass::OnDestroyDevice()
{
    std::lock_guard lock(fetch_mutex_);
    fetch_heap_.Reset();
    fetch_next_ = 0;
    lvf_ring_.Reset();
}
