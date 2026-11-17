// rt/rt_skin_draw.cpp -- PTSKIN: a mod's own vertices, skinned at the game's pass-through draw with the
// bones of the section the draw stands for, and the palette a merged-skeleton mod is handed.
#include "rt_internal.h"

using namespace rt_portable;

namespace {

/**
 * The result of a pass-through draw whose mod could not be skinned (no section or palette yet, as in the first
 * frames after a character switch): the draw must not run, since the mod's bind-pose buffers do not fit the layout.
 *
 * @return The result that tells the caller to draw nothing.
 */
VfRestore CannotSkin()
{
    VfRestore result;
    result.skip = true;
    return result;
}

} // namespace

VfRestore RtDrawPass::SkinDraw(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* d3d12_cl, int dm,
    const WwmiMod::Component* wc, uint32_t, uint32_t, uint32_t first_index, uint32_t index_count, int32_t vertex_offset,
    uint64_t vs_hash, uint64_t, int, bool)
{
    if (!wc || !d3d12_cl || dm < 0) return {};
    const CmdState& state = g_command_lists.Gfx(cmd_list);
    if (!IsPassThroughPso(state.gfx_pso)) return {};
    const int comp = wc->index;
    const int once_key = dm * 256 + (comp & 0xFF); // the mod and component, for the log and the output buffers
    if (g_debug) { // which vertex shaders draw each component (PTSKIN-VS), once per component and shader
        static std::mutex s_vs_seen_mutex;
        static std::set<std::pair<int, uint64_t>> s_vs_seen;
        bool is_new = false;
        {
            std::lock_guard<std::mutex> lock(s_vs_seen_mutex);
            is_new = s_vs_seen.size() < 400 && s_vs_seen.insert({ once_key, vs_hash }).second;
        }
        if (is_new)
            logger::write("PTSKIN-VS mod=%d comp=%d vs=%016llx pso=%llx first_index=%u index_count=%u frame=%u", dm,
                comp, (unsigned long long)vs_hash, (unsigned long long)state.gfx_pso, first_index, index_count,
                g_frame_id);
    }

    const MigotoPort::PipeState& port = MigotoPort::State();
    const MigotoPort::ShimVbRecord& positions = port.shim_vb[0];
    const MigotoPort::ShimVbRecord& vectors = port.shim_vb[1];
    const MigotoPort::ShimVbRecord& texcoords = port.shim_vb[2];
    const MigotoPort::ShimVbRecord& colors = port.shim_vb[3];
    const MigotoPort::ShimVbRecord& blend = port.shim_vb[4];
    if (positions.stride != kModPositionStride || vectors.stride != kModVectorStride ||
        texcoords.stride != kModTexcoordStride || colors.stride != kModColorStride ||
        (blend.stride != kModBlendStride && blend.stride != kModBlendStrideFour) ||
        !positions.va || !vectors.va || !texcoords.va || !colors.va || !blend.va || port.layout_override.count) {
        s_ptskin_skipped.fetch_add(1, std::memory_order_relaxed);
        if (LogOnce(10, once_key))
            logger::write("PTSKIN mod=%d comp=%d: not the WWMI vertex layout (vb0..vb4 strides %u/%u/%u/%u/%u, layout "
                          "override %u) -- drawn as it is",
                dm, comp, positions.stride, vectors.stride, texcoords.stride, colors.stride, blend.stride,
                (unsigned)port.layout_override.count);
        return {};
    }
    // Mods' buffers can differ by a few vertices (padding); skin as many as every one of them holds.
    uint32_t vertices = positions.size / kModPositionStride;
    vertices = (std::min)(vertices, (uint32_t)(vectors.size / kModVectorStride));
    vertices = (std::min)(vertices, (uint32_t)(blend.size / blend.stride));
    vertices = (std::min)(vertices, (uint32_t)(colors.size / kModColorStride));
    if (vertices == 0) {
        s_ptskin_skipped.fetch_add(1, std::memory_order_relaxed);
        if (LogOnce(11, once_key))
            logger::write("PTSKIN mod=%d comp=%d: buffer sizes do not agree (%u vertices, vb1 %u, vb4 %u bytes)", dm,
                comp, vertices, vectors.size, blend.size);
        return {};
    }
    // The game's own draw binds this mesh's whole skinned output at vb0; its sections lie inside, in order.
    const uint64_t window_va = state.vb[0].va + (int64_t)vertex_offset * kModPositionStride;
    const uint64_t window_end = state.vb[0].va + state.vb[0].size;
    SkinSection section;
    size_t section_total = 0;
    if (!state.vb[0].va || !SectionInWindow(dm, comp, window_va, window_end, &section, &section_total)) {
        // The game's dispatches into this buffer are recorded only once it is known as the mod's. A BLAS build
        // tells that some frames after a character appears; this draw tells it now.
        if (state.vb[0].va) g_rt_rings.Note(state.vb[0].va, state.vb[0].size, dm);
        static std::atomic<int> s_skip_logs{ 0 };
        if (g_debug && comp == 0 && s_skip_logs.fetch_add(1) < 80)
            logger::write("PTSKIN-SKIP mod=%d frame=%u: not drawn, no section yet in vb0 %llx (%zu there)", dm, g_frame_id,
                (unsigned long long)state.vb[0].va, section_total);
        static std::atomic<int> s_broken_logs{ 0 };
        if (g_debug && comp == 0 && section_total > 0 && s_broken_logs.fetch_add(1) < 40)
            logger::write("PTSKIN-CHAIN mod=%d comp=%d frame=%u: %zu section(s) in vb0 %llx but none leads to this "
                          "component from the start -- not drawn rather than skinned with another section's bones",
                dm, comp, g_frame_id, section_total, (unsigned long long)state.vb[0].va);
        s_ptskin_skipped.fetch_add(1, std::memory_order_relaxed);
        if (LogOnce(12, once_key))
            logger::write("PTSKIN mod=%d comp=%d: no section %d among the %zu recorded in the game's vb0 %llx..%llx "
                          "(base vertex %d)",
                dm, comp, comp, section_total, (unsigned long long)window_va, (unsigned long long)window_end,
                vertex_offset);
        return CannotSkin();
    }
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    if (FAILED(d3d12_cl->GetDevice(IID_PPV_ARGS(&device)))) return {};
    std::lock_guard lock(s_gpu.mutex);
    if (!EnsurePtSkinPipeline(device.Get())) return {};
    PurgePtSkinBuffers();
    // The bones to skin with. A mod that keeps the game's skeletons apart uses its section's palette, as copied
    // at the game's skin dispatch. A mod that merges them numbers its bones over the merged skeleton, which its
    // own commands build (from the palettes PalPublish hands them) and bind at vs-cb4.
    const bool merged_skeleton = wc->vg_count > 0;
    uint64_t bones_va = 0;
    uint32_t bone_count = 0;
    Owned* current_slot = nullptr; // the palette copy the bones are in; none for a merged skeleton
    Owned* previous_slot = nullptr; // the palette copy the previous frame's bones are in; none for a merged skeleton
    uint64_t previous_bones_va = 0; // the previous frame's bones; 0 = not known, no motion
    uint32_t previous_bone_count = 0;
    int current_index = -1;
    int previous_index = -1;
    uint32_t current_frame = 0;
    uint32_t previous_frame = 0;
    MigotoPort::ShimBufView skeleton; // the mod's merged skeleton, when that is what the bones come from
    if (merged_skeleton) {
        const char* why = nullptr;
        void* bound = MigotoPort::AcquireShimVsCb(4);
        const bool is_the_mods = bound && !MigotoPort::ShimObjectForeign(bound);
        MigotoPort::ReleaseShimObject(bound);
        if (!is_the_mods || !MigotoPort::ShimBufferForCompute(d3d12_cl, 'c', 4, &skeleton, &why) || !skeleton.va) {
            s_ptskin_skipped.fetch_add(1, std::memory_order_relaxed);
            static std::atomic<int> s_no_skeleton_logs{ 0 };
            if (g_debug && s_no_skeleton_logs.fetch_add(1) < 40)
                logger::write("PTSKIN-MERGED mod=%d comp=%d frame=%u: the mod bound no merged skeleton at vs-cb4 "
                              "(%s) -- not drawn",
                    dm, comp, g_frame_id,
                    !bound ? "nothing there" : !is_the_mods ? "the game's buffer" : why ? why : "?");
            return CannotSkin();
        }
        bones_va = skeleton.va;
        bone_count = skeleton.size / kBoneBytes;
        // The mod's second merged skeleton, bound at vs-cb3, holds the previous frame's bones.
        void* previous_bound = MigotoPort::AcquireShimVsCb(3);
        const bool previous_is_the_mods = previous_bound && !MigotoPort::ShimObjectForeign(previous_bound);
        MigotoPort::ReleaseShimObject(previous_bound);
        MigotoPort::ShimBufView previous_skeleton;
        const char* previous_why = nullptr;
        if (previous_is_the_mods && g_settings.ptskin_test != 1 &&
            MigotoPort::ShimBufferForCompute(d3d12_cl, 'c', 3, &previous_skeleton, &previous_why) &&
            previous_skeleton.va && previous_skeleton.va != skeleton.va) {
            previous_bones_va = previous_skeleton.va;
            previous_bone_count = previous_skeleton.size / kBoneBytes;
        }
        if (LogOnce(18, once_key))
            logger::write("PTSKIN-MERGED mod=%d comp=%d: previous frame's skeleton at vs-cb3: %s", dm, comp,
                previous_bones_va           ? "the mod's, previous positions are skinned with it"
                    : !previous_bound       ? "nothing bound -- no motion vectors"
                    : !previous_is_the_mods ? "the game's buffer, the mod merged none -- no motion vectors"
                    : previous_why          ? previous_why
                                            : "not usable -- no motion vectors");
        if (LogOnce(16, once_key))
            logger::write("PTSKIN-MERGED mod=%d comp=%d: skinned with the skeleton the mod binds at vs-cb4 (%llx, %u "
                          "bytes, %u bones; vg_offset %u, vg_count %u, remap %d)",
                dm, comp, (unsigned long long)skeleton.va, skeleton.size, bone_count, wc->vg_offset, wc->vg_count,
                wc->remap_id);
    } else {
        const auto palette_found = s_gpu.palettes.find({ dm, PaletteKey(section) });
        if (palette_found == s_gpu.palettes.end()) {
            s_ptskin_skipped.fetch_add(1, std::memory_order_relaxed);
            if (LogOnce(13, once_key))
                logger::write("PTSKIN mod=%d comp=%d: no copy of the section's bone palette yet", dm, comp);
            return CannotSkin();
        }
        PtSkinGpu::Palette& palette = palette_found->second;
        // This frame's slot, else the newest one; the previous palette is the newest slot older than that.
        for (int i = 0; i < PtSkinGpu::kSlots; ++i) {
            if (palette.bytes[i] == 0 || palette.frame[i] > g_frame_id) continue;
            if (current_index < 0 || palette.frame[i] > palette.frame[current_index]) current_index = i;
        }
        if (current_index < 0) {
            s_ptskin_skipped.fetch_add(1, std::memory_order_relaxed);
            if (LogOnce(13, once_key))
                logger::write("PTSKIN mod=%d comp=%d: no copy of the section's bone palette yet", dm, comp);
            return CannotSkin();
        }
        for (int i = 0; i < PtSkinGpu::kSlots; ++i) {
            if (palette.bytes[i] == 0 || palette.frame[i] >= palette.frame[current_index]) continue;
            if (previous_index < 0 || palette.frame[i] > palette.frame[previous_index]) previous_index = i;
        }
        current_slot = &palette.slot[current_index];
        bones_va = current_slot->va();
        bone_count = palette.bytes[current_index] / kBoneBytes;
        current_frame = palette.frame[current_index];
        // The previous palette gives the positions motion vectors are made from; without it, no motion.
        if (previous_index >= 0 && g_settings.ptskin_test != 1) {
            previous_slot = &palette.slot[previous_index];
            previous_bones_va = previous_slot->va();
            previous_bone_count = palette.bytes[previous_index] / kBoneBytes;
            previous_frame = palette.frame[previous_index];
        }
    }
    const bool has_previous = previous_bones_va != 0;
    static std::atomic<int> s_trace_draws{ 0 };
    if (g_debug && comp == 0 && s_trace_draws.fetch_add(1) < 64)
        logger::write("PTSKIN-TRACE draw mod=%d comp=%d frame=%u out=%llx slot=%d (frame %u) previous=%d (frame %u) "
                      "pso=%llx list_type=%d",
            dm, comp, g_frame_id, (unsigned long long)section.out_va, current_index, current_frame, previous_index,
            previous_frame, (unsigned long long)state.gfx_pso, (int)d3d12_cl->GetType());
    LogPtSkinReadback();
    PtSkinGpu::Outputs* outputs = OutputsOf(device.Get(), once_key, vertices);
    if (!outputs) return {};
    const uint64_t gen = g_command_lists.Gen(cmd_list);
    if (current_slot) Xit(d3d12_cl, gen, *current_slot, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    if (previous_slot) Xit(d3d12_cl, gen, *previous_slot, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    ReadyModBufferForSkin(d3d12_cl, positions.res);
    ReadyModBufferForSkin(d3d12_cl, vectors.res);
    ReadyModBufferForSkin(d3d12_cl, blend.res);
    ReadyModBufferForSkin(d3d12_cl, colors.res);
    Xit(d3d12_cl, gen, outputs->positions, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Xit(d3d12_cl, gen, outputs->tangents, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    Xit(d3d12_cl, gen, outputs->colors, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    if (has_previous) Xit(d3d12_cl, gen, outputs->previous_positions, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);

    const uint32_t weights_per_vertex = blend.stride / 2; // half of vb4 is bone indices, half weights
    uint32_t constants[5] = { vertices, bone_count, 0, 1, weights_per_vertex };
    memcpy(&constants[2], &section.scale, sizeof(float));
    d3d12_cl->SetComputeRootSignature(s_gpu.root_signature.Get());
    d3d12_cl->SetPipelineState(s_gpu.pso.Get());
    d3d12_cl->SetComputeRoot32BitConstants(0, 5, constants, 0);
    d3d12_cl->SetComputeRootShaderResourceView(1, bones_va);
    d3d12_cl->SetComputeRootShaderResourceView(2, positions.va);
    d3d12_cl->SetComputeRootShaderResourceView(3, vectors.va);
    d3d12_cl->SetComputeRootShaderResourceView(4, blend.va);
    d3d12_cl->SetComputeRootShaderResourceView(5, colors.va);
    d3d12_cl->SetComputeRootUnorderedAccessView(6, outputs->positions.va());
    d3d12_cl->SetComputeRootUnorderedAccessView(7, outputs->tangents.va());
    d3d12_cl->SetComputeRootUnorderedAccessView(8, outputs->colors.va());
    d3d12_cl->Dispatch((vertices + 63) / 64, 1, 1);
    if (has_previous) { // the same vertices with the previous frame's palette, positions only
        uint32_t previous_constants[5] = { vertices, previous_bone_count, 0, 0, weights_per_vertex };
        memcpy(&previous_constants[2], &section.scale, sizeof(float));
        d3d12_cl->SetComputeRoot32BitConstants(0, 5, previous_constants, 0);
        d3d12_cl->SetComputeRootShaderResourceView(1, previous_bones_va);
        d3d12_cl->SetComputeRootUnorderedAccessView(6, outputs->previous_positions.va());
        d3d12_cl->Dispatch((vertices + 63) / 64, 1, 1);
    }

    Xit(d3d12_cl, gen, outputs->tangents, kDrawReadState);
    Xit(d3d12_cl, gen, outputs->colors, D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER);
    if (g_debug && comp == s_gpu.readback_done % 8 && s_gpu.readback_frame == 0 && s_gpu.readback_done < 480 &&
        g_frame_id >= s_gpu.readback_next_frame) {
        if (!s_gpu.readback) {
            s_gpu.readback = WwmiMakeBuffer(device.Get(), 512, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_FLAG_NONE,
                D3D12_RESOURCE_STATE_COPY_DEST);
            void* mapped = nullptr;
            if (s_gpu.readback && SUCCEEDED(s_gpu.readback->Map(0, nullptr, &mapped)))
                s_gpu.readback_data = static_cast<const uint8_t*>(mapped);
        }
        if (s_gpu.readback_data) {
            Xit(d3d12_cl, gen, outputs->positions, D3D12_RESOURCE_STATE_COPY_SOURCE);
            s_gpu.readback_first_bone = 0;
            s_gpu.readback_palette_frame = 0;
            if (current_slot) {
                Xit(d3d12_cl, gen, *current_slot, D3D12_RESOURCE_STATE_COPY_SOURCE);
                d3d12_cl->CopyBufferRegion(s_gpu.readback.Get(), 0, current_slot->res.Get(), 0, 96);
            } else if (skeleton.res) {
                // The mod's merged skeleton: the first two bones this component owns in it (a remapped
                // skeleton starts with them), and the game's palette for the section, to compare.
                const uint32_t first_bone = wc->remap_id >= 0 ? 0 : wc->vg_offset;
                const uint64_t offset = skeleton.va - skeleton.res->GetGPUVirtualAddress() +
                                        (uint64_t)first_bone * kBoneBytes;
                if (((uint64_t)first_bone + 2) * kBoneBytes <= skeleton.size) {
                    const bool readable = (skeleton.state & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
                    D3D12_RESOURCE_BARRIER barrier{};
                    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
                    barrier.Transition.pResource = skeleton.res;
                    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
                    barrier.Transition.StateBefore = (D3D12_RESOURCE_STATES)skeleton.state;
                    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
                    if (!readable) d3d12_cl->ResourceBarrier(1, &barrier);
                    d3d12_cl->CopyBufferRegion(s_gpu.readback.Get(), 0, skeleton.res, offset, 96);
                    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
                    if (!readable) d3d12_cl->ResourceBarrier(1, &barrier);
                    s_gpu.readback_first_bone = first_bone;
                }
                const auto section_palette = s_gpu.palettes.find({ dm, PaletteKey(section) });
                if (section_palette != s_gpu.palettes.end()) {
                    PtSkinGpu::Palette& palette = section_palette->second;
                    int newest = -1;
                    for (int i = 0; i < PtSkinGpu::kSlots; ++i) {
                        if (palette.bytes[i] == 0 || palette.frame[i] > g_frame_id) continue;
                        if (newest < 0 || palette.frame[i] > palette.frame[newest]) newest = i;
                    }
                    if (newest >= 0) {
                        Xit(d3d12_cl, gen, palette.slot[newest], D3D12_RESOURCE_STATE_COPY_SOURCE);
                        d3d12_cl->CopyBufferRegion(s_gpu.readback.Get(), 288, palette.slot[newest].res.Get(), 0, 96);
                        Xit(d3d12_cl, gen, palette.slot[newest], D3D12_RESOURCE_STATE_COMMON);
                        s_gpu.readback_palette_frame = palette.frame[newest];
                    }
                }
            }
            for (uint32_t i = 0; i < 16; ++i) { // vertex i * vertices / 16
                const uint64_t vertex = (uint64_t)i * vertices / 16;
                d3d12_cl->CopyBufferRegion(s_gpu.readback.Get(), 96 + i * 12, outputs->positions.res.Get(),
                    vertex * kModPositionStride, kModPositionStride);
            }
            s_gpu.readback_vertices = vertices;
            s_gpu.readback_frame = g_frame_id;
            s_gpu.readback_key = once_key;
            s_gpu.readback_slot_frame = current_frame;
        }
    }
    if (g_debug) {
        WriteSkinDumps();
        if (IsSkinDumpFrame(dm)) {
            Xit(d3d12_cl, gen, outputs->positions, D3D12_RESOURCE_STATE_COPY_SOURCE);
            RecordSkinDump(device.Get(), d3d12_cl, dm, once_key, outputs->positions.res.Get(), vertices);
        }
    }
    Xit(d3d12_cl, gen, outputs->positions, kDrawReadState);
    if (has_previous) Xit(d3d12_cl, gen, outputs->previous_positions, kDrawReadState);
    if (current_slot) Xit(d3d12_cl, gen, *current_slot, D3D12_RESOURCE_STATE_COMMON);
    if (previous_slot) Xit(d3d12_cl, gen, *previous_slot, D3D12_RESOURCE_STATE_COMMON);
    if (const uint64_t game_pso = state.gfx_restore())
        d3d12_cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(game_pso));
    const bool compute_restored = RestoreGameComputeRoot(cmd_list, d3d12_cl);

    // The game's layout: position, tangents, color, UVs, previous position.
    const uint64_t previous_va = has_previous ? outputs->previous_positions.va() : outputs->positions.va();
    const D3D12_VERTEX_BUFFER_VIEW views[5] = {
        { outputs->positions.va(), vertices * kModPositionStride, kModPositionStride },
        { outputs->tangents.va(), vertices * kOutTangentStride, kOutTangentStride },
        { outputs->colors.va(), vertices * kModColorStride, kModColorStride },
        { texcoords.va, texcoords.size, kModTexcoordStride },
        { previous_va, vertices * kModPositionStride, kModPositionStride },
    };
    d3d12_cl->IASetVertexBuffers(0, 5, views);

    // The pass-through VS takes only the position from the input layout. Tangents, UVs, the color and the previous
    // position it loads by vertex id from buffer SRVs, which the game points at its own mesh.
    ReadyModBufferForSkin(d3d12_cl, texcoords.res);
    PtSkinPass::FetchViews fetch;
    fetch.tan = outputs->tangents.res.Get();
    fetch.prev = has_previous ? outputs->previous_positions.res.Get() : outputs->positions.res.Get();
    fetch.uv = texcoords.res;
    fetch.uv_first = texcoords.res ? (texcoords.va - texcoords.res->GetGPUVirtualAddress()) / 4 : 0;
    fetch.uv_elems = texcoords.size / 4;
    fetch.color = colors.res;
    fetch.color_first = colors.res ? (colors.va - colors.res->GetGPUVirtualAddress()) / kModColorStride : 0;
    fetch.vcount = vertices;
    const VfRestore vf_restore = StampVertexFetch(cmd_list, d3d12_cl, fetch, dm, comp);
    s_ptskin_draws.fetch_add(1, std::memory_order_relaxed);
    if (LogOnce(14, once_key))
        logger::write("PTSKIN mod=%d comp=%d: %u vertices skinned with the section at %llx (%zu in the game's vb0; %u game vertices, %u bones, "
                      "scale %.3f, previous frame %s)%s",
            dm, comp, vertices, (unsigned long long)section.out_va, section_total, section.count, bone_count, section.scale,
            has_previous ? "skinned" : "not known yet", compute_restored ? "" : " -- no game compute root to put back");
    return vf_restore;
}

/**
 * Hands a merged-skeleton mod the bones of the game's section before the mod's component section runs. Without
 * ray tracing such a mod reads the game's skeleton at vs-cb4 and copies it into its merged skeleton (WWMI's
 * SkeletonMerger). On the ray-tracing draw path the game binds no skeleton there, so the palette copied at the
 * game's skin dispatch is published as vs-cb4 instead: same layout, 3 float4 rows per bone.
 *
 * @param cmd_list      The command list the draw is recorded on.
 * @param d3d12_cl      The same list.
 * @param dm            The mod's id.
 * @param wc            The component about to run.
 * @param vertex_offset The game draw's base vertex.
 * @return tried when the draw is a pass-through draw of a merged-skeleton mod (the caller then publishes no
 *         other skeleton); published when the palette was handed over.
 */
RtPalResult RtDrawPass::PalPublish(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* d3d12_cl, int dm,
    const WwmiMod::Component* wc, int32_t vertex_offset)
{
    RtPalResult result;
    if (!wc || !d3d12_cl || dm < 0 || wc->vg_count == 0) return result;
    const CmdState& state = g_command_lists.Gfx(cmd_list);
    if (!IsPassThroughPso(state.gfx_pso)) return result;
    result.tried = true;
    const int comp = wc->index;
    const uint64_t window_va = state.vb[0].va + (int64_t)vertex_offset * kModPositionStride;
    const uint64_t window_end = state.vb[0].va + state.vb[0].size;
    SkinSection section;
    size_t section_total = 0;
    if (!state.vb[0].va || !SectionInWindow(dm, comp, window_va, window_end, &section, &section_total)) return result;

    std::lock_guard lock(s_gpu.mutex);
    const auto palette_found = s_gpu.palettes.find({ dm, PaletteKey(section) });
    if (palette_found == s_gpu.palettes.end()) return result;
    PtSkinGpu::Palette& palette = palette_found->second;
    int current_index = -1; // this frame's copy, else the newest one
    for (int i = 0; i < PtSkinGpu::kSlots; ++i) {
        if (palette.bytes[i] == 0 || palette.frame[i] > g_frame_id) continue;
        if (current_index < 0 || palette.frame[i] > palette.frame[current_index]) current_index = i;
    }
    if (current_index < 0) return result;
    Owned& slot = palette.slot[current_index];
    // A buffer left in the common state is readable as a constant buffer without a transition.
    Xit(d3d12_cl, g_command_lists.Gen(cmd_list), slot, D3D12_RESOURCE_STATE_COMMON);
    PortApi::PushGameConstantBuffer('v', 4, slot.res.Get(), slot.va(), kDx11SkeletonCbByteWidth);
    result.published = true;
    // The newest copy older than that one is the previous frame's palette. The mod merges it into its second
    // skeleton (vs-cb3), which SkinDraw skins the previous positions with: without them the game sees no motion
    // of the character and its anti-aliasing breaks up while the character moves.
    int previous_index = -1;
    for (int i = 0; i < PtSkinGpu::kSlots; ++i) {
        if (palette.bytes[i] == 0 || palette.frame[i] >= palette.frame[current_index]) continue;
        if (previous_index < 0 || palette.frame[i] > palette.frame[previous_index]) previous_index = i;
    }
    if (previous_index >= 0 && g_settings.ptskin_test != 1) {
        Owned& previous = palette.slot[previous_index];
        Xit(d3d12_cl, g_command_lists.Gen(cmd_list), previous, D3D12_RESOURCE_STATE_COMMON);
        PortApi::PushGameConstantBuffer('v', 3, previous.res.Get(), previous.va(), kDx11SkeletonCbByteWidth);
        result.previous_published = true;
    }
    if (LogOnce(17, dm * 256 + (comp & 0xFF)))
        logger::write("PTSKIN-MERGED mod=%d comp=%d: the section's palette (%u bones, copied in frame %u) is published "
                      "as vs-cb4 for the mod's skeleton merge (vg_offset %u, vg_count %u)",
            dm, comp, palette.bytes[current_index] / kBoneBytes, palette.frame[current_index], wc->vg_offset,
            wc->vg_count);
    return result;
}
