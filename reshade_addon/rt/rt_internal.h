#pragma once
// rt/rt_internal.h -- what the source files of the portable RT module (WwmiRT=portable) share with each other.
// Nothing outside rt\ includes this; the module's public entry points are in rt_hooks.h, rt_rings.h,
// rt_pal_record.h, rt_diag_dispatch.h, rt_blas.h and rt_draw.h.
//
// The module's files, by job:
//   rt_portable.cpp       the globals, the skin rings, the BLAS hook, device loss, the overlay
//   rt_scene.cpp          RTSCENE (debug): where a mod character sits in the game's ray-tracing scene
//   rt_hide.cpp           RTHIDE: what a mod hides is cut out of the game's ray-tracing geometry
//   rt_pal_record.cpp     RtPalRecorder: which dispatch writes a mod's ring (debug log)
//   rt_skin_sections.cpp  PTSKIN: the game's skin dispatches recorded as a mod character's sections
//   rt_skin_gpu.cpp       PTSKIN: our compute pipeline, output buffers and bone palette copies
//   rt_skin_draw.cpp      PTSKIN: a mod's vertices skinned at the game's pass-through draw
//   rt_vertex_fetch.cpp   PTSKIN: the buffers the pass-through vertex shader loads by vertex id
//   rt_dumps.cpp          RTCSDUMP / RTVSDUMP (debug): the game's shaders saved for offline study
#include <imgui.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <set>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <wrl/client.h>
#include <string_view>
#include <string>
#include <atomic>
#include "../addon_common.h"
#include "../core/frame_clock.h"
#include "../core/log.h"
#include "../mods/character_recognition.h"
#include "../mods/layout_registry.h"
#include "../hooks/descriptor_shadow.h"
#include "../hooks/resource_tracker.h"
#include "../instrumentation/removal_pin.h"
#include "../migoto/dx12_bridge.h" // MigotoPort::kResStateUnknown
#include "../hooks/command_list_registry.h"
#include "../hooks/pipeline_registry.h"    // g_pipelines.SrvRegs
#include "../rabbitfx/rabbitfx_bind.h"     // g_rfx_bind, ShadowSlotOvr
#include "../mods/game_cb_reader.h"
#include "../mods/wwmi_runtime.h" // WwmiReadFile, WwmiMakeBuffer
#include "../addon/addon.h"        // g_addon.cso_dir
#include "../addon/settings.h"     // g_settings.ptskin_test
#include "../core/dxil_cb.h"
#include "../migoto/dx12_pipeline.h"     // MigotoPort::State, OwnedTransition
#include "../migoto/dx12_context_impl.h" // MigotoPort::ShimBufferForCompute: the skeleton a mod binds
#include "../migoto/port_api.h"          // PortApi::PushGameConstantBuffer
#include "../mods/port_section.h"        // kDx11SkeletonCbByteWidth
#include "../migoto/dx12_input_layout.h" // MigotoPort::DescribeInputLayout
#include "pt_skin_owned.h"               // Owned, Xit
#include "rt_hooks.h"
#include "rt_rings.h"
#include "rt_pal_record.h"
#include "rt_diag_dispatch.h"
#include "rt_blas.h"
#include "rt_draw.h"

namespace rt_portable {

// ---- rt_scene.cpp ----

// Debug: logs the game's scene build, and each instance in it that uses a mod character's BLAS.
void LogSceneInstances(uint32_t input_count, const reshade::api::acceleration_structure_build_input* inputs);
// Debug: remembers a BLAS the game builds from a mod character's mesh, for the RTSCENE log.
void NoteModBlas(uint64_t blas, int mod, uint32_t input_count);
// The device is being destroyed: forgets the BLAS noted so far.
void SceneOnDestroyDevice();

// ---- rt_hide.cpp ----

// Builds a mod character's BLAS in the game's place when the mod hides some of the game's geometry.
// Returns true when the BLAS was built here, so the game's build must not run.
bool BuildCutBlas(reshade::api::command_list* cmd_list, int mod, reshade::api::acceleration_structure_build_flags build_flags,
    uint32_t input_count, const reshade::api::acceleration_structure_build_input* inputs, reshade::api::resource_view dest);
// The device is being destroyed: releases the BLAS inputs built here and forgets the game's index buffers.
void HideOnDestroyDevice();

// ---- rt_skin_sections.cpp ----

/** One section of a mod character as the game skinned it in a frame. */
struct SkinSection {
    uint64_t out_va = 0;      // where the game wrote the section's skinned positions
    uint32_t count = 0;       // its vertices
    uint64_t bones_va = 0;    // its bone palette: 3 float4 rows per bone
    uint64_t bones_bytes = 0; // the palette's size
    ID3D12Resource* bones_res = nullptr; // the buffer the palette lives in
    float scale = 1.0f;
    uint32_t frame = 0;       // the frame the game skinned it in
};

// The vertex formats of the mod buffers PTSKIN reads (the WWMI ini's vb0..vb4) and of the draw it feeds.
constexpr uint32_t kModPositionStride = 12; // vb0 R32G32B32_FLOAT
constexpr uint32_t kModVectorStride = 8;    // vb1 TangentX + TangentZ, R8G8B8A8_SNORM each
constexpr uint32_t kModTexcoordStride = 16; // vb2
constexpr uint32_t kModColorStride = 4;     // vb3 R8G8B8A8_UNORM
constexpr uint32_t kModBlendStride = 16;    // vb4: 8 bone indices, 8 weights
constexpr uint32_t kModBlendStrideFour = 8; // vb4 of mods made with 4 weights per vertex: 4 indices, 4 weights
constexpr uint32_t kOutTangentStride = 16;  // our tangents: R16G16B16A16_SNORM x2
// Our outputs at the draw: vertex buffers, and buffer SRVs the vertex shader loads by vertex id.
constexpr D3D12_RESOURCE_STATES kDrawReadState =
    D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
constexpr uint32_t kBoneBytes = 48;         // 3 float4 rows
constexpr uint32_t kPtSkinMaxBones = 256;         // indices are 8-bit
// A palette copy's buffer: as big as the DX11 skeleton constant buffer a mod's merge commands read it as.
constexpr uint32_t kPaletteBufferBytes = 16384;

// How many pass-through draws PTSKIN skinned a mod for, and how many it could not (the overlay shows both).
extern std::atomic<uint64_t> s_ptskin_draws;
extern std::atomic<uint64_t> s_ptskin_skipped;

// Tells whether a message (`what`, about `key`) was not logged yet, and marks it logged. Only in debug.
bool LogOnce(int what, int key);
// Remembers where a skin-cache shader keeps its section values, read from the cbuffer size its DXIL declares.
void NoteSkinCbLayout(uint64_t cs_hash, const void* code, size_t code_size);
// Records a skin-cache dispatch that writes a mod character's ring as one of its sections.
void RecordSkinSection(reshade::api::command_list* cmd_list, const CmdState& state, uint64_t cs_hash);
// Finds a component's section among those the game skinned into the window its own draw binds at vb0.
// Returns false when the window holds no comp-th section; `total` receives how many sections lie in it.
bool SectionInWindow(int mod, int comp, uint64_t window_va, uint64_t window_end, SkinSection* out, size_t* total);
// Tells whether a graphics PSO reads the skin cache's vertex layout (the ray-tracing pass-through draw).
bool IsPassThroughPso(uint64_t pso);
// A graphics pipeline handle is new: forgets what IsPassThroughPso remembered for a destroyed one with that handle.
void ForgetPassThroughPso(uint64_t pso);
// The device is being destroyed: forgets the recorded sections, meshes and pipelines.
void SectionsOnDestroyDevice();

// ---- rt_skin_gpu.cpp ----

/**
 * Our compute pipeline (RtPtSkin.cso) and the output buffers of each mod component. Each component has its own:
 * the game records its lists in parallel and the GPU may run lists of one submission side by side, so one buffer
 * shared by a mod's components could be rewritten with another component's bones while a draw still reads it.
 */
struct PtSkinGpu {
    struct Outputs {
        Owned positions;
        Owned tangents;
        Owned previous_positions; // skinned with the previous frame's palette: the draw's slot 4
        Owned colors;             // the mod's colors with red and blue swapped: the game reads B8G8R8A8
        uint32_t vertices = 0;
        uint32_t last_frame = 0;  // the last frame a draw asked for them
    };
    // A section's bone palette, copied at the game's skin dispatch (RtPalCopy.cso). The game reuses its own
    // palette memory after the frame, so the mod is skinned from these.
    // One slot per frame, in turn: the depth, base and velocity passes of a frame all read that frame's slot,
    // even when the game records or runs the next frame's skin pass in between. The slot of the frame before
    // gives the motion vectors' previous positions.
    static constexpr int kSlots = 3;
    struct Palette {
        Owned slot[kSlots];
        uint32_t frame[kSlots] = {}; // the frame each slot was copied in
        uint32_t bytes[kSlots] = {}; // how much of it holds a palette; 0 = never copied
    };
    std::mutex mutex;
    bool tried = false;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pso;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> copy_pso; // RtPalCopy.cso, on the same root signature
    std::map<int, Outputs> by_component; // by mod * 256 + component
    // By mod and PaletteKey: the game writes a section to one of two rings in turn, so out_va changes every frame.
    std::map<std::pair<int, uint64_t>, Palette> palettes;
    // Debug only: a readback of one palette and a few skinned vertices, logged some frames later.
    Microsoft::WRL::ComPtr<ID3D12Resource> readback;
    const uint8_t* readback_data = nullptr;
    uint32_t readback_frame = 0;      // when the copy was recorded; 0 = none waiting
    uint32_t readback_next_frame = 0; // the earliest frame for the next one
    int readback_done = 0;
    int readback_key = 0;
    uint32_t readback_slot_frame = 0;
    uint32_t readback_first_bone = 0;    // the first of the two bones read from the skeleton skinned with
    uint32_t readback_palette_frame = 0; // the frame of the section palette read with it; 0 = none
    uint32_t readback_vertices = 0;
    // Outgrown buffers a list in flight may still read, with the frame they were replaced in.
    std::vector<std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, uint32_t>> retired;
    uint32_t purge_frame = 0; // the frame PurgePtSkinBuffers last ran in
};
extern PtSkinGpu s_gpu;

// The functions below that work on s_gpu are called with s_gpu.mutex held, except CopySectionPalette and
// SkinGpuOnDestroyDevice, which take it themselves.

// Creates the PTSKIN root signature and pipeline once. Returns true when they exist.
bool EnsurePtSkinPipeline(ID3D12Device* device);
// Releases the PTSKIN buffers that are no longer used, at most once per frame.
void PurgePtSkinBuffers();
// Gives a mod component's output buffers (`key` = mod * 256 + component), large enough for `vertices`; nullptr
// when they could not be made.
PtSkinGpu::Outputs* OutputsOf(ID3D12Device* device, int key, uint32_t vertices);
// Puts the game's compute root signature and its arguments back after our dispatch on its list. Returns false
// when the game had not bound one yet.
bool RestoreGameComputeRoot(reshade::api::command_list* cmd_list, ID3D12GraphicsCommandList* cl);
// Names a section the same way every frame: its vertex count and palette size.
uint64_t PaletteKey(const SkinSection& section);
// Copies a section's bone palette into the add-on's buffers, on the game's compute list right before the game's
// own skin dispatch for it.
void CopySectionPalette(reshade::api::command_list* cmd_list, const CmdState& state, int mod,
    const SkinSection& section);
// Makes a mod buffer readable by our compute pass, keeping it readable as a vertex buffer.
void ReadyModBufferForSkin(ID3D12GraphicsCommandList* cl, ID3D12Resource* res);
// Debug only: tells whether a mod's components are copied in this frame (or no frame was chosen for it yet).
bool IsSkinDumpFrame(int mod);
// Debug only: records a copy of a component's skinned positions when this is its mod's dump frame. Returns true
// when a copy was recorded.
bool RecordSkinDump(ID3D12Device* device, ID3D12GraphicsCommandList* d3d12_cl, int dm, int key,
    ID3D12Resource* positions, uint32_t vertices);
// Debug only: saves the copies RecordSkinDump made, once the GPU is surely done with them.
void WriteSkinDumps();
// Debug only: logs the readback recorded by SkinDraw once the GPU is surely done with it.
void LogPtSkinReadback();
// The device is being destroyed: releases the pipeline, the output buffers and the palette copies.
void SkinGpuOnDestroyDevice();

} // namespace rt_portable
