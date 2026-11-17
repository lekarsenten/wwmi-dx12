// rt/rt_skin_gpu.cpp -- PTSKIN: our compute pipeline (RtPtSkin.cso, RtPalCopy.cso), the output buffers of
// each mod component and the copies of the game's bone palettes. PtSkinGpu itself is in rt_internal.h.
#include "rt_internal.h"

using namespace rt_portable;

namespace rt_portable {

PtSkinGpu s_gpu;

/**
 * Creates the PTSKIN root signature and pipeline once. Call with s_gpu.mutex held.
 *
 * @param device The game's device.
 * @return true when they exist.
 */
bool EnsurePtSkinPipeline(ID3D12Device* device)
{
    if (s_gpu.pso) return true;
    if (s_gpu.tried) return false;
    s_gpu.tried = true;
    D3D12_ROOT_PARAMETER params[9] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[0].Constants.ShaderRegister = 0; // b0: vertex count, bone count, scale
    params[0].Constants.Num32BitValues = 5;
    for (UINT t = 0; t < 5; ++t) { // t0 bones, t1 positions, t2 vectors, t3 weights, t4 colors
        params[1 + t].ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        params[1 + t].Descriptor.ShaderRegister = t;
    }
    for (UINT u = 0; u < 3; ++u) { // u0 positions out, u1 tangents out, u2 colors out
        params[6 + u].ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        params[6 + u].Descriptor.ShaderRegister = u;
    }
    D3D12_ROOT_SIGNATURE_DESC desc = {};
    desc.NumParameters = 9;
    desc.pParameters = params;
    Microsoft::WRL::ComPtr<ID3DBlob> blob;
    Microsoft::WRL::ComPtr<ID3DBlob> error;
    if (FAILED(D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error)) ||
        FAILED(device->CreateRootSignature(
            0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&s_gpu.root_signature)))) {
        logger::write("PTSKIN: root signature not created");
        return false;
    }
    const std::vector<uint8_t> code = WwmiReadFile((g_addon.cso_dir + L"\\RtPtSkin.cso").c_str());
    if (code.empty()) {
        logger::write("PTSKIN: RtPtSkin.cso missing in %S", g_addon.cso_dir.c_str());
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc = {};
    pso_desc.pRootSignature = s_gpu.root_signature.Get();
    pso_desc.CS.pShaderBytecode = code.data();
    pso_desc.CS.BytecodeLength = code.size();
    if (FAILED(device->CreateComputePipelineState(&pso_desc, IID_PPV_ARGS(&s_gpu.pso)))) {
        logger::write("PTSKIN: pipeline not created");
        return false;
    }
    const std::vector<uint8_t> copy_code = WwmiReadFile((g_addon.cso_dir + L"\\RtPalCopy.cso").c_str());
    if (copy_code.empty()) {
        logger::write("PTSKIN: RtPalCopy.cso missing in %S", g_addon.cso_dir.c_str());
        s_gpu.pso.Reset();
        return false;
    }
    D3D12_COMPUTE_PIPELINE_STATE_DESC copy_desc = {};
    copy_desc.pRootSignature = s_gpu.root_signature.Get();
    copy_desc.CS.pShaderBytecode = copy_code.data();
    copy_desc.CS.BytecodeLength = copy_code.size();
    if (FAILED(device->CreateComputePipelineState(&copy_desc, IID_PPV_ARGS(&s_gpu.copy_pso)))) {
        logger::write("PTSKIN: palette copy pipeline not created");
        s_gpu.pso.Reset();
        return false;
    }
    logger::write("PTSKIN: pipeline ready");
    return true;
}

constexpr uint32_t kPtSkinRetireFrames = 32; // a replaced buffer is kept this long: lists in flight may still read it
constexpr uint32_t kPtSkinIdleFrames = 600;  // buffers of a component not drawn for this long are released

/**
 * Releases the PTSKIN buffers that are no longer used, at most once per frame: replaced output buffers, and the
 * output buffers and palette copies of components that have not been drawn for a while (the character left,
 * or its level of detail changed). Call with s_gpu.mutex held, before taking any of them for a draw.
 */
void PurgePtSkinBuffers()
{
    if (s_gpu.purge_frame == g_frame_id) return;
    s_gpu.purge_frame = g_frame_id;
    for (size_t i = 0; i < s_gpu.retired.size();) {
        if (g_frame_id > s_gpu.retired[i].second + kPtSkinRetireFrames) {
            s_gpu.retired[i] = std::move(s_gpu.retired.back());
            s_gpu.retired.pop_back();
        } else {
            ++i;
        }
    }
    for (auto it = s_gpu.by_component.begin(); it != s_gpu.by_component.end();) {
        if (g_frame_id > it->second.last_frame + kPtSkinIdleFrames) {
            it = s_gpu.by_component.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = s_gpu.palettes.begin(); it != s_gpu.palettes.end();) {
        uint32_t newest_frame = 0;
        for (int i = 0; i < PtSkinGpu::kSlots; ++i) {
            if (it->second.frame[i] > newest_frame) newest_frame = it->second.frame[i];
        }
        if (g_frame_id > newest_frame + kPtSkinIdleFrames) {
            it = s_gpu.palettes.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * Gives a mod component's output buffers, large enough for the mod's vertices. Call with s_gpu.mutex held.
 *
 * @param device   The game's device.
 * @param key      The mod and component, as mod * 256 + component.
 * @param vertices The mod's vertex count.
 * @return The buffers, or nullptr when they could not be made.
 */
PtSkinGpu::Outputs* OutputsOf(ID3D12Device* device, int key, uint32_t vertices)
{
    PtSkinGpu::Outputs& outputs = s_gpu.by_component[key];
    if (outputs.vertices >= vertices && outputs.positions.res && outputs.tangents.res &&
        outputs.previous_positions.res && outputs.colors.res) {
        outputs.last_frame = g_frame_id;
        return &outputs;
    }
    if (outputs.positions.res) s_gpu.retired.push_back({ outputs.positions.res, g_frame_id });
    if (outputs.tangents.res) s_gpu.retired.push_back({ outputs.tangents.res, g_frame_id });
    if (outputs.previous_positions.res) s_gpu.retired.push_back({ outputs.previous_positions.res, g_frame_id });
    if (outputs.colors.res) s_gpu.retired.push_back({ outputs.colors.res, g_frame_id });
    outputs = PtSkinGpu::Outputs{};
    outputs.last_frame = g_frame_id;
    outputs.positions.res = WwmiMakeBuffer(device, (UINT64)vertices * kModPositionStride, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    outputs.tangents.res = WwmiMakeBuffer(device, (UINT64)vertices * kOutTangentStride, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    outputs.previous_positions.res = WwmiMakeBuffer(device, (UINT64)vertices * kModPositionStride,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    outputs.colors.res = WwmiMakeBuffer(device, (UINT64)vertices * kModColorStride, D3D12_HEAP_TYPE_DEFAULT,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
    if (!outputs.positions.res || !outputs.tangents.res || !outputs.previous_positions.res || !outputs.colors.res) {
        s_gpu.by_component.erase(key);
        return nullptr;
    }
    outputs.vertices = vertices;
    return &outputs;
}

/**
 * Puts the game's compute root signature and its arguments back after our dispatch on its list.
 *
 * @param cmd_list The game's command list.
 * @param cl       The same list, native.
 * @return false when the game had not bound a compute root signature on it yet (nothing to put back).
 */
bool RestoreGameComputeRoot(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl)
{
    const CmdGfxRoot& root = g_command_lists.CmpRoot(cmd_list);
    if (!root.layout) return false;
    cl->SetComputeRootSignature(reinterpret_cast<ID3D12RootSignature*>(root.layout));
    for (UINT p = 0; p < 64; ++p) {
        switch (root.kind[p]) {
        case CmdGfxRoot::kTable:
            cl->SetComputeRootDescriptorTable(p, D3D12_GPU_DESCRIPTOR_HANDLE{ root.value[p] });
            break;
        case CmdGfxRoot::kCbv:
            if (root.value[p]) cl->SetComputeRootConstantBufferView(p, root.value[p]);
            break;
        case CmdGfxRoot::kSrv:
            if (root.value[p]) cl->SetComputeRootShaderResourceView(p, root.value[p]);
            break;
        case CmdGfxRoot::kUav:
            if (root.value[p]) cl->SetComputeRootUnorderedAccessView(p, root.value[p]);
            break;
        case CmdGfxRoot::kNone:
            break;
        }
    }
    for (uint32_t i = 0; i < root.constant_count; ++i)
        cl->SetComputeRoot32BitConstant(root.constants[i].param, root.constants[i].value, root.constants[i].offset);
    return true;
}

/**
 * Names a section the same way every frame: its vertex count and palette size. Its output address is no good,
 * since the game writes each frame to the other of two rings.
 *
 * @param section The section.
 * @return The key.
 */
uint64_t PaletteKey(const SkinSection& section)
{
    return ((uint64_t)section.count << 32) | (section.bones_bytes & 0xFFFFFFFFull);
}

/**
 * Copies a section's bone palette into the add-on's buffers, on the game's compute list right before the game's
 * own skin dispatch for it. The copy made last time becomes the previous palette first. Puts the game's compute
 * pipeline and root arguments back afterwards; does nothing when they are not known.
 *
 * @param cmd_list The game's compute list.
 * @param state    Its bindings.
 * @param mod      The mod whose ring the section writes.
 * @param section  The section, with the game's palette address.
 */
void CopySectionPalette(reshade::api::command_list* cmd_list, const CmdState& state, int mod,
    const SkinSection& section)
{
    uint64_t bytes = section.bones_bytes;
    if (bytes > (uint64_t)kPtSkinMaxBones * kBoneBytes) bytes = (uint64_t)kPtSkinMaxBones * kBoneBytes;
    bytes -= bytes % kBoneBytes;
    if (bytes == 0 || !section.bones_va) return;
    if (!state.cmp_pso || !g_command_lists.CmpRoot(cmd_list).layout) {
        if (LogOnce(20, mod))
            logger::write("PTSKIN mod=%d: the game's compute pipeline is not known on this list, palette not copied",
                mod);
        return;
    }
    ID3D12GraphicsCommandList* cl = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (!cl || FAILED(cl->GetDevice(IID_PPV_ARGS(&device)))) return;

    std::lock_guard lock(s_gpu.mutex);
    if (!EnsurePtSkinPipeline(device.Get())) return;
    PtSkinGpu::Palette& palette = s_gpu.palettes[{ mod, PaletteKey(section) }];
    if (!palette.slot[0].res) {
        const UINT64 size = kPaletteBufferBytes;
        for (int i = 0; i < PtSkinGpu::kSlots; ++i) {
            palette.slot[i].res = WwmiMakeBuffer(device.Get(), size, D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            if (!palette.slot[i].res) {
                s_gpu.palettes.erase({ mod, PaletteKey(section) });
                return;
            }
        }
    }
    const int index = (int)(g_frame_id % PtSkinGpu::kSlots);
    Owned& slot = palette.slot[index];
    const uint64_t gen = g_command_lists.Gen(cmd_list);
    cl->SetComputeRootSignature(s_gpu.root_signature.Get());
    cl->SetPipelineState(s_gpu.copy_pso.Get());
    Xit(cl, gen, slot, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t constants[4] = { (uint32_t)(bytes / 4), 0, 0, 0 };
    cl->SetComputeRoot32BitConstants(0, 4, constants, 0);
    cl->SetComputeRootShaderResourceView(1, section.bones_va);
    cl->SetComputeRootUnorderedAccessView(6, slot.va());
    cl->Dispatch((uint32_t)((bytes / 4 + 63) / 64), 1, 1);
    // Back to COMMON, so the draw lists can read it whatever they assume at their start.
    Xit(cl, gen, slot, D3D12_RESOURCE_STATE_COMMON);
    palette.frame[index] = g_frame_id;
    palette.bytes[index] = (uint32_t)bytes;
    static std::atomic<int> s_trace_copies{ 0 };
    if (g_debug && s_trace_copies.fetch_add(1) < 64)
        logger::write("PTSKIN-TRACE copy mod=%d frame=%u out=%llx bones=%llx bytes=%llu slot=%d list_type=%d", mod,
            g_frame_id, (unsigned long long)section.out_va, (unsigned long long)section.bones_va,
            (unsigned long long)bytes, index, (int)cl->GetType());

    cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(state.cmp_pso));
    RestoreGameComputeRoot(cmd_list, cl);
}

/**
 * Makes a mod buffer readable by our compute pass, keeping it readable as a vertex buffer.
 *
 * @param cl  The game's command list.
 * @param res The mod buffer.
 */
void ReadyModBufferForSkin(ID3D12GraphicsCommandList* cl, ID3D12Resource* res)
{
    // The port tracks its own buffers' state per list; anything else is promoted from COMMON on first read.
    if (res && MigotoPort::OwnedBuffer(res))
        MigotoPort::OwnedTransition(
            cl, res, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// Debug only (SKINDUMP): each component's whole skinned position buffer, copied in one chosen frame per mod and
// saved to wwmi_dx12\rt_dump some frames later, so the components can be compared with each other offline.
struct SkinDump {
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer; // readback copy of the positions
    const uint8_t* data = nullptr;
    uint32_t bytes = 0;
    bool written = false;
};
constexpr uint32_t kSkinDumpDelayFrames = 600; // after the mod is first skinned
std::map<int, SkinDump> s_skin_dumps;          // by mod * 256 + component
std::map<int, uint32_t> s_skin_dump_frame;     // by mod: the frame its components are copied in

/**
 * Debug only: records a copy of a component's skinned positions when this is its mod's dump frame.
 * Call with s_gpu.mutex held and the positions readable as a copy source.
 *
 * @param device    The device, to make the readback buffer.
 * @param d3d12_cl  The command list the copy is recorded on.
 * @param dm        The mod.
 * @param key       mod * 256 + component.
 * @param positions The skinned positions (float3 per vertex).
 * @param vertices  How many vertices they hold.
 * @return true when a copy was recorded.
 */
bool RecordSkinDump(ID3D12Device* device, ID3D12GraphicsCommandList* d3d12_cl, int dm, int key,
    ID3D12Resource* positions, uint32_t vertices)
{
    const auto frame_found = s_skin_dump_frame.find(dm);
    if (frame_found == s_skin_dump_frame.end()) {
        s_skin_dump_frame[dm] = g_frame_id + kSkinDumpDelayFrames;
        return false;
    }
    if (g_frame_id != frame_found->second) return false;
    SkinDump& dump = s_skin_dumps[key];
    if (dump.buffer) return false;
    dump.bytes = vertices * kModPositionStride;
    dump.buffer = WwmiMakeBuffer(device, dump.bytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
        D3D12_RESOURCE_STATE_COPY_DEST);
    void* mapped = nullptr;
    if (!dump.buffer || FAILED(dump.buffer->Map(0, nullptr, &mapped))) return false;
    dump.data = static_cast<const uint8_t*>(mapped);
    d3d12_cl->CopyBufferRegion(dump.buffer.Get(), 0, positions, 0, dump.bytes);
    return true;
}

/**
 * Debug only: saves the copies RecordSkinDump made, once the GPU is surely done with them (10 frames later),
 * as wwmi_dx12\rt_dump\skin_mod<mod>_comp<component>.bin. Call with s_gpu.mutex held.
 */
void WriteSkinDumps()
{
    for (auto& entry : s_skin_dumps) {
        SkinDump& dump = entry.second;
        if (dump.written || !dump.data) continue;
        const int dm = entry.first / 256;
        const auto frame_found = s_skin_dump_frame.find(dm);
        if (frame_found == s_skin_dump_frame.end() || g_frame_id < frame_found->second + 10) continue;
        dump.written = true;
        const std::wstring folder = g_addon.data_dir + L"\\rt_dump\\";
        CreateDirectoryW(g_addon.data_dir.c_str(), nullptr);
        CreateDirectoryW(folder.c_str(), nullptr);
        wchar_t name[64];
        swprintf_s(name, L"skin_mod%d_comp%d.bin", dm, entry.first % 256);
        const HANDLE file = CreateFileW((folder + name).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD written = 0;
        if (file != INVALID_HANDLE_VALUE) {
            WriteFile(file, dump.data, dump.bytes, &written, nullptr);
            CloseHandle(file);
        }
        logger::write("SKINDUMP mod=%d comp=%d: %u of %u bytes of skinned positions of frame %u saved", dm,
            entry.first % 256, (uint32_t)written, dump.bytes, frame_found->second);
    }
}

/**
 * Debug only: logs the readback recorded by SkinDraw once the GPU is surely done with it (10 frames later):
 * the first two bones of the palette and the first four skinned positions. Call with s_gpu.mutex held.
 */
void LogPtSkinReadback()
{
    if (!s_gpu.readback_frame || g_frame_id < s_gpu.readback_frame + 10) return;
    float values[24 + 48 + 24];
    memcpy(values, s_gpu.readback_data, sizeof(values));
    std::string samples;
    for (uint32_t i = 0; i < 16; ++i) {
        char part[96];
        snprintf(part, sizeof(part), " v%u (%.2f %.2f %.2f)", (uint32_t)((uint64_t)i * s_gpu.readback_vertices / 16),
            values[24 + i * 3], values[25 + i * 3], values[26 + i * 3]);
        samples += part;
    }
    logger::write("PTSKIN-READBACK mod=%d comp=%d frame=%u (palette of frame %u): bone0 [%.3f %.3f %.3f %.3f | %.3f %.3f "
                  "%.3f %.3f | %.3f %.3f %.3f %.3f] bone1 [%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f "
                  "%.3f] skinned:%s",
        s_gpu.readback_key / 256, s_gpu.readback_key % 256, s_gpu.readback_frame, s_gpu.readback_slot_frame,
        values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7], values[8], values[9],
        values[10], values[11], values[12], values[13], values[14], values[15], values[16], values[17], values[18],
        values[19], values[20], values[21], values[22], values[23], samples.c_str());
    // Where the two bones are (the last column of their rows): the skeleton skinned with, and the game's section
    // palette of the same moment. A part that stands still shows here as bones that do not change.
    logger::write("PTSKIN-BONES mod=%d comp=%d frame=%u: skinned with bones %u and %u at (%.2f %.2f %.2f) (%.2f %.2f "
                  "%.2f); section palette of frame %u: bones 0 and 1 at (%.2f %.2f %.2f) (%.2f %.2f %.2f)",
        s_gpu.readback_key / 256, s_gpu.readback_key % 256, s_gpu.readback_frame, s_gpu.readback_first_bone,
        s_gpu.readback_first_bone + 1, values[3], values[7], values[11], values[15], values[19], values[23],
        s_gpu.readback_palette_frame, values[72 + 3], values[72 + 7], values[72 + 11], values[72 + 15],
        values[72 + 19], values[72 + 23]);
    s_gpu.readback_frame = 0;
    s_gpu.readback_done += 1;
    s_gpu.readback_next_frame = g_frame_id + 2;
}

bool IsSkinDumpFrame(int mod)
{
    const auto dump_frame = s_skin_dump_frame.find(mod);
    return dump_frame == s_skin_dump_frame.end() || dump_frame->second == g_frame_id;
}

void SkinGpuOnDestroyDevice()
{
    std::lock_guard lock(s_gpu.mutex);
    s_gpu.root_signature.Reset();
    s_gpu.pso.Reset();
    s_gpu.copy_pso.Reset();
    s_gpu.tried = false;
    s_gpu.by_component.clear();
    s_gpu.palettes.clear();
    s_gpu.retired.clear();
    s_gpu.readback.Reset();
    s_gpu.readback_data = nullptr;
    s_gpu.readback_frame = 0;
    s_skin_dumps.clear();
    s_skin_dump_frame.clear();
}

} // namespace rt_portable
