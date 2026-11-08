#pragma once
// What the addon hands the ported engine: the DX12 objects a transition writes into, per frame and per
// draw scope, and the resource-state query. Hand-written; dx12_context.h is generated.
#include <d3d11.h>
#include <d3d12.h>
#include <cstdint>
#include <string>

namespace MigotoPort {

// A list's own shaders at a draw in the game's state, and the state a [CustomShader] set around them; a null state
// (or no targets) is the game's.
struct ModPipeline {
    const void* code[5] = {}; // vs hs ds gs ps (StageIndex order); null = the list set none
    size_t code_size[5] = {};
    ID3D12RootSignature* root_signature = nullptr;
    const D3D11_BLEND_DESC* blend = nullptr;
    const D3D11_RASTERIZER_DESC* raster = nullptr;
    const D3D11_DEPTH_STENCIL_DESC* depth = nullptr;
    const D3D12_INPUT_ELEMENT_DESC* layout = nullptr;
    unsigned layout_count = 0;
    bool own_targets = false; // the list bound its own targets: these formats, and no depth when ds_format is none
    unsigned num_rt = 0;
    DXGI_FORMAT rt_format[8] = {};
    DXGI_FORMAT ds_format = DXGI_FORMAT_UNKNOWN;
    uint64_t key = 0; // of everything above
};

// The game's own bindings at the draw a scope runs for, for a vs/ps register their lists read and did not set since
// the scope opened (DX11: their lists run in the game's state).
struct GameBindings {
    // Writes the game's descriptor for <kind><reg> of <stage> (b or t) into dst; false when it has none to give.
    bool (*descriptor)(
        void* user, char stage, char kind, unsigned reg, unsigned cb_bytes, D3D12_CPU_DESCRIPTOR_HANDLE dst) = nullptr;
    // Publishes the game's texture at <stage>-t<slot> into the slot, or clears what an earlier publish left there.
    void (*publish_srv)(void* user, char stage, unsigned slot) = nullptr;
    // The game's pipeline at that draw with the list's shaders and root signature in its place and the rest of its
    // state kept, as DX11 keeps the context's state around a [CustomShader]'s shaders; null when it cannot be made.
    ID3D12PipelineState* (*pipeline)(void* user, const ModPipeline& mp) = nullptr;
    void* user = nullptr;
    uint32_t srv_gen = 0; // the slot write counters when the scope opened (SrvWriteGen, CbWriteGen)
    uint32_t cb_gen = 0;
};

// Set by the addon each frame: the DX12 objects a transition writes into.
struct Dx12Bridge {
    ID3D12Device* device = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr; // the game's current command list
    ID3D12CommandQueue* queue = nullptr;
    unsigned rt_width = 0;
    unsigned rt_height = 0;
    DXGI_FORMAT rt_format = DXGI_FORMAT_UNKNOWN;
    DXGI_FORMAT ds_format = DXGI_FORMAT_UNKNOWN;
    ID3D12Resource* back_buffer = nullptr;
    D3D12_RESOURCE_STATES back_buffer_state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    void (*restore_game_state)(void*) = nullptr;
    void* restore_user = nullptr;

    bool own_list = false; // true = ours ([Present]); false = the game's

    int (*vs_cb_root)(void* user, unsigned slot) = nullptr;
    void* vs_cb_root_user = nullptr;

    GameBindings game;

    unsigned long owner_tid = 0;
};
void SetDx12Bridge(const Dx12Bridge& b);
const Dx12Bridge& GetDx12Bridge();
unsigned long long BridgeForeignListRefusals(); // times a thread was handed the global with its cl nulled
void BridgePush(const Dx12Bridge& scoped); // DrawListScope ctor: `scoped` already carries cl/restore/roots
void BridgePop(); // DrawListScope dtor
void BridgeSetTargets(DXGI_FORMAT rt_format, DXGI_FORMAT ds_format); // this thread's innermost scope
void BridgeSetBackBuffer(ID3D12Resource* bb); // ...the same
void BridgeSetGameBindings(const GameBindings& game); // ...the same
const Dx12Bridge* BridgeScope(); // this thread's innermost scope; null when none is open
unsigned BridgeDepth(); // open scopes, all threads
void OweGameState(const Dx12Bridge& b); // after our compute work on the game's list
void PayGameState(); // before anything that draws with the game's state
bool BridgeNoteIssue(ID3D12GraphicsCommandList* cl, bool dispatch, unsigned a, unsigned b, unsigned c);
unsigned long long BridgeDisplacedCount();
unsigned long long BridgeWrongListDraws();
unsigned long long BridgeWrongListDispatches();

static const uint32_t kResStateUnknown = 0xFFFFFFFFu;
void SetResourceStateQuery(uint32_t (*fn)(void* d3d12_resource));
// kResStateUnknown when never transitioned or no query is installed; COMMON (0) is a real answer here.
uint32_t ObservedResourceState(void* d3d12_resource);
// The same with unknown folded into 0, for the copies that SKIP on 0 rather than guess a StateBefore.
unsigned QueryResourceState(void* d3d12_resource);

void SetDumpDir(const wchar_t* dir);
const std::wstring& DumpDir();
unsigned long long VbNullUnbinds();
void NoteVbNullUnbind();
void NoteEmittedDispatch(uint32_t x, uint32_t y, uint32_t z);
void ResetEmittedDispatch();
unsigned TakeEmittedDispatch(uint32_t* x, uint32_t* y, uint32_t* z); // count since the reset; resets
void NoteVbBind(const void* state, int nonnull);
void VbBindStats(unsigned* calls, unsigned* nonnull, uint64_t* last_ps);

inline thread_local bool g_touched_viewport = false;
inline thread_local bool g_touched_blend = false;
inline thread_local bool g_touched_stencil = false;
inline thread_local bool g_touched_root_sig = false; // a draw of ours bound its own graphics root signature
inline thread_local bool g_touched_rt = false;
inline thread_local bool g_touched_ia = false;
// [Present] run again for a present no game frame came before (port_api.cpp PresentRedraw): its draws only.
inline thread_local bool g_present_redraw = false;
} // namespace MigotoPort
