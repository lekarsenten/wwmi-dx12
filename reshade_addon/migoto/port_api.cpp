// ===========================================================================
//  port_api.cpp -- OURS. The addon side of the seam, speaking their engine's language.
//
//  Compiled with THEIR flags (see tools/build_migoto_lib.bat) so it can include their headers; the addon
//  only ever sees port_api.h, which mentions nothing of theirs.
//
//  Every function here is a thin call into their code. That is the point: [Present] is their
//  present_command_list, key handling is their DispatchInputEvents, the override dispatch is their
//  lookup_shaderoverride / lookup_textureoverride. Nothing is re-derived here -- when this file starts
//  containing logic instead of calls, the port has begun drifting again.
// ===========================================================================
#include "port_api.h"
#include "port_api_internal.h"

#include "globals.h"
#include "IniHandler.h"
#include "CommandList.h"
#include "ResourceHash.h"
#include "DrawCallInfo.h"
#include "Input.h"
#include "Override.h" // CurrentTransition -- the commit half of a keypress, see TickInput
#include "Overlay.h"
#include "HackerDevice.h"
#include "HackerContext.h"
#include "frame_analysis_engine.h" // FrameAnalysisContext -- g_ctx IS one
#include "key_gate.h"
#include "command_observer.h"
#include "selection_watch.h"
#include "port_compat.h"
#include "dx12_context.h"
#include "dx12_context_impl.h"
#include "dx12_pipeline.h"
#include "dx12_present_snap.h"
#include "dx12_wrap.h"
#include "../core/log.h"

#include <chrono>
#include <mutex>
#include <vector>
#include <set>
#include <map>
#include <string>
#include <atomic>
#include <unordered_set>
#include <unordered_map>
#include <share.h> // _SH_DENYNO -- their exact log-open flags (see the pre-open in Load)

namespace MigotoPort {
void InitEngineGlobals();
}

namespace PortApi {

HackerDevice g_dev;
FrameAnalysisContext g_ctx{ nullptr, nullptr };
bool g_loaded = false;
static unsigned g_constants_generation = ~0u;
unsigned g_generation = 0;
static EngineValues g_step_inputs; // what the step's [Present] started from: a redraw runs from the same
static bool g_step_inputs_taken = false; // ...for this load of the inis

FrameAnalysisContext* FrameAnalysisCtx()
{
    return &g_ctx;
}

std::recursive_mutex& EngineLock()
{
    static auto* m = new std::recursive_mutex();
    return *m;
}

static void (*g_parse_observer)(void* user) = nullptr;
static void* g_parse_observer_user = nullptr;
static bool (*g_reload_gate_fn)(void* user) = nullptr;
static void* g_reload_gate_user = nullptr;
static double g_last_parse_ms = 0.0;

static void NotifyParse(std::chrono::steady_clock::time_point parse_start)
{
    if (!G->gConfigInitialized && G->gSkipEarlyIncludesLoad) return;
    g_last_parse_ms =
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - parse_start).count();
    if (g_parse_observer) g_parse_observer(g_parse_observer_user);
}

bool Load(const wchar_t* ini_root)
{
    if (g_loaded) return true;

    MigotoPort::InitEngineGlobals();
    // Before LoadConfigFile: this is what it resolves d3dx.ini against.
    MigotoPort::SetIniRoot(ini_root);

    {
        wchar_t dir[MAX_PATH] = {};
        MigotoPort::MigotoDir(dir, MAX_PATH);
        wchar_t asseen[MAX_PATH] = {};
        PortRealGetModuleFileNameW(migoto_handle, asseen, MAX_PATH);
        logger::write("PORT-PATHS: ini_root='%S'  MigotoDir->'%S'  migoto_handle=%p  real module='%S'",
            ini_root ? ini_root : L"(null)", dir, (void*)migoto_handle, asseen);
    }
    g_dev.mHackerContext = &g_ctx;
    g_ctx.mHackerDevice = &g_dev;

    const auto parse_start = std::chrono::steady_clock::now();
    LoadConfigFile();
    RebuildParseSnapshots();

    g_dev.CreateIniParamResources();

    g_ctx.Bind3DMigotoResources();
    logger::write("PORT: IniParams t%d -- texture %p view %p (%d params reserved)", G->IniParamsReg,
        (void*)g_dev.mIniTexture, (void*)g_dev.mIniResourceView, G->iniParamsReserved);

    g_loaded = true;
    ++g_generation;
    g_step_inputs_taken = false;
    logger::write("PORT: engine loaded -- %zu command lists, %zu custom shaders, %zu resources, "
                  "%zu texture overrides, %zu shader overrides, %d ini params",
        registered_command_lists.size(), customShaders.size(), customResources.size(), G->mTextureOverrideMap.size(),
        G->mShaderOverrideMap.size(), G->iniParamsReserved);
    {
        std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
        NotifyParse(parse_start);
    }
    return true;
}

void SetParseObserver(void (*after_parse)(void* user), void* user)
{
    g_parse_observer = after_parse;
    g_parse_observer_user = user;
}

void SetReloadGate(bool (*may_reload)(void* user), void* user)
{
    g_reload_gate_fn = may_reload;
    g_reload_gate_user = user;
}

void RequestReload()
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    G->gReloadConfigPending = true;
}

double LastParseMs()
{
    return g_last_parse_ms;
}

void SaveSettingsOnExit()
{
    if (!g_loaded) {
        logger::write("PORT-EXIT: no settings save -- engine not loaded (probe unload)");
        return;
    }
    if (!G->gConfigInitialized) {
        logger::write("PORT-EXIT: no settings save -- deferred config reload never ran, so the mod tree "
                      "was never parsed and every variable is still at its declared default");
        return;
    }
    if (g_constants_generation == ~0u) {
        logger::write("PORT-EXIT: no settings save -- [Constants] never applied, so d3dx_user.ini was "
                      "never read back into the variables it would be written from");
        return;
    }
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    logger::write("PORT-EXIT: saving persistent settings (dirty=%d)", G->user_config_dirty);
    if (SavePersistentSettings()) SaveUnknownPersistentSettings();
}

void SetFrameContext(ID3D12Device* dev, ID3D12GraphicsCommandList* cl, ID3D12CommandQueue* queue, unsigned rt_width,
    unsigned rt_height, DXGI_FORMAT rt_format, DXGI_FORMAT ds_format, ID3D12Resource* back_buffer,
    D3D12_RESOURCE_STATES back_buffer_state, void (*restore_game_state)(void*), void* restore_user)
{
    MigotoPort::Dx12Bridge b;
    b.restore_user = restore_user;
    b.device = dev;
    b.cl = cl;
    b.queue = queue;
    b.rt_width = rt_width;
    b.rt_height = rt_height;
    b.rt_format = rt_format;
    b.ds_format = ds_format;
    b.back_buffer = back_buffer;
    b.back_buffer_state = back_buffer_state;
    b.restore_game_state = restore_game_state;
    b.own_list = (cl != nullptr); // the [Present] bracket's list is ours
    MigotoPort::SetDx12Bridge(b);
    MigotoPort::BackBufferPublished(back_buffer, back_buffer_state);

    if (back_buffer && G->mResolutionInfo.from == GetResolutionFrom::SWAP_CHAIN) {
        const D3D12_RESOURCE_DESC rd = back_buffer->GetDesc();
        if (G->mResolutionInfo.width != (int)rd.Width || G->mResolutionInfo.height != (int)rd.Height) {
            G->mResolutionInfo.width = (int)rd.Width;
            G->mResolutionInfo.height = (int)rd.Height;
            logger::write(
                "PORT: resolution from swap chain: %dx%d", G->mResolutionInfo.width, G->mResolutionInfo.height);
        }
    }
}

void SetGameWindow(void* hwnd)
{
    static std::atomic<void*> s_set{ nullptr };
    if (!hwnd || s_set.load(std::memory_order_relaxed) == hwnd) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    G->hWnd = (HWND)hwnd;
    s_set.store(hwnd, std::memory_order_relaxed);
    logger::write("PORT: game window %p -- cursor_x/y and window_width/height are taken from its client area", hwnd);
}

unsigned BridgeStackDepth()
{
    return MigotoPort::BridgeDepth();
}

DrawListScope::DrawListScope(ID3D12GraphicsCommandList* cl, void (*restore_game_state)(void*), void* restore_user,
    int (*vs_cb_root)(void*, unsigned), void* vs_cb_root_user)
{
    MigotoPort::Dx12Bridge b = MigotoPort::GetDx12Bridge();
    b.cl = cl;
    b.restore_game_state = restore_game_state;
    b.restore_user = restore_user;
    b.vs_cb_root = vs_cb_root;
    b.vs_cb_root_user = vs_cb_root_user;
    b.game = {}; // an outer scope's game draw is not this one's
    b.own_list = false; // a draw scope records onto the GAME's list -- inherit its state
    b.back_buffer = nullptr; // the present thread's, when it is mid-tick: already presented when this list runs
    MigotoPort::BridgePush(b);
}

DrawListScope::~DrawListScope()
{
    MigotoPort::FlushUavBarriers(MigotoPort::GetDx12Bridge().cl);
    MigotoPort::BridgePop();
}

void DrawListScope::Targets(DXGI_FORMAT rt_format, DXGI_FORMAT ds_format)
{
    MigotoPort::BridgeSetTargets(rt_format, ds_format);
}

bool DrawListScope::BackBuffer(ID3D12Resource* game_rt0)
{
    if (!MigotoPort::IsBackBuffer(game_rt0)) return false;
    MigotoPort::BridgeSetBackBuffer(game_rt0);
    MigotoPort::BackBufferPublished(game_rt0, D3D12_RESOURCE_STATE_RENDER_TARGET);
    return true;
}

void DrawListScope::Inherit(bool (*descriptor)(void*, char, char, unsigned, unsigned, D3D12_CPU_DESCRIPTOR_HANDLE),
    void (*publish_srv)(void*, char, unsigned), ID3D12PipelineState* (*pipeline)(void*, const MigotoPort::ModPipeline&),
    void* user)
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    MigotoPort::GameBindings g;
    g.descriptor = descriptor;
    g.publish_srv = publish_srv;
    g.pipeline = pipeline;
    g.user = user;
    g.srv_gen = MigotoPort::SrvWriteGen();
    g.cb_gen = MigotoPort::CbWriteGen();
    MigotoPort::BridgeSetGameBindings(g);
}

static std::atomic<unsigned> g_refused_no_cl{ 0 };
unsigned TakeRefusedNoCl()
{
    return g_refused_no_cl.exchange(0);
}

static bool HaveCommandList(const char* what)
{
    if (MigotoPort::GetDx12Bridge().cl) return true;
    if (!logger::enabled(1)) return false;
    const unsigned n = g_refused_no_cl.fetch_add(1);
    if ((n % 600u) == 0u)
        logger::write("PORT-REFUSE %s: no DX12 command list bound (occurrence %u). Running it would mutate "
                      "their variables while every dispatch/copy is discarded -- which is how the ini ends "
                      "up claiming work that never reached the GPU.",
            what, n + 1);
    return false;
}

static void RunConstantsOnce()
{
    if (g_constants_generation == g_generation) return;
    g_constants_generation = g_generation;

    RunCommandList(&g_dev, &g_ctx, &G->constants_command_list, nullptr, false);
    G->user_config_dirty = false;
    RunCommandList(&g_dev, &g_ctx, &G->post_constants_command_list, nullptr, true);

    logger::write("PORT: [Constants] applied (generation %u) -- persisted globals from d3dx_user.ini "
                  "are now live; hotkey toggles have a value to hold",
        g_generation);
}

static void FrameDriverClock()
{
    // Theirs: HackerSwapChain::RunFrameActions. fflush first so a crash loses at most one frame of their log.
    if (LogFile) fflush(LogFile);
    const uint64_t system_tick_count = GetSystemTicks();
    G->gFrameTime = (system_tick_count - G->gSystemTickCount) / 1'000'000.0f;
    G->gTime = (system_tick_count - G->ticks_at_launch) / 1'000'000.0f;
    G->gSystemTickCount = system_tick_count;
    G->gFPSCounter.Update(system_tick_count);
}

static void BumpGeneration()
{
    std::atomic_thread_fence(std::memory_order_seq_cst);
    volatile unsigned& gen = g_generation;
    gen = gen + 1;
    std::atomic_thread_fence(std::memory_order_seq_cst);
}

static void FrameDriverConfig()
{
    if (G->gReloadConfigPending && (!g_reload_gate_fn || g_reload_gate_fn(g_reload_gate_user))) {
        const auto parse_start = std::chrono::steady_clock::now();
        BumpGeneration();
        ReloadConfig(&g_dev);
        RebuildParseSnapshots(); // MANDATORY after every parse
        BumpGeneration();
        g_step_inputs_taken = false;
        MigotoPort::g_selection_watch.Clear();
        NotifyParse(parse_start);
    }

    if (G->gConfigInitialized) {
        if (G->gTime - G->gSettingsSaveTime > G->gSettingsAutoSaveInterval)
            if (SavePersistentSettings()) // updates gSettingsSaveTime itself
                SaveUnknownPersistentSettings();
    } else if (G->gTime > G->gConfigInitializationDelay) {
        G->gConfigInitialized = true;
        logger::write("PORT-FRAME: deferred config reload firing at gTime=%.3f (delay=%d) -- this is what "
                      "loads the [Include] tree when skip_early_includes_load=1",
            G->gTime, G->gConfigInitializationDelay);
        const auto parse_start = std::chrono::steady_clock::now();
        BumpGeneration();
        ReloadConfig(&g_dev);
        RebuildParseSnapshots(); // THE PARSE THAT ACTUALLY LOADS THE MODS.
        BumpGeneration();
        MigotoPort::g_selection_watch.Clear();
        NotifyParse(parse_start);
    }
}

void PresentPre()
{
    {
        std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
        if (g_loaded) FrameDriverClock();
    }
    if (!HaveCommandList("[Present] pre")) {
        std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
        if (g_loaded) FrameDriverConfig();
        return;
    }
    MigotoPort::UploadFrameTick(); // releases retired resources the GPU has passed (src/gpu_progress.h)
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    // Before the first [Present]: their load order is [Constants] then everything else.
    RunConstantsOnce();
    G->frame_no++;
    MigotoPort::g_selection_watch.SweepVars();
    g_step_inputs.Take();
    g_step_inputs_taken = true;
    {
        using Snap = MigotoPort::PresentSnapshots;
        const MigotoPort::Dx12Bridge& b = MigotoPort::GetDx12Bridge();
        const Snap::Mode mode = b.own_list && !b.back_buffer ? Snap::Mode::Capture : Snap::Mode::Off;
        MigotoPort::g_present_snapshots.Begin(mode, g_generation);
    }
    RunCommandList(&g_dev, &g_ctx, &G->present_command_list, nullptr, false);
    MigotoPort::g_present_snapshots.End();
    RunSkelCopyUpdates();
    FrameDriverConfig();
    g_ctx.ResetCallCounters();
}

void PresentPost()
{
    if (!HaveCommandList("[Present] post")) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    MigotoPort::g_present_snapshots.Resume();
    RunCommandList(&g_dev, &g_ctx, &G->post_present_command_list, nullptr, true);
    MigotoPort::g_present_snapshots.End();
}

void PresentRedraw()
{
    if (!HaveCommandList("[Present] redraw")) return;
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    EngineValues before;
    before.Take();
    if (g_step_inputs_taken) g_step_inputs.PutBack();
    MigotoPort::g_present_redraw = true;
    MigotoPort::g_present_snapshots.Begin(MigotoPort::PresentSnapshots::Mode::Replay, g_generation);
    RunCommandList(&g_dev, &g_ctx, &G->present_command_list, nullptr, false);
    RunCommandList(&g_dev, &g_ctx, &G->post_present_command_list, nullptr, true);
    MigotoPort::g_present_snapshots.End();
    MigotoPort::g_present_redraw = false;
    before.PutBack();
    g_ctx.ResetCallCounters();
}

void SetKeyGate(bool (*fn)(const wchar_t* ns, const wchar_t* section))
{
    MigotoPort::g_key_gate.SetGate(fn);
}

void TickInput()
{
    std::lock_guard<std::recursive_mutex> engine_lk(EngineLock());
    if (!g_loaded) return;
    DispatchInputEvents(&g_dev);

    CurrentTransition.UpdatePresets(&g_dev);
    CurrentTransition.UpdateTransitions(&g_dev);
    MigotoPort::g_selection_watch.SweepVars();
}

EngineScope::EngineScope()
{
    EngineLock().lock();
}
EngineScope::~EngineScope()
{
    EngineLock().unlock();
}

unsigned LoadGeneration()
{
    return g_generation;
}
long WrapResourcesCreated()
{
    long c = 0;
    long d = 0;
    long l = 0;
    MigotoPort::WrapStats(&c, &d, &l);
    return c;
}
long WrapChurn()
{
    long c = 0;
    long d = 0;
    MigotoPort::WrapStats(&c, &d, nullptr);
    return c + d;
}
unsigned SelectionInputsGen()
{
    return MigotoPort::g_selection_watch.InputsGen();
}
bool SelectionCacheOk()
{
    return g_loaded && !G->analyse_frame && !MigotoPort::g_command_observer.Any();
}

Stats GetStats()
{
    Stats s;
    s.loaded = g_loaded;
    if (!g_loaded) return s;
    s.command_lists = registered_command_lists.size();
    s.custom_shaders = customShaders.size();
    s.custom_resources = customResources.size();
    s.texture_overrides = G->mTextureOverrideMap.size();
    s.shader_overrides = G->mShaderOverrideMap.size();
    s.ini_params = G->iniParamsReserved;
    return s;
}

void GetGpuWorkTotals(GpuWorkTotals* out)
{
    if (!out) return;
    out->iniparams_updates = Profiling::iniparams_updates;
    out->full_copies = Profiling::resource_full_copies;
    out->reference_copies = Profiling::resource_reference_copies;
    out->region_copies = Profiling::buffer_region_copies;
    out->inter_device_copies = Profiling::inter_device_copies;
    out->msaa_resolutions = Profiling::msaa_resolutions;
    out->views_cleared = Profiling::views_cleared;
    out->resources_created = Profiling::resources_created;
    out->pool_swaps = Profiling::resource_pool_swaps;
    out->max_copies_exceeded = Profiling::max_copies_per_frame_exceeded;
    out->injected_draws = Profiling::injected_draw_calls;
    out->max_executions_exceeded = Profiling::max_executions_per_frame_exceeded;
    ReplayCounts(&out->replay_top_replayed, &out->replay_top_recorded, &out->replay_top_plain,
        &out->replay_sub_replayed, &out->replay_sub_recorded, &out->replay_sub_plain);
}

} // namespace PortApi
