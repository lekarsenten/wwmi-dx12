// rt/rt_hide.cpp -- RTHIDE: what a mod hides of the game's own geometry is cut out of the game's
// ray-tracing geometry.
//
// The game traces rays against its own mesh. Where a mod skips one of the game's draws without drawing anything
// (a hidden tail), or draws only some index ranges of it (a hidden hair strand), reflections still showed the
// part. Such a BLAS is built here in the game's place, with the hidden triangles' indices set to 0.

/** What a mod's component section leaves of the game's draw it replaces. */
#include "rt_internal.h"

using namespace rt_portable;

namespace rt_portable {

struct ComponentCut {
    uint32_t index_count = 0; // the game's draw
    bool cut = false; // false = the game's geometry stays whole
    std::vector<RtDrawnRange> keep; // index ranges that stay, counted from the draw's first index
    uint64_t signature = 0; // changes when `cut` or `keep` do
};

/** One input of a BLAS built here: the index buffer with the hidden triangles' indices set to 0. */
struct CutInput {
    Microsoft::WRL::ComPtr<ID3D12Resource> indices;
    uint64_t signature = 0;
    uint64_t source_va = 0;
};

/** A BLAS of the game's that is built here every frame. */
struct CutBlas {
    Microsoft::WRL::ComPtr<ID3D12Resource> scratch;
    uint64_t scratch_bytes = 0;
    std::vector<CutInput> inputs;
    uint32_t last_frame = 0;
};

constexpr uint32_t kCutRetireFrames = 32; // a replaced buffer is kept this long: lists in flight may still read it
constexpr uint32_t kCutIdleFrames = 600; // a BLAS not built for this long is forgotten
constexpr uint32_t kCutMaxInputs = 64;

std::mutex s_cut_mutex; // guards everything below
std::map<std::pair<int, uint32_t>, ComponentCut> s_cuts; // by mod and the game draw's first index
/** An index buffer as the game binds it for its draws. */
struct GameIndexBuffer {
    ID3D12Resource* res = nullptr;
    uint32_t bytes = 0;
};
std::map<uint64_t, GameIndexBuffer> s_game_index_buffers; // by the address the game binds
std::unordered_map<uint64_t, CutBlas> s_cut_blas; // by the BLAS's address
std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, uint32_t>> s_cut_retired;
std::atomic<int> s_cut_logs{ 0 };
uint32_t s_cut_purge_frame = 0; // the frame PurgeCutBuffers last ran in

void PurgeCutBuffers();

/**
 * Mixes a value into a signature.
 *
 * @param signature The signature so far.
 * @param value     The value to add.
 * @return The new signature.
 */
uint64_t MixSignature(uint64_t signature, uint64_t value)
{
    return (signature ^ value) * 1099511628211ull;
}

} // namespace rt_portable

void RtNoteComponentDraw(int mod, ID3D12Resource* game_ib, uint64_t game_ib_va, uint32_t game_ib_bytes,
    uint32_t game_first_index, uint32_t game_index_count, bool skip_original, const RtDrawnRange* drawn,
    uint32_t drawn_count)
{
    // Cut only when the game's draw is skipped and what is drawn instead lies inside the game's own index range
    // and is less than all of it: then the mod draws the game's geometry in part (or not at all). Anything else
    // is the mod's own geometry, which the game's ray-tracing mesh does not have.
    bool cut = skip_original;
    uint64_t total = 0;
    uint64_t signature = MixSignature(14695981039346656037ull, game_index_count);
    for (uint32_t i = 0; i < drawn_count && cut; ++i) {
        const uint64_t end = (uint64_t)drawn[i].first + drawn[i].count;
        if (drawn[i].first < game_first_index || end > (uint64_t)game_first_index + game_index_count) cut = false;
        total += drawn[i].count;
        signature = MixSignature(MixSignature(signature, drawn[i].first), drawn[i].count);
    }
    if (total >= game_index_count) cut = false;
    if (!cut) signature = 1;

    std::lock_guard lock(s_cut_mutex);
    PurgeCutBuffers();
    if (game_ib && game_ib_va) s_game_index_buffers[game_ib_va] = GameIndexBuffer{ game_ib, game_ib_bytes };
    ComponentCut& entry = s_cuts[{ mod, game_first_index }];
    if (entry.signature == signature) return;
    entry.index_count = game_index_count;
    entry.cut = cut;
    entry.signature = signature;
    entry.keep.clear();
    for (uint32_t i = 0; i < drawn_count && cut; ++i)
        entry.keep.push_back(RtDrawnRange{ drawn[i].count, drawn[i].first - game_first_index });
    if (g_debug && s_cut_logs.fetch_add(1) < 40)
        logger::write("RTHIDE mod=%d first_index=%u (%u indices): %s, %zu range(s) kept (frame %u)", mod,
            game_first_index, game_index_count, cut ? "cut from the game's ray-tracing geometry" : "left whole",
            entry.keep.size(), g_frame_id);
}

namespace rt_portable {

/**
 * Releases what RTHIDE no longer needs, at most once per frame. Call with s_cut_mutex held.
 */
void PurgeCutBuffers()
{
    if (s_cut_purge_frame == g_frame_id) return;
    s_cut_purge_frame = g_frame_id;
    for (size_t i = 0; i < s_cut_retired.size();) {
        if (g_frame_id > s_cut_retired[i].second + kCutRetireFrames) {
            s_cut_retired[i] = std::move(s_cut_retired.back());
            s_cut_retired.pop_back();
        } else {
            ++i;
        }
    }
    for (auto it = s_cut_blas.begin(); it != s_cut_blas.end();) {
        if (g_frame_id > it->second.last_frame + kCutIdleFrames) {
            if (it->second.scratch) s_cut_retired.push_back({ it->second.scratch, g_frame_id });
            for (CutInput& input : it->second.inputs)
                if (input.indices) s_cut_retired.push_back({ input.indices, g_frame_id });
            it = s_cut_blas.erase(it);
        } else {
            ++it;
        }
    }
}

/**
 * Finds the index buffer, as the game's draws bind it, that holds an address. Call with s_cut_mutex held.
 *
 * @param index_va The address of some indices.
 * @param bound_va Receives the address the game binds the buffer at.
 * @return The buffer; its `res` is null when no draw has bound a buffer holding the address.
 */
GameIndexBuffer GameIndexBufferAt(uint64_t index_va, uint64_t* bound_va)
{
    auto it = s_game_index_buffers.upper_bound(index_va);
    if (it == s_game_index_buffers.begin()) return {};
    --it;
    if (index_va - it->first >= it->second.bytes) return {};
    *bound_va = it->first;
    return it->second;
}

/**
 * Makes the index buffer of one cut input: zeros, with the kept ranges copied in from the game's index buffer.
 *
 * @param cl          The game's command list.
 * @param device      The device.
 * @param game_ib     The game's index buffer.
 * @param game_offset Where the input's indices start in it, in bytes.
 * @param index_size  Bytes per index.
 * @param cut         What stays of the input.
 * @return The buffer, readable by the build; null when it could not be made.
 */
Microsoft::WRL::ComPtr<ID3D12Resource> MakeCutIndices(ID3D12GraphicsCommandList* cl, ID3D12Device* device,
    ID3D12Resource* game_ib, uint64_t game_offset, uint32_t index_size, const ComponentCut& cut)
{
    // The copy needs the game's buffer as a copy source; its state is known only from the game's own barriers.
    const uint32_t state = g_resources.StateOf(game_ib);
    if (state == MigotoPort::kResStateUnknown) return nullptr;
    // A new committed buffer is all zeros: index 0 three times is a triangle no ray can hit.
    Microsoft::WRL::ComPtr<ID3D12Resource> indices = WwmiMakeBuffer(device, (uint64_t)cut.index_count * index_size,
        D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_FLAG_NONE, D3D12_RESOURCE_STATE_COPY_DEST);
    if (!indices) return nullptr;
    const D3D12_RESOURCE_STATES before = (D3D12_RESOURCE_STATES)state;
    const bool readable = (before & D3D12_RESOURCE_STATE_COPY_SOURCE) != 0;
    if (!cut.keep.empty() && !readable) TransitionBarrier(cl, game_ib, before, before | D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (const RtDrawnRange& range : cut.keep)
        cl->CopyBufferRegion(indices.Get(), (uint64_t)range.first * index_size, game_ib,
            game_offset + (uint64_t)range.first * index_size, (uint64_t)range.count * index_size);
    if (!cut.keep.empty() && !readable) TransitionBarrier(cl, game_ib, before | D3D12_RESOURCE_STATE_COPY_SOURCE, before);
    TransitionBarrier(cl, indices.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return indices;
}

/**
 * Builds a mod character's BLAS in the game's place when the mod hides some of the game's geometry. Once a BLAS
 * has been built here it always is: the game only updates it, and an update keeps the triangles of the last build.
 *
 * @param cmd_list    The game's command list (ReShade's).
 * @param mod         The mod the BLAS belongs to.
 * @param build_flags The game's build flags.
 * @param input_count How many inputs the build has.
 * @param inputs      The game's inputs.
 * @param dest        The BLAS.
 * @return true when the BLAS was built here, so the game's build must not run.
 */
bool BuildCutBlas(reshade::api::command_list* cmd_list, int mod, reshade::api::acceleration_structure_build_flags build_flags,
    uint32_t input_count, const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource_view dest)
{
    if (g_settings.ptskin_test == 4 || !cmd_list || !dest.handle || !input_count || input_count > kCutMaxInputs)
        return false;
    for (uint32_t i = 0; i < input_count; ++i)
        if (inputs[i].type != reshade::api::acceleration_structure_build_input_type::triangles) return false;

    std::lock_guard lock(s_cut_mutex);
    PurgeCutBuffers();
    const ComponentCut* cuts[kCutMaxInputs] = {};
    bool any_cut = false;
    for (uint32_t i = 0; i < input_count; ++i) {
        // The draw the input belongs to: its first index, counted from where the game's draws bind the buffer.
        const uint32_t index_size = inputs[i].triangles.index_format == reshade::api::format::r16_uint ? 2u : 4u;
        const uint64_t index_va = RtBlasHook::Va(inputs[i].triangles.index_buffer, inputs[i].triangles.index_offset);
        uint64_t bound_va = 0;
        const bool bound = GameIndexBufferAt(index_va, &bound_va).res != nullptr;
        const uint32_t first = bound ? (uint32_t)((index_va - bound_va) / index_size) : 0;
        static std::atomic<int> s_match_logs{ 0 };
        if (g_debug && s_match_logs.fetch_add(1) < 24)
            logger::write("RTHIDE match: mod=%d input %u indices at %llx (%u): %s, first_index=%u", mod, i,
                (unsigned long long)index_va, inputs[i].triangles.index_count,
                bound ? "inside an index buffer the game draws with" : "in no index buffer a draw has bound", first);
        if (!bound) continue;
        const auto found = s_cuts.find({ mod, first });
        if (found == s_cuts.end() || !found->second.cut || found->second.index_count != inputs[i].triangles.index_count)
            continue;
        cuts[i] = &found->second;
        any_cut = true;
    }
    auto blas_found = s_cut_blas.find(dest.handle);
    if (!any_cut && blas_found == s_cut_blas.end()) return false;

    ID3D12GraphicsCommandList* cl = reinterpret_cast<ID3D12GraphicsCommandList*>(cmd_list->get_native());
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList4> cl4;
    Microsoft::WRL::ComPtr<ID3D12Device5> device;
    if (!cl || FAILED(cl->QueryInterface(IID_PPV_ARGS(&cl4))) || FAILED(cl->GetDevice(IID_PPV_ARGS(&device)))) return false;

    if (blas_found == s_cut_blas.end()) {
        blas_found = s_cut_blas.emplace(dest.handle, CutBlas{}).first;
        if (g_debug)
            logger::write("RTHIDE: the BLAS at %llx of mod %d (%u inputs) is built by the add-on from now on (frame %u)",
                (unsigned long long)dest.handle, mod, input_count, g_frame_id);
    }
    CutBlas& blas = blas_found->second;
    blas.last_frame = g_frame_id;
    blas.inputs.resize(input_count);

    D3D12_RAYTRACING_GEOMETRY_DESC geometries[kCutMaxInputs] = {};
    for (uint32_t i = 0; i < input_count; ++i) {
        const auto& triangles = inputs[i].triangles;
        const uint32_t index_size = triangles.index_format == reshade::api::format::r16_uint ? 2u : 4u;
        const uint64_t index_va = RtBlasHook::Va(triangles.index_buffer, triangles.index_offset);
        CutInput& input = blas.inputs[i];
        uint64_t use_index_va = index_va;
        if (cuts[i]) {
            if (!input.indices || input.signature != cuts[i]->signature || input.source_va != index_va) {
                uint64_t bound_va = 0;
                ID3D12Resource* game_ib = GameIndexBufferAt(index_va, &bound_va).res;
                Microsoft::WRL::ComPtr<ID3D12Resource> made;
                if (game_ib && g_resources.Live(game_ib))
                    made = MakeCutIndices(cl, device.Get(), game_ib, index_va - game_ib->GetGPUVirtualAddress(),
                        index_size, *cuts[i]);
                if (input.indices) s_cut_retired.push_back({ input.indices, g_frame_id });
                input.indices = made;
                input.signature = cuts[i]->signature;
                input.source_va = index_va;
                if (g_debug && s_cut_logs.fetch_add(1) < 40)
                    logger::write("RTHIDE: input %u of the BLAS at %llx: %s (%zu range(s) of %u indices kept)", i,
                        (unsigned long long)dest.handle,
                        made ? "cut index buffer made" : "no cut -- the game's index buffer is not known or its state is not",
                        cuts[i]->keep.size(), cuts[i]->index_count);
            }
            if (input.indices) use_index_va = input.indices->GetGPUVirtualAddress();
        } else if (input.indices) {
            s_cut_retired.push_back({ input.indices, g_frame_id });
            input = CutInput{};
        }
        D3D12_RAYTRACING_GEOMETRY_DESC& geometry = geometries[i];
        geometry.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
        geometry.Flags = (D3D12_RAYTRACING_GEOMETRY_FLAGS)inputs[i].flags;
        geometry.Triangles.Transform3x4 = RtBlasHook::Va(triangles.transform_buffer, triangles.transform_offset);
        geometry.Triangles.IndexFormat = (DXGI_FORMAT)triangles.index_format;
        geometry.Triangles.VertexFormat = (DXGI_FORMAT)triangles.vertex_format;
        geometry.Triangles.IndexCount = triangles.index_count;
        geometry.Triangles.VertexCount = triangles.vertex_count;
        geometry.Triangles.IndexBuffer = use_index_va;
        geometry.Triangles.VertexBuffer.StartAddress = RtBlasHook::Va(triangles.vertex_buffer, triangles.vertex_offset);
        geometry.Triangles.VertexBuffer.StrideInBytes = triangles.vertex_stride;
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC desc = {};
    desc.Inputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    desc.Inputs.Flags = (D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS)(
        (uint32_t)build_flags & ~(uint32_t)D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE);
    desc.Inputs.NumDescs = input_count;
    desc.Inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    desc.Inputs.pGeometryDescs = geometries;
    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO sizes = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&desc.Inputs, &sizes);
    if (!sizes.ScratchDataSizeInBytes) return false;
    if (!blas.scratch || blas.scratch_bytes < sizes.ScratchDataSizeInBytes) {
        if (blas.scratch) s_cut_retired.push_back({ blas.scratch, g_frame_id });
        blas.scratch = WwmiMakeBuffer(device.Get(), sizes.ScratchDataSizeInBytes, D3D12_HEAP_TYPE_DEFAULT,
            D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        blas.scratch_bytes = blas.scratch ? sizes.ScratchDataSizeInBytes : 0;
        if (!blas.scratch) return false;
    }
    desc.DestAccelerationStructureData = dest.handle;
    desc.ScratchAccelerationStructureData = blas.scratch->GetGPUVirtualAddress();
    cl4->BuildRaytracingAccelerationStructure(&desc, 0, nullptr);
    RemovalPin::Ours(cl, "rt-cut-blas");
    return true;
}

void HideOnDestroyDevice()
{
    std::lock_guard lock(s_cut_mutex);
    s_cut_blas.clear();
    s_cut_retired.clear();
    s_game_index_buffers.clear();
}

} // namespace rt_portable
